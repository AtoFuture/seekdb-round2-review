#include "minivectordb/database.hpp"
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
int main(){try{
 auto root=std::filesystem::path(std::getenv("MVDB_TEST_DIR")?std::getenv("MVDB_TEST_DIR"):"/tmp")/("probation-"+std::to_string(getpid()));std::filesystem::create_directories(root);
 Options on,off;on.journal_store=off.journal_store=true;off.projection_cache_bytes=0;
 {Database a(root/"a.db",on),b(root/"b.db",off);
 auto apply=[&](std::string sql){CHECK(a.execute(sql).affected_rows==b.execute(sql).affected_rows);};
 apply("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(3),g VARCHAR(16),body VARCHAR(1000))");
 for(int i=0;i<100;++i)apply("INSERT INTO t VALUES("+std::to_string(i)+",'["+std::to_string(i%17)+",1,2]','group-"+std::to_string(i%5)+"','"+std::string(256,char('a'+i%26))+"')");
 auto verify=[&](){ExecutionInfo latest;for(int r=0;r<8;++r){auto q=std::string("SELECT id,body,L2_DISTANCE(v,'[1,2,3]') AS d FROM t")+(r%2?" WHERE g='group-1'":"")+" ORDER BY d LIMIT 10";std::vector<Row> x,y;latest=a.execute(q,{{},[&](const Row& row){x.push_back(row);}});b.execute(q,{{},[&](const Row& row){y.push_back(row);}});CHECK(x==y);}std::cout<<"projection_bytes="<<latest.projection_cache_bytes<<" typed_bytes="<<latest.typed_cache_bytes<<'\n';CHECK(latest.projection_cache_bytes>0);CHECK(latest.projection_cache_bytes+latest.typed_cache_bytes<=on.scan_snapshot_bytes);};
 verify();apply("CREATE VECTOR INDEX ix ON t(v)");verify();apply("BEGIN");apply("UPDATE t SET body='changed' WHERE g='group-1'");verify();apply("ROLLBACK");verify();apply("DELETE FROM t WHERE id=3");verify();}
 std::filesystem::remove_all(root);std::cout<<"alternating VARCHAR probation, exact output, invalidation, rollback and index passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
