#include "meta.hpp"

#include <erofs/dirent.hpp>
#include <erofs/meta.hpp>

#include <fmt/format.h>
#include <deque>

std::expected<ChunkMap, std::string>
try_get_chunks(ostree::File& file, FileInfo const& info, Mirror& mirror) {
  if (auto fd = file.get_fd(); !fd.has_value()) {
    return std::unexpected(fmt::format("failed to get file ({}) extents: {}", file.csum, fd.error()));
  } else {
    return mirror.insert_file(fd.value(), info.size);
  }
}

std::expected<std::span<std::byte const>, std::string>
try_get_inline(ostree::File& file, FileInfo const& info, std::size_t inline_threshold) {
  std::span<std::byte const> content;

  if(info.is_link() || info.size <= inline_threshold) {
    if (auto _content = file.content(); _content.has_value()) {
      return _content.value();
    } else {
      return std::unexpected(fmt::format("failed to inline file content, size = {}, is_link = {}", info.size, info.is_link()));
    }
  }

  return std::unexpected("");
}

std::expected<MetadataResult, std::string>
build_meta(size_t base_addr, ostree::Commit& commit, Mirror& mirror, size_t inline_threshold) {
  struct Entry {
    FileInfo info;
    std::optional<ostree::CommitIter> iter;
    std::string csum; // TODO: should be std::array<char>
    std::pair<erofs::nid_t, size_t> link = {-1, -1};
    uint8_t parent_type = EROFS_FT_DIR;
  };

  MetadataResult ret;
  erofs::MetadataBuilder meta(base_addr);

  std::deque<Entry> q;
  std::map<std::string, erofs::nid_t> nodes;

  uint32_t next_ino = 1;
  q.emplace_front(FileInfo { .mode = S_IFDIR,}, commit.begin());

  auto complete_link = [&](erofs::nid_t self, erofs::nid_t parent, size_t off) {
    fmt::println(stderr, "parent nid: {}, offset: {}", parent, off);
    block_view<erofs_dirent>(meta.get_block(parent, off))[0]->nid = self;
  };

  while(q.size()) {
    auto entry = std::move(q.front());
    q.pop_front();

    erofs::nid_t nid = -1;

    if (entry.iter) {
      size_t start = q.size();
      std::vector<erofs::DirEnt> kids;

      entry.info.ino = next_ino++;

      for (auto& it = *entry.iter; it; it++) {
        std::visit(overloaded {
            [&](ostree::Directory child) {
              if (auto _info = child.info(); _info.has_value()) {
                FileInfo info = *_info;

                if (child.name == ".") {
                  entry.info = info;
                  return;
                }

                kids.emplace_back(info.name, erofs::mode_to_filetype(info.mode));

                info.name.clear();
                q.emplace_back(info, child.begin());
              }
            },
            [&](ostree::File child) {
              if (auto _info = child.info(); _info.has_value()) {
                FileInfo info = *_info;
                kids.emplace_back(info.name, erofs::mode_to_filetype(info.mode));

                info.name.clear();
                q.emplace_back(info, std::nullopt, child.csum);
              }
            },
            [](std::monostate) {}
          }, *it);
      }

      kids.emplace_back("..", entry.parent_type);

      auto dirs = erofs::build_directory(kids);

      nid = meta.push_inline(entry.info, std::as_bytes(std::span{dirs.buf}));

      for (int i = 0, idx = start; idx < q.size(); i++, idx++) {
        q[idx].link = { nid, dirs.entry_off[i] };
        q[idx].parent_type = erofs::mode_to_filetype(entry.info.mode);
      }

      if (entry.link.first != -1)
        complete_link(entry.link.first, nid, dirs.entry_off.back());

      if (ret.root_nid == -1)
        ret.root_nid = nid;
    } else {
      if (auto it = nodes.find(entry.csum); it != nodes.end()) {
        nid = it->second;
      } else {
        entry.info.ino = next_ino++;

        if (auto file = commit.get_file(entry.csum); file.has_value()) {
          if (auto content = try_get_inline(*file, entry.info, inline_threshold); content.has_value()) {
            nid = meta.push_inline(entry.info, *content);
          } else if (auto chunks = try_get_chunks(*file, entry.info, mirror); chunks.has_value()) {
            nid = meta.push_chunks(entry.info, chunks.value());
          } else {
            fmt::println(stderr, "warning: failed to push file content: '{}' and '{}'", content.error(), chunks.error());
          }
        } else {
          fmt::println(stderr, "warning: failed to get file obj: {}", file.error());
        }

        if (nid == -1) nid = meta.push_inline(entry.info);
        nodes[entry.csum] = nid;
      }
    }

    if (entry.link.first != -1)
      complete_link(nid, entry.link.first, entry.link.second);
  }

  ret.meta = meta.finalize();
  return ret;
}
