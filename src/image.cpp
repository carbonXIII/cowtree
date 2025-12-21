#include "image.hpp"

std::expected<std::vector<std::byte>, std::string>
build_fs(erofs::nid_t root_nid, std::span<std::byte> meta, std::span<DeviceInfo> devices) {
  erofs_super_block super {
    .magic = EROFS_SUPER_MAGIC_V1,
    .blkszbits = std::countr_zero(erofs::BLOCK_SIZE),
    .root_nid = (uint16_t)root_nid,
    // TODO: inos
    .feature_incompat = EROFS_FEATURE_INCOMPAT_DEVICE_TABLE,
    .extra_devices = (uint16_t)devices.size(),
  };

  std::vector<std::byte> ret;
  auto super_blkaddr = EROFS_SUPER_OFFSET / erofs::BLOCK_SIZE;
  auto meta_size = round_up(meta.size(), erofs::BLOCK_SIZE) / erofs::BLOCK_SIZE;

  super.extra_devices = (uint16_t)devices.size();
  auto devt_offset = round_up(EROFS_SUPER_OFFSET + sizeof(erofs_super_block), EROFS_DEVT_SLOT_SIZE);
  super.devt_slotoff = devt_offset / EROFS_DEVT_SLOT_SIZE;

  super.meta_blkaddr = round_up(devt_offset + super.extra_devices * EROFS_DEVT_SLOT_SIZE, erofs::BLOCK_SIZE) / erofs::BLOCK_SIZE;

  super.blocks = super.meta_blkaddr + meta_size;
  ret.resize(super.blocks * erofs::BLOCK_SIZE);

  {
    auto device_slots = block_view<erofs_deviceslot>(std::span{ret}.subspan(devt_offset));
    for (int i = 0; auto device: devices) {
      device_slots[i++] = device;
      uint32_t end_block = device.block_offset + (device.sz_bytes + erofs::BLOCK_SIZE - 1) / erofs::BLOCK_SIZE;
      super.blocks = std::max(super.blocks, end_block);
    }
  }

  fmt::println(stderr, "total blocks: {}", super.blocks);

  copy(as_bytes(std::span{&super, 1}), std::span{ret}.subspan(EROFS_SUPER_OFFSET));
  copy(meta, std::span{ret}.subspan(super.meta_blkaddr * erofs::BLOCK_SIZE));

  return ret;
}
