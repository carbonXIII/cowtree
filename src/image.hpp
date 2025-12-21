#pragma once

#include "mirror.hpp"

std::expected<std::vector<std::byte>, std::string>
build_fs(erofs::nid_t root_nid, std::span<std::byte> meta, std::span<DeviceInfo> devices);
