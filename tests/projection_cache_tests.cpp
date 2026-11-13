#include "minivectordb/database.hpp"
#include "minivectordb/row_codec.hpp"
#include "../src/sql/projection_cache.hpp"
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
std::vector<Row> query(Database& db,const std::string& sql){std::vector<Row> rows;db.execute(sql,{{},[&](const Row& r){rows.push_back(r);}});return rows;}
int main(){try {
  const auto root=std::filesystem::path(std::getenv("MVDB_TEST_DIR")?std::getenv("MVDB_TEST_DIR"):"/tmp")/("projection-"+std::to_string(getpid()));
  std::filesystem::create_directories(root);
  Options enabled,disabled;disabled.projection_cache_bytes=0;
  {
    Database a(root/"on.db",enabled),b(root/"off.db",disabled);
    auto execute=[&](const std::string& sql){auto x=a.execute(sql),y=b.execute(sql);CHECK(x.affected_rows==y.affected_rows);};
    execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(3),g BIGINT,body VARCHAR(80000))");
    for(int i=0;i<100;++i)execute("INSERT INTO t VALUES("+std::to_string(i-50)+",'["+std::to_string(i%11)+",1,-2]',"+std::to_string(i%7)+",'"+std::string(256,'a'+i%26)+"中😀')");
    auto verify=[&](){
      for(int round=0;round<5;++round)for(const std::string tail:{""," WHERE g=4"," WHERE g=999"}) {
        const auto q="SELECT id,body,L2_DISTANCE(v,'[2,0,1]') AS d FROM t"+tail+" ORDER BY d LIMIT 10";
        CHECK(query(a,q)==query(b,q));
      }
      for(const std::string q:{"SELECT id,v,g,body,L2_DISTANCE(v,'[2,0,1]') AS d FROM t ORDER BY d LIMIT 10",
          "SELECT id,L2_DISTANCE(v,'[4,3,2]') AS other,L2_DISTANCE(v,'[2,0,1]') AS d FROM t ORDER BY d LIMIT 10"})CHECK(query(a,q)==query(b,q));
    };
    verify();
    const auto active=a.execute("SELECT id,body,L2_DISTANCE(v,'[2,0,1]') AS d FROM t ORDER BY d LIMIT 10");
    CHECK(active.projection_cache_bytes>0);CHECK(active.typed_cache_bytes+active.projection_cache_bytes<=enabled.scan_snapshot_bytes);
    execute("CREATE VECTOR INDEX vi ON t(v)");verify();
    execute("BEGIN");execute("UPDATE t SET body='changed' WHERE g=4");verify();execute("ROLLBACK");verify();
    execute("BEGIN");execute("DELETE FROM t WHERE g=3");execute("COMMIT");verify();
    execute("UPDATE t SET body='"+std::string(70000,'q')+"' WHERE id=1");verify(); // bounded refusal
    execute("CREATE TABLE u(id BIGINT PRIMARY KEY,v VECTOR(3),g BIGINT,body VARCHAR(9))");
    execute("INSERT INTO u VALUES(4,'[0,0,0]',1,'other')");
    const std::string uq="SELECT id,body,L2_DISTANCE(v,'[0,0,0]') AS d FROM u ORDER BY d LIMIT 10";
    for(int i=0;i<4;++i)CHECK(query(a,uq)==query(b,uq));verify();
  }
  {Database a(root/"on.db",enabled),b(root/"off.db",disabled);CHECK(query(a,"SELECT * FROM t")==query(b,"SELECT * FROM t"));}
  std::filesystem::remove_all(root);std::cout<<"projection cache equivalence, rollback, large-row fallback and reopen passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
