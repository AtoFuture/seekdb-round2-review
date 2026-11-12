#include "../src/storage/page_checksum.hpp"
#include "minivectordb/store.hpp"
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <unistd.h>

using Page = std::array<unsigned char,4096>;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("check failed: ") + #x + " line " + std::to_string(__LINE__)); } while(false)

// Deliberately independent bit-at-a-time format-02 reference.
std::uint32_t reference(Page page,std::size_t offset) {
  for(std::size_t i=0;i<4;++i) page[offset+i]=0;
  std::uint32_t crc=0xffffffffu;
  for(auto byte:page) {
    crc^=byte;
    for(unsigned bit=0;bit<8;++bit) {
      if(crc&1u) crc=(crc>>1)^0xedb88320u;
      else crc>>=1;
    }
  }
  return crc^0xffffffffu;
}
void verify(const Page& page) {
  for(std::size_t offset:{24u,32u}) {
    const auto before=page;
    const auto expected=reference(page,offset);
    CHECK(minivectordb::detail::page_checksum(page,offset)==expected);
    CHECK(page==before);
    auto changed=page;
    for(std::size_t i=0;i<4;++i) changed[offset+i]^=0xff;
    CHECK(minivectordb::detail::page_checksum(changed,offset)==expected);
    for(auto position:{offset-1,offset+4,std::size_t{4095}}) {
      changed=page; changed[position]^=1;
      CHECK(minivectordb::detail::page_checksum(changed,offset)!=expected);
    }
  }
}
void pages() {
  Page page{};
  CHECK(reference(page,24)==0xc71c0011u); // CRC-32 of 4096 zero bytes.
  for(unsigned fill:{0u,255u,85u,170u}) { page.fill(static_cast<unsigned char>(fill)); verify(page); }
  for(std::size_t i=0;i<page.size();++i) page[i]=static_cast<unsigned char>(i);
  verify(page);
  // Every byte position, including both checksum boundaries and the final byte.
  for(std::size_t i=0;i<page.size();++i) { page.fill(0); page[i]=0x80; verify(page); }
  std::mt19937 rng(0x43524332u);
  for(unsigned n=0;n<256;++n) { for(auto& byte:page) byte=static_cast<unsigned char>(rng()); verify(page); }
}
void file_compatibility() {
  namespace fs=std::filesystem;
  const auto* configured=std::getenv("MVDB_TEST_DIR");
  const auto base=configured?fs::path(configured):fs::temp_directory_path()/"minivectordb-tests";
  fs::create_directories(base);
  auto pattern=(base/"checksum-XXXXXX").string();
  CHECK(::mkdtemp(pattern.data())!=nullptr);
  const auto dir=fs::path(pattern), path=dir/"pages.db";
  {
    minivectordb::Store store(path); store.begin();
    store.put(1,std::string(17000,'z')); store.commit();
    store.begin(); store.put(2,"second generation"); store.commit();
  }
  std::ifstream input(path,std::ios::binary);
  Page page{}; std::size_t count=0;
  while(input.read(reinterpret_cast<char*>(page.data()),page.size())) {
    const std::size_t offset=count<2?32:24;
    std::uint32_t stored=0;
    for(unsigned i=0;i<4;++i) stored|=static_cast<std::uint32_t>(page[offset+i])<<(8*i);
    CHECK(stored==reference(page,offset)); ++count;
  }
  CHECK(input.eof() && input.gcount()==0 && count>20);
  input.close();
  { minivectordb::Store store(path); CHECK(store.get(1)==std::string(17000,'z')); CHECK(store.get(2)=="second generation"); }
  fs::remove_all(dir);
}
int main() {
  try { pages(); file_compatibility(); std::cout<<"PASS checksum\n"; }
  catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
