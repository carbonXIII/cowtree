#pragma once

#include <ostree/ostree.hpp>
#include <ostree/commit.hpp>

#include <hostfs/extents.hpp>
#include <hostfs/mirror.hpp>

#include <erofs/meta.hpp>

struct cowtree_index_extension {
  static constexpr uint32_t MAGIC = 0x78646E69;
  uint32_t magic = MAGIC; // 'INDX'
  erofs::nid_t index_nid = -1;
};

struct TreeWriter {
  Mirror& mirror;
  erofs::MetadataBuilder meta;

  size_t inline_threshold = 4096;
  uint32_t next_ino = 1;

  struct IndexEntry { erofs::nid_t nid; uint8_t filetype; };
  std::map<std::string, IndexEntry> index;

  TreeWriter(Mirror& mirror, size_t base_addr): mirror(mirror), meta(base_addr) {}

  std::expected<erofs::nid_t, std::string> write(ostree::Commit& commit);

  std::expected<cowtree_index_extension, std::string> write_index();
  std::expected<std::monostate, std::string> read_index(int fd, erofs::nid_t nid);

  std::vector<std::byte> finalize() { return meta.finalize(); }

private:
  std::expected<ChunkMap, std::string>
  try_get_chunks(ostree::File& file, FileInfo const& info);

  std::expected<std::span<std::byte const>, std::string>
  try_get_inline(ostree::File& file, FileInfo const& info);
};
