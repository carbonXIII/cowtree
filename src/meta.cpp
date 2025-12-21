#include "meta.hpp"

std::expected<ChunkMap, std::string>
try_get_chunks(ostree::File& file, FileInfo const& info, Mirror& mirror) {
  if (auto fd = file.get_fd(); !fd.has_value()) {
    return std::unexpected(fmt::format("failed to get file ({}) extents: {}", file.csum, fd.error()));
  } else {
    return mirror.insert_file(fd.value(), info.size);
  }
}

std::expected<MetadataResult, std::string>
build_meta(ostree::Commit& commit, Mirror& mirror, size_t inline_threshold) {
  struct pop_sentry_t {};
  using entry_t = cat_variant_t<ostree::CommitIter::value_type, pop_sentry_t>;

  MetadataResult ret;
  erofs::MetadataBuilder meta;

  std::vector<std::pair<FileInfo, erofs::DirectoryBuilder>> dir_stack;
  std::vector<entry_t> stack;
  std::map<std::string, erofs::MetadataBuilder::Node> nodes;

  uint32_t next_ino = 1;
  dir_stack.emplace_back(FileInfo{}, erofs::DirectoryBuilder{});

  stack.emplace_back(pop_sentry_t{});
  for(auto entry: commit) {
    stack.emplace_back(variant_cast(std::move(entry)));
  }

  std::reverse(stack.begin() + 1, stack.end());

  int depth = 0;
  while(stack.size()) {
    auto entry = std::move(stack.back());
    stack.pop_back();

    std::visit(overloaded {
        [&](ostree::Directory& dir) {
          if (auto info = dir.info()) {
            fmt::println(stderr, "{}{}", std::string(depth, '-'), dir.name);
            ++depth;

            FileInfo _info = info->get();
            _info.ino = next_ino++;
            if (dir.name == ".") {
              dir_stack.front().first = _info;
            } else {
              dir_stack.emplace_back(_info, erofs::DirectoryBuilder{});

              stack.emplace_back(pop_sentry_t{});

              for(auto kid: dir) {
                stack.emplace_back(variant_cast(std::move(kid)));
              }
            }
          } else {
            fmt::println(stderr, "failed to get info for directory: {}", info.error());
          }
        },
        [&](pop_sentry_t) {
          auto info = dir_stack.back().first;
          auto node = meta.push(info, dir_stack.back().second);
          dir_stack.pop_back();

          --depth;
          if (dir_stack.size()) {
            dir_stack.back().second.push(erofs_dirent {
                .nid = node,
                .file_type = erofs::mode_to_filetype(info.mode),
              }, info.name);
          } else {
            ret.root_nid = node;
          }
        },
        [&](ostree::File& file) {
          fmt::println(stderr, "file: {}", file.name);

          if (dir_stack.size()) {
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
                  node = meta.push(info.value(), content);
                } else if(auto chunks = try_get_chunks(file, info.value(), mirror); chunks.has_value()) {
                  fmt::println(stderr, "\tchunks");
                  node = meta.push(info.value(), chunks.value());
                } else {
                  fmt::println(stderr, "failed to get file chunks: {}", chunks.error());

                  node = meta.push(info.value());
                }
              }

              dir_stack.back().second.push(erofs_dirent {
                  .nid = node,
                  .file_type = erofs::mode_to_filetype(info.value().get().mode),
                }, file.name);
            } else {
              fmt::println(stderr, "failed to get info for file: {}", info.error());
            }
          }
        },
        [](std::monostate) {},
      }, entry);
  }

  ret.meta = meta.finalize();
  return ret;
}
