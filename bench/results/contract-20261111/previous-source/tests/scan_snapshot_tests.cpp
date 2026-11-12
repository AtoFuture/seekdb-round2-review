#include "minivectordb/database.hpp"
#include "../src/sql/scan_cache.hpp"
#include <cstdlib>
#include <new>
#include <filesystem>
#include <iostream>
#include <unistd.h>
// Count only array allocations during the isolated cache test. This proves
// reuse without exposing or depending on a private arena address.
namespace {
bool track_arrays=false;
std::size_t array_allocations=0,array_releases=0;
}
void* operator new[](std::size_t bytes) {
  if(auto* result=std::malloc(bytes?bytes:1)) {
    if(track_arrays)++array_allocations;
    return result;
  }
  throw std::bad_alloc();
}
void operator delete[](void* pointer) noexcept {
  if(track_arrays && pointer)++array_releases;
  std::free(pointer);
}
void operator delete[](void* pointer,std::size_t) noexcept {
  ::operator delete[](pointer);
}
using namespace minivectordb;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__)); } while(false)
void arena_reuse() {
 struct Tracking {
  Tracking(){array_allocations=array_releases=0;track_arrays=true;}
  ~Tracking(){track_arrays=false;}
 } tracking;
 sql::ScanSnapshot cache;
 auto empty=[&] {unsigned count=0;cache.scan([&](auto,auto){++count;});CHECK(count==0);};
 auto one=[&](std::int64_t expected_key,std::string_view expected_bytes) {
  unsigned count=0;cache.scan([&](auto key,auto bytes){++count;CHECK(key==expected_key);CHECK(bytes==expected_bytes);});CHECK(count==1);
 };
 cache.start(1,0,256,true);CHECK(cache.bytes()==0);CHECK(array_allocations==0);
 cache.start(1,0,256,true);CHECK(cache.building());CHECK(array_allocations==1);CHECK(array_releases==0);
 cache.append(-1,"old first");cache.append(99,"old tail");cache.finish();CHECK(cache.ready(1,0));
 cache.invalidate();CHECK(cache.bytes()==256);CHECK(!cache.ready(1,0));CHECK(!cache.building());empty();
 CHECK(array_allocations==1);CHECK(array_releases==0);
 cache.start(1,0,256,true);CHECK(cache.bytes()==256);CHECK(!cache.building());CHECK(!cache.ready(1,0));
 cache.start(1,0,256,true);CHECK(cache.building());CHECK(array_allocations==1);CHECK(array_releases==0);
 cache.append(7,"short");cache.finish();CHECK(cache.ready(1,0));one(7,"short");
 // A different identity retains capacity but never the previous rows or proof.
 cache.start(2,3,256,true);CHECK(cache.bytes()==256);CHECK(!cache.ready(1,0));CHECK(!cache.ready(2,3));empty();
 cache.start(2,3,256,true);cache.append(-9,"different");cache.finish();one(-9,"different");
 CHECK(cache.ready(2,3));CHECK(array_allocations==1);CHECK(array_releases==0);
 // A changed effective budget releases the old allocation before probation.
 cache.start(2,3,128,true);CHECK(cache.bytes()==0);CHECK(!cache.ready(2,3));empty();
 CHECK(array_allocations==1);CHECK(array_releases==1);
 cache.start(2,3,128,true);CHECK(cache.bytes()==128);CHECK(array_allocations==2);
 cache.append(0,"smaller");cache.finish();one(0,"smaller");
 cache.clear();CHECK(cache.bytes()==0);CHECK(!cache.ready(2,3));empty();CHECK(array_releases==2);
 // Disabled reuse releases both on identity switch and after invalidate.
 cache.start(1,0,256,false);cache.start(1,0,256,false);cache.append(1,"before");cache.finish();
 CHECK(array_allocations==3);
 cache.start(2,0,256,false);CHECK(cache.bytes()==0);CHECK(array_releases==3);empty();
 cache.start(2,0,256,false);cache.append(2,"after");cache.finish();one(2,"after");CHECK(array_allocations==4);
 cache.invalidate();CHECK(cache.bytes()==256);
 cache.start(2,0,256,false);CHECK(cache.bytes()==0);CHECK(array_releases==4);empty();
 cache.start(2,0,256,false);cache.finish();CHECK(cache.ready(2,0));empty();CHECK(array_allocations==5);
 cache.start(2,0,0,true);CHECK(cache.bytes()==0);CHECK(array_releases==5);
 cache.start(2,0,0,true);CHECK(!cache.ready(2,0));CHECK(!cache.building());CHECK(array_allocations==5);empty();
 cache.clear();CHECK(cache.bytes()==0);CHECK(array_releases==5);
}
struct Result { std::vector<Row> rows; ExecutionInfo info; };
Result query(Database& d,std::string filter="",SearchMode mode=SearchMode::Exact) {
 Result r; r.info=d.execute("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t "+filter+" ORDER BY d LIMIT 9",{{},[&](const Row& row){r.rows.push_back(row);}},mode);return r;
}
void run(const std::filesystem::path& path,bool packed,bool selective,std::size_t budget) {
 Options o; o.typed_scan_cache=false; // Isolate raw snapshot counters and validation.
 o.packed_values=packed;o.selective_decode=selective;o.scan_snapshot_bytes=budget;
 const bool fits=budget>=4096;
 {Database d(path,o);
  d.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2),g BIGINT)");
  d.execute("INSERT INTO t VALUES(3,'[3,4]',1),(-2,'[0,0]',0),(0,'[1,0]',1)");
  d.execute("CREATE VECTOR INDEX tv ON t(v)");
  auto cold=query(d);CHECK(cold.info.snapshot_bytes==0);auto building=query(d);CHECK(building.info.vector_index_entries_examined==3);auto warm=query(d);CHECK(cold.rows==warm.rows);CHECK(cold.info.vector_index_entries_examined==3);
  CHECK(warm.info.snapshot_rows_examined==(fits?3U:0U));CHECK(warm.info.snapshot_bytes<=budget);
  CHECK(query(d,"WHERE g=1",SearchMode::Ann).rows.size()==2);
  CHECK(query(d,"WHERE g=100").rows.empty());
  std::vector<Row> changed_vector;
  auto changed=d.execute("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[3,4]') LIMIT 1",{{},[&](const Row& row){changed_vector.push_back(row);}});
  CHECK(std::get<std::int64_t>(changed_vector[0][0])==3);CHECK(changed.snapshot_rows_examined==(fits?3U:0U));
  d.execute("BEGIN");d.execute("UPDATE t SET v='[0,0]' WHERE id=3");d.execute("DELETE FROM t WHERE id=-2");
  CHECK(std::get<std::int64_t>(query(d).rows[0][0])==3);query(d);CHECK(query(d).info.snapshot_rows_examined==(fits?2U:0U));
  d.execute("ROLLBACK");CHECK(query(d).rows==cold.rows);query(d);CHECK(query(d).info.snapshot_rows_examined==(fits?3U:0U));
  d.execute("BEGIN");d.execute("INSERT INTO t VALUES(-9,'[0,0]',1)");query(d);query(d);CHECK(query(d).info.snapshot_rows_examined==(fits?4U:0U));
  try{d.execute("INSERT INTO t VALUES(88,'[0,0]',1),(0,'[1,0]',1)");CHECK(false);}catch(const Error&){}
  CHECK(query(d).info.vector_index_entries_examined==4);query(d);CHECK(query(d).info.snapshot_rows_examined==(fits?4U:0U));d.execute("COMMIT");CHECK(query(d).info.vector_index_entries_examined==4);CHECK(query(d).rows.size()==4);
  d.execute("CREATE TABLE other(id BIGINT PRIMARY KEY,v VECTOR(2))");CHECK(query(d).info.vector_index_entries_examined==4);
  d.execute("INSERT INTO other VALUES(77,'[0,0]')");d.execute("CREATE VECTOR INDEX oi ON other(v)");query(d);query(d);
  std::vector<Row> other;
  d.execute("SELECT id FROM other ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 1",{{},[&](const Row& row){other.push_back(row);}});
  CHECK(std::get<std::int64_t>(other[0][0])==77);CHECK(query(d).info.vector_index_entries_examined==4);
  // A sink exception cannot poison the already complete immutable snapshot.
  bool sink_failed=false;
  try {d.execute("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 2",{{},[](const Row&){throw std::runtime_error("sink");}});}catch(const std::runtime_error& e){sink_failed=std::string(e.what())=="sink";}CHECK(sink_failed);
  auto after_sink=query(d);CHECK(after_sink.rows.size()==4);CHECK(after_sink.info.snapshot_rows_examined==(fits?4U:0U));
 }
 std::vector<Row> reopened;
 {Database d(path,o);CHECK(query(d).info.vector_index_entries_examined==4);reopened=query(d).rows;CHECK(reopened.size()==4);}
 {Options ref=o;ref.scan_snapshot_bytes=0;Database d(path,ref);CHECK(query(d).rows==reopened);CHECK(query(d).info.snapshot_rows_examined==0);}
 // Cross-open mutations must not retain a previously verified index proof.
 {Store s(path);s.begin();s.put_in(2,3,std::string(8,'\0'));s.commit();}
 {Database d(path,o);for(int attempt=0;attempt<2;++attempt){bool failed=false;try{query(d,"WHERE g=0");}catch(const Error& e){failed=e.code()==ErrorCode::Corruption;}CHECK(failed);}}
}
int main(){const char* env=std::getenv("MVDB_TEST_DIR");auto root=std::filesystem::path(env?env:"/data")/("snapshot-"+std::to_string(getpid()));std::filesystem::create_directories(root);
 try{arena_reuse();sql::ScanSnapshot cache;cache.start(1,2,16*1024*1024);CHECK(cache.bytes()==0);cache.start(1,2,16*1024*1024);CHECK(cache.bytes()==8*1024*1024);CHECK(!cache.ready(1,2));cache.append(-1,"abc");cache.finish();CHECK(cache.ready(1,2));cache.clear();CHECK(!cache.ready(1,2));
  cache.start(1,2,17);cache.start(1,2,17);cache.append(1,"xx");cache.finish();CHECK(!cache.ready(1,2));CHECK(cache.bytes()==0);cache.start(1,2,100);CHECK(cache.bytes()==0);cache.start(3,4,100);cache.start(3,4,100);cache.finish();CHECK(cache.ready(3,4));
  for(bool packed:{false,true})for(bool selective:{false,true})for(std::size_t budget:{0U,1U,60U,4096U})run(root/(std::to_string(packed)+std::to_string(selective)+"-"+std::to_string(budget)),packed,selective,budget);
  Options o;o.scan_snapshot_bytes=4096;Database empty(root/"empty",o);empty.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2),g BIGINT)");empty.execute("CREATE VECTOR INDEX tv ON t(v)");CHECK(query(empty).rows.empty());CHECK(query(empty).rows.empty());empty.execute("INSERT INTO t VALUES(1,'[0,0]',0)");CHECK(query(empty).rows.size()==1);
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
 std::filesystem::remove_all(root);std::cout<<"scan snapshot tests passed\n";
}
