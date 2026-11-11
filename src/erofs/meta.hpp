#pragma once

#include <ostree/fileinfo.hpp>

#include <util/util.hpp>
#include <util/alloc.hpp>
#include <hostfs/extents.hpp>
#include "base.hpp"

#include <fmt/core.h>

namespace erofs {
  static constexpr uint8_t mode_to_filetype(repo_mode_t mode) {
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

  static constexpr uint16_t inode_layout(erofs_inode_compact const& ino) {
    return (ino.i_format >> EROFS_I_DATALAYOUT_BIT) & EROFS_I_DATALAYOUT_MASK;
  }

  static constexpr uint16_t inode_chunk_bits(erofs_inode_compact const& ino) {
    return (ino.i_u.c.format & EROFS_CHUNK_FORMAT_BLKBITS_MASK);
  }

  static constexpr size_t inode_tail_size(erofs_inode_compact const& ino) {
    switch (inode_layout(ino)) {
      case EROFS_INODE_FLAT_INLINE: return ino.i_size % BLOCK_SIZE;
      case EROFS_INODE_CHUNK_BASED: {
        auto chunk_size = BLOCK_SIZE << inode_chunk_bits(ino);
        return round_up(ino.i_size, chunk_size) / chunk_size * sizeof(uint32_t);
      }
      default: return 0;
    }
  }

  static constexpr uint16_t inode_format(int layout) {
    uint16_t f = EROFS_INODE_LAYOUT_COMPACT;
    uint16_t l = layout & EROFS_I_DATALAYOUT_MASK;
    return (f << EROFS_I_VERSION_BIT) | (l << EROFS_I_DATALAYOUT_BIT) | (1 << EROFS_I_NLINK_1_BIT);
  }

  static constexpr erofs_inode_chunk_info chunk_format(int shift, bool use_struct) {
    uint16_t s = shift & EROFS_CHUNK_FORMAT_BLKBITS_MASK;
    uint16_t i = use_struct;
    static constexpr auto EROFS_CHUNK_FORMAT_INDEXES_BIT = std::countr_zero((unsigned)EROFS_CHUNK_FORMAT_INDEXES);
    return erofs_inode_chunk_info {
      .format = uint16_t(s | (i << EROFS_CHUNK_FORMAT_INDEXES_BIT)),
    };
  }

  static erofs_inode_compact inode_from_info(FileInfo const& info, int layout, uint32_t size = -1) {
    return {
      .i_format = inode_format(layout),
      .i_xattr_icount = 0,
      .i_mode = (uint16_t)info.mode,
      .i_nb = { .startblk_hi = (uint16_t)0, },
      .i_size = size == -1 ? (uint32_t)info.size : size,
      .i_u = erofs_inode_i_u { .startblk_lo = (uint32_t)-1 },
      .i_ino = (uint32_t)info.ino,
      .i_uid = (uint16_t)info.uid,
      .i_gid = (uint16_t)info.gid,
    };
  }

  struct MetadataBuilder {
    struct Node {
      MetadataBuilder* p;
      nid_t nid;

      explicit operator bool() { return !!p; }
      operator nid_t() { return nid; }

      erofs_inode_compact& get() {
        return block_view<erofs_inode_compact>(p->blocks.get_block(nid * INODE_SIZE + p->real_base))[0];
      }

      erofs_inode_compact* operator->() { return &get(); }
      erofs_inode_compact& operator*() { return get(); }

      std::span<std::byte> get_block(std::size_t offset) {
        auto const& ino = get();
        std::size_t tail_size = inode_tail_size(ino);

        if (inode_layout(ino) == EROFS_INODE_FLAT_INLINE || inode_layout(ino) == EROFS_INODE_FLAT_PLAIN) {
          if (offset < ino.i_size - tail_size)
            return p->blocks.get_block(ino.i_u.startblk_lo * BLOCK_SIZE + offset);

          offset -= ino.i_size - tail_size;
        }

        auto addr = (nid + 1) * INODE_SIZE + p->real_base + offset;
        return p->blocks.get_block(addr, tail_size - offset);
      }
    };

    std::size_t real_base;
    BlockAllocator<INODE_SIZE, BLOCK_SIZE> blocks;

    MetadataBuilder(std::size_t base_addr, std::size_t offset = 0): real_base(base_addr - offset), blocks(base_addr) {}

    Node get(nid_t nid) { return { this, nid }; }
    std::span<std::byte> get_block(nid_t nid, size_t offset) { return get(nid).get_block(offset); }

    Node push(erofs_inode_compact ino, std::span<std::byte const> inlined) {
      auto size = INODE_SIZE + inode_tail_size(ino);
      auto block = blocks.allocate(size);
      Node ret = get((blocks.get_address(block) - real_base) / INODE_SIZE);
      ret.get() = ino;

      if (inlined.size())
        copy(inlined, block.subspan(INODE_SIZE));

      return ret;
    }

    Node push_inline(FileInfo const& info, std::span<std::byte const> content = {}) {
      auto ino = inode_from_info(info, EROFS_INODE_FLAT_INLINE, content.size());
      auto tail_size = inode_tail_size(ino);

      if (INODE_SIZE + tail_size > BLOCK_SIZE || tail_size == 0) {
        ino.i_format = inode_format(EROFS_INODE_FLAT_PLAIN);
        tail_size = 0;
      }

      if (content.size() > tail_size) {
        auto body = blocks.allocate(round_up(content.size() - tail_size, BLOCK_SIZE));
        copy(content.subspan(0, content.size() - tail_size), body);
        ino.i_u.startblk_lo = blocks.get_address(body) / BLOCK_SIZE;
      }

      return push(ino, tail_size ? content.subspan(content.size() - tail_size) : std::span<std::byte>{});
    }

    Node push_chunks(FileInfo const& info, ChunkMap const& chunks) {
      auto ino = inode_from_info(info, EROFS_INODE_CHUNK_BASED, chunks.size);
      ino.i_u.c = chunk_format(chunks.shift - std::countr_zero(BLOCK_SIZE), false);

      return push(ino, as_bytes(chunks.indexes));
    }

    std::vector<std::byte> finalize() {
      auto data = blocks.data();
      return std::vector<std::byte>{data.begin(), data.end()};
    }
  };
}
