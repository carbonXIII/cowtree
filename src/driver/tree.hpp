#pragma once

#include <ostree/ostree.hpp>
#include <ostree/commit.hpp>

#include <hostfs/extents.hpp>
#include <hostfs/mirror.hpp>

struct TreeWriter {
  struct Result {
    std::vector<std::byte> meta;
    erofs::nid_t root_nid = std::numeric_limits<erofs::nid_t>::max();
  };

  Mirror& mirror;
  size_t inline_threshold = 4096;

  uint32_t next_ino = 1;
  std::map<std::string, erofs::nid_t> index;

  std::expected<Result, std::string> write(size_t base_addr, ostree::Commit& commit);

  std::expected<std::monostate, std::string> read_index(int fd, erofs::nid_t nid);
  std::expected<Result, std::string> write_index(size_t base_addr);

private:
  std::expected<ChunkMap, std::string>
  try_get_chunks(ostree::File& file, FileInfo const& info);

  std::expected<std::span<std::byte const>, std::string>
  try_get_inline(ostree::File& file, FileInfo const& info);
};
