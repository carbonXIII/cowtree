#pragma once

#include <util/util.hpp>

#include <expected>
#include <string>
#include <utility>
#include <array>
#include <fmt/format.h>

#include <fcntl.h>
#include <unistd.h>

static std::expected<off_t, std::string>
splice_files(int in, int out, off_t offset) {
  struct Pipe {
    std::array<int, 2> fds;

    Pipe(Pipe const&) = delete;
    Pipe(Pipe&& o): fds(std::exchange(o.fds, {-1, -1})) {}
    ~Pipe() { for(auto fd: fds) close(fd); }

    static std::expected<Pipe, std::string> create() {
      Pipe ret;
      if (pipe(ret.fds.data()) == -1)
        return std::unexpected(fmt::format("pipe() failed: errno={}", errno));
      return ret;
    }

  protected:
    Pipe(): fds{-1, -1} {};
  };

  auto p = TRY(Pipe::create());

  int last = 0;
  while (true) {
    if (last = splice(in, &offset, p.fds[1], nullptr, 4096, SPLICE_F_MOVE); last <= 0)
      break;

    if (auto count = splice(p.fds[0], nullptr, out, nullptr, 4096, SPLICE_F_MOVE); count == -1)
      return std::unexpected(fmt::format("splice() failed: errno={}", errno));
  }

  if (last == -1)
    return std::unexpected(fmt::format("splice() failed: errno={}", errno));

  return offset;
}
