#include <util/alloc.hpp>
#include <util/util.hpp>

#include <gtest/gtest.h>

template <size_t Unit, size_t Block>
auto& operator<<(auto& os, BlockAllocator<Unit, Block> const& b) {
  os << "{[" << b.blocks.size() << "]";
  for (int i = 0; i < b.free.size(); i++) {
    if (b.free[i] == -1) continue;
    os << " " << (b.unit_size << i) << ": " << b.free[i] << ",";
  }
  return os << "}";
}

template <typename BlockAllocator>
struct BlockAllocatorTest : testing::Test {
  using type = BlockAllocator;
};

using Subjects = testing::Types <BlockAllocator<1, 16>, BlockAllocator<16, 256>>;

TYPED_TEST_CASE(BlockAllocatorTest, Subjects);

TYPED_TEST(BlockAllocatorTest, Allocate) {
  typename TypeParam::BlockAllocator blocks(0);

  auto total = 0;
  for (auto sz: {141, 592, 653, 589, 793, 238, 462, 643, 383,}) {
    auto p = sz % std::countr_zero(blocks.block_size);
    sz = ((sz / std::countr_zero(blocks.block_size)) << p) % blocks.block_size + 1;

    total += ((sz + blocks.unit_size) / blocks.unit_size) * blocks.unit_size;

    auto a = blocks.allocate(sz);
    ASSERT_EQ(a.size(), sz);
    std::ranges::fill(block_view<uint8_t>(a), sz);

    std::cerr << blocks << "\n";
  }

  ASSERT_GE(blocks.size(), total);
  ASSERT_LE(blocks.size(), total * 2 + blocks.block_size);

  size_t next, remaining = 0;
  for (size_t i = 0; i < blocks.size(); i++) {
    auto x = (uint8_t)blocks.data()[i];
    if (remaining) {
      remaining--;
      ASSERT_EQ(x, next);
    } else if(x) {
      next = x;
      remaining = x - 1;
    }
  }
}
