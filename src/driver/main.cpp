#include <fmt/format.h>

#include <ostree/ostree.hpp>
#include <ostree/commit.hpp>
#include <argparse/argparse.hpp>

#include <iostream>
#include <fstream>

#include <fcntl.h>

#include <hostfs/mirror.hpp>
#include <erofs/image.hpp>

#include "tree.hpp"

std::expected<std::vector<std::byte>, std::string>
build_image_from_commit(std::filesystem::path const& path, std::string const& commit_csum) {
  auto repo = TRY(ostree::Repo::get(path));
  auto commit = TRY(repo.load_commit(commit_csum));

  mkdirat(repo.get_root_dfd(), "image", 0777);
  Mirror mirror(TRY(repo.get_fd("image/mirror", O_CREAT | O_RDWR | O_LARGEFILE, 0755)));

  auto super = TRY(start_filesystem(1, sizeof(cowtree_index_extension)));

  TreeWriter writer(mirror, super.meta_blkaddr * erofs::BLOCK_SIZE);
  auto root = TRY(writer.write(commit));

  auto index = TRY(writer.write_index());
  super.extensions = std::as_bytes(std::span{&index, 1});

  fmt::println(stderr, "mirror size: {}", mirror.size);
  auto dev = std::array { mirror.device_info(), };

  return finish_filesystem(super, root, writer.finalize(), dev);
}

int main(int argc, char** argv) {
  enum subcommand { NONE, BUILD_IMAGE } cmd = NONE;
  struct {
    std::string repo, commit;
    std::string output;
  } build_image;

  {
    argparse::ArgumentParser prog(argv[0]);

    argparse::ArgumentParser _build_image("build_image");
    std::string repo, commit;
    _build_image.add_description("Build image from ostree commit");
    _build_image.add_argument("repo").help("Path to ostree repo").store_into(build_image.repo);
    _build_image.add_argument("commit").help("ostree commit checksum").store_into(build_image.commit);
    _build_image.add_argument("-o").help("Output file. Use '-' to redirect to stdout").store_into(build_image.output);
    prog.add_subparser(_build_image);

    try {
      prog.parse_args(argc, argv);
    } catch (const std::exception& e) {
      fmt::println(stderr, "{}", e.what());
      return -1;
    }

    if (prog.is_subcommand_used(_build_image)) {
      cmd = BUILD_IMAGE;
    }
  }

  if (cmd == BUILD_IMAGE) {
    if (auto res = build_image_from_commit(build_image.repo, build_image.commit); !res.has_value()) {
      fmt::println(stderr, "error: {}", res.error());
      return -1;
    } else {
      auto do_write = [&](auto& os) {
        os.write((char const*)res.value().data(), res.value().size());
      };

      if (build_image.output == "-") {
        do_write(std::cout);
      } else {
        if (build_image.output.empty()) {
          build_image.output = fmt::format("{}/image/{}.erofs", build_image.repo, build_image.commit);
        }

        std::ofstream fout(build_image.output);
        do_write(fout);
      }

      return 0;
    }
  }

  return 0;
}
