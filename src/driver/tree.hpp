#pragma once

#include <ostree/ostree.hpp>
#include <ostree/commit.hpp>

#include <hostfs/extents.hpp>
#include <hostfs/mirror.hpp>

#include <driver/index.hpp>

#include <erofs/meta.hpp>

struct TreeWriter {
  Mirror& mirror;
  FileIndex& index;
  erofs::MetadataBuilder meta;

  size_t inline_threshold = 4096;
  uint32_t next_ino = 1;

  std::vector<Extent> appendix;

  TreeWriter(Mirror& mirror, FileIndex& index, size_t base_addr, size_t offset = 0):
    mirror(mirror), index(index), meta(base_addr, offset) {}

  std::expected<erofs::nid_t, std::string> write(ostree::Commit& commit, bool files_only = false);
  std::expected<cowtree_index_extension, std::string> write_index();
  std::expected<std::size_t, std::string> write_appendix(int fd, std::size_t& offset);

  std::vector<std::byte> finalize() { return meta.finalize(); }

private:
  std::expected<ChunkMap, std::string>
  try_get_chunks(ostree::File& file, FileInfo const& info);

  std::expected<std::span<std::byte const>, std::string>
  try_get_inline(ostree::File& file, FileInfo const& info);
};
