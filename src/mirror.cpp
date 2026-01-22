#include "mirror.hpp"

#include <expected>

#include <linux/fs.h>
#undef BLOCK_SIZE

#include <fcntl.h>

static std::expected<void, std::string> clone_range(int dest, file_clone_range range) {
    if (ioctl(dest, FICLONERANGE, &range) != 0)
      return std::unexpected(fmt::format("ficlonerange(dest={}, src={}, offset={:X}, len={:X}, dest={:X}) failed: errno={}",
                                         dest,
                                         range.src_fd,
                                         range.src_offset,
                                         range.src_length,
                                         range.dest_offset,
                                         errno));

    return {};
}

static std::expected<void, std::string> clone_tail_hacky(int dest, file_clone_range range, uint64_t len) {
  range.src_offset = round_up(len, erofs::BLOCK_SIZE) - erofs::BLOCK_SIZE;
  if (range.src_offset == len)
    return {};

  range.src_length = len - range.src_offset;

  uint64_t eof;
  {
    struct stat buf;
    fstat(dest, &buf);
    eof = buf.st_size;
  }

  auto old_offset = std::exchange(range.dest_offset, eof);

  if (auto ret = clone_range(dest, range); !ret.has_value())
    return ret;

  fallocate(dest, 0, range.dest_offset, erofs::BLOCK_SIZE);

  range.src_fd = dest;
  range.src_offset = range.dest_offset;
  auto old_length = std::exchange(range.src_length, erofs::BLOCK_SIZE);
  old_offset = std::exchange(range.dest_offset, old_offset);

  if (auto ret = clone_range(dest, range); !ret.has_value())
    return ret;

  fallocate(dest, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, old_offset, old_length);
  return {};
}

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

    if (range.src_offset + range.src_length > len) {
      if(auto ret = clone_tail_hacky(mirror_fd, range, len); !ret.has_value())
        return std::unexpected(ret.error());
      range.src_length = (len & ~(erofs::BLOCK_SIZE - 1)) - range.src_offset;
    }

    resize(e.phys + e.length);

    if (range.src_length > 0) {
      fallocate(mirror_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, range.dest_offset, e.length);
      if (auto ret = clone_range(mirror_fd, range); !ret.has_value())
        return std::unexpected(ret.error());
    }
  }

  BlockMap block_map { .size = erofs::BLOCK_SIZE, .offset = block_offset };
  return extents_to_chunks(extents, len, erofs::CHUNK_MIN_SHIFT, erofs::CHUNK_MAX_SHIFT, block_map);
}
