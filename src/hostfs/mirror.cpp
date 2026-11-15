#include "mirror.hpp"

#include <fmt/format.h>
#include <linux/fs.h>
#undef BLOCK_SIZE

#include <fcntl.h>

std::expected<std::monostate, std::string> clone_range(int dest, file_clone_range range) {
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

/// FICLONERANGE only works with ranges smaller than a block (e.g. tails) when the src and dst range are aligned with the end of the file.
/// If necessary, make a temporary clone at the end of the mirror.
static std::expected<std::monostate, std::string> clone_tail_hacky(int dest, file_clone_range range, uint64_t len) {
  range.src_length = len - range.src_offset;
  auto next_length = round_up(range.src_length, erofs::BLOCK_SIZE);
  size_t tail_size = range.src_length % erofs::BLOCK_SIZE;

  uint64_t eof;
  {
    struct stat buf;
    fstat(dest, &buf);
    eof = buf.st_size;
  }

  auto old_offset = std::exchange(range.dest_offset, eof);
  if (old_offset != range.dest_offset && old_offset + next_length > range.dest_offset)
    range.dest_offset = old_offset + next_length;

  if (auto ret = clone_range(dest, range); !ret.has_value())
    return ret;

  fallocate(dest, 0, range.dest_offset + next_length - erofs::BLOCK_SIZE, erofs::BLOCK_SIZE);

  if (old_offset != range.dest_offset) {
    range.src_fd = dest;
    range.src_offset = range.dest_offset;
    range.src_length = next_length;
    old_offset = std::exchange(range.dest_offset, old_offset);

    if (auto ret = clone_range(dest, range); !ret.has_value())
      return ret;

    fallocate(dest, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
              std::max(eof, uint64_t { range.dest_offset + range.src_length }), old_offset + next_length);
  }

  return {};
}

std::expected<ChunkMap, std::string>
Mirror::insert_file(int fd, uint64_t len, int max_shift) {
  auto extents = TRY(get_extents(fd, 0, len));

  auto tail_size = len % erofs::BLOCK_SIZE;
  std::optional<Extent> tail;

  for (auto const& e: extents) {
    file_clone_range range = {
      .src_fd = fd,
      .src_offset = e.logical,
      .src_length = e.length,
      .dest_offset = e.phys,
    };

    if (e.is_tail && range.src_offset + range.src_length > len) {
      if(auto ret = clone_tail_hacky(mirror_fd, range, len); !ret.has_value())
        return std::unexpected(ret.error());

      if (tail_size > 0) {
        tail = e;
        tail->logical = round_up(tail->logical + tail->length, erofs::BLOCK_SIZE) - erofs::BLOCK_SIZE;
        tail->phys = round_up(tail->phys + tail->length, erofs::BLOCK_SIZE) - erofs::BLOCK_SIZE;
        tail->length = erofs::BLOCK_SIZE;
      }
    } else {
      fallocate(mirror_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, range.dest_offset, e.length);
      if (auto ret = clone_range(mirror_fd, range); !ret.has_value())
        return std::unexpected(ret.error());
    }

    resize(e.phys + e.length);
  }

  BlockMap block_map { .size = erofs::BLOCK_SIZE, .offset = block_offset };
  auto ret = TRY(extents_to_chunks(extents, len, erofs::CHUNK_MIN_SHIFT, max_shift, block_map));
  ret.tail = tail;
  return ret;
}

std::expected<std::size_t, std::string> Mirror::trim() {
  if (!size)
    return 0;

  auto extents = TRY(get_extents(mirror_fd, 0, size));

  std::size_t saved = 0;
  std::size_t new_size = 0;
  for (auto const& e: extents) {
    if (!e.is_shared) {
      fallocate(mirror_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, e.logical, e.length);
      saved += e.length;
      continue;
    }

    new_size = std::max(new_size, e.logical + e.length);
  }

  ftruncate(mirror_fd, new_size);
  return saved;
}
