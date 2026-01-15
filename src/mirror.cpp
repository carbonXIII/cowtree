#include "mirror.hpp"

#include <linux/fs.h>
#undef BLOCK_SIZE

std::expected<ChunkMap, std::string>
Mirror::insert_file(int fd, uint64_t len) {
  auto extents = TRY(get_extents(fd, 0, len));

  for (auto const& e: extents) {
    file_clone_range range = {
      .src_fd = fd,
      .src_offset = e.logical,
      .src_length = e.length,
      .dest_offset = e.phys,
    };

    resize(e.phys + e.length);

    if (ioctl(mirror_fd, FICLONERANGE, &range) != 0)
      return std::unexpected(fmt::format("ficlonerange(offset={}, len={} < total_len={}) failed: errno={}",
                                         range.src_offset,
                                         range.src_length,
                                         len,
                                         errno));
  }

  BlockMap block_map { .size = erofs::BLOCK_SIZE, .offset = block_offset };
  return extents_to_chunks(extents, len, erofs::CHUNK_MIN_SHIFT, erofs::CHUNK_MAX_SHIFT, block_map);
}
