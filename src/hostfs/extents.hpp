#pragma once

#include <vector>
#include <expected>
#include <string>
#include <cstdint>
#include <span>

struct Extent {
  uint64_t logical;
  uint64_t phys;
  uint64_t length;

  Extent() = default;
  Extent(struct fiemap_extent const& ext);
  auto operator<=>(Extent const& o) const { return logical <=> o.logical; }
};

std::expected<std::vector<Extent>, std::string>
get_extents(int fd, uint64_t offset, uint64_t len);

struct ChunkMap {
  int shift;
  size_t size;
  std::vector<uint32_t> indexes;
};

struct BlockMap {
  uint32_t size;
  uint32_t offset;
  uint32_t operator()(size_t addr) const { return addr / size + offset; }
};

std::expected<ChunkMap, std::string>
extents_to_chunks(std::span<Extent> extents, size_t len, int min_shift, int max_shift, BlockMap const& map);
