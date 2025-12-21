#pragma once

#include <cstdint>
#include <bit>
#include <linux/types.h>

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
    using type = std::array<char, BLOCK_SIZE>;

    bool sorted = false;
    type buf {};
    int n_ent = 0;
    int p_str = BLOCK_SIZE;

    erofs_dirent& get(int i) {
      return block_view<erofs_dirent>(buf)[i];
    }

    void push(erofs_dirent const& ent, std::string_view name) {
      p_str -= name.size();
      copy(name, std::span{buf}.subspan(p_str));

      auto& _ent = get(n_ent++);
      _ent = ent;
      _ent.nameoff = p_str;
    }

    std::span<std::byte const> finalize() {
      if (!sorted) {
        std::vector<std::pair<std::string_view, int>> names;

        names.reserve(n_ent);
        for(int i = 0; i < n_ent; i++) {
          size_t start = get(i).nameoff;
          size_t end = i > 0 ? get(i-1).nameoff : buf.size();
          names.emplace_back(std::string_view(&buf[start], end - start), i);
        }
        std::ranges::sort(names);

        {
          type tmp {};
          auto view = block_view<erofs_dirent>(tmp);

          int j = 0;
          p_str = n_ent * sizeof(erofs_dirent);
          for(auto const& [name, i]: names) {
            view[j] = get(i);
            view[j]->nameoff = p_str;

            copy(name, std::span{tmp}.subspan(p_str));
            p_str += name.size();
            ++j;
          }
          swap(tmp, buf);
        }

        sorted = true;
      }

      return as_bytes(buf).subspan(0, p_str);
    }
  };

  struct MetadataBuilder {
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

      void link() { get().i_nlink++; }
    };

    std::vector<std::array<std::byte, INODE_SIZE>> inodes;

    Node get(nid_t nid, std::size_t aux_size) { return { this, nid, aux_size }; }

    Node push(erofs_inode_compact inode, size_t aux_size = 0) {
      auto next = inodes.size();
      auto size = round_up(sizeof(erofs_inode_compact) + aux_size, INODE_SIZE);
      inodes.resize(next + size / INODE_SIZE, {});
      fmt::println(stderr, "nid: {}, size: {}", next, size);
      Node ret = get(next, size);
      ret.get() = inode;
      return ret;
    }

    Node push(FileInfo const& info,
              uint16_t i_format = inode_format(false, EROFS_INODE_FLAT_PLAIN), size_t aux_size = 0) {
      Node ret = push(inode_from_info(info), aux_size);
      ret->i_format = i_format;
      return ret;
    }

    Node push(FileInfo const& info, std::span<std::byte const> content) {
      Node ret = push(info, inode_format(false, EROFS_INODE_FLAT_INLINE), content.size());
      ret->i_size = content.size();
      copy(content, ret.aux());
      return ret;
    }

    Node push(FileInfo const& info, DirectoryBuilder& dir) {
      auto content = dir.finalize();
      return push(info, as_bytes(content));
    }

    Node push(FileInfo const& info, ChunkMap const& chunks) {
      Node ret = push(info, inode_format(false, EROFS_INODE_CHUNK_BASED), chunks.indexes.size() * 4);
      ret->i_size = chunks.size;
      ret->i_u.c = chunk_format(chunks.shift - std::countr_zero(BLOCK_SIZE), false);
      copy(as_bytes(chunks.indexes), ret.aux());
      return ret;
    }

    std::vector<std::byte> finalize() {
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
        .i_nlink = 1,
        .i_size = (uint32_t)info.size,
        .i_u = erofs_inode_i_u { .raw_blkaddr = (__le32)-1 },
        .i_ino = (uint32_t)info.ino,
        .i_uid = (uint16_t)info.uid,
        .i_gid = (uint16_t)info.gid,
      };
    }
  };
}
