#include "image.hpp"

struct SuperBlock {
  uint64_t blocks;
  uint64_t root_nid;
  uint32_t meta_blkaddr;
  uint16_t extra_devices;
  uint16_t devt_slotoff;
};

erofs_super_block make_super_block_ext_addr(SuperBlock sb) {
  return {
    .magic = EROFS_SUPER_MAGIC_V1,
    .blkszbits = std::countr_zero(erofs::BLOCK_SIZE),

    .rb = { .blocks_hi = (uint16_t)(sb.blocks >> 32), },
    .blocks_lo = (uint16_t)(sb.blocks & ((1 << 16) - 1)),

    .meta_blkaddr = (uint32_t) sb.meta_blkaddr,

    .feature_incompat = EROFS_FEATURE_INCOMPAT_DEVICE_TABLE | EROFS_FEATURE_INCOMPAT_48BIT,

    .extra_devices = sb.extra_devices,
    .devt_slotoff = sb.devt_slotoff,

    .rootnid_8b = sb.root_nid,
  };
}

erofs_super_block make_super_block(SuperBlock sb) {
  return {
    .magic = EROFS_SUPER_MAGIC_V1,
    .blkszbits = std::countr_zero(erofs::BLOCK_SIZE),

    .rb = { .rootnid_2b = (uint16_t)sb.root_nid, },
    .blocks_lo = (uint16_t)(sb.blocks & ((1 << 16) - 1)),

    .meta_blkaddr = (uint32_t) sb.meta_blkaddr,

    .feature_incompat = EROFS_FEATURE_INCOMPAT_DEVICE_TABLE,

    .extra_devices = sb.extra_devices,
    .devt_slotoff = sb.devt_slotoff,
  };
}

std::expected<std::vector<std::byte>, std::string>
build_fs(erofs::nid_t root_nid, std::span<std::byte> meta, std::span<DeviceInfo> devices, bool use_ext_addr) {
  SuperBlock sb = {
    .root_nid = root_nid,
    .extra_devices = (uint16_t)devices.size(),
  };

  std::vector<std::byte> ret;
  auto super_blkaddr = EROFS_SUPER_OFFSET / erofs::BLOCK_SIZE;
  auto meta_size = round_up(meta.size(), erofs::BLOCK_SIZE) / erofs::BLOCK_SIZE;

  sb.extra_devices = (uint16_t)devices.size();
  auto devt_offset = round_up(EROFS_SUPER_OFFSET + sizeof(erofs_super_block), EROFS_DEVT_SLOT_SIZE);
  sb.devt_slotoff = devt_offset / EROFS_DEVT_SLOT_SIZE;

  sb.meta_blkaddr = round_up(devt_offset + sb.extra_devices * EROFS_DEVT_SLOT_SIZE, erofs::BLOCK_SIZE) / erofs::BLOCK_SIZE;

  sb.blocks = sb.meta_blkaddr + meta_size;
  ret.resize(sb.blocks * erofs::BLOCK_SIZE);

  {
    auto device_slots = block_view<erofs_deviceslot>(std::span{ret}.subspan(devt_offset));
    for (int i = 0; auto device: devices) {
      device_slots[i++] = device(use_ext_addr);
      uint64_t end_block = device.block_offset + (device.sz_bytes + erofs::BLOCK_SIZE - 1) / erofs::BLOCK_SIZE;
      sb.blocks = std::max(sb.blocks, end_block);
    }
  }

  fmt::println(stderr, "total blocks: {}", sb.blocks);

  auto super = use_ext_addr ? make_super_block_ext_addr(sb) : make_super_block(sb);
  copy(as_bytes(std::span{&super, 1}), std::span{ret}.subspan(EROFS_SUPER_OFFSET));
  copy(meta, std::span{ret}.subspan(super.meta_blkaddr * erofs::BLOCK_SIZE));

  return ret;
}
