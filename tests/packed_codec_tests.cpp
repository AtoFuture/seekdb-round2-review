#include "minivectordb/store.hpp"
#include "minivectordb/database.hpp"
#include "../src/storage/packed_codec.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <map>
#include <cmath>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
using namespace minivectordb;
namespace fs=std::filesystem;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
void codec() {
  std::string encoded;
  CHECK(detail::pack_bytes(std::string(130,'x'),encoded));
  CHECK(encoded==std::string("\x82\0\0\0\xffx",6));
  CHECK(detail::unpack_bytes(encoded)==std::string(130,'x'));
  std::mt19937 random(0x822a941u);
  for(unsigned n:{0u,1u,63u,64u,65u,127u,128u,129u,130u,131u,255u,256u,257u,2098u,4056u,4064u,4065u})
    for(unsigned mode=0;mode<5;++mode)for(unsigned trial=0;trial<12;++trial) {
      std::string input;
      for(unsigned i=0;i<n;++i)input.push_back(static_cast<char>(mode==0?trial:mode==1?i:mode==2?random():mode==3?i/129:((i%137)<130?0:random())));
      if(detail::pack_bytes(input,encoded)) {
        CHECK(encoded.size()+8<=input.size());CHECK(detail::unpack_bytes(encoded)==input);
        for(std::size_t size=0;size<encoded.size();++size) {
          bool rejected=false;try{(void)detail::unpack_bytes(std::string_view(encoded).substr(0,size));}catch(...){rejected=true;}CHECK(rejected);
        }
        auto trailing=encoded;trailing.push_back(128);trailing.push_back('z');
        bool rejected=false;try{(void)detail::unpack_bytes(trailing);}catch(...){rejected=true;}CHECK(rejected);
      }
    }
  for(unsigned byte=0;byte<256;++byte){const std::string input(4064,char(byte));CHECK(detail::pack_bytes(input,encoded));CHECK(detail::unpack_bytes(encoded)==input);}
  for(const std::string invalid:{std::string("\xff\xff\xff\xff",4),std::string("\x40\0\0\0\xffx",6),std::string("\x40\0\0\0\x7f",5)}) {
    bool rejected=false;try{(void)detail::unpack_bytes(invalid);}catch(...){rejected=true;}CHECK(rejected);
  }
}
void store_trace(const fs::path& path) {
  std::map<std::int64_t,std::string> model;
  std::mt19937 random(0x82414u);
  for(unsigned phase=0;phase<4;++phase) {
    Store store(path,phase%2?65536:0,true,32768,phase%2?65536:0,CommitSyncPolicy::ProcessCrash,true,true,phase==0);
    auto check=[&](){std::map<std::int64_t,std::string> actual;store.scan_in(7,[&](auto k,auto v){actual.emplace(k,v);});CHECK(actual==model);for(const auto&[k,v]:model)CHECK(store.get_in(7,k)==v);};
    check();store.begin();const auto saved=store.savepoint();const auto before=model;
    for(unsigned batch=0;batch<4;++batch) {
      std::vector<Store::Mutation> changes;
      for(unsigned i=0;i<100;++i) {
        const auto key=std::int64_t(batch*100+i)-200;
        std::string value;
        const unsigned count=(i%11==0?5000:i%7==0?4056:64+i*13);
        for(unsigned j=0;j<count;++j)value.push_back(char(i%3==0?random():i%3==1?j/130:i));
        if(phase==2&&i%2==0){changes.push_back({key,std::nullopt});model.erase(key);}
        else{changes.push_back({key,value});model[key]=value;}
      }
      store.apply_batch_in(7,changes);
    }
    check();
    if(phase==1){store.restore(saved);model=before;check();store.rollback();}
    else store.commit();
  }
  {Store reopened(path);for(const auto&[k,v]:model)CHECK(reopened.get_in(7,k)==v);}
  std::ifstream file(path,std::ios::binary);std::array<char,8> magic{};file.read(magic.data(),8);CHECK(std::string(magic.data(),8)=="MVDBRT05");
  // Reopening an old file with the new option must never migrate its format.
  const auto old=path.string()+"-old";
  {Store s(old,0,true,0,0,CommitSyncPolicy::ProcessCrash,true,true,false);s.begin();s.put(1,"old");s.commit();}
  {Store s(old,0,true,0,0,CommitSyncPolicy::ProcessCrash,true,true,true);s.begin();const std::array<Store::Mutation,2> rows{{{2,std::string(1000,'a')},{3,std::string(1000,'b')}}};s.apply_batch_in(0,rows);s.commit();CHECK(s.get(1)=="old");}
  std::ifstream old_file(old,std::ios::binary);old_file.read(magic.data(),8);CHECK(std::string(magic.data(),8)=="MVDBRT04");
}
void crash_and_corruption(const fs::path& path) {
  const auto child=fork();CHECK(child>=0);
  if(child==0) {
    try {
      Store s(path,0,true,0,0,CommitSyncPolicy::ProcessCrash,true,true,true);
      s.begin();const std::array<Store::Mutation,1> first{{{1,std::string(1000,'a')}}};
      s.apply_batch_in(0,first);s.commit();s.begin();
      const std::array<Store::Mutation,2> pending{{{1,std::string(1000,'b')},{2,std::string(1000,'c')}}};
      s.apply_batch_in(0,pending);kill(getpid(),SIGKILL);_exit(99);
    }catch(...){_exit(98);}
  }
  int status=0;CHECK(waitpid(child,&status,0)==child);CHECK(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
  {Store s(path);CHECK(s.get(1)==std::string(1000,'a'));CHECK(!s.get(2));}
  // Page two is the first, still-live compressed value. Alter its payload
  // without repairing CRC: reopening/get must reject it before use.
  {std::fstream file(path,std::ios::in|std::ios::out|std::ios::binary);file.seekg(3*4096-1);char c;file.get(c);c^=1;file.seekp(3*4096-1);file.put(c);CHECK(file.good());}
  bool rejected=false;try{Store s(path);(void)s.get(1);}catch(...){rejected=true;}CHECK(rejected);
}
void sql_trace(const fs::path& path) {
  Options options;options.compact_values=true;
  std::vector<Row> expected;
  for(int i=0;i<97;++i)expected.push_back({std::int64_t(i),std::string(256,'a'+i%20)+"中😀",std::sqrt(double((i%7)*(i%7)+(i%3)*(i%3)))});
  std::sort(expected.begin(),expected.end(),[](const Row&a,const Row&b){return std::get<double>(a[2])<std::get<double>(b[2])||(std::get<double>(a[2])==std::get<double>(b[2])&&std::get<std::int64_t>(a[0])<std::get<std::int64_t>(b[0]));});expected.resize(10);
  const std::string query="SELECT id,body,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 10";
  auto verify=[&](Database& db){for(int round=0;round<4;++round){std::vector<Row> rows;db.execute(query,{{},[&](const Row&r){rows.push_back(r);}});CHECK(rows==expected);}};
  {
    Database db(path,options);db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2),body VARCHAR(512))");
    std::string sql="INSERT INTO t VALUES ";
    for(int i=0;i<97;++i){if(i)sql+=',';sql+="("+std::to_string(i)+",'["+std::to_string(i%7)+","+std::to_string(i%3)+"]','"+std::string(256,'a'+i%20)+"中😀')";}
    db.execute(sql);verify(db);db.execute("CREATE VECTOR INDEX vi ON t(v)");verify(db);
    db.execute("BEGIN");db.execute("DELETE FROM t");db.execute("ROLLBACK");verify(db);
  }
  Database reopened(path);verify(reopened);
}
int main(){try{codec();const char* env=std::getenv("MVDB_TEST_DIR");CHECK(env);fs::create_directories(env);auto path=(fs::path(env)/"codec-XXXXXX").string();CHECK(mkdtemp(path.data()));store_trace(fs::path(path)/"db");crash_and_corruption(fs::path(path)/"crash");sql_trace(fs::path(path)/"sql");fs::remove_all(path);std::cout<<"PASS bounded byte-codec, malformed inputs, map/SQL oracle, SIGKILL recovery, checksum rejection and old-format preservation\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
