#pragma once

#include <expected>

#include <ostree/ostree.hpp>

#include <sys/ioctl.h>
#include <sys/stat.h>

#include <erofs/base.hpp>
#include <util/util.hpp>
#include "extents.hpp"

struct DeviceInfo {
  std::string tag;
  size_t sz_bytes;
  size_t block_offset;

  erofs_deviceslot operator() (bool use_ext_addr = true) {
    size_t blocks = round_up(sz_bytes, erofs::BLOCK_SIZE) / erofs::BLOCK_SIZE;
    erofs_deviceslot ret = {
      .blocks_lo = (uint32_t)blocks,
      .uniaddr_lo = (uint32_t)block_offset,
    };

    if (use_ext_addr) {
      ret.blocks_hi = blocks >> 32;
      ret.uniaddr_hi = block_offset >> 32;
    }

    copy(tag, std::span{ret.tag, sizeof(ret.tag)});
    return ret;
  }
};

struct Mirror {
  static constexpr size_t DEFAULT_BLOCK_OFFSET = (1 << (30 - std::countr_zero(erofs::BLOCK_SIZE)));

  ostree::FileDescriptor mirror_fd;
  uint32_t block_offset = DEFAULT_BLOCK_OFFSET;
  size_t size;

  Mirror(ostree::FileDescriptor&& fd): mirror_fd(FORWARD(fd)) {
    struct stat s;
    fstat(mirror_fd, &s);
    size = s.st_size;
    static_assert(sizeof(stat::st_size) == 8);
  }

  void resize(size_t new_size) {
    if(size < new_size) {
      // ftruncate(mirror_fd, new_size);
      size = new_size;
    }
  }

  std::expected<ChunkMap, std::string>
  insert_file(int fd, uint64_t len);

  DeviceInfo device_info() {
    return DeviceInfo {
      .tag = "mirror",
      .sz_bytes = size,
      .block_offset = block_offset
    };
  }
};
