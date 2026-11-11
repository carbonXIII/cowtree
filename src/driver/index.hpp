#pragma once

#include <erofs/meta.hpp>

#include <string>
#include <map>

struct cowtree_index_extension {
  static constexpr uint32_t MAGIC = 0x78646E69;
  uint32_t magic = MAGIC; // 'INDX'
  erofs::nid_t index_nid = -1;
};

struct Mirror;
struct FileIndex {
  struct Entry {
    erofs::nid_t nid;
    uint8_t filetype;
    bool original;
  };

  std::map<std::string, Entry> m;

  auto find(std::string const& s) { return m.find(s); }
  auto begin() { return m.begin(); }
  auto end() { return m.end(); }
  auto emplace(std::string const& csum, erofs::nid_t nid, uint8_t filetype, bool original = false) {
    m.emplace(csum, Entry { nid, filetype, original });
  }

  std::expected<std::monostate, std::string> import_image(int src, int dest, size_t& offset, size_t dest_meta_addr);
};
