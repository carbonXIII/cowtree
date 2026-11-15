#include "tree.hpp"

#include <erofs/dirent.hpp>
#include <erofs/meta.hpp>
#include <util/io.hpp>

#include <fmt/format.h>
#include <deque>

#include <linux/fs.h>
#undef BLOCK_SIZE

std::expected<ChunkMap, std::string>
TreeWriter::try_get_chunks(ostree::File& file, FileInfo const& info) {
  if (auto fd = file.get_fd(); !fd.has_value()) {
    return std::unexpected(fmt::format("failed to get file ({}) extents: {}", file.csum, fd.error()));
  } else {
    auto ret = TRY(mirror.insert_file(fd.value(), info.size));
    if (ret.tail.has_value())
      appendix.push_back(*ret.tail);
    return ret;
  }
}

std::expected<std::span<std::byte const>, std::string>
TreeWriter::try_get_inline(ostree::File& file, FileInfo const& info) {
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

std::expected<erofs::nid_t, std::string>
TreeWriter::write(ostree::Commit& commit) {
  struct Entry {
    FileInfo info;
    std::optional<ostree::CommitIter> iter;
    std::string csum; // TODO: should be std::array<char>
    std::pair<erofs::nid_t, size_t> link = {-1, -1};
    uint8_t parent_type = EROFS_FT_DIR;
  };

  std::deque<Entry> q;

  q.emplace_front(FileInfo { .mode = S_IFDIR,}, commit.begin());

  erofs::nid_t root_nid = -1;

  auto complete_link = [&](erofs::nid_t self, erofs::nid_t parent, size_t off) {
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

      if (entry.link.first != -1)
        kids.emplace_back("..", entry.parent_type);

      auto dirs = erofs::build_directory(kids);

      nid = meta.push_inline(entry.info, std::as_bytes(std::span{dirs.buf}));

      for (int i = 0, idx = start; idx < q.size(); i++, idx++) {
        q[idx].link = { nid, dirs.entry_off[i] };
        q[idx].parent_type = erofs::mode_to_filetype(entry.info.mode);
      }

      if (entry.link.first != -1)
        complete_link(entry.link.first, nid, dirs.entry_off.back());

      if (root_nid == -1)
        root_nid = nid;
    } else {
      if (auto it = index.find(entry.csum); it != index.end()) {
        nid = it->second.nid;
      } else {
        entry.info.ino = next_ino++;

        if (auto file = commit.get_file(entry.csum); file.has_value()) {
          if (auto content = try_get_inline(*file, entry.info); content.has_value()) {
            nid = meta.push_inline(entry.info, *content);
          } else if (auto chunks = try_get_chunks(*file, entry.info); chunks.has_value()) {
            nid = meta.push_chunks(entry.info, chunks.value());
          } else {
            fmt::println(stderr, "warning: failed to push file content: '{}' and '{}'", content.error(), chunks.error());
          }
        } else {
          fmt::println(stderr, "warning: failed to get file obj: {}", file.error());
        }

        if (nid == -1) nid = meta.push_inline(entry.info);
        index.emplace(entry.csum, nid, erofs::mode_to_filetype(entry.info.mode), true);
      }
    }

    if (entry.link.first != -1)
      complete_link(nid, entry.link.first, entry.link.second);
  }

  return root_nid;
}

std::expected<cowtree_index_extension, std::string>
TreeWriter::write_index() {
  cowtree_index_extension ret { .magic = cowtree_index_extension::MAGIC };

  std::vector<erofs::DirEnt> entries;
  for (auto const& [csum, entry]: index) {
    if (!entry.original) continue;
    entries.emplace_back(csum, entry.filetype);
  }

  auto dirs = erofs::build_directory(entries);
  ret.index_nid = meta.push_inline(FileInfo { .mode = S_IFDIR,}, std::as_bytes(std::span{dirs.buf}));

  for (size_t i = 0; auto const& [csum, entry]: index) {
    if (!entry.original) continue;
    block_view<erofs_dirent>(meta.get_block(ret.index_nid, dirs.entry_off[i]))[0]->nid = entry.nid;
    i++;
  }

  return ret;
}

std::expected<std::size_t, std::string>
TreeWriter::write_appendix(int fd, std::size_t& pos) {
  auto start = pos;
  for (auto const& e: appendix) {
    TRY(clone_range(fd, file_clone_range {
          .src_fd = (int)mirror.mirror_fd,
          .src_offset = e.phys,
          .src_length = e.length,
          .dest_offset = pos,
        }));

    pos += e.length;
  }

  return pos - start;
}
