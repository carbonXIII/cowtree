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

struct Context {
  std::filesystem::path repo_path;
  ostree::Repo repo;
  Mirror mirror;

  static std::expected<Context, std::string> create(std::filesystem::path const& path) {
    auto repo = TRY(ostree::Repo::get(path));

    mkdirat(repo.get_root_dfd(), "image", 0777);
    Mirror mirror(TRY(repo.get_fd("image/mirror", O_CREAT | O_RDWR | O_LARGEFILE, 0755)));

    return std::expected<Context, std::string> { std::in_place, path, std::move(repo), std::move(mirror) };
  }

  std::expected<std::monostate, std::string>
  build_image_from_commit(std::string const& commit_csum,
                          bool as_index = false,
                          int out_fd = -1) {
    if (auto res = mirror.trim(); res.has_value()) {
      fmt::println(stderr, "trimmed 0x{:X} bytes from mirror, new size 0x{:X}", res.value(), mirror.size);
    } else {
      fmt::println(stderr, "warning: could not trim mirror: {}", res.error());
    }

    auto pkg = TRY(Package::load(repo, mirror, commit_csum));

    auto fd = TRY(pkg.build_or_find_image(as_index));

    if (out_fd >= 0)
      TRY(splice_files(fd, out_fd, 0));

    return {};
  }

  std::expected<std::monostate, std::string> prune() {
    for (auto const& f: std::filesystem::directory_iterator {repo_path / "image"}) {
      if (f.path().extension() != ".erofs") continue;

      std::string csum = f.path().stem();
      if (csum.size() != (256 / 16)) continue;

      if (!std::filesystem::exists(repo_path / fmt::format("objects/{}/{}.commit", csum.substr(0, 2), csum.substr(2)))) {
        std::filesystem::remove(f);
        fmt::println(stdout, "{}", csum);
      }
    }

    if (auto res = mirror.trim(); res.has_value()) {
      fmt::println(stderr, "trimmed 0x{:X} bytes from mirror, new size 0x{:X}", res.value(), mirror.size);
    } else {
      fmt::println(stderr, "warning: could not trim mirror: {}", res.error());
    }

    return {};
  }
};

int main(int argc, char** argv) {
  enum subcommand { NONE, BUILD_IMAGE, PRUNE } cmd = NONE;

  std::string repo;
  struct {
    std::string commit;
    std::string output;
    bool as_index;
  } build_image;

  {
    argparse::ArgumentParser prog(argv[0]);
    prog.add_argument("repo").help("Path to ostree repo").store_into(repo);

    argparse::ArgumentParser _build_image("build_image");
    std::string repo, commit;
    _build_image.add_description("Build image from ostree commit");
    _build_image.add_argument("commit").help("ostree commit checksum").store_into(build_image.commit);
    _build_image.add_argument("-o").help("Output file. Use '-' to redirect to stdout").store_into(build_image.output);
    _build_image.add_argument("--as-index").help("Map from file checksums to files replaces the normal tree.").flag().store_into(build_image.as_index);
    prog.add_subparser(_build_image);

    argparse::ArgumentParser _prune("prune");
    _prune.add_description("Remove images corrresponding to non-existing commits.");
    prog.add_subparser(_prune);

    try {
      prog.parse_args(argc, argv);
    } catch (const std::exception& e) {
      fmt::println(stderr, "{}", e.what());
      return -1;
    }

    if (prog.is_subcommand_used(_build_image)) {
      cmd = BUILD_IMAGE;
    } else if (prog.is_subcommand_used(_prune)) {
      cmd = PRUNE;
    }
  }

  if (auto context = Context::create(repo); !context.has_value()) {
    fmt::println(stderr, "error: Failed to open repo: {}", context.error());
    return -1;
  } else {
    if (cmd == BUILD_IMAGE) {
      int out_fd = -1;

      if (build_image.output == "-") {
        out_fd = fileno(stdout);
      } else if(build_image.output.size()) {
        out_fd = open(build_image.output.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0755);
        if (out_fd < 0)
          fmt::println(stderr, "WARN: failed to open output file: errno={}", errno);
      }

      if (auto res = context->build_image_from_commit(build_image.commit, build_image.as_index, out_fd); !res.has_value()) {
        fmt::println(stderr, "error: {}", res.error());
        return -1;
      }

      if (out_fd >= 0 && out_fd != fileno(stdout))
        close(out_fd);
    } else if (cmd == PRUNE) {
      if (auto res = context->prune(); !res.has_value()) {
        fmt::println(stderr, "error: {}", res.error());
        return -1;
      }
    }
  }


  return 0;
}
