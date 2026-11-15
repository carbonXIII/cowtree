#include "index.hpp"

#include <erofs/dirent.hpp>
#include <util/util.hpp>
#include <util/io.hpp>
#include <hostfs/mirror.hpp>

#include <fmt/format.h>

#include <linux/fs.h>
#undef BLOCK_SIZE

std::expected<std::monostate, std::string>
FileIndex::import_image(int src, int dest, std::size_t& offset, std::size_t dest_meta_addr) {
  uint64_t eof;
  {
    struct stat buf;
    fstat(src, &buf);
    eof = buf.st_size;
  }

  if (eof < EROFS_SUPER_OFFSET + sizeof(erofs_super_block))
    return std::unexpected(fmt::format("No superblock: eof={} < super block size", eof));

  erofs_super_block sb;
  TRY(read_at(src, sb, EROFS_SUPER_OFFSET));

  ssize_t ext_size = sb.sb_extslots * 16 - round_up(sizeof(erofs_super_block) - 128, 16);
  if (ext_size < sizeof(cowtree_index_extension))
    return std::unexpected(fmt::format("No index extension: ext_size={}", ext_size));

  std::vector<std::byte> extensions(ext_size);
  TRY(read_at(src, std::span{extensions}, EROFS_SUPER_OFFSET + sizeof(erofs_super_block)));

  size_t index_nid = -1;
  for (size_t i = 0; i + 16 <= extensions.size(); i += 16) {
    auto view = block_view<cowtree_index_extension>(subspan(extensions, i));
    if (view.size() && view[0]->magic == cowtree_index_extension::MAGIC && view[0]->index_nid != -1) {
      index_nid = view[0]->index_nid;
      break;
    }
  }

  if (index_nid == -1)
    return std::unexpected(fmt::format("No index extension: ext_size={}", ext_size));

  auto ino_addr = sb.meta_blkaddr * erofs::BLOCK_SIZE + index_nid * erofs::INODE_SIZE;
  if (eof < ino_addr + erofs::INODE_SIZE)
    return std::unexpected(fmt::format("Invalid index nid: 0x{:X} > 0x{:X}", ino_addr, eof));

  erofs_inode_compact ino;
  TRY(read_at(src, ino, ino_addr));

  if (auto layout = erofs::inode_layout(ino); layout != EROFS_INODE_FLAT_INLINE && layout != EROFS_INODE_FLAT_PLAIN)
    return std::unexpected(fmt::format("Invalid index inode layout {}", layout));

  std::vector<std::byte> content(ino.i_size);
  auto tail_size = erofs::inode_tail_size(ino);
  auto head_size = ino.i_size - tail_size;

  if (head_size > 0)
    TRY(read_at(src, std::span{content}.subspan(0, head_size), ino.i_u.startblk_lo * erofs::BLOCK_SIZE));

  if (tail_size > 0)
    TRY(read_at(src, std::span{content}.subspan(head_size), ino_addr + erofs::INODE_SIZE));

  auto blocks = sb.blocks_lo | (uint64_t(sb.rb.blocks_hi) << 32);
  auto length = (blocks * erofs::BLOCK_SIZE < eof) ? (blocks - sb.meta_blkaddr) * erofs::BLOCK_SIZE : 0;
  TRY(clone_range(dest, file_clone_range {
        .src_fd = src,
        .src_offset = sb.meta_blkaddr * erofs::BLOCK_SIZE,
        .src_length = length,
        .dest_offset = offset,
      }));
  size_t start = std::exchange(offset, offset + (blocks - sb.meta_blkaddr) * erofs::BLOCK_SIZE);

  erofs::read_dirents(std::span{content}, [&](std::string_view name, erofs::nid_t nid, uint8_t filetype) {
    auto addr = /* sb.meta_blkaddr * erofs::BLOCK_SIZE + */ nid * erofs::INODE_SIZE;
    erofs::nid_t real_nid = (addr + start - dest_meta_addr) / erofs::INODE_SIZE;
    emplace(std::string{name}, real_nid, filetype, false);
  });

  return {};
}
