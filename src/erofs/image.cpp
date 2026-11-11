#include "image.hpp"

erofs_super_block make_super_block(SuperBlock sb) {
  return {
    .magic = EROFS_SUPER_MAGIC_V1,
    .blkszbits = std::countr_zero(erofs::BLOCK_SIZE),

    .sb_extslots = (uint8_t)(round_up(sb.extensions.size() + sizeof(erofs_super_block) - 128, 16) / 16),

    .rb = { .blocks_hi = (uint16_t)(sb.blocks >> 32), },
    .blocks_lo = (uint32_t)sb.blocks,

    .meta_blkaddr = (uint32_t) sb.meta_blkaddr,

    .feature_incompat = EROFS_FEATURE_INCOMPAT_DEVICE_TABLE | EROFS_FEATURE_INCOMPAT_48BIT | EROFS_FEATURE_INCOMPAT_CHUNKED_FILE,

    .extra_devices = sb.extra_devices,
    .devt_slotoff = sb.devt_slotoff,

    .rootnid_8b = sb.root_nid,
  };
}

std::expected<SuperBlock, std::string>
start_filesystem(std::size_t num_devices, std::size_t extension_size) {
  SuperBlock sb = {
    .extra_devices = (uint16_t)num_devices,
  };

  extension_size = round_up(extension_size, 16);
  auto devt_offset = round_up(EROFS_SUPER_OFFSET + sizeof(erofs_super_block) + extension_size, EROFS_DEVT_SLOT_SIZE);
  sb.devt_slotoff = devt_offset / EROFS_DEVT_SLOT_SIZE;

  sb.meta_blkaddr = round_up(devt_offset + sb.extra_devices * EROFS_DEVT_SLOT_SIZE, erofs::BLOCK_SIZE) / erofs::BLOCK_SIZE;

  return sb;
}

std::expected<std::vector<std::byte>, std::string>
finish_filesystem(SuperBlock sb, erofs::nid_t root_nid,
                  std::size_t meta_size, std::span<DeviceInfo const> devices) {
  sb.root_nid = root_nid;

  meta_size = round_up(meta_size, erofs::BLOCK_SIZE) / erofs::BLOCK_SIZE;
  fmt::println(stderr, "super ({} blocks) + meta ({} blocks)", sb.meta_blkaddr, meta_size);
  sb.blocks = sb.meta_blkaddr + meta_size;
  std::vector<std::byte> ret((sb.devt_slotoff + devices.size()) * EROFS_DEVT_SLOT_SIZE);

  {
    auto device_slots = block_view<erofs_deviceslot>(std::span{ret}.subspan(sb.devt_slotoff * EROFS_DEVT_SLOT_SIZE));
    for (int i = 0; auto device: devices) {
      device_slots[i++] = device();
      uint64_t end_block = device.block_offset + (device.sz_bytes + erofs::BLOCK_SIZE - 1) / erofs::BLOCK_SIZE;
    }
  }

  fmt::println(stderr, "total blocks: {}", sb.blocks);

  auto super = make_super_block(sb);
  copy(as_bytes(std::span{&super, 1}), std::span{ret}.subspan(EROFS_SUPER_OFFSET));

  if (sb.extensions.size())
    copy(sb.extensions, std::span{ret}.subspan(EROFS_SUPER_OFFSET + sizeof(erofs_super_block)));

  return ret;
}
