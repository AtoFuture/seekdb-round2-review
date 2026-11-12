#include "minivectordb/database.hpp"
#include "minivectordb/row_codec.hpp"
#include "minivectordb/profile.hpp"
#include <algorithm>
#include <cstdlib>
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace minivectordb;
namespace fs=std::filesystem;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__)); } while(false)
namespace {
Options options(const fs::path& dir,bool packed,bool selective,bool batch,std::size_t cache,std::size_t scan) {
  Options o; o.sort_directory=dir/"sort"; o.memory_budget_bytes=4096;
  o.packed_values=packed; o.selective_decode=selective; o.batch_writes=batch;
  o.data_cache_bytes=cache; o.packed_scan_cache_bytes=scan; return o;
}
std::vector<Row> query(Database& db,const std::string& sql) {
  std::vector<Row> rows; db.execute(sql,{{},[&](const Row& row){rows.push_back(row);}}); return rows;
}
void schema(Database& db) { db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2),s VARCHAR(20000),n BIGINT)"); }
std::string insert(int first,int count) {
  std::string sql="INSERT INTO t VALUES";
  for(int i=first;i<first+count;++i) {
    if(i!=first)sql+=",";
    sql+="("+std::to_string(i)+",["+std::to_string(i%7-3)+","+std::to_string(i%5-2)+"],'row"+std::to_string(i)+"',"+std::to_string(i%3)+")";
  }
  return sql;
}
std::vector<Row> expected_rows() {
  std::vector<Row> rows;
  for(int i=0;i<40;++i)rows.push_back({std::int64_t(i),std::vector<float>{float(i%7-3),float(i%5-2)},std::string("row")+std::to_string(i),std::int64_t(i%3)});
  rows.push_back({std::int64_t(100),std::vector<float>{1,2},std::string(13000,'z'),std::int64_t(1)});
  return rows;
}
void comparisons(const fs::path& dir) {
  const auto expected=expected_rows();
  std::vector<std::pair<int,int>> ranks;
  for(int i=0;i<40;++i)if(i%3==1)ranks.emplace_back((i%7-3)*(i%7-3)+(i%5-2)*(i%5-2),i);
  ranks.emplace_back(5,100); std::sort(ranks.begin(),ranks.end());
  std::vector<Row> top;
  for(int i=0;i<7;++i)top.push_back({std::int64_t(ranks[i].second)});
  int mode=0;
  for(bool packed:{false,true})for(bool selective:{false,true})for(bool batch:{false,true})
  for(std::size_t cache:{0u,4u*1024*1024})for(std::size_t scan:{0u,262144u}) {
    auto path=dir/("mode-"+std::to_string(mode++)); auto o=options(dir,packed,selective,batch,cache,scan);
    {
      Database db(path,o); schema(db); db.execute("BEGIN"); db.execute(insert(0,40));
      db.execute("INSERT INTO t VALUES(100,[1,2],'"+std::string(13000,'z')+"',1)"); db.execute("COMMIT");
      CHECK(query(db,"SELECT * FROM t")==expected);
      profile::reset(); CHECK(query(db,"SELECT * FROM t")==expected);
      const auto first=profile::snapshot(); const auto index_before=db.cache_stats();
      db.reset_data_cache_stats();
      CHECK(db.cache_stats().hits==index_before.hits);
      CHECK(db.cache_stats().misses==index_before.misses);
      CHECK(db.data_cache_stats().hits==0 && db.data_cache_stats().misses==0);
      profile::reset(); CHECK(query(db,"SELECT * FROM t")==expected);
      const auto warm=profile::snapshot(); const auto stats=db.data_cache_stats();
      const auto validated=selective?warm.row_validation.calls:warm.row_decode.calls;
      CHECK(validated==41);
      CHECK(warm.row_validation.calls==first.row_validation.calls);
      CHECK(warm.row_decode.calls==first.row_decode.calls);
      if(cache) { CHECK(stats.hits>0); CHECK(stats.capacity_bytes>0 && stats.capacity_bytes<=cache); CHECK(stats.resident_pages>0); }
      else { CHECK(stats.hits==0 && stats.capacity_bytes==0 && stats.resident_pages==0); }
      CHECK(query(db,"SELECT id FROM t WHERE n=1 ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 7")==top);
      CHECK(query(db,"SELECT id FROM t WHERE n=1 ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 7")==top);
      db.execute("BEGIN"); db.execute("UPDATE t SET n=9 WHERE id=1");
      CHECK(query(db,"SELECT n FROM t WHERE id=1")==std::vector<Row>{Row{std::int64_t(9)}});
      // Preflight rejection retains prior transaction writes; partial-write restoration is tested below.
      bool rejected=false;
      try { db.execute(insert(1000,140)+",(1,[0,0],'duplicate',0)"); }
      catch(const Error& e) { rejected=e.code()==ErrorCode::Constraint; }
      CHECK(rejected && db.in_transaction());
      auto changed=expected; changed[1][3]=std::int64_t(9);
      CHECK(query(db,"SELECT * FROM t")==changed);
      db.execute("DELETE FROM t WHERE id=100"); CHECK(query(db,"SELECT * FROM t").size()==40);
      db.execute("ROLLBACK"); CHECK(query(db,"SELECT * FROM t")==expected);
      db.execute("BEGIN"); db.execute("UPDATE t SET s='committed' WHERE id=2"); db.execute("COMMIT");
      auto committed=expected; committed[2][2]=std::string("committed"); CHECK(query(db,"SELECT * FROM t")==committed);
    }
    { Database db(path,o); auto committed=expected; committed[2][2]=std::string("committed");
      CHECK(query(db,"SELECT * FROM t")==committed); CHECK(query(db,"SELECT * FROM t")==committed); }
  }
}
void partial_statement_failure(const fs::path& dir) {
  int mode=0;
  for(bool packed:{false,true})for(bool batch:{false,true})for(std::size_t cache:{0u,4u*1024*1024}) {
    auto o=options(dir,packed,true,batch,cache,0);
    auto calibration=dir/("calibration-"+std::to_string(mode));
    std::uintmax_t chunk_bytes=0;
    {
      Database db(calibration,o); schema(db); db.execute("BEGIN"); db.execute("INSERT INTO t VALUES(-1,[0,0],'prior',0)");
      auto before=fs::file_size(calibration); db.execute(insert(1000,128)); chunk_bytes=fs::file_size(calibration)-before;
      CHECK(chunk_bytes>0); db.execute("ROLLBACK");
    }
    const auto path=dir/("partial-"+std::to_string(mode++));
    const std::vector<Row> prior{{std::int64_t(-1),std::vector<float>{0,0},std::string("prior"),std::int64_t(0)}};
    const auto pid=fork(); CHECK(pid>=0);
    if(pid==0) {
      try {
        Database db(path,o); schema(db); db.execute("BEGIN"); db.execute("INSERT INTO t VALUES(-1,[0,0],'prior',0)");
        CHECK(query(db,"SELECT * FROM t")==prior);
        const auto before=fs::file_size(path);
        struct rlimit original{}; CHECK(getrlimit(RLIMIT_FSIZE,&original)==0); auto limited=original;
        limited.rlim_cur=before+chunk_bytes+1000; CHECK(signal(SIGXFSZ,SIG_IGN)!=SIG_ERR); CHECK(setrlimit(RLIMIT_FSIZE,&limited)==0);
        bool failed=false; try {db.execute(insert(1000,140));}catch(const std::exception&){failed=true;}
        CHECK(setrlimit(RLIMIT_FSIZE,&original)==0); CHECK(failed);
        CHECK(fs::file_size(path)>=before+chunk_bytes); CHECK(db.in_transaction());
        CHECK(query(db,"SELECT * FROM t")==prior); CHECK(query(db,"SELECT * FROM t")==prior);
        db.execute("COMMIT"); _exit(0);
      }catch(const std::exception& e){std::cerr<<e.what()<<'\n';_exit(1);}
    }
    int status=0; CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
    Database db(path,o); CHECK(query(db,"SELECT * FROM t")==prior);
  }
}
void malformed_cached_rows(const fs::path& dir) {
  Schema s{1,"t",{{"id",ColumnType::Bigint,0},{"v",ColumnType::Vector,2},{"s",ColumnType::Varchar,20000},{"n",ColumnType::Bigint,0}},0};
  int mode=0;
  for(bool packed:{false,true})for(bool overflow:{false,true}) {
    auto good=encode_row(s,{std::int64_t(1),std::vector<float>{1,2},std::string(overflow?13000:2,'x'),std::int64_t(0)});
    std::vector<std::string> bad;
    for(auto bits:{0x7f800000u,0xff800000u,0x7fc00001u}) {
      auto bytes=good; for(unsigned i=0;i<4;++i)bytes[17+i]=static_cast<char>(bits>>(8*i)); bad.push_back(bytes);
    }
    auto unicode=good; unicode[29]=char(0x80); bad.push_back(unicode);
    for(const auto& bytes:bad) {
      const auto path=dir/("malformed-"+std::to_string(mode++));
      { Database db(path,options(dir,packed,true,true,0,0)); schema(db); }
      // Store writes correct page checksums and structures around deliberately invalid row bytes.
      { Store store(path,0,packed); store.begin();
        auto other=encode_row(s,{std::int64_t(2),std::vector<float>{1,2},std::string("ok"),std::int64_t(0)});
        std::vector<Store::Mutation> batch{{1,bytes},{2,other}}; store.apply_batch_in(1,batch); store.commit(); }
      for(bool selective:{false,true})for(std::size_t scan:{0u,262144u}) {
        Database db(path,options(dir,packed,selective,true,4*1024*1024,scan));
        for(const auto* sql:{"SELECT id FROM t WHERE n=1","SELECT id FROM t WHERE id=1","SELECT id FROM t ORDER BY n","SELECT id FROM t WHERE n=1 ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 1"}) {
          for(int repeat=0;repeat<2;++repeat) {
            db.reset_data_cache_stats(); bool rejected=false;
            try { db.execute(sql); } catch(const Error& e) { rejected=e.code()==ErrorCode::Corruption; }
            CHECK(rejected);
            if(repeat)CHECK(db.data_cache_stats().hits>0);
          }
        }
      }
    }
  }
}
}
int main() {
  try {
    const auto* root=std::getenv("MVDB_TEST_DIR"); CHECK(root); fs::create_directories(root);
    auto name=(fs::path(root)/"data-cache-sql-XXXXXX").string(); CHECK(mkdtemp(name.data())); fs::path dir(name);
    profile::set_enabled(true); comparisons(dir); partial_statement_failure(dir); malformed_cached_rows(dir); profile::set_enabled(false);
    fs::remove_all(dir); std::cout<<"PASS data cache SQL\n";
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
