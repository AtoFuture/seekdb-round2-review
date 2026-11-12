#include "minivectordb/database.hpp"
#include "minivectordb/profile.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <unistd.h>
using namespace minivectordb;
#define CHECK(x) do {if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
using Values=std::map<std::int64_t,std::string>;
void store_cases(const std::filesystem::path& path,bool packed,std::size_t budget) {
 Store s(path,0,packed,budget); unsigned count=0;
 s.scan_equal_in(1,UINT64_MAX,[&](auto,auto,auto){++count;}); CHECK(count==0);
 Values expected={{INT64_MIN,"min"},{-0x123456789abcLL,"sparse"},{-1,""},{0,std::string(9000,'x')},{1,"one"},{0x123456789abcLL,"sparse2"},{INT64_MAX,"max"}};
 std::vector<Store::Mutation> left,right;
 for(const auto& [key,value]:expected){left.push_back({key,value});right.push_back({key,"right"+value});}
 s.begin(); s.apply_batch_in(1,left);s.apply_batch_in(UINT64_MAX,right);s.commit();
 auto check=[&] {Values got;std::vector<std::int64_t> order,wanted;for(const auto& entry:expected)wanted.push_back(entry.first);s.scan_equal_in(1,UINT64_MAX,[&](auto key,auto a,auto b){CHECK(a==expected.at(key));CHECK(b=="right"+expected.at(key));got.emplace(key,a);order.push_back(key);});CHECK(got==expected);CHECK(order==wanted);};
 check();s.scan_equal_in(1,1,[&](auto key,auto a,auto b){CHECK(a==b);CHECK(a==expected.at(key));});
 // Both directions, missing first and last key; late corruption may follow visits.
 for(auto space:{std::uint64_t(1),UINT64_MAX})for(auto key:{INT64_MIN,INT64_MAX}) {
  s.begin();s.erase_in(space,key);count=0;bool failed=false;
  try{s.scan_equal_in(1,UINT64_MAX,[&](auto,auto,auto){++count;});}catch(const Store::KeySetMismatch&){failed=true;}
  CHECK(failed);CHECK(key==INT64_MIN?count==0:count==expected.size()-1);s.rollback();
 }
 for(auto space:{std::uint64_t(1),UINT64_MAX}) {
  bool failed=false;try{s.scan_equal_in(space,99,[](auto,auto,auto){});}catch(const Store::KeySetMismatch&){failed=true;}CHECK(failed);
 }
 s.begin();auto saved=s.savepoint();Values visited;
 s.scan_equal_in(1,UINT64_MAX,[&](auto key,auto a,auto b){
  const std::string a_copy(a),b_copy(b);visited.emplace(key,a);
  s.put_in(1,key,"changed");s.erase_in(UINT64_MAX,INT64_MAX);s.put_in(1,9,"new");
  s.restore(saved);check();CHECK(a==a_copy);CHECK(b==b_copy);
 });CHECK(visited==expected);s.rollback();
 // Rollback from callback must retain both pre-callback namespace snapshots.
 s.begin();s.put_in(1,0,"uncommitted");s.put_in(UINT64_MAX,0,"rightuncommitted");count=0;
 s.scan_equal_in(1,UINT64_MAX,[&](auto key,auto a,auto b){if(count++==0)s.rollback();CHECK(a==(key==0?"uncommitted":expected.at(key)));CHECK(b=="right"+std::string(a));});
 CHECK(count==expected.size());check();
 struct CallbackFailure{};bool propagated=false;
 try{s.scan_equal_in(1,UINT64_MAX,[](auto,auto,auto){throw CallbackFailure{};});}catch(const CallbackFailure&){propagated=true;}CHECK(propagated);
}
struct Result {std::vector<Row> rows;ExecutionInfo info;profile::Snapshot stats;};
Result query(Database& db,SearchMode mode=SearchMode::Exact,std::string_view filter="") {
 Result r;profile::set_enabled(true);profile::reset();
 r.info=db.execute("select id,l2_distance(v,[0,0]) as d from t "+std::string(filter)+" order by d limit 7",{{},[&](const Row& row){r.rows.push_back(row);}},mode);
 r.stats=profile::snapshot();profile::set_enabled(false);return r;
}
void sql_cases(const std::filesystem::path& file,bool packed,bool selective) {
 Options options;options.packed_values=packed;options.selective_decode=selective;options.index_cache_bytes=0;options.data_cache_bytes=0;options.paired_index_scan=false;
 Result plain,reference;
 {Database db(file,options);db.execute("create table t(id bigint primary key,v vector(2),tag bigint)");db.execute("begin");
  std::string insert="insert into t values ";for(int i=0;i<300;++i){if(i)insert+=",";insert+="("+std::to_string(i)+",["+std::to_string(i%11)+",0],"+std::to_string(i%2)+")";}db.execute(insert);db.execute("commit");plain=query(db);db.execute("create vector index ix on t(v)");reference=query(db);CHECK(plain.rows==reference.rows);}
 options.paired_index_scan=true;
 {Database db(file,options);auto paired=query(db);CHECK(paired.rows==reference.rows);CHECK(paired.info.rows_examined==300);CHECK(paired.info.vector_index_entries_examined==300);CHECK(paired.stats.row_validation.calls==reference.stats.row_validation.calls);CHECK(paired.stats.row_decode.calls==reference.stats.row_decode.calls);CHECK(paired.stats.page_read_branch.calls*4<reference.stats.page_read_branch.calls);CHECK(query(db,SearchMode::Ann).rows==reference.rows);
  db.execute("begin");db.execute("update t set v=[0,0] where id=299");db.execute("delete from t where id=298");db.execute("insert into t values(-1,[0,0],1)");CHECK(std::get<std::int64_t>(query(db).rows[0][0])==-1);db.execute("rollback");CHECK(query(db).rows==reference.rows);
 }
 Result filtered;
 {Database db(file,options);filtered=query(db,SearchMode::Ann,"where tag=1");}
 options.paired_index_scan=false;
 {Database db(file,options);CHECK(query(db,SearchMode::Exact,"where tag=1").rows==filtered.rows);}
 // Table namespace is 1; index namespace is allocated after it (2).
 std::string base,index;
 {Store s(file);base=*s.get_in(1,299);index=*s.get_in(2,299);}
 for(auto space:{1ULL,2ULL}) {
  {Store s(file);s.begin();s.erase_in(space,299);s.commit();}
  options.paired_index_scan=true;
  {Database db(file,options);bool failed=false;try{query(db);}catch(const Error& e){failed=e.code()==ErrorCode::Corruption;}CHECK(failed);}
  {Store s(file);s.begin();s.put_in(space,299,space==1?base:index);s.commit();}
 }
 // The filter must not hide a malformed full base row or a stale index vector.
 for(auto space:{1ULL,2ULL}) {
  {Store s(file);s.begin();s.put_in(space,299,"bad");s.commit();}
  for(bool paired:{false,true}) {options.paired_index_scan=paired;Database db(file,options);bool failed=false;try{query(db,SearchMode::Ann,"where tag=0");}catch(const Error& e){failed=e.code()==ErrorCode::Corruption;}CHECK(failed);}
  {Store s(file);s.begin();s.put_in(space,299,space==1?base:index);s.commit();}
 }
 // A finite, correctly sized but stale vector must fail even when WHERE excludes it.
 {Store s(file);s.begin();s.put_in(2,299,std::string(8,'\0'));s.commit();}
 for(bool paired:{false,true}) {options.paired_index_scan=paired;Database db(file,options);bool failed=false;try{query(db,SearchMode::Ann,"where tag=0");}catch(const Error& e){failed=e.code()==ErrorCode::Corruption;}CHECK(failed);}
 {Store s(file);s.begin();s.put_in(2,299,index);s.commit();}
}
int main() {
 const char* root=std::getenv("MVDB_TEST_DIR");auto dir=std::filesystem::path(root?root:"/data")/("paired-"+std::to_string(getpid()));std::filesystem::create_directories(dir);
 try {for(bool packed:{false,true}){for(std::size_t budget:{0U,1U,8192U})store_cases(dir/("store"+std::to_string(packed)+"-"+std::to_string(budget)),packed,budget);for(bool selective:{false,true})sql_cases(dir/("sql"+std::to_string(packed)+std::to_string(selective)),packed,selective);}}
 catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
 std::filesystem::remove_all(dir);std::cout<<"paired scan tests passed\n";
}
