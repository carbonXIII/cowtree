#include "package.hpp"

#include <erofs/image.hpp>

#include <fcntl.h>
#include <fmt/format.h>

std::expected<std::monostate, std::string>
Package::build_image(bool as_index) {
  auto super = TRY(start_filesystem(1, sizeof(cowtree_index_extension)));

  FileIndex index;
  size_t meta_addr = super.meta_blkaddr * erofs::BLOCK_SIZE;
  size_t pos = meta_addr;

  if (auto packages = commit.get_metadata<std::vector<std::string>>(std::string{METADATA_KEY_PACKAGES});
      packages.has_value()) {
    for (auto const& pkg: packages.value()) {
      if (auto ret = import_package(index, pos, meta_addr, pkg); !ret.has_value())
        fmt::println(stderr, "INFO: Could not load metadata for {}: {}", pkg, ret.error());
    }
  }

  TreeWriter tree(mirror, index, pos, pos - meta_addr);

  std::vector<std::byte> sb;
  std::vector<std::byte> meta;
  {
    auto root = TRY(tree.write(commit));
    auto index = TRY(tree.write_index());
    super.extensions = std::as_bytes(std::span{&index, 1});

    auto dev = std::array { mirror.device_info(), };

    meta = tree.finalize();
    sb = TRY(finish_filesystem(super, as_index ? index.index_nid : root, pos + meta.size(), dev));
  }

  if (auto written = write(image_fd, sb.data(), sb.size()); written < sb.size())
    return std::unexpected(fmt::format("Failed to write super block: written={} errno={}", written, errno));

  auto rc = lseek(image_fd, pos, SEEK_SET);
  if (auto written = write(image_fd, meta.data(), meta.size()); written < meta.size())
    return std::unexpected(fmt::format("Failed to write metadata blocks: written={} errno={}", written, errno));

  pos += meta.size();
  auto appendix_size = TRY(tree.write_appendix(image_fd, pos));
  fmt::println(stderr, "appendix: 0x{:X} bytes", appendix_size);
  fmt::println(stderr, "total size: 0x{:X} bytes", pos);

  // fsync(image_fd);

  size = pos;
  return {};
}

std::expected<std::monostate, std::string>
Package::import_package(FileIndex& index, std::size_t& pos, std::size_t meta_addr, std::string const& csum) {
  auto pkg = TRY(Package::load(repo, mirror, csum));

  // TODO: recursion here is sloppy, but for the primary use case we don't expect nested packages
  auto fd = TRY(pkg.build_or_find_image(true));

  return index.import_image(fd, image_fd, pos, meta_addr);
}

std::expected<int, std::string>
Package::build_or_find_image(bool as_index) {
  if (!image_fd)
    image_fd = std::move(TRY(repo.get_fd(fmt::format("image/{}.erofs", commit.csum), O_CREAT | O_RDWR | O_LARGEFILE, 0755)));

  struct stat buf;
  fstat(image_fd, &buf);
  size = buf.st_size;

  if (size >= EROFS_SUPER_OFFSET + sizeof(erofs_super_block))
    return image_fd;

  TRY(build_image(as_index));

  return image_fd;
}
