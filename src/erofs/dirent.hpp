#pragma once

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <string>
#include <vector>

#include <util/util.hpp>
#include "base.hpp"

namespace erofs {
  struct DirEnt {
    std::string name;
    uint8_t type;
  };

  static decltype(auto) build_directory(auto const& dir_ents) {
    auto ents = dir_ents | std::views::enumerate |
      std::ranges::to<std::vector<std::pair<size_t, DirEnt>>>();
    std::ranges::sort(ents, [](auto const& lhs, auto const& rhs) {
      return std::get<1>(lhs).name < std::get<1>(rhs).name;
    });

    const size_t n = ents.size();

    struct DirBuff {
      std::vector<char> buf;
      std::vector<size_t> entry_off;

      std::span<char> pad(std::size_t n) {
        buf.resize(buf.size() + n, 0);
        return std::span{buf}.subspan(buf.size() - n);
      }

      void trim(std::size_t n) {
        buf.resize(buf.size() - n);
      }
    } ret;

    ret.entry_off.resize(dir_ents.size());

    for (int i = 0; i < n; ) {
      auto block_start = ret.buf.size();
      auto block = ret.pad(erofs::BLOCK_SIZE);
      auto block_ents = block_view<erofs_dirent>(block);

      int start = i;
      {
        size_t req_size = 0;
        for (; i < n; ++i) {
          req_size += ents[i].second.name.size() + sizeof(erofs_dirent);
          if (req_size > block.size())
            break;
        }
      }

      size_t nameoff = (i - start) * sizeof(erofs_dirent);
      for (int j = start; j < i; ++j) {
        block_ents[j - start] = erofs_dirent {
          .nameoff = (uint16_t)nameoff,
          .file_type = ents[j].second.type,
        };

        ret.entry_off[ents[j].first] = (j - start) * sizeof(erofs_dirent) + block_start;
        copy(ents[j].second.name, block.subspan(nameoff));
        nameoff += ents[j].second.name.size();
      }

      if (i == n && nameoff < block.size())
        ret.trim(block.size() - nameoff);
    }

    return ret;
  }
}
