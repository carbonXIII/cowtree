#pragma once

#include <hostfs/mirror.hpp>
#include <erofs/base.hpp>

struct SuperBlock {
  uint64_t blocks;
  uint64_t root_nid;
  uint32_t meta_blkaddr;
  uint16_t extra_devices;
  uint16_t devt_slotoff;

  bool use_ext_addr;
};

std::expected<SuperBlock, std::string>
start_filesystem(std::size_t num_devices, bool use_ext_addr = true);

std::expected<std::vector<std::byte>, std::string>
finish_filesystem(SuperBlock sb, erofs::nid_t root_nid,
                  std::span<std::byte const> meta, std::span<DeviceInfo const> devices);
