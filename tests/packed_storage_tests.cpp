#include "minivectordb/store.hpp"
#include "minivectordb/profile.hpp"
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <map>
#include <limits>
#include <unistd.h>
#include <array>
#include <fstream>
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
using namespace minivectordb;
#define CHECK(x) do {if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)

template<class F> void rejects(F f) {bool caught=false; try {f();}catch(const std::exception&){caught=true;}CHECK(caught);}
using Page=std::array<unsigned char,4096>;
std::uint64_t le(const Page& p,unsigned o,unsigned n=8) {std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(p[o+i])<<(8*i);return v;}
void set(Page& p,unsigned o,std::uint64_t v,unsigned n=8) {for(unsigned i=0;i<n;++i)p[o+i]=static_cast<unsigned char>(v>>(8*i));}
Page read(std::fstream& f,std::uint64_t id) {Page p{};f.seekg(id*4096);f.read(reinterpret_cast<char*>(p.data()),4096);CHECK(f.good());return p;}
void write(std::fstream& f,std::uint64_t id,Page p,unsigned crc_offset=24) {
 set(p,crc_offset,0,4);std::uint32_t crc=~0U;for(auto c:p){crc^=c;for(int i=0;i<8;++i)crc=(crc>>1)^((crc&1)?0xedb88320U:0);}
 set(p,crc_offset,~crc,4);f.seekp(id*4096);f.write(reinterpret_cast<char*>(p.data()),4096);CHECK(f.good());
}
void boundaries(const std::filesystem::path& path) {
 std::vector<Store::Mutation> rows={{INT64_MIN,std::string(4056,'a')},{-1,std::string()},{0,std::string(4057,'b')},{1,std::string(9000,'c')},{INT64_MAX,"last"}};
 {Store s(path,262144,true);s.begin();s.apply_batch_in(UINT64_MAX,rows);s.commit();
  for(auto& r:rows)CHECK(s.get_in(UINT64_MAX,r.key)==r.value);
  s.begin();auto save=s.savepoint();s.apply_batch_in(UINT64_MAX,std::vector<Store::Mutation>{{-1,"changed"},{0,std::nullopt}});
  CHECK(!s.get_in(UINT64_MAX,0));s.restore(save);CHECK(s.get_in(UINT64_MAX,-1)=="");
  unsigned visited=0;s.scan_in(UINT64_MAX,[&](auto key,auto value){CHECK(value==*rows[visited].value);++visited;auto copy=std::string(value);
   s.apply_batch_in(UINT64_MAX,std::vector<Store::Mutation>{{key,"updated"}});
   unsigned inner=0;s.scan_in(UINT64_MAX,[&](auto,auto){++inner;});CHECK(inner==5);CHECK(value==copy);
  });CHECK(visited==5);s.rollback();}
 {Store s(path,0,false);for(auto& r:rows)CHECK(s.get_in(UINT64_MAX,r.key)==r.value);s.begin();s.put(10,"overflow in03");s.commit();}
 {Store s(path,0,true);CHECK(s.get(10)=="overflow in03");}
}
void corruption(const std::filesystem::path& dir) {
 for(unsigned kind=0;kind<17;++kind) {
  auto path=dir/("corrupt"+std::to_string(kind));
  {Store s(path,0,true);s.begin();s.apply_batch_in(0,std::vector<Store::Mutation>{{0,"a"},{1,"b"}});s.commit();}
  {
   std::fstream f(path,std::ios::in|std::ios::out|std::ios::binary);auto p=read(f,2);auto leaf=read(f,3);
   CHECK(le(p,8,4)==3 && le(leaf,12,4)==15);
   switch(kind) {
    case 0:set(p,12,0,4);break;
    case 1:set(p,12,509,4);break;
    case 2:set(p,32,4096,4);break;
    case 3:set(p,44,2,4);break;
    case 4:set(p,40,32,4);break;
    case 5:set(p,8,2,4);break;
    case 6:set(p,16,2);break;
    case 7:set(leaf,32,(1ULL<<63)|(2ULL<<9)|508);break;
    case 8:set(leaf,32,(1ULL<<63)|(2ULL<<9)|2);break;
    case 9:set(leaf,32,(1ULL<<63)|(3ULL<<9));break;
    case 10:set(leaf,32,(1ULL<<63)|(511ULL<<9));break;
    case 11:set(leaf,32,1ULL<<63);break;
    case 12:{auto parent=read(f,4);set(parent,32,le(leaf,32));write(f,4,parent);break;}
    case 13:{auto root=read(f,0);root[7]='2';write(f,0,root,32);break;}
    case 14:p[7]='2';break;
    case 15:{auto root=read(f,1);set(root,16,le(leaf,32));write(f,1,root,32);auto other=read(f,0);set(other,16,le(leaf,32));write(f,0,other,32);break;}
    case 16:set(p,32,31,4);break;
   }
   write(f,2,p);write(f,3,leaf);
  }
  rejects([&]{Store s(path,262144,true);(void)s.get(0);});
 }
}
void compatibility(const std::filesystem::path& dir) {
 auto legacy=dir/"legacy-reopen";
 {Store s(legacy);s.begin();s.put(1,"old");s.commit();}
 {Store s(legacy,0,true);s.begin();s.apply_batch_in(0,std::vector<Store::Mutation>{{2,"new"}});s.commit();CHECK(s.get(1)=="old");}
 {std::fstream f(legacy,std::ios::in|std::ios::out|std::ios::binary);CHECK(read(f,0)[7]=='2' && read(f,1)[7]=='2');}
 auto fallback=dir/"fallback";
 {Store s(fallback,0,true);s.begin();s.apply_batch_in(0,std::vector<Store::Mutation>{{1,"first"}});s.commit();s.begin();s.apply_batch_in(0,std::vector<Store::Mutation>{{1,"second"}});s.commit();}
 {std::fstream f(fallback,std::ios::in|std::ios::out|std::ios::binary);auto p=read(f,0);p[100]^=1;f.seekp(0);f.write(reinterpret_cast<char*>(p.data()),4096);}
 {Store s(fallback);CHECK(s.get(1)=="first");}
}
void failure(const std::filesystem::path& dir,unsigned pages) {
 auto path=dir/("failure"+std::to_string(pages));auto pid=fork();CHECK(pid>=0);
 if(!pid) {try {
  Store s(path,262144,true);s.begin();s.apply_batch_in(0,std::vector<Store::Mutation>{{0,"old"}});s.commit();s.begin();
  struct rlimit original{};CHECK(getrlimit(RLIMIT_FSIZE,&original)==0);auto limit=original;limit.rlim_cur=std::filesystem::file_size(path)+pages*4096+1000;
  CHECK(signal(SIGXFSZ,SIG_IGN)!=SIG_ERR);CHECK(setrlimit(RLIMIT_FSIZE,&limit)==0);
  rejects([&]{s.apply_batch_in(0,std::vector<Store::Mutation>{{0,"failed"},{1,"new"}});});CHECK(s.get(0)=="old");CHECK(!s.get(1));
  CHECK(setrlimit(RLIMIT_FSIZE,&original)==0);s.apply_batch_in(0,std::vector<Store::Mutation>{{0,"retry"},{1,"new"}});s.commit();_exit(0);
 }catch(...){_exit(1);}}
 int status=0;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);Store s(path);CHECK(s.get(0)=="retry" && s.get(1)=="new");
}


void singleton_fallback(const std::filesystem::path& dir) {
 // A singleton has no density benefit, regardless of its value length.
 for(auto length:{0U,16U,3000U,4056U,4057U}) {
  auto path=dir/("singleton-"+std::to_string(length));
  {Store s(path,0,true);s.begin();
   for(int key=0;key<3;++key)s.apply_batch_in(0,std::vector<Store::Mutation>{{key,std::string(length,'x')}});
   s.commit();}
  Store s(path,0,false);profile::set_enabled(true);profile::reset();unsigned count=0;
  s.scan([&](auto,auto value){CHECK(value==std::string(length,'x'));++count;});
  const auto metrics=profile::snapshot();profile::set_enabled(false);
  CHECK(count==3);CHECK(metrics.page_read_packed.calls==0);CHECK(metrics.packed_cache_hit.calls==0);
  CHECK(metrics.page_read_overflow.calls==3);
 }
 // Different large rows that cannot coexist must also fall back within one batch.
 auto wide=dir/"singleton-wide-batch";
 {Store s(wide,0,true);s.begin();s.apply_batch_in(0,std::vector<Store::Mutation>{{0,std::string(3000,'a')},{1,std::string(3000,'b')}});s.commit();
  profile::set_enabled(true);profile::reset();s.scan([&](auto k,auto v){CHECK(v==std::string(3000,k?'b':'a'));});
  auto m=profile::snapshot();profile::set_enabled(false);CHECK(m.page_read_packed.calls==0 && m.packed_cache_hit.calls==0);}
 // Large + small coexist in exactly one packed page; one small tail is ordinary.
 auto mixed=dir/"singleton-tail";
 {Store s(mixed,0,true);s.begin();s.apply_batch_in(0,std::vector<Store::Mutation>{{0,std::string(4000,'a')},{1,std::string(48,'b')},{2,"tail"}});s.commit();
  profile::set_enabled(true);profile::reset();unsigned count=0;
  s.scan([&](auto k,auto v){CHECK(v==(k==0?std::string(4000,'a'):k==1?std::string(48,'b'):"tail"));++count;});
  auto m=profile::snapshot();profile::set_enabled(false);CHECK(count==3);CHECK(m.page_read_packed.calls==2 && m.packed_cache_hit.calls==1);CHECK(m.page_read_overflow.calls==1);
 }
 // A CRC-valid historical format03 singleton remains readable after the write-policy change.
 auto historical=dir/"historical-singleton";
 {Store s(historical,0,true);s.begin();s.put(0,"old");s.commit();}
 {std::fstream f(historical,std::ios::in|std::ios::out|std::ios::binary);auto page=read(f,2);page.fill(0);
  const char magic[]="MVDBPG03";std::copy(magic,magic+8,page.begin());set(page,8,3,4);set(page,12,1,4);set(page,32,4093,4);set(page,36,3,4);
  page[4093]='o';page[4094]='l';page[4095]='d';write(f,2,page);
  auto leaf=read(f,3);set(leaf,32,(1ULL<<63)|(2ULL<<9));write(f,3,leaf);
 }
 {Store s(historical,0,false);CHECK(s.get(0)=="old");unsigned count=0;s.scan([&](auto,auto v){CHECK(v=="old");++count;});CHECK(count==1);}
 {Store s(historical,0,true);s.begin();s.apply_batch_in(0,std::vector<Store::Mutation>{{1,"new"}});s.commit();CHECK(s.get(0)=="old");}
 {Store s(historical);CHECK(s.get(0)=="old" && s.get(1)=="new");}
}
void eviction(const std::filesystem::path& path) {
 std::map<std::int64_t,std::string> expected;
 {Store s(path,0,true);s.begin();
  for(int batch=0;batch<4;++batch) {
   std::vector<Store::Mutation> rows;
   for(int i=127;i>=0;--i){auto key=i*4+batch;auto value=std::string(i%31,'a'+batch);rows.push_back({key,value});expected[key]=value;}
   s.apply_batch_in(0,rows);
  }
  s.commit();}
 std::uint64_t uncached=0, full=0;
 for(auto budget:{0UL,4104UL,262144UL}) {
  Store s(path,0,false,budget);profile::set_enabled(true);profile::reset();auto it=expected.begin();
  s.scan([&](auto key,auto value){CHECK(it!=expected.end());CHECK(key==it->first && value==it->second);++it;});CHECK(it==expected.end());
  const auto measured=profile::snapshot();profile::set_enabled(false);
  CHECK(measured.page_read_packed.calls==expected.size());
  if(!budget) {uncached=measured.pread.calls;CHECK(measured.packed_cache_hit.calls==0);}
  if(budget==4104)CHECK(measured.packed_cache_hit.calls==0); // Four interleaved pages evict each other.
  if(budget==262144) {full=measured.pread.calls;CHECK(measured.packed_cache_hit.calls==508);}
 }
 CHECK(full+500<uncached);
}
void overflow_tag(const std::filesystem::path& path) {
 {Store s(path,0,true);s.begin();s.put(0,std::string(9000,'x'));s.commit();}
 {std::fstream f(path,std::ios::in|std::ios::out|std::ios::binary);auto p=read(f,4);CHECK(le(p,8,4)==2);set(p,16,(1ULL<<63)|(2ULL<<9));write(f,4,p);}
 rejects([&]{Store s(path);(void)s.get(0);});
}
void crash(const std::filesystem::path& path) {
 auto pid=fork();CHECK(pid>=0);if(!pid){try {Store s(path,0,true);s.begin();s.apply_batch_in(0,std::vector<Store::Mutation>{{0,"durable"}});s.commit();s.begin();s.apply_batch_in(0,std::vector<Store::Mutation>{{0,"uncommitted"}});raise(SIGKILL);}catch(...){_exit(1);}}
 int status=0;CHECK(waitpid(pid,&status,0)==pid && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);Store s(path);CHECK(s.get(0)=="durable");
}
int main() {try {
 const char* base=getenv("MVDB_TEST_DIR"); CHECK(base); std::filesystem::create_directories(base);
 auto pattern=(std::filesystem::path(base)/"packed-XXXXXX").string(); CHECK(mkdtemp(pattern.data())); std::filesystem::path dir(pattern);
 std::vector<Store::Mutation> rows; for(int i=0;i<128;++i)rows.push_back({i,std::string(16,'a'+i%26)});
 for(bool packed:{false,true}) {Store s(dir/(packed?"packed":"legacy"),0,packed); s.begin(); s.apply_batch_in(0,rows); s.commit(); for(auto& r:rows)CHECK(s.get(r.key)==r.value);}
 CHECK(std::filesystem::file_size(dir/"packed")<std::filesystem::file_size(dir/"legacy")/2);
 for(auto budget:{0UL,1UL,4104UL,262144UL}) {Store s(dir/"packed",0,false,budget); unsigned count=0; profile::set_enabled(true); profile::reset(); s.scan([&](auto k,auto v){CHECK(v==*rows[k].value); ++count;}); auto m=profile::snapshot(); profile::set_enabled(false); CHECK(count==128); if(budget==262144)CHECK(m.pread.calls<25); if(budget==0)CHECK(m.pread.calls>=128);}
 singleton_fallback(dir);boundaries(dir/"boundaries");corruption(dir);compatibility(dir);failure(dir,0);failure(dir,1);failure(dir,8);crash(dir/"crash");eviction(dir/"eviction");overflow_tag(dir/"overflow-tag");
 std::filesystem::remove_all(dir); std::cout<<"packed storage tests passed\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
