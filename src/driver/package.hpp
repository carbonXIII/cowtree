#pragma once

#include <ostree/commit.hpp>
#include <hostfs/mirror.hpp>
#include <driver/tree.hpp>
#include <erofs/image.hpp>

static constexpr std::string_view METADATA_KEY_PACKAGES = "cowtree.import-packages";

struct Package {
  ostree::Repo& repo;
  Mirror& mirror;
  ostree::Commit commit;

  ostree::FileDescriptor image_fd;
  std::size_t size;

  static std::expected<Package, std::string>
  load(ostree::Repo& repo, Mirror& mirror, std::string const& commit_csum) {
    ostree::Commit commit = TRY(repo.load_commit(commit_csum));
    return Package { repo, mirror, std::move(commit), };
  }

  std::expected<int, std::string> build_or_find_image(bool as_index = false);

private:
  std::expected<std::monostate, std::string> import_package(FileIndex& index, std::size_t& pos, size_t meta_addr, std::string const& csum);
  std::expected<std::monostate, std::string> build_image(bool as_index = false);
};
