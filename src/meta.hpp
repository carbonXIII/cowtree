#pragma once

#include <ostree/ostree.hpp>
#include <ostree/commit.hpp>

#include "extents.hpp"
#include "mirror.hpp"

std::expected<ChunkMap, std::string>
try_get_chunks(ostree::File& file, FileInfo const& info, Mirror& mirror);

struct MetadataResult {
  erofs::MetadataBuilder meta;
  erofs::nid_t root_nid;
};

std::expected<MetadataResult, std::string>
build_meta(ostree::Commit& commit, Mirror& mirror, size_t inline_threshold = erofs::BLOCK_SIZE);
