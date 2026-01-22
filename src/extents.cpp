#include "extents.hpp"

#include <algorithm>
#include <fmt/core.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>

#include "util.hpp"

Extent::Extent(fiemap_extent const& ext)
  : logical(ext.fe_logical), phys(ext.fe_physical), length(ext.fe_length) {}

std::expected<std::vector<Extent>, std::string>
get_extents(int fd, uint64_t offset, size_t len) {
  fiemap args = {
    .fm_start = offset,
    .fm_length = len,
    .fm_flags = 0,
    .fm_extent_count = 0,
  };

  if (ioctl(fd, FS_IOC_FIEMAP, &args) != 0)
    return std::unexpected(fmt::format("fiemap failed: errno={}", errno));

  std::vector<std::byte> buf;
  buf.resize(sizeof(args) + sizeof(fiemap_extent) * args.fm_mapped_extents);
  args.fm_extent_count = args.fm_mapped_extents;
  copy(as_bytes(std::span{&args, 1}), buf);

  if (ioctl(fd, FS_IOC_FIEMAP, buf.data()) != 0)
    return std::unexpected(fmt::format("fiemap failed: errno={}", errno));

  auto ret = std::ranges::to<std::vector<Extent>>(block_view<fiemap_extent>(std::span{buf}.subspan(sizeof(args))) |
                                                  std::views::transform([](fiemap_extent const& e) { return Extent{e}; }));

  for (auto e: block_view<fiemap_extent>(std::span{buf}.subspan(sizeof(args))))
    fmt::println(stderr, "[{:X}, +{:X}) => {:X}, flags={:X}", e->fe_logical, e->fe_length, e->fe_physical, e->fe_flags);

  std::ranges::sort(ret, std::less<Extent>{});
  {
    int j = 1;
    for(int i = 0; j < ret.size(); j++) {
      if ((ret[i].logical + ret[i].length == ret[j].logical) &&
          (ret[i].phys + ret[i].length == ret[j].phys)) {
        ret[i].length += ret[j].length;
      } else {
        ++i;
        if (i < j)
          ret[i] = ret[j];
      }
    }

    ret.resize(j);
  }

  return ret;
}

std::expected<ChunkMap, std::string>
extents_to_chunks(std::span<Extent> extents, size_t len, int min_shift, int max_shift, BlockMap const& map) {
  int shift = sizeof(size_t) * 8;
  for (auto const& e: extents) {
    shift = std::min(shift, std::countr_zero(e.logical));
    shift = std::min(shift, std::countr_zero(e.logical + e.length));
  }

  if (shift < min_shift)
    return std::unexpected(fmt::format("Needed chunk shift {} < required {}", shift, min_shift));

  shift = std::min(shift, max_shift);
  auto sz = (1ll << shift);

  ChunkMap ret { .shift = shift, .size = len, };
  ret.indexes.reserve(len / sz);

  auto it = extents.begin();
  for (size_t i = 0; i < len; i += sz) {
    while (it != extents.end() && (it->logical + it->length) <= i)
      ++it;

    if (it != extents.end() && it->logical <= i && i < it->logical + it->length) {
      ret.indexes.push_back(map((i - it->logical) + it->phys));
    } else {
      ret.indexes.push_back(-1);
    }
  }

  return ret;
}
