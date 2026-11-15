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

      erofs_inode_compact& get();
      erofs_inode_compact* operator->() { return &get(); }
      erofs_inode_compact& operator*() { return get(); }

      std::span<std::byte> get_block(std::size_t offset);
    };

    std::size_t real_base;
    BlockAllocator<INODE_SIZE, BLOCK_SIZE> blocks;

    MetadataBuilder(std::size_t base_addr, std::size_t offset = 0)
      : real_base(base_addr - offset), blocks(base_addr) {}

    Node get(nid_t nid) { return { this, nid }; }
    std::span<std::byte> get_block(nid_t nid, size_t offset) { return get(nid).get_block(offset); }

    Node push(erofs_inode_compact ino, std::span<std::byte const> inlined);
    Node push_inline(FileInfo const& info, std::span<std::byte const> content = {});
    Node push_chunks(FileInfo const& info, ChunkMap const& chunks);

    std::vector<std::byte> finalize() {
      auto data = blocks.data();
      return std::vector<std::byte>{data.begin(), data.end()};
    }
  };
}
