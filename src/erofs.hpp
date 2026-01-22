#pragma once

#include <cstdint>
#include <bit>
#include <linux/types.h>
#include <numeric>
#include <queue>
#include <algorithm>

#include <ostree/fileinfo.hpp>

#define __packed __attribute__((__packed__))

using u8 = uint8_t;

constexpr uint16_t le16_to_cpu(uint16_t x) {
  if constexpr(std::endian::native == std::endian::big)
    return std::byteswap(x);
  return x;
}

constexpr auto round_up(auto x, auto m) {
  return ((x + m - 1) / m) * m;
}

#define BUILD_BUG_ON(x) static_assert(!(x))

#include <erofs_fs.h>

#undef BLOCK_SIZE

#include "util.hpp"
#include "extents.hpp"

namespace erofs {
  static uint8_t mode_to_filetype(repo_mode_t mode) {
    switch ((mode & S_IFMT)) {
      case S_IFREG:
        return EROFS_FT_REG_FILE;
      case S_IFLNK:
        return EROFS_FT_SYMLINK;
      case S_IFDIR:
        return EROFS_FT_DIR;
      default:
        return EROFS_FT_UNKNOWN;
    }
  }

  static constexpr size_t BLOCK_SIZE = 4096;
  static constexpr size_t INODE_SIZE = sizeof(erofs_inode_compact);

  static constexpr int CHUNK_MIN_SHIFT = std::countr_zero(BLOCK_SIZE);
  static constexpr int CHUNK_MAX_SHIFT = CHUNK_MIN_SHIFT + EROFS_CHUNK_FORMAT_BLKBITS_MASK;

  using nid_t = uint64_t;
  using blkaddr_t = uint32_t;

  struct DirectoryBuilder {
    std::vector<std::array<char, BLOCK_SIZE>> buf;
    size_t len = 0;

    std::vector<erofs_dirent> dirs;
    std::vector<std::pair<size_t, size_t>> names;
    std::string name_buf;

    void push(erofs_dirent const& ent, std::string_view name) {
      auto start = name_buf.size();
      name_buf.resize(name_buf.size() + name.size());
      copy(name, std::span{name_buf}.subspan(start));

      names.emplace_back(start, name.size());
      dirs.push_back(ent);
    }

    size_t final_size(bool need_dotdot) {
      size_t ret = name_buf.size() + dirs.size() * sizeof(erofs_dirent);
      if (need_dotdot) ret += 2 + sizeof(erofs_dirent);
      return ret;
    }

    std::span<std::byte const> finalize() {
      if (dirs.size()) {
        std::vector<size_t> order(dirs.size());
        std::iota(order.begin(), order.end(), 0);

        auto get_name = [&](int i) {
          return std::string_view { &name_buf[names[i].first], names[i].second };
        };

        std::ranges::sort(order, std::ranges::less{}, [&](int i){ return get_name(i); });

        std::vector<size_t> total_space(dirs.size(), 0);
        for (int i = 0; i < order.size(); i++) {
          total_space[i] = sizeof(erofs_dirent) + get_name(order[i]).size();
          if (i) total_space[i] += total_space[i-1];
        }

        auto for_block = [&](auto&& f) {
          for (size_t s = 0, nblocks = 0; s < order.size(); nblocks++) {
            size_t prefix = s ? total_space[s - 1] : 0;
            size_t t = std::ranges::lower_bound(total_space, prefix + BLOCK_SIZE) - total_space.begin();

            if (auto sz = total_space[t - 1] - prefix; sz > BLOCK_SIZE)
              throw std::runtime_error(fmt::format("logic error: [{}, {}), size={}", s, t, sz));

            FORWARD(f)(nblocks, std::span{ &order[s], t - s });
            s = t;
          }
        };

        {
          int nblocks = 0;
          for_block([&](auto, auto){ nblocks++; });
          buf.resize(nblocks);
        }

        len = 0;
        for_block([&](size_t nblocks, auto order) {
          auto block = std::span{ buf[nblocks].data(), BLOCK_SIZE };
          auto block_dirs = block_view<erofs_dirent>(block);

          int nameoff = order.size() * sizeof(erofs_dirent);

          int next = 0;
          for (int i: order) {
            block_dirs[next] = dirs[i];
            block_dirs[next]->nameoff = nameoff;
            next++;

            auto name = get_name(i);
            copy(name, block.subspan(nameoff));
            nameoff += name.size();
          }

          len = std::max(len, nblocks * BLOCK_SIZE + nameoff);
        });

        dirs.clear();
        names.clear();
        name_buf.clear();
      }

      return as_bytes(buf).subspan(0, len);
    }
  };

  struct MetadataBuilder {
    static auto constexpr INODES_PER_BLOCK = BLOCK_SIZE / INODE_SIZE;

    struct Node {
      MetadataBuilder* p;
      nid_t nid;
      std::size_t size;

      explicit operator bool() { return !!p; }
      operator nid_t() { return nid; }

      erofs_inode_compact& get() {
        return block_view<erofs_inode_compact>(p->inodes)[nid];
      }

      erofs_inode_compact* operator->() { return &get(); }
      erofs_inode_compact& operator*() { return get(); }

      std::span<std::byte> aux() {
        return std::span(&p->inodes[nid][0], size).subspan(sizeof(erofs_inode_compact));
      }

      void link() { get().i_nb.nlink++; }
    };

    std::vector<std::array<std::byte, INODE_SIZE>> inodes;
    size_t next_inode = 0;

    std::queue<std::pair<size_t, size_t>> holes;
    std::vector<nid_t> with_addr_in_meta;

    Node get(nid_t nid, std::size_t aux_size) { return { this, nid, aux_size }; }

    std::span<std::byte> punch_hole(int len_blocks) {
      auto start = round_up(next_inode, INODES_PER_BLOCK);
      auto end = start + len_blocks * INODES_PER_BLOCK;

      if (inodes.size() < end) inodes.resize(end);
      holes.push({start, end});

      return std::span{&inodes[start][0], (end - start) * INODE_SIZE};
    }

    Node push(erofs_inode_compact inode, size_t aux_size = 0) {
      auto size = round_up(sizeof(erofs_inode_compact) + aux_size, INODE_SIZE) / INODE_SIZE;
      for(; holes.size() && holes.front().first < next_inode + size; holes.pop()) {
        if (next_inode + size <= holes.front().second) next_inode = holes.front().second;

        if ((next_inode + 1) / INODES_PER_BLOCK != (next_inode + size - 1) / INODES_PER_BLOCK) {
          auto offset = INODES_PER_BLOCK - ((next_inode + 1) % INODES_PER_BLOCK);
          next_inode += offset;
        }
      }

      auto next = next_inode;

      next_inode = next + size;
      if (inodes.size() < next_inode)
        inodes.resize(next_inode);

      fmt::println(stderr, "nid: {}, size: {}", next, size);
      Node ret = get(next, size * INODE_SIZE);

      ret.get() = inode;
      return ret;
    }

    Node push(FileInfo const& info,
              uint16_t i_format = inode_format(false, EROFS_INODE_FLAT_PLAIN), size_t aux_size = 0) {
      Node ret = push(inode_from_info(info), aux_size);
      ret->i_format = i_format;
      return ret;
    }

    static constexpr auto tail_size(size_t size) {
      auto nblocks = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
      return nblocks <= 1 ? size : size - (nblocks - 1) * BLOCK_SIZE;
    }

    Node& fill(Node& node, std::span<std::byte const> content) {
      node->i_size = content.size();

      if (content.size() > node.aux().size()) {
        auto nblocks = (content.size() + BLOCK_SIZE - 1) / BLOCK_SIZE;

        int tail_blocks = 1;
        auto tail = (nblocks - tail_blocks) * BLOCK_SIZE;

        if (content.size() - tail > node.aux().size()) {
          tail = content.size();
          tail_blocks = 0;

          node->i_format &= ~(EROFS_I_DATALAYOUT_MASK << EROFS_I_DATALAYOUT_BIT);
          node->i_format |= (EROFS_INODE_FLAT_PLAIN << EROFS_I_DATALAYOUT_BIT);
        }

        auto ext = punch_hole(nblocks - tail_blocks);

        copy(content.subspan(0, tail), ext);
        node->i_u.startblk_lo = (ext.data() - &inodes[0][0]) / BLOCK_SIZE;

        content = content.subspan(tail);
      }

      if (content.size())
        copy(content, node.aux());

      return node;
    }

    Node push(FileInfo const& info, std::span<std::byte const> content) {
      Node ret = push(info, inode_format(false, EROFS_INODE_FLAT_INLINE), tail_size(content.size()));
      return fill(ret, content);
    }

    Node push(FileInfo const& info, DirectoryBuilder& dir) {
      auto content = dir.finalize();
      Node ret = push(info, as_bytes(content));
      ret->i_format |= (1 << EROFS_I_DOT_OMITTED_BIT);
      return ret;
    }

    Node push(FileInfo const& info, ChunkMap const& chunks) {
      Node ret = push(info, inode_format(false, EROFS_INODE_CHUNK_BASED), chunks.indexes.size() * 4);
      ret->i_size = chunks.size;
      ret->i_u.c = chunk_format(chunks.shift - std::countr_zero(BLOCK_SIZE), false);
      copy(as_bytes(chunks.indexes), ret.aux());
      return ret;
    }

    std::vector<std::byte> finalize(size_t meta_blkaddr) {
      for(auto id: with_addr_in_meta) {
        auto node = get(id, 0);
        fmt::println(stderr, "fixing {}", id);
        node->i_u.startblk_lo += meta_blkaddr;
      }
      with_addr_in_meta.clear();

      auto b = as_bytes(inodes);
      return std::vector<std::byte>{b.begin(), b.end()};
    }

    static constexpr uint16_t inode_format(bool is_extended, int layout) {
      uint16_t f = is_extended ? EROFS_INODE_LAYOUT_EXTENDED : EROFS_INODE_LAYOUT_COMPACT;
      uint16_t l = layout & EROFS_I_DATALAYOUT_MASK;
      return (f << EROFS_I_VERSION_BIT) | (l << EROFS_I_DATALAYOUT_BIT);
    }

    static constexpr erofs_inode_chunk_info chunk_format(int shift, bool use_struct) {
      uint16_t s = shift & EROFS_CHUNK_FORMAT_BLKBITS_MASK;
      uint16_t i = use_struct;
      static constexpr auto EROFS_CHUNK_FORMAT_INDEXES_BIT = std::countr_zero((unsigned)EROFS_CHUNK_FORMAT_INDEXES);
      return erofs_inode_chunk_info {
        .format = uint16_t(s | (i << EROFS_CHUNK_FORMAT_INDEXES_BIT)),
      };
    }

    erofs_inode_compact inode_from_info(FileInfo const& info) {
      return {
        .i_xattr_icount = 0,
        .i_mode = (uint16_t)info.mode,
        .i_nb = { .nlink = 1 },
        .i_size = (uint32_t)info.size,
        .i_u = erofs_inode_i_u { .startblk_lo = (__le32)-1 },
        .i_ino = (uint32_t)info.ino,
        .i_uid = (uint16_t)info.uid,
        .i_gid = (uint16_t)info.gid,
      };
    }
  };
}
