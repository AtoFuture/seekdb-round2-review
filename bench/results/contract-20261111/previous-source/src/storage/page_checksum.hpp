#pragma once
#include <array>
#include "minivectordb/profile.hpp"
#include <cstddef>
#include <cstdint>

namespace minivectordb::detail {
// Reflected IEEE CRC-32 used by format 02 (not the CRC-32C of x86 CRC32).
inline constexpr auto page_crc32_table=[] {
  std::array<std::uint32_t,256> table{};
  for(std::size_t i=0;i<table.size();++i) {
    auto crc=static_cast<std::uint32_t>(i);
    for(unsigned bit=0;bit<8;++bit)
      crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u)));
    table[i]=crc;
  }
  return table;
}();
inline constexpr auto page_crc32_slicing_tables=[] {
  std::array<std::array<std::uint32_t,256>,8> tables{};
  tables[0]=page_crc32_table;
  for(std::size_t slice=1;slice<tables.size();++slice)
    for(std::size_t i=0;i<256;++i) {
      const auto crc=tables[slice-1][i];
      tables[slice][i]=(crc>>8)^page_crc32_table[crc&0xffu];
    }
  return tables;
}();

// Internal per-thread control for same-binary comparisons; no disk-format flag.
enum class PageChecksumMode { bytewise, slicing_by_8 };
inline thread_local PageChecksumMode page_checksum_mode=PageChecksumMode::slicing_by_8;

inline std::uint32_t page_checksum_bytewise(const std::array<unsigned char,4096>& page,
                                           std::size_t checksum_offset) {
  std::uint32_t crc=0xffffffffu;
  for(std::size_t i=0;i<page.size();++i) {
    const auto byte=(i>=checksum_offset && i<checksum_offset+4)?0u:page[i];
    crc=(crc>>8)^page_crc32_table[(crc^byte)&0xffu];
  }
  return ~crc;
}

// Explicit little-endian byte assembly permits unaligned input on any host.
inline std::uint32_t page_crc32_load_le(const unsigned char* p) {
  return static_cast<std::uint32_t>(p[0])
       | (static_cast<std::uint32_t>(p[1])<<8)
       | (static_cast<std::uint32_t>(p[2])<<16)
       | (static_cast<std::uint32_t>(p[3])<<24);
}
inline std::uint32_t page_crc32_span(std::uint32_t crc,const unsigned char* p,
                                    std::size_t size) {
  const auto& t=page_crc32_slicing_tables;
  while(size>=8) {
    const auto lo=page_crc32_load_le(p)^crc;
    const auto hi=page_crc32_load_le(p+4);
    crc=t[7][lo&0xffu]^t[6][(lo>>8)&0xffu]
       ^t[5][(lo>>16)&0xffu]^t[4][lo>>24]
       ^t[3][hi&0xffu]^t[2][(hi>>8)&0xffu]
       ^t[1][(hi>>16)&0xffu]^t[0][hi>>24];
    p+=8; size-=8;
  }
  while(size--) crc=(crc>>8)^page_crc32_table[(crc^*p++)&0xffu];
  return crc;
}

// The four checksum bytes are treated as zero, without modifying the page.
inline std::uint32_t page_checksum(const std::array<unsigned char,4096>& page,
                                   std::size_t checksum_offset) {
  profile::detail::Scope timer(&profile::Snapshot::page_checksum);
  // Keep the historical bytewise behavior even for offsets outside a full field.
  if(page_checksum_mode==PageChecksumMode::bytewise || checksum_offset>page.size()-4)
    return page_checksum_bytewise(page,checksum_offset);
  auto crc=page_crc32_span(0xffffffffu,page.data(),checksum_offset);
  for(unsigned i=0;i<4;++i) crc=(crc>>8)^page_crc32_table[crc&0xffu];
  crc=page_crc32_span(crc,page.data()+checksum_offset+4,page.size()-checksum_offset-4);
  return ~crc;
}
}
