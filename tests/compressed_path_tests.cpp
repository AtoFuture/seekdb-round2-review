#include "minivectordb/store.hpp"
#include "../src/storage/page_checksum.hpp"
#include <array>
#include <bit>
#include <csignal>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace minivectordb;
namespace fs=std::filesystem;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
using Rows=std::map<std::int64_t,std::string>;
using Model=std::map<std::uint64_t,Rows>;
Rows rows(Store& db,std::uint64_t space) {
  Rows out;db.scan_in(space,[&](auto key,auto value){CHECK(out.emplace(key,value).second);});return out;
}
void verify(Store& db,const Model& model) {
  for(const auto& [space,expected]:model) {
    CHECK(rows(db,space)==expected);
    for(const auto& [key,value]:expected)CHECK(db.get_in(space,key)==value);
    CHECK(!db.get_in(space,123456789));
  }
}
Store open(const fs::path& path,bool compressed,bool cache,bool packed) {
  return Store(path,cache?65536:0,packed,0,cache?65536:0,CommitSyncPolicy::ProcessCrash,compressed);
}
void differential(const fs::path& dir,bool cache,bool packed) {
  const std::array<std::uint64_t,6> spaces{0,1,256,1ULL<<32,1ULL<<63,UINT64_MAX};
  Model model;for(auto s:spaces)model[s]={};
  std::mt19937_64 rng(20261111);
  const auto a_path=dir/"reference",b_path=dir/"compressed";
  for(int round=0;round<24;++round) {
    auto a=open(a_path,false,cache,packed),b=open(b_path,true,cache,packed);
    a.begin();b.begin();auto ap=a.savepoint(),bp=b.savepoint();
    Model next=model;
    for(auto space:spaces) {
      std::vector<Store::Mutation> batch;
      for(int i=0;i<32;++i) {
        // Many shared prefixes, sign boundaries, and unrelated prefixes.
        const auto key=i==0?INT64_MIN:i==1?INT64_MAX:i<16?std::int64_t(i-8):std::bit_cast<std::int64_t>(rng());
        std::optional<std::string> value;
        if(round%3 || i%2)value=std::string(i%11==0?5000:std::size_t(i*7),char('a'+round%26));
        batch.push_back({key,value});
        if(value)next[space][key]=*value;else next[space].erase(key);
      }
      a.apply_batch_in(space,batch);b.apply_batch_in(space,batch);
    }
    verify(a,next);verify(b,next);
    if(round%4==0) {
      a.restore(ap);b.restore(bp);verify(a,model);verify(b,model);
      a.commit();b.commit();
    } else if(round%4==1) {a.rollback();b.rollback();}
    else {a.commit();b.commit();model=std::move(next);}
  }
  auto a=open(a_path,false,cache,packed),b=open(b_path,true,cache,packed);
  verify(a,model);verify(b,model);
  CHECK(fs::file_size(b_path)<fs::file_size(a_path));
  std::cout<<"model cache="<<cache<<" packed="<<packed<<" old_bytes="<<fs::file_size(a_path)<<" new_bytes="<<fs::file_size(b_path)<<'\n';
}
void legacy(const fs::path& path,bool packed) {
  {auto db=open(path,false,true,packed);db.begin();db.put_in(UINT64_MAX,INT64_MIN,"old");db.commit();}
  {auto db=open(path,true,true,true);CHECK(db.get_in(UINT64_MAX,INT64_MIN)=="old");db.begin();db.put_in(0,INT64_MAX,"new");db.commit();}
  {std::ifstream in(path,std::ios::binary);in.seekg(7);CHECK(in.get()==(packed?'3':'2'));}
  {auto db=open(path,false,false,false);CHECK(db.get_in(0,INT64_MAX)=="new");CHECK(db.get_in(UINT64_MAX,INT64_MIN)=="old");}
}
void callback(const fs::path& path) {
  auto db=open(path,true,true,true);db.begin();
  for(int i=0;i<40;++i){db.put_in(1,i,"old");db.put_in(2,i,"old");}db.commit();db.begin();
  int count=0;
  db.scan_equal_in(1,2,[&](auto key,auto a,auto b){CHECK(a=="old"&&b=="old");++count;db.erase_in(1,key);db.put_in(2,key,"new");});
  CHECK(count==40);CHECK(rows(db,1).empty());db.rollback();CHECK(rows(db,1).size()==40);
}
void failure(const fs::path& path) {
  auto child=fork();CHECK(child>=0);
  if(!child) {
    try {
      auto db=open(path,true,true,true);db.begin();db.put_in(1,-1,"old");db.commit();db.begin();
      struct rlimit before{};CHECK(getrlimit(RLIMIT_FSIZE,&before)==0);auto limit=before;
      limit.rlim_cur=fs::file_size(path)+2*4096+1000;
      CHECK(signal(SIGXFSZ,SIG_IGN)!=SIG_ERR);CHECK(setrlimit(RLIMIT_FSIZE,&limit)==0);
      std::vector<Store::Mutation> changes;
      for(std::int64_t i=0;i<64;++i)changes.push_back({(i<<40)|i,"value"});
      bool failed=false;try{db.apply_batch_in(1,changes);}catch(...){failed=true;}
      CHECK(failed);CHECK(setrlimit(RLIMIT_FSIZE,&before)==0);
      CHECK(rows(db,1)==Rows({{-1,"old"}}));db.commit();_exit(0);
    }catch(...){_exit(1);}
  }
  int status=0;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  auto db=open(path,true,false,true);CHECK(rows(db,1)==Rows({{-1,"old"}}));
}
void malformed(const fs::path& dir) {
  for(bool packed_nodes:{false,true})for(int variant=0;variant<(packed_nodes?5:3);++variant) {
    auto path=dir/(std::to_string(packed_nodes)+std::to_string(variant));
    {Store db(path,0,true,0,0,CommitSyncPolicy::ProcessCrash,true,packed_nodes);db.begin();db.put_in(5,7,"value");db.commit();}
    std::fstream io(path,std::ios::binary|std::ios::in|std::ios::out);
    std::array<unsigned char,4096> root{},page{};io.seekg(4096);io.read(reinterpret_cast<char*>(root.data()),4096);
    std::uint64_t id=0;for(unsigned i=0;i<8;++i)id|=std::uint64_t(root[16+i])<<(8*i);
    const auto physical=packed_nodes?(id&~(1ULL<<62))>>9:id;
    io.seekg(static_cast<std::streamoff>(physical*4096));io.read(reinterpret_cast<char*>(page.data()),4096);
    std::size_t start=0;
    if(packed_nodes)for(unsigned i=0;i<4;++i)start|=std::size_t(page[32+(id&511)*8+i])<<(8*i);
    if(variant==0)page[packed_nodes?start+1:12]=16; // impossible depth
    if(variant==1)page[packed_nodes?start+2+15:32+2048+15]=1; // noncanonical prefix suffix
    if(variant==2)for(unsigned i=0;i<8;++i)page[(packed_nodes?start+50:32+7*8)+i]=static_cast<unsigned char>(id>>(8*i)); // cycle/wrong reference kind
    if(variant==3)page[start+18]|=1; // bitmap claims an absent child payload
    if(variant==4)page[start]=2; // unknown node version
    const auto sum=detail::page_checksum(page,24);for(unsigned i=0;i<4;++i)page[24+i]=static_cast<unsigned char>(sum>>(8*i));
    io.seekp(static_cast<std::streamoff>(physical*4096));io.write(reinterpret_cast<char*>(page.data()),4096);io.close();
    bool rejected=false;try{auto db=open(path,true,false,true);(void)db.get_in(5,7);}catch(...){rejected=true;}CHECK(rejected);
  }
  // Two leaves and their parent share a node page. A persisted parent may
  // reference earlier slots only; reject both a self-edge and a forward edge.
  for(unsigned forward:{0u,1u}) {
    const auto path=dir/("edge"+std::to_string(forward));
    {auto db=open(path,true,false,true);db.begin();db.apply_batch_in(5,std::vector<Store::Mutation>{{1,"a"},{257,"b"}});db.commit();}
    std::fstream io(path,std::ios::binary|std::ios::in|std::ios::out);
    std::array<unsigned char,4096> root{},page{};io.seekg(4096);io.read(reinterpret_cast<char*>(root.data()),4096);
    std::uint64_t ref=0;for(unsigned i=0;i<8;++i)ref|=std::uint64_t(root[16+i])<<(8*i);
    CHECK(ref&(1ULL<<62));const auto id=(ref&~(1ULL<<62))>>9;
    io.seekg(static_cast<std::streamoff>(id*4096));io.read(reinterpret_cast<char*>(page.data()),4096);
    std::size_t start=0;for(unsigned i=0;i<4;++i)start|=std::size_t(page[32+(ref&511)*8+i])<<(8*i);
    CHECK(page[start+1]<15);
    for(unsigned i=0;i<8;++i)page[start+50+i]=static_cast<unsigned char>((ref+forward)>>(8*i));
    auto sum=detail::page_checksum(page,24);for(unsigned i=0;i<4;++i)page[24+i]=static_cast<unsigned char>(sum>>(8*i));
    io.seekp(static_cast<std::streamoff>(id*4096));io.write(reinterpret_cast<char*>(page.data()),4096);io.close();
    bool failed=false;try{auto db=open(path,true,true,true);}catch(...){failed=true;}CHECK(failed);
  }
}
int main() {
  try {
    const auto* env=std::getenv("MVDB_TEST_DIR");CHECK(env);fs::create_directories(env);
    auto name=(fs::path(env)/"compressed-path-XXXXXX").string();CHECK(mkdtemp(name.data()));fs::path dir=name;
    for(bool cache:{false,true})for(bool packed:{false,true}) {auto p=dir/(std::to_string(cache)+std::to_string(packed));fs::create_directory(p);differential(p,cache,packed);}
    legacy(dir/"legacy02",false);legacy(dir/"legacy03",true);callback(dir/"callback");failure(dir/"failure");
    fs::create_directory(dir/"bad");malformed(dir/"bad");fs::remove_all(dir);
    std::cout<<"PASS compressed paths\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
