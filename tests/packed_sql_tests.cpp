#include "minivectordb/database.hpp"
#include "minivectordb/row_codec.hpp"
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace minivectordb;
namespace fs=std::filesystem;
bool compressed_paths=false;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__)); } while(false)
Options options(const fs::path& dir,bool selective=true,bool batch=true,bool packed=true,std::size_t cache=262144) {
  Options o; o.sort_directory=dir/"sort"; o.memory_budget_bytes=4096; o.compressed_paths=compressed_paths;
  o.compact_values=false; // This suite explicitly verifies format03/04 compatibility.
  o.selective_decode=selective; o.batch_writes=batch; o.packed_values=packed; o.packed_scan_cache_bytes=cache; return o;
}
std::vector<Row> query(Database& db,const std::string& sql) {
  std::vector<Row> rows; db.execute(sql,{{},[&](const Row& row){rows.push_back(row);}}); return rows;
}
template<class F> void rejects(F f) { bool failed=false; try {f();}catch(const std::exception&){failed=true;} CHECK(failed); }
std::string insert(const std::string& table,int first,int count) {
  std::string sql="INSERT INTO "+table+" VALUES";
  for(int i=first;i<first+count;++i) {
    if(i!=first)sql+=",";
    sql+="("+std::to_string(i)+",["+std::to_string(i%17-8)+","+std::to_string(i%11-5)+"],'row"+std::to_string(i)+"',"+std::to_string(i%3)+")";
  }
  return sql;
}
void schema(Database& db) {
  db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2),s VARCHAR(20000),n BIGINT)");
  db.execute("CREATE TABLE u(id BIGINT PRIMARY KEY,v VECTOR(2),s VARCHAR(20000),n BIGINT)");
}
void format03(const fs::path& path) {
  std::ifstream f(path,std::ios::binary); std::string magic(8,'\0'); f.read(magic.data(),8); CHECK(magic==(compressed_paths?"MVDBRT04":"MVDBRT03"));
}
void comparisons(const fs::path& dir) {
  const std::vector<std::string> queries{
    "SELECT * FROM t", "SELECT * FROM u", "SELECT id FROM t WHERE n=1",
    "SELECT id,s,L2_DISTANCE(v,[0,0]) AS d FROM t ORDER BY d LIMIT 19",
    "SELECT id,s,L2_DISTANCE(v,[0,0]) AS d FROM t WHERE n=1 ORDER BY d LIMIT 19",
    "SELECT * FROM t ORDER BY s DESC", "SELECT id FROM t WHERE id=9999"};
  std::vector<std::vector<Row>> expected;
  for(int mode=0;mode<9;++mode) {
    bool packed=mode!=0, selective=packed&&((mode-1)&1), batch=!packed||((mode-1)&2);
    std::size_t cache=packed&&((mode-1)&4)?0:262144;
    auto path=dir/("compare-"+std::to_string(mode)); auto o=options(dir,selective,batch,packed,cache);
    {
      Database db(path,o); schema(db); db.execute("BEGIN"); CHECK(db.execute(insert("t",0,300)).affected_rows==300); db.execute(insert("u",0,150));
      CHECK(db.execute("UPDATE t SET n=7 WHERE n=0").affected_rows==100);
      CHECK(db.execute("DELETE FROM u WHERE n=2").affected_rows==50);
      db.execute("INSERT INTO t VALUES(9999,[1,2],'"+std::string(12000,'z')+"',7)"); db.execute("COMMIT");
      auto before=query(db,"SELECT * FROM t");
      db.execute("BEGIN"); db.execute("UPDATE t SET n=9"); db.execute("DELETE FROM u"); db.execute("ROLLBACK");
      CHECK(query(db,"SELECT * FROM t")==before); CHECK(query(db,"SELECT * FROM u").size()==100);
      db.execute("BEGIN"); db.execute("UPDATE u SET n=8 WHERE id=1");
      rejects([&]{db.execute(insert("t",10000,260)+",(1,[0,0],'duplicate',0)");});
      CHECK(query(db,"SELECT * FROM t")==before); CHECK(db.in_transaction()); db.execute("COMMIT");
      // Independent integer-distance reference, with primary-key tie ordering.
      std::vector<std::pair<int,int>> ranks;
      for(int i=0;i<300;++i)if(i%3==1)ranks.emplace_back((i%17-8)*(i%17-8)+(i%11-5)*(i%11-5),i);
      std::sort(ranks.begin(),ranks.end());
      auto top=query(db,"SELECT id FROM t WHERE n=1 ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 19"); CHECK(top.size()==19);
      for(std::size_t i=0;i<top.size();++i)CHECK(std::get<std::int64_t>(top[i][0])==ranks[i].second);
      for(std::size_t i=0;i<queries.size();++i) { auto actual=query(db,queries[i]); if(mode==0)expected.push_back(actual); else CHECK(actual==expected[i]); }
    }
    if(packed)format03(path);
    { Database db(path,options(dir,false,false,false)); for(std::size_t i=0;i<queries.size();++i)CHECK(query(db,queries[i])==expected[i]); }
  }
}
void corrupt_rows(const fs::path& dir) {
  Schema s{1,"t",{{"id",ColumnType::Bigint,0},{"v",ColumnType::Vector,2},{"s",ColumnType::Varchar,20},{"n",ColumnType::Bigint,0}},0};
  auto good=encode_row(s,{std::int64_t(1),std::vector<float>{1,2},std::string("ok"),std::int64_t(0)}); std::vector<std::string> bad;
  for(auto bits:{0x7f800000u,0xff800000u,0x7fc00001u}) { auto bytes=good; for(unsigned i=0;i<4;++i)bytes[17+i]=static_cast<char>(bits>>(8*i)); bad.push_back(bytes); }
  auto unicode=good; unicode[29]=char(0x80); bad.push_back(unicode);
  auto wrongkey=good; wrongkey[5]=2; bad.push_back(wrongkey); bad.push_back(good+"x");
  for(std::size_t n=0;n<good.size();++n)bad.push_back(good.substr(0,n));
  int index=0;
  for(const auto& bytes:bad) {
    auto path=dir/("bad-"+std::to_string(index++));
    { Database db(path,options(dir)); db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2),s VARCHAR(20),n BIGINT)"); }
    { Store store(path,0,true); store.begin(); std::vector<Store::Mutation> batch{{1,bytes}}; store.apply_batch_in(1,batch); store.commit(); }
    format03(path);
    for(bool selective:{false,true})for(std::size_t cache:{0u,262144u}) {
      Database db(path,options(dir,selective,true,true,cache));
      for(const auto* sql:{"SELECT id FROM t WHERE n=1","SELECT id FROM t WHERE id=1","SELECT id FROM t ORDER BY n","SELECT id FROM t WHERE n=1 ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 1"}) {
        bool rejected=false; try { db.execute(sql); } catch(const Error& e) { rejected=e.code()==ErrorCode::Corruption; } CHECK(rejected);
      }
    }
  }
}
void wait_ok(pid_t pid) { int status=0; CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0); }
void partial_failure(const fs::path& dir) {
  // Calibrate an entire 128-row chunk using identical pre-statement state.
  std::uintmax_t first_chunk=0;
  {
    auto path=dir/"calibrate"; Database db(path,options(dir)); schema(db); db.execute("BEGIN");
    db.execute("INSERT INTO u VALUES(-1,[0,0],'previous',0)"); auto before=fs::file_size(path);
    db.execute(insert("t",0,128)); first_chunk=fs::file_size(path)-before; CHECK(first_chunk>0); db.execute("ROLLBACK");
  }
  auto path=dir/"failure"; auto pid=fork(); CHECK(pid>=0);
  if(pid==0) {
    try {
      Database db(path,options(dir)); schema(db); db.execute("BEGIN");
      db.execute("INSERT INTO u VALUES(-1,[0,0],'previous',0)"); auto before=fs::file_size(path);
      struct rlimit original{}; CHECK(getrlimit(RLIMIT_FSIZE,&original)==0); auto limited=original;
      limited.rlim_cur=before+first_chunk+1000; CHECK(signal(SIGXFSZ,SIG_IGN)!=SIG_ERR); CHECK(setrlimit(RLIMIT_FSIZE,&limited)==0);
      rejects([&]{db.execute(insert("t",0,300));});
      CHECK(fs::file_size(path)>=before+first_chunk); CHECK(query(db,"SELECT * FROM t").empty()); CHECK(query(db,"SELECT * FROM u").size()==1); CHECK(db.in_transaction());
      CHECK(setrlimit(RLIMIT_FSIZE,&original)==0); db.execute("COMMIT"); _exit(0);
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';_exit(1);}
  }
  wait_ok(pid); Database db(path,options(dir)); CHECK(query(db,"SELECT * FROM t").empty());
  CHECK(query(db,"SELECT s FROM u")==std::vector<Row>{Row{std::string("previous")}});
}
void crash(const fs::path& dir) {
  auto path=dir/"crash"; auto pid=fork(); CHECK(pid>=0);
  if(pid==0) {
    try {
      Database db(path,options(dir)); schema(db); db.execute("BEGIN"); db.execute(insert("t",0,300)); db.execute(insert("u",0,150)); db.execute("COMMIT");
      db.execute("BEGIN"); db.execute("UPDATE t SET n=99"); db.execute("DELETE FROM u"); db.execute(insert("t",1000,260));
      _exit(0); // Crash: bypass destruction and rollback.
    }catch(...){_exit(1);}
  }
  wait_ok(pid); Database db(path,options(dir)); auto rows=query(db,"SELECT id,n FROM t"); CHECK(rows.size()==300);
  for(int i=0;i<300;++i)CHECK(rows[static_cast<std::size_t>(i)]==Row({std::int64_t(i),std::int64_t(i%3)}));
  CHECK(query(db,"SELECT * FROM u").size()==150);
}
int main(int argc,char** argv) {
  compressed_paths=argc>1 && std::string(argv[1])=="--compressed";
  try {
    const auto* root=std::getenv("MVDB_TEST_DIR"); CHECK(root); fs::create_directories(root);
    auto name=(fs::path(root)/"packed-sql-XXXXXX").string(); CHECK(mkdtemp(name.data())); fs::path dir(name);
    comparisons(dir); corrupt_rows(dir); partial_failure(dir); crash(dir); fs::remove_all(dir); std::cout<<"PASS packed SQL\n";
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
