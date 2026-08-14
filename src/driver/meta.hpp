#pragma once

#include <ostree/ostree.hpp>
#include <ostree/commit.hpp>

#include <hostfs/extents.hpp>
#include <hostfs/mirror.hpp>

std::expected<ChunkMap, std::string>
try_get_chunks(ostree::File& file, FileInfo const& info, Mirror& mirror);

struct MetadataResult {
  std::vector<std::byte> meta;
  erofs::nid_t root_nid = std::numeric_limits<erofs::nid_t>::max();
};

std::expected<MetadataResult, std::string>
build_meta(size_t base_addr, ostree::Commit& commit, Mirror& mirror, size_t inline_threshold = 512);
