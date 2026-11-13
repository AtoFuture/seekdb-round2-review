#include "minivectordb/store.hpp"
#include "minivectordb/database.hpp"
#include "minivectordb/profile.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <unistd.h>
#include <map>
#include <vector>
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
  rejects([&]{Store s(path,262144,true,0,1048576);(void)s.get(0);});
 }
}

using Map=std::map<std::int64_t,std::string>;
Map snapshot(Store& s) {Map out;s.scan([&](auto k,auto v){CHECK(out.emplace(k,v).second);});return out;}
void budgets(const std::filesystem::path& dir) {
 for(bool packed:{false,true})for(auto budget:{0UL,1UL,4096UL,16384UL,20000UL,1048576UL}) {
  auto path=dir/(std::to_string(packed)+"-"+std::to_string(budget));
  Map expected={{-1,""},{0,"zero"},{1,"one"},{2,std::string(9000,'x')}};
  {Store s(path,1048576,packed,0,budget);CHECK(s.data_cache_stats().capacity_bytes==0);
   s.begin();std::vector<Store::Mutation> batch;for(auto& [k,v]:expected)batch.push_back({k,v});s.apply_batch_in(0,batch);s.commit();
   CHECK(s.data_cache_stats().capacity_bytes==0); // Writes never admit data.
   CHECK(snapshot(s)==expected);CHECK(s.data_cache_stats().capacity_bytes<=budget);
   if(budget<20000)CHECK(s.data_cache_stats().capacity_bytes==0);
   s.reset_data_cache_stats();s.reset_cache_stats();profile::set_enabled(true);profile::reset();
   CHECK(snapshot(s)==expected);
   for(auto& [k,v]:expected)CHECK(s.get(k)==v);
   if(budget==1048576) {CHECK(profile::snapshot().pread.calls==0);CHECK(profile::snapshot().page_checksum.calls==0);CHECK(s.data_cache_stats().misses==0);CHECK(s.data_cache_stats().hits>0);}
   if(budget<20000) {CHECK(s.data_cache_stats().hits==0);CHECK(s.data_cache_stats().misses==0);CHECK(profile::snapshot().pread.calls>0);}
   profile::set_enabled(false);
   auto before=s.data_cache_stats();auto index=s.cache_stats();s.reset_data_cache_stats();auto after=s.data_cache_stats();
   CHECK(after.capacity_bytes==before.capacity_bytes && after.resident_pages==before.resident_pages);
   CHECK(after.hits==0 && after.misses==0 && after.evictions==0);CHECK(s.cache_stats().hits==index.hits);
   s.begin();auto save=s.savepoint();s.put(1,"changed");CHECK(s.get(1)=="changed");s.restore(save);CHECK(s.get(1)=="one");
   unsigned visited=0;s.scan([&](auto k,auto v){CHECK(v==expected.at(k));auto owned=std::string(v);++visited;
    s.put(k,"new");auto nested=snapshot(s);CHECK(nested.at(k)=="new");CHECK(v==owned);
   });CHECK(visited==expected.size());s.rollback();CHECK(snapshot(s)==expected);
  }
  {Store s(path,1048576,packed,0,budget);CHECK(s.data_cache_stats().capacity_bytes==0);CHECK(snapshot(s)==expected);}
 }
}
void recency(const std::filesystem::path& path) {
 Store s(path,1048576,true,0,20000);s.begin();for(int i=0;i<8;++i)s.put(i,std::to_string(i));s.commit();
 for(int i=0;i<4;++i)CHECK(s.get(i)==std::to_string(i));
 CHECK(s.data_cache_stats().resident_pages==4);CHECK(s.data_cache_stats().evictions==0);
 CHECK(s.get(0)=="0");CHECK(s.get(4)=="4");s.reset_data_cache_stats();
 CHECK(s.get(0)=="0");CHECK(s.data_cache_stats().hits==1);CHECK(s.data_cache_stats().misses==0);
 CHECK(s.get(1)=="1");CHECK(s.data_cache_stats().misses==1);CHECK(s.data_cache_stats().evictions==1);
 Map expected;for(int i=0;i<8;++i)expected[i]=std::to_string(i);
 for(int pass=0;pass<5;++pass)CHECK(snapshot(s)==expected);
 CHECK(s.data_cache_stats().resident_pages==4);CHECK(s.data_cache_stats().capacity_bytes<=20000);
}
void isolation(const std::filesystem::path& dir) {
 Store a(dir/"isolation-a",1048576,true,0,1048576),b(dir/"isolation-b",1048576,true,0,1048576);
 a.begin();a.put(0,"a");a.commit();b.begin();b.put(0,"b");b.commit();
 CHECK(a.get(0)=="a");CHECK(b.data_cache_stats().capacity_bytes==0);CHECK(b.get(0)=="b");
 CHECK(a.get(0)=="a");CHECK(b.get(0)=="b");CHECK(a.data_cache_stats().hits==1 && b.data_cache_stats().hits==1);
}
void cached_bad_reference(const std::filesystem::path& dir) {
 for(unsigned kind=0;kind<3;++kind) {
  auto path=dir/("bad-hit"+std::to_string(kind));
  {Store s(path,0,true);s.begin();s.apply_batch_in(0,std::vector<Store::Mutation>{{0,"a"},{1,"b"}});s.commit();}
  {std::fstream f(path,std::ios::in|std::ios::out|std::ios::binary);auto leaf=read(f,3);
   // All references pass physical range checks; second read must still enforce type/slot.
   set(leaf,40,kind==0?2:((1ULL<<63)|(2ULL<<9)|2));
   if(kind==2) {auto data=read(f,2);set(data,8,2,4);set(data,12,1,4);set(data,16,0);data[32]='a';write(f,2,data);
    set(leaf,32,2);set(leaf,40,(1ULL<<63)|(2ULL<<9));}
   write(f,3,leaf);
  }
  Store s(path,1048576,true,0,1048576);CHECK(s.get(0)=="a");s.reset_data_cache_stats();
  profile::set_enabled(true);profile::reset();rejects([&]{(void)s.get(1);});
  CHECK(profile::snapshot().pread.calls==0);CHECK(profile::snapshot().page_checksum.calls==0);profile::set_enabled(false);
  CHECK(s.data_cache_stats().resident_pages==1);CHECK(s.data_cache_stats().misses==0);CHECK(s.get(0)=="a");
 }
}
void overflow_corruption(const std::filesystem::path& dir) {
 for(unsigned kind=0;kind<5;++kind) {
  auto path=dir/("overflow-corrupt"+std::to_string(kind));
  {Store s(path,1048576,false,0,1048576);s.begin();s.put(0,"valid");s.commit();CHECK(s.get(0)=="valid");}
  {std::fstream f(path,std::ios::in|std::ios::out|std::ios::binary);auto page=read(f,2);
   if(kind==0)set(page,12,4065,4);
   if(kind==1)set(page,16,2);
   if(kind==2)set(page,16,(1ULL<<63)|(2ULL<<9));
   if(kind==3)set(page,8,1,4);
   write(f,2,page);
   if(kind==4) {page=read(f,2);page[4095]^=1;f.seekp(2*4096);f.write(reinterpret_cast<char*>(page.data()),4096);CHECK(f.good());}
  }
  Store s(path,1048576,false,0,1048576);CHECK(s.data_cache_stats().capacity_bytes==0);
  for(int repeat=0;repeat<2;++repeat) {rejects([&]{(void)s.get(0);});CHECK(s.data_cache_stats().resident_pages==0);}
 }
}
void failed_write(const std::filesystem::path& dir,unsigned pages) {
 auto path=dir/("failed"+std::to_string(pages));auto pid=fork();CHECK(pid>=0);
 if(!pid) {try {
  Store s(path,1048576,true,0,1048576);s.begin();s.put(0,"old");s.commit();CHECK(s.get(0)=="old");s.begin();
  struct rlimit original{};CHECK(getrlimit(RLIMIT_FSIZE,&original)==0);auto limit=original;limit.rlim_cur=std::filesystem::file_size(path)+pages*4096+1000;
  CHECK(signal(SIGXFSZ,SIG_IGN)!=SIG_ERR);CHECK(setrlimit(RLIMIT_FSIZE,&limit)==0);
  rejects([&]{s.apply_batch_in(0,std::vector<Store::Mutation>{{0,"failed"},{1,"new"}});});CHECK(s.get(0)=="old");CHECK(!s.get(1));
  CHECK(std::filesystem::file_size(path)%4096==1000); // A real partial write occurred.
  CHECK(setrlimit(RLIMIT_FSIZE,&original)==0);s.apply_batch_in(0,std::vector<Store::Mutation>{{0,"retry"},{1,"new"}});
  CHECK(s.get(0)=="retry" && s.get(1)=="new");s.commit();_exit(0);
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';_exit(1);}}
 int status=0;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
 Store s(path,1048576,true,0,1048576);CHECK(s.get(0)=="retry" && s.get(1)=="new");
}
void crash(const std::filesystem::path& path) {
 auto pid=fork();CHECK(pid>=0);if(!pid){try {
  Store s(path,1048576,true,0,1048576);s.begin();s.apply_batch_in(0,std::vector<Store::Mutation>{{0,"durable"},{1,"also durable"}});s.commit();CHECK(s.get(0)=="durable");
  s.begin();s.apply_batch_in(0,std::vector<Store::Mutation>{{0,"uncommitted"},{1,"changed"}});CHECK(s.get(0)=="uncommitted");raise(SIGKILL);
 }catch(...){_exit(1);}}
 int status=0;CHECK(waitpid(pid,&status,0)==pid && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
 Store s(path,1048576,true,0,1048576);CHECK(s.data_cache_stats().capacity_bytes==0);CHECK(s.get(0)=="durable" && s.get(1)=="also durable");
}
void phase_growth(const std::filesystem::path& path) {
 Store s(path,1048576,true,0,4*1048576,CommitSyncPolicy::ProcessCrash,true,true,true,false,true);
 const Map original{{0,std::string(900,'a')},{1,std::string(900,'b')}};
 s.begin();std::vector<Store::Mutation> changes;for(const auto&[k,v]:original)changes.push_back({k,v});
 s.apply_batch_in(0,changes);CHECK(snapshot(s)==original);
 CHECK(s.data_cache_stats().capacity_bytes>0&&s.data_cache_stats().capacity_bytes<=512*1024);
 auto saved=s.savepoint();s.put(0,"changed");CHECK(s.get(0)=="changed");s.restore(saved);CHECK(snapshot(s)==original);
 s.commit();CHECK(snapshot(s)==original);
 CHECK(s.data_cache_stats().capacity_bytes>512*1024&&s.data_cache_stats().capacity_bytes<=4*1048576);
 s.begin();s.put(1,"uncommitted");CHECK(s.get(1)=="uncommitted");s.rollback();CHECK(snapshot(s)==original);
 s.reset_data_cache_stats();CHECK(snapshot(s)==original);CHECK(s.data_cache_stats().misses==0);
}
void sql_phase_growth(const std::filesystem::path& path) {
 Options options;options.data_cache_bytes=4*1048576;Database db(path,options);
 db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(1000))");
 for(int i=0;i<16;++i)db.execute("INSERT INTO t VALUES("+std::to_string(i)+",'"+std::string(900,'a'+i)+"')");
 CHECK(db.data_cache_stats().capacity_bytes>0&&db.data_cache_stats().capacity_bytes<=512*1024);
 std::vector<Row> rows;db.execute("SELECT id,s FROM t ORDER BY id",{{},[&](const Row&r){rows.push_back(r);}});
 CHECK(rows.size()==16);
 for(int i=0;i<16;++i)CHECK(rows[i]==Row({std::int64_t(i),std::string(900,'a'+i)}));
 CHECK(db.data_cache_stats().capacity_bytes>512*1024&&db.data_cache_stats().capacity_bytes<=4*1048576);
}
int main(){try {
 const char* base=getenv("MVDB_TEST_DIR");CHECK(base);std::filesystem::create_directories(base);
 auto path=(std::filesystem::path(base)/"data-cache-XXXXXX").string();CHECK(mkdtemp(path.data()));const std::filesystem::path dir(path);
 budgets(dir);recency(dir/"recency");isolation(dir);corruption(dir);cached_bad_reference(dir);overflow_corruption(dir);
 for(auto pages:{0U,1U,8U})failed_write(dir,pages);
 crash(dir/"crash");
 phase_growth(dir/"phase-growth");
 sql_phase_growth(dir/"sql-phase-growth");
 std::filesystem::remove_all(dir);std::cout<<"data cache tests passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
