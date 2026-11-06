#include "tree.hpp"

#include <erofs/dirent.hpp>
#include <erofs/meta.hpp>

#include <fmt/format.h>
#include <deque>

std::expected<ChunkMap, std::string>
TreeWriter::try_get_chunks(ostree::File& file, FileInfo const& info) {
  if (auto fd = file.get_fd(); !fd.has_value()) {
    return std::unexpected(fmt::format("failed to get file ({}) extents: {}", file.csum, fd.error()));
  } else {
    return mirror.insert_file(fd.value(), info.size);
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
        index[entry.csum] = { nid, erofs::mode_to_filetype(entry.info.mode) };
      }
    }

    if (entry.link.first != -1)
      complete_link(nid, entry.link.first, entry.link.second);
  }

  return root_nid;
}

std::expected<cowtree_index_extension, std::string>
TreeWriter::write_index() {
  cowtree_index_extension ret {};

  std::vector<erofs::DirEnt> entries;
  for (auto const& [csum, entry]: index) {
    entries.emplace_back(csum, entry.filetype);
  }

  auto dirs = erofs::build_directory(entries);
  ret.index_nid = meta.push_inline(FileInfo { .mode = S_IFDIR,}, std::as_bytes(std::span{dirs.buf}));

  for (size_t i = 0; auto const& [csum, entry]: index) {
    block_view<erofs_dirent>(meta.get_block(ret.index_nid, dirs.entry_off[i]))[0]->nid = entry.nid;
    i++;
  }

  return ret;
}

std::expected<std::monostate, std::string>
read_at(int fd, std::span<std::byte> buf, size_t offset) {
  auto ret = pread(fd, buf.data(), buf.size(), offset);

  if (ret != buf.size())
    return std::unexpected(fmt::format("pread failed to read {} bytes, fd={} @ 0x{:X}, ret={}, errno={}",
                                       buf.size(), fd, offset, ret, errno));

  return {};
}

std::expected<std::monostate, std::string>
read_at(int fd, auto& out, size_t offset) {
  return read_at(fd, as_writable_bytes(std::span{&out, 1}), offset);
}

std::expected<std::monostate, std::string>
TreeWriter::read_index(int fd, size_t base_addr) {
  uint64_t eof;
  {
    struct stat buf;
    fstat(fd, &buf);
    eof = buf.st_size;
  }

  if (eof < EROFS_SUPER_OFFSET + sizeof(erofs_super_block))
    return std::unexpected(fmt::format("No superblock: eof={} < super block size", eof));

  erofs_super_block sb;
  TRY(read_at(fd, sb, EROFS_SUPER_OFFSET));

  ssize_t ext_size = sb.sb_extslots * 16 - round_up(sizeof(erofs_super_block) - 128, 16);
  if (ext_size < sizeof(cowtree_index_extension))
    return std::unexpected("No index extension");

  std::vector<std::byte> extensions(ext_size);
  TRY(read_at(fd, std::span{extensions}, EROFS_SUPER_OFFSET + sizeof(erofs_super_block)));

  size_t index_nid = -1;
  for (size_t i = 0; i + 16 < extensions.size(); i += 16) {
    auto view = block_view<cowtree_index_extension>(subspan(extensions, i));
    if (view.size() && view[0]->magic == cowtree_index_extension::MAGIC && view[0]->index_nid != -1) {
      index_nid = view[0]->index_nid;
      break;
    }
  }

  if (index_nid == -1)
    return std::unexpected("No index extension");

  auto ino_addr = sb.meta_blkaddr * erofs::BLOCK_SIZE + index_nid * erofs::INODE_SIZE;
  if (eof < ino_addr + erofs::INODE_SIZE)
    return std::unexpected(fmt::format("Invalid index nid: 0x{:X} > 0x{:X}", ino_addr, eof));

  erofs_inode_compact ino;
  TRY(read_at(fd, ino, ino_addr));

  if (auto layout = erofs::inode_layout(ino); layout != EROFS_INODE_FLAT_INLINE && layout != EROFS_INODE_FLAT_PLAIN)
    return std::unexpected(fmt::format("Invalid index inode layout {}", layout));

  std::vector<std::byte> content(ino.i_size);
  auto tail_size = erofs::inode_tail_size(ino);
  auto head_size = ino.i_size - tail_size;

  if (head_size > 0)
    TRY(read_at(fd, std::span{content}.subspan(0, head_size), ino.i_u.startblk_lo * erofs::BLOCK_SIZE));

  if (tail_size > 0)
    TRY(read_at(fd, std::span{content}.subspan(head_size), ino_addr + erofs::INODE_SIZE));

  auto extents = TRY(get_extents(fd, 0, eof));
  erofs::read_dirents(std::span{content}, [&](std::string_view name, erofs::nid_t nid, uint8_t filetype) {
    auto addr = sb.meta_blkaddr + nid * erofs::INODE_SIZE;
    auto ext = std::lower_bound(extents.begin(), extents.end(), addr);
    if (ext == extents.end() || ext->logical > addr || addr >= (ext->logical + ext->length))
      return;

    auto phys = (addr - ext->logical) + ext->phys;
    erofs::nid_t real_nid = (phys + mirror.block_offset * erofs::BLOCK_SIZE - base_addr) / erofs::INODE_SIZE;
    index.emplace(std::string { name }, IndexEntry { real_nid, filetype });
    });

  return {};
}
