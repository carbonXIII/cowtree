#pragma once
#include <bit>
#include <cstdint>
#include <linux/types.h>

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

namespace erofs {
  static constexpr std::size_t BLOCK_SIZE = 4096;

  static constexpr std::size_t INODE_SIZE = sizeof(erofs_inode_compact);

  static constexpr int CHUNK_MIN_SHIFT = std::countr_zero(BLOCK_SIZE);
  static constexpr int CHUNK_MAX_SHIFT = CHUNK_MIN_SHIFT + EROFS_CHUNK_FORMAT_BLKBITS_MASK;

  using nid_t = uint64_t;
  using blkaddr_t = uint32_t;
}
