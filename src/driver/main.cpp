#include <fmt/format.h>

#include <ostree/ostree.hpp>
#include <ostree/commit.hpp>
#include <argparse/argparse.hpp>

#include <fcntl.h>
#include <sys/sendfile.h>

#include <hostfs/mirror.hpp>
#include <erofs/image.hpp>
#include <util/splice.hpp>

#include <driver/package.hpp>

std::expected<std::monostate, std::string>
build_image_from_commit(std::filesystem::path const& path,
                        std::string const& commit_csum,
                        bool as_index = false,
                        int out_fd = -1) {
  auto repo = TRY(ostree::Repo::get(path));


  mkdirat(repo.get_root_dfd(), "image", 0777);
  Mirror mirror(TRY(repo.get_fd("image/mirror", O_CREAT | O_RDWR | O_LARGEFILE, 0755)));

  auto pkg = TRY(Package::load(repo, mirror, commit_csum));

  auto fd = TRY(pkg.build_or_find_image(as_index));

  if (out_fd >= 0)
    TRY(splice_files(fd, out_fd, 0));

  return {};
}

int main(int argc, char** argv) {
  enum subcommand { NONE, BUILD_IMAGE } cmd = NONE;
  struct {
    std::string repo, commit;
    std::string output;
    bool as_index;
  } build_image;

  {
    argparse::ArgumentParser prog(argv[0]);

    argparse::ArgumentParser _build_image("build_image");
    std::string repo, commit;
    _build_image.add_description("Build image from ostree commit");
    _build_image.add_argument("repo").help("Path to ostree repo").store_into(build_image.repo);
    _build_image.add_argument("commit").help("ostree commit checksum").store_into(build_image.commit);
    _build_image.add_argument("-o").help("Output file. Use '-' to redirect to stdout").store_into(build_image.output);
    _build_image.add_argument("--as-index").help("Map from file checksums to files replaces the normal tree.").flag().store_into(build_image.as_index);
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
    int out_fd = -1;

    if (build_image.output == "-") {
      out_fd = fileno(stdout);
    } else if(build_image.output.size()) {
      out_fd = open(build_image.output.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0755);
      if (out_fd < 0)
        fmt::println(stderr, "WARN: failed to open output file: errno={}", errno);
    }

    if (auto res = build_image_from_commit(build_image.repo, build_image.commit, build_image.as_index, out_fd); !res.has_value()) {
      fmt::println(stderr, "error: {}", res.error());
      return -1;
    }

    if (out_fd >= 0 && out_fd != fileno(stdout))
      close(out_fd);
  }

  return 0;
}
