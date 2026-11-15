#include "meta.hpp"

namespace erofs {
  using Node = MetadataBuilder::Node;

  erofs_inode_compact& Node::get() {
    return block_view<erofs_inode_compact>(p->blocks.get_block(nid * INODE_SIZE + p->real_base))[0];
  }

  std::span<std::byte> Node::get_block(std::size_t offset) {
    auto const& ino = get();
    std::size_t tail_size = inode_tail_size(ino);

    if (inode_layout(ino) == EROFS_INODE_FLAT_INLINE || inode_layout(ino) == EROFS_INODE_FLAT_PLAIN) {
      if (offset < ino.i_size - tail_size)
        return p->blocks.get_block(ino.i_u.startblk_lo * BLOCK_SIZE + offset);

      offset -= ino.i_size - tail_size;
    }

    auto addr = (nid + 1) * INODE_SIZE + p->real_base + offset;
    return p->blocks.get_block(addr, tail_size - offset);
  }

  Node MetadataBuilder::push(erofs_inode_compact ino, std::span<std::byte const> inlined) {
    auto size = INODE_SIZE + inode_tail_size(ino);
    auto block = blocks.allocate(size);
    Node ret = get((blocks.get_address(block) - real_base) / INODE_SIZE);
    ret.get() = ino;

    if (inlined.size())
      copy(inlined, block.subspan(INODE_SIZE));

    return ret;
  }

  Node MetadataBuilder::push_inline(FileInfo const& info, std::span<std::byte const> content) {
    auto ino = inode_from_info(info, EROFS_INODE_FLAT_INLINE, content.size());
    auto tail_size = inode_tail_size(ino);

    if (INODE_SIZE + tail_size > BLOCK_SIZE || tail_size == 0) {
      ino.i_format = inode_format(EROFS_INODE_FLAT_PLAIN);
      tail_size = 0;
    }

    if (content.size() > tail_size) {
      auto body = blocks.allocate(round_up(content.size() - tail_size, BLOCK_SIZE));
      copy(content.subspan(0, content.size() - tail_size), body);
      ino.i_u.startblk_lo = blocks.get_address(body) / BLOCK_SIZE;
    }

    return push(ino, tail_size ? content.subspan(content.size() - tail_size) : std::span<std::byte>{});
  }

  Node MetadataBuilder::push_chunks(FileInfo const& info, ChunkMap const& chunks) {
    auto ino = inode_from_info(info, EROFS_INODE_CHUNK_BASED, chunks.size);
    ino.i_u.c = chunk_format(chunks.shift - std::countr_zero(BLOCK_SIZE), false);

    return push(ino, as_bytes(chunks.indexes));
  }
}
