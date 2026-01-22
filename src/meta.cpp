#include "meta.hpp"

std::expected<ChunkMap, std::string>
try_get_chunks(ostree::File& file, FileInfo const& info, Mirror& mirror) {
  if (auto fd = file.get_fd(); !fd.has_value()) {
    return std::unexpected(fmt::format("failed to get file ({}) extents: {}", file.csum, fd.error()));
  } else {
    return mirror.insert_file(fd.value(), info.size);
  }
}

struct CommitWalker {
  size_t inline_threshold;
  Mirror& mirror;
  erofs::MetadataBuilder& builder;

  struct Directory {
    int depth;
    FileInfo info;
    erofs::DirectoryBuilder builder;
    erofs::MetadataBuilder::Node node;
  };

  int root_nid;

  std::vector<Directory> dirs;
  std::vector<size_t> stack;
  std::map<std::string, erofs::MetadataBuilder::Node> nodes;
  uint32_t next_ino = 1;

  void push_file(ostree::File& file) {
    if (stack.size()) {
      if (auto info = file.info(); info.has_value()) {
        erofs::MetadataBuilder::Node& node = nodes[file.csum];

        FileInfo _info = info->get();
        if (node) {
          node.link();
          _info.ino = node->i_ino;
        } else {
          _info.ino = next_ino++;

          std::span<std::byte const> content;

          std::string link;

          if (_info.is_link() || _info.size < inline_threshold) {
            if (auto _content = file.content(); _content.has_value()) {
              content = _content.value();
            } else {
              fmt::println(stderr, "warning: failed to inline file content, size = {}, is_link = {}", _info.size, _info.is_link());
            }
          }

          if (content.size()) {
            fmt::println(stderr, "\tinline");
            node = builder.push(info.value(), content);
          } else if(auto chunks = try_get_chunks(file, info.value(), mirror); chunks.has_value()) {
            fmt::println(stderr, "\tchunks");
            node = builder.push(info.value(), chunks.value());
          } else {
            fmt::println(stderr, "failed to get file chunks: {}", chunks.error());
            node = builder.push(info.value());
          }
        }

        push_child(node, _info, _info.name);
      } else {
        fmt::println(stderr, "failed to get info for file: {}", info.error());
      }
    }
  }

  void push_directory(FileInfo const& info) {
    fmt::println(stderr, "push {}", info.name);
    dirs.emplace_back(stack.size(), info);
    stack.push_back(dirs.size() - 1);
  }

  void push_child(erofs::nid_t node, FileInfo const& info, std::string_view name) {
    if (stack.size()) {
      auto& parent = dirs[stack.back()];
      parent.builder.push(erofs_dirent {
          .nid = node,
          .file_type = erofs::mode_to_filetype(info.mode),
        }, info.name);
    } else {
      root_nid = node;
    }
  }

  void pop_directory() {
    auto& self = dirs[stack.back()];
    fmt::println(stderr, "pop {}", self.info.name);
    stack.pop_back();

    self.info.size = self.builder.final_size(false);

    auto format = builder.inode_format(false, EROFS_INODE_FLAT_INLINE);
    auto aux_size = self.info.size;
    if (self.info.size > erofs::BLOCK_SIZE) {
      format = builder.inode_format(false, EROFS_INODE_FLAT_PLAIN);
      aux_size = 0;
    }

    format |= (1 << EROFS_I_DOT_OMITTED_BIT);

    self.node = builder.push(self.info, format, aux_size);
    push_child(self.node, self.info, self.info.name);
  }

  erofs::nid_t finish() {
    if (stack.size())
      throw std::runtime_error("stack should be clean");

    for (int i = 0; i < dirs.size(); i++) {
      auto& self = dirs[i];
      while (self.depth < stack.size()) stack.pop_back();

      if (false && stack.size()) {
        auto& parent = dirs[stack.back()];
        self.builder.push(erofs_dirent {
            .nid = parent.node,
            .file_type = erofs::mode_to_filetype(parent.info.mode)
          }, "..");
      }

      builder.fill(self.node, self.builder.finalize());

      stack.push_back(i);
    }

    return root_nid;
  }
};

std::expected<MetadataResult, std::string>
build_meta(ostree::Commit& commit, Mirror& mirror, size_t inline_threshold) {
  struct pop_sentry_t {};
  using entry_t = cat_variant_t<ostree::CommitIter::value_type, pop_sentry_t>;

  if (inline_threshold < erofs::BLOCK_SIZE)
    return std::unexpected("inline_threshold must be >= BLOCK_SIZE");

  MetadataResult ret;

  CommitWalker walker{inline_threshold, mirror, ret.meta};

  std::vector<entry_t> stack;

  stack.emplace_back(pop_sentry_t{});
  for(auto entry: commit) {
    stack.emplace_back(variant_cast(entry));
  }

  std::reverse(stack.begin() + 1, stack.end());

  int depth = 0;
  while(stack.size()) {
    entry_t entry = std::move(stack.back());
    stack.pop_back();

    std::visit(overloaded {
        [&](ostree::Directory& dir) {
          auto info = dir.info()->get();
          walker.push_directory(info);
          if (info.name != ".") {
            stack.emplace_back(pop_sentry_t{});
            for(auto kid: dir)
              stack.emplace_back(variant_cast(kid));
          }
        },
        [&](pop_sentry_t) { walker.pop_directory(); },
        [&](ostree::File& file) { walker.push_file(file); },
        [](std::monostate) {},
      }, entry);
  }

  ret.root_nid = walker.finish();

  return ret;
}
