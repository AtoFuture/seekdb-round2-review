#include "../src/storage/page_checksum.hpp"
#include <cerrno>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>
using Page=std::array<unsigned char,4096>;
using namespace minivectordb::detail;
#define CHECK(x) do { if(!(x)) throw std::runtime_error("check failed: " #x); } while(false)
// Independent, bit-at-a-time IEEE CRC-32 oracle.
std::uint32_t oracle(const Page& p,std::size_t offset) {
  std::uint32_t c=0xffffffffu;
  for(std::size_t i=0;i<p.size();++i) {
    c^=(i>=offset && i<offset+4)?0u:p[i];
    for(unsigned bit=0;bit<8;++bit) c=(c&1u)?(c>>1)^0xedb88320u:c>>1;
  }
  return c^0xffffffffu;
}
void verify(const Page& p,std::size_t offset) {
  const auto before=p;
  const auto expected=oracle(p,offset);
  page_checksum_mode=PageChecksumMode::bytewise;
  const auto reference=page_checksum(p,offset);
  page_checksum_mode=PageChecksumMode::slicing_by_8;
  CHECK(page_checksum(p,offset)==reference);
  CHECK(reference==expected);
  CHECK(p==before);
  if(offset<=p.size()-4) {
    auto changed=p;
    for(std::size_t i=offset;i<offset+4;++i) changed[i]^=0xff;
    CHECK(page_checksum(changed,offset)==expected);
    for(auto pos:{offset==0?4:offset-1,offset+4<p.size()?offset+4:offset-1}) {
      changed=p; changed[pos]^=1;
      CHECK(page_checksum(changed,offset)!=expected);
    }
  }
}
int main() {
  try {
    CHECK(page_checksum_mode==PageChecksumMode::slicing_by_8);
    Page p{};
    CHECK(oracle(p,24)==0xc71c0011u);
    const std::array<std::size_t,24> offsets{0,1,3,4,5,7,8,9,15,16,23,24,25,28,31,32,33,4087,4088,4089,4092,4093,4096,std::numeric_limits<std::size_t>::max()};
    for(unsigned fill:{0u,255u,85u,170u}) {
      p.fill(static_cast<unsigned char>(fill));
      for(auto offset:offsets) verify(p,offset);
    }
    for(std::size_t i=0;i<p.size();++i) p[i]=static_cast<unsigned char>(i);
    for(std::size_t offset=0;offset<=p.size()-4;++offset) verify(p,offset);
    std::mt19937 rng(0x534c4943u);
    for(unsigned trial=0;trial<128;++trial) {
      for(auto& byte:p) byte=static_cast<unsigned char>(rng());
      for(auto offset:offsets) verify(p,offset);
    }
    for(std::size_t i=0;i<p.size();++i) {
      p.fill(0); p[i]=0x80; verify(p,24); verify(p,32);
    }
    for(auto mode:{PageChecksumMode::bytewise,PageChecksumMode::slicing_by_8}) {
      page_checksum_mode=mode;
      minivectordb::profile::set_enabled(true);
      minivectordb::profile::reset();
      errno=EDOM;
      CHECK(page_checksum(p,24)==oracle(p,24));
      CHECK(errno==EDOM);
      const auto snapshot=minivectordb::profile::snapshot();
      CHECK(snapshot.page_checksum.calls==1);
      CHECK(snapshot.page_checksum.bytes==0);
      CHECK(snapshot.pread.calls==0 && snapshot.pwrite.calls==0);
      minivectordb::profile::set_enabled(false);
      CHECK(page_checksum(p,32)==oracle(p,32));
      CHECK(minivectordb::profile::snapshot()==snapshot);
    }
    page_checksum_mode=PageChecksumMode::bytewise;
    bool thread_default=false;
    std::thread worker([&] { thread_default=page_checksum_mode==PageChecksumMode::slicing_by_8; });
    worker.join();
    CHECK(thread_default);
    CHECK(page_checksum_mode==PageChecksumMode::bytewise);
    page_checksum_mode=PageChecksumMode::slicing_by_8;
    std::cout<<"PASS checksum_slicing\n";
  } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
