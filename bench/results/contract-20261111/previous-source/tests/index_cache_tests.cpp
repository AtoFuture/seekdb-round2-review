#include "minivectordb/store.hpp"
#include "minivectordb/database.hpp"
#include "minivectordb/profile.hpp"
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <array>
#include <fstream>
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
using namespace minivectordb;
#define CHECK(x) do {if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
template<class F> void rejects(F f) {bool failed=false; try {f();}catch(const std::exception&){failed=true;} CHECK(failed);}
void failed_write(const std::filesystem::path& path) {
 auto pid=fork(); CHECK(pid>=0);
 if(pid==0) {
  try {
   Store s(path,256*1024); s.begin(); s.put(1,"old"); s.commit(); CHECK(s.get(1)=="old"); s.begin();
   struct rlimit original{}; CHECK(getrlimit(RLIMIT_FSIZE,&original)==0);
   auto limited=original; limited.rlim_cur=std::filesystem::file_size(path)+4096+1000;
   CHECK(signal(SIGXFSZ,SIG_IGN)!=SIG_ERR); CHECK(setrlimit(RLIMIT_FSIZE,&limited)==0);
   rejects([&]{s.put(1,"failed");}); CHECK(s.get(1)=="old");
   CHECK(setrlimit(RLIMIT_FSIZE,&original)==0);
   // Retry overwrites the incomplete branch address; failed pages must never be cached.
   s.put(1,"retry"); CHECK(s.get(1)=="retry"); s.commit(); _exit(0);
  }catch(...){_exit(1);}
 }
 int status=0; CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
 Store s(path,256*1024); CHECK(s.get(1)=="retry");
}
void corruption(const std::filesystem::path& dir) {
 for(unsigned kind=0;kind<4;++kind) {
  auto path=dir/("corrupt"+std::to_string(kind));
  {Store s(path,256*1024); s.begin(); s.put(1,"data"); s.commit(); CHECK(s.get(1)=="data");}
  // A one-key tree has value page 2, leaf INDEX 3 and root INDEX 18.
  // Modify closed files only: no live-cache external mutation is supported.
  std::fstream f(path,std::ios::in|std::ios::out|std::ios::binary);
  std::array<unsigned char,4096> page{}; f.seekg(3*4096); f.read(reinterpret_cast<char*>(page.data()),4096); CHECK(f.good());
  if(kind==0)page[4095]^=1;
  if(kind==1)page[8]=2; // Wrong page type, with valid CRC.
  if(kind==2)page[12]=14; // Wrong expected depth, with valid CRC.
  if(kind==3) {page[40]=3;} // Child points at itself, with valid CRC.
  if(kind) {
   for(unsigned i=24;i<28;++i)page[i]=0;
   std::uint32_t crc=0xffffffffU;
   for(auto byte:page){crc^=byte; for(unsigned i=0;i<8;++i)crc=(crc>>1)^((crc&1)?0xedb88320U:0);}
   crc=~crc; for(unsigned i=0;i<4;++i)page[24+i]=static_cast<unsigned char>(crc>>(8*i));
  }
  f.seekp(3*4096); f.write(reinterpret_cast<const char*>(page.data()),4096); f.close();
  Store s(path,256*1024); rejects([&]{(void)s.get(1);});
  auto count=s.cache_stats().resident_pages; CHECK(count==15);
  rejects([&]{(void)s.get(1);}); CHECK(s.cache_stats().resident_pages==count);
 }
}
using TestPage=std::array<unsigned char,4096>;
std::uint64_t read_le(const TestPage& page,std::size_t offset,unsigned bytes=8) {
 std::uint64_t result=0;
 for(unsigned i=0;i<bytes;++i) result|=static_cast<std::uint64_t>(page[offset+i])<<(8*i);
 return result;
}
void write_le(TestPage& page,std::size_t offset,std::uint64_t value,unsigned bytes=8) {
 for(unsigned i=0;i<bytes;++i) page[offset+i]=static_cast<unsigned char>(value>>(8*i));
}
TestPage read_test_page(std::fstream& file,std::uint64_t id) {
 TestPage page{}; file.seekg(static_cast<std::streamoff>(id*page.size()));
 file.read(reinterpret_cast<char*>(page.data()),page.size()); CHECK(file.good());
 return page;
}
void write_index_page(std::fstream& file,std::uint64_t id,TestPage page) {
 // Independent bitwise IEEE CRC, so the fixture remains structurally CRC-valid.
 write_le(page,24,0,4);
 std::uint32_t crc=0xffffffffu;
 for(auto byte:page) {
  crc^=byte;
  for(unsigned bit=0;bit<8;++bit) crc=(crc>>1)^((crc&1u)?0xedb88320u:0);
 }
 write_le(page,24,~crc,4);
 file.seekp(static_cast<std::streamoff>(id*page.size()));
 file.write(reinterpret_cast<const char*>(page.data()),page.size()); CHECK(file.good());
}
void cached_wrong_depth(const std::filesystem::path& path) {
 {Store store(path); store.begin(); store.put_in(0,1,"valid"); store.commit();}
 // Patch only while closed. Add a second namespace path which skips depth 1.
 {
  std::fstream file(path,std::ios::in|std::ios::out|std::ios::binary); CHECK(file.good());
  const auto a=read_test_page(file,0), b=read_test_page(file,1);
  const auto root_id=read_le(read_le(a,8)>read_le(b,8)?a:b,16);
  auto root=read_test_page(file,root_id);
  const auto depth1=read_test_page(file,read_le(root,32));
  const auto depth2_id=read_le(depth1,32);
  const auto depth2=read_test_page(file,depth2_id);
  CHECK(read_le(root,12,4)==0 && read_le(depth1,12,4)==1 && read_le(depth2,12,4)==2);
  CHECK(read_le(root,40)==0 && depth2_id<root_id);
  write_le(root,40,depth2_id); write_index_page(file,root_id,root);
 }
 Store store(path,256*1024);
 CHECK(store.get_in(0,1)=="valid"); // Includes depth-2 page in the cache.
 CHECK(store.cache_stats().resident_pages==16);
 store.reset_cache_stats(); profile::set_enabled(true); profile::reset();
 bool corrupt=false;
 try {(void)store.get_in(1ULL<<56,1);}
 catch(const std::runtime_error& error) {corrupt=std::string(error.what())=="corrupt MiniVectorDB file";}
 const auto measured=profile::snapshot(); profile::set_enabled(false);
 CHECK(corrupt);
 CHECK(store.cache_stats().hits==1 && store.cache_stats().misses==0);
 CHECK(measured.page_read_branch.calls==2);
 CHECK(measured.pread.calls==0 && measured.page_checksum.calls==0);
 CHECK(store.get_in(0,1)=="valid");
}
void independent_store_caches(const std::filesystem::path& dir) {
 const auto first=dir/"independent-a.db", second=dir/"independent-b.db";
 {Store store(first); store.begin(); store.put(1,"first"); store.commit();}
 {Store store(second); store.begin(); store.put(2,"second"); store.commit();}
 // Both files allocate matching page IDs, but leaf child slots differ (keys 1/2).
 CHECK(std::filesystem::file_size(first)==std::filesystem::file_size(second));
 Store a(first,256*1024), b(second,256*1024);
 CHECK(a.cache_stats().resident_pages==0 && b.cache_stats().resident_pages==0);
 CHECK(a.get(1)=="first");
 CHECK(a.cache_stats().resident_pages==16);
 CHECK(b.cache_stats().resident_pages==0 && b.cache_stats().hits==0);
 b.reset_cache_stats(); CHECK(b.get(2)=="second");
 CHECK(b.cache_stats().misses==16 && b.cache_stats().hits==0);
 CHECK(!b.get(1));
 a.reset_cache_stats(); CHECK(a.get(1)=="first"); CHECK(!a.get(2));
 CHECK(a.cache_stats().hits==32 && a.cache_stats().misses==0);
 CHECK(b.get(2)=="second");
}
int main() {
 try {
  const auto* root=std::getenv("MVDB_TEST_DIR"); CHECK(root);
  std::filesystem::create_directories(root);
  auto pattern=(std::filesystem::path(root)/"cache-XXXXXX").string();
  CHECK(mkdtemp(pattern.data())); const auto dir=std::filesystem::path(pattern);
  for(std::size_t budget:{0UL,1UL,4096UL,256UL*1024,1024UL*1024}) {
   auto path=dir/std::to_string(budget);
   {
    Store s(path,budget); CHECK(s.cache_stats().capacity_bytes<=budget);
    s.begin(); s.put(1,std::string(10000,'x')); s.put(2,"two"); s.commit();
    CHECK(s.get(1)==std::string(10000,'x'));
    s.reset_cache_stats(); profile::set_enabled(true); profile::reset();
    CHECK(s.get(1)==std::string(10000,'x'));
    auto stats=s.cache_stats(); auto reads=profile::snapshot();
    if(budget>=256*1024) {CHECK(stats.hits==16); CHECK(stats.misses==0); CHECK(reads.pread.calls==3); CHECK(stats.resident_pages>=16);}
    else {CHECK(stats.hits==0); CHECK(stats.capacity_bytes==0); CHECK(reads.pread.calls==19);}
    profile::set_enabled(false);
    s.begin(); auto point=s.savepoint(); s.put(1,"new"); CHECK(s.get(1)=="new");
    s.restore(point); CHECK(s.get(1)==std::string(10000,'x'));
    unsigned visited=0;
    s.scan([&](auto key,auto value){CHECK(value==(key==1?std::string(10000,'x'):"two")); ++visited; s.put(key,"updated");});
    CHECK(visited==2); CHECK(s.get(2)=="updated"); s.rollback(); CHECK(s.get(2)=="two");
    s.begin(); for(int i=3;i<50;++i)s.put(i,"entry"); s.commit();
    CHECK(s.cache_stats().resident_pages*4096<=s.cache_stats().capacity_bytes);
    if(budget==256*1024)CHECK(s.cache_stats().evictions>0);
   }
   {Store s(path,budget); CHECK(s.cache_stats().resident_pages==0); CHECK(s.cache_stats().hits==0); CHECK(s.get(2)=="two");}
   Options options; options.index_cache_bytes=budget;
   Database db(dir/("sql"+std::to_string(budget)),options);
   db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY)"); db.execute("INSERT INTO t VALUES(1)");
   db.execute("SELECT * FROM t"); db.reset_cache_stats(); db.execute("SELECT * FROM t");
   CHECK(db.cache_stats().capacity_bytes<=budget);
   if(budget>=256*1024)CHECK(db.cache_stats().hits>0);
  }
  failed_write(dir/"failed.db"); corruption(dir);
  cached_wrong_depth(dir/"cached-depth.db"); independent_store_caches(dir);
  std::filesystem::remove_all(dir);
  std::cout<<"index cache tests passed\n";
 } catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
