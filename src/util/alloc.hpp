#pragma once

#include <util/util.hpp>

#include <vector>

template <std::size_t Unit, std::size_t Block>
struct BlockAllocator {
  using block_t = std::array<std::byte, Block>;
  static constexpr auto unit_size = Unit;
  static constexpr auto block_size = Block;

  std::vector<block_t> blocks;
  std::size_t base_addr;

  std::array<std::size_t, count_bits(Block / Unit - 1)> free;

  BlockAllocator(std::size_t base_addr): base_addr(base_addr) {
    free.fill(-1);
  }

  std::span<std::byte const> data() const { return as_bytes(blocks); }
  std::size_t size() const { return blocks.size() * Block; };

  std::span<block_t> allocate_blocks(std::size_t n_blocks) {
    auto l = blocks.size();
    blocks.resize(blocks.size() + n_blocks);
    return subspan(blocks, l);
  }

  std::span<std::byte> allocate(std::size_t n) {
    if (n > Block)
      return std::as_writable_bytes(allocate_blocks((n + Block - 1) / Block));

    std::size_t n_units = (n + Unit - 1) / Unit;

    std::span<std::byte> src;

    std::size_t l = count_bits(n_units - 1);
    std::size_t r = l;
    for (; r < free.size(); r++) {
      if (free[r] != -1) {
        src = subspan(blocks[free[r] / Block], free[r] % Block, Unit << r);
        free[r] = -1;
        break;
      }
    }

    if (src.empty())
      src = as_writable_bytes(allocate_blocks(1));

    std::size_t L = src.data() - (std::byte*)blocks.data();
    for (std::size_t i = r - 1; i != -1 && i >= l; i--) {
      free[i] = L + (src.size() / 2);
      src = src.subspan(0, src.size() / 2);
    }

    return src.subspan(0, n);
  }

  std::span<std::byte> get_block(std::size_t addr, std::size_t size = std::dynamic_extent) {
    addr -= base_addr;
    return std::as_writable_bytes(subspan(blocks[addr / Block], addr % Block, size));
  }

  std::size_t get_address(std::span<std::byte> s) {
    return s.data() - (std::byte*)blocks.data() + base_addr;
  }
};
