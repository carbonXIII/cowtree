#pragma once

#include <unistd.h>
#include <util/util.hpp>

#include <expected>

#include <fmt/format.h>

static std::expected<std::monostate, std::string>
read_at(int fd, std::span<std::byte> buf, size_t offset) {
  auto ret = pread(fd, buf.data(), buf.size(), offset);

  if (ret != buf.size())
    return std::unexpected(fmt::format("pread failed to read {} bytes, fd={} @ 0x{:X}, ret={}, errno={}",
                                       buf.size(), fd, offset, ret, errno));

  return {};
}

static std::expected<std::monostate, std::string>
read_at(int fd, auto& out, size_t offset) {
  return read_at(fd, as_writable_bytes(std::span{&out, 1}), offset);
}
