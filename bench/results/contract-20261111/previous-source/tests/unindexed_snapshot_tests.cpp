#include "minivectordb/database.hpp"
#include "minivectordb/profile.hpp"
#include "minivectordb/row_codec.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>

using namespace minivectordb;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__)); } while(false)

// Inject a read failure in this executable only, after earlier rows have been
// consumed. No open database file is modified and there are no production hooks.
namespace {
bool fail_read=false;
bool reuse_arena=true;
std::string failure_needle;
unsigned failed_reads=0;
}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* buffer,size_t bytes,off_t offset) {
  const auto result=__real_pread(fd,buffer,bytes,offset);
  if(fail_read && result>0 && !failure_needle.empty()) {
    const auto* begin=static_cast<const char*>(buffer);
    const auto* end=begin+result;
    if(std::search(begin,end,failure_needle.begin(),failure_needle.end())!=end) {
      ++failed_reads;
      errno=EIO;
      return -1;
    }
  }
  return result;
}

namespace {
struct Item {std::int64_t id;int x,y,group;std::string label;};
const std::vector<Item> initial{
  {-12,0,0,0,"a"},{-2,3,4,1,"b"},{0,0,1,1,"c"},
  {7,-3,-4,0,"d"},{99,9,12,1,"e"}};
struct Result {std::vector<Row> rows;ExecutionInfo info;profile::Snapshot stats;};
Options settings(bool packed,bool selective,std::size_t budget) {
  Options o;
  o.typed_scan_cache=false; // This suite exercises the retained raw-row snapshot.
  o.packed_values=packed;o.selective_decode=selective;o.scan_snapshot_bytes=budget;
  o.reuse_scan_snapshot_buffer=reuse_arena;
  return o;
}
Schema fixture_schema() {
  return {1,"t",{{"id",ColumnType::Bigint,0},{"v",ColumnType::Vector,2},
    {"g",ColumnType::Bigint,0},{"label",ColumnType::Varchar,6000}},0};
}
Row stored_row(const Item& item) {
  return {item.id,std::vector<float>{float(item.x),float(item.y)},
    std::int64_t(item.group),item.label};
}
void seed(Database& db,const std::vector<Item>& items=initial,const std::string& table="t") {
  db.execute("CREATE TABLE "+table+"(id BIGINT PRIMARY KEY,v VECTOR(2),g BIGINT,label VARCHAR(6000))");
  if(items.empty())return;
  std::string sql="INSERT INTO "+table+" VALUES ";
  for(std::size_t n=0;n<items.size();++n) {
    const auto& item=items[n];
    if(n)sql+=",";
    sql+="("+std::to_string(item.id)+",'["+std::to_string(item.x)+","+std::to_string(item.y)+"]',"+
      std::to_string(item.group)+",'"+item.label+"')";
  }
  db.execute(sql);
}
std::string statement(int qx=0,int qy=0,std::optional<int> group={},
                      std::size_t limit=9,const std::string& table="t",const std::string& column="v") {
  return "SELECT id,L2_DISTANCE("+column+",'["+std::to_string(qx)+","+std::to_string(qy)+"]') AS d,g,label FROM "+table+
    (group?" WHERE g="+std::to_string(*group):"")+" ORDER BY d LIMIT "+std::to_string(limit);
}
Result query(Database& db,const std::string& sql=statement(),SearchMode mode=SearchMode::Exact) {
  Result r;
  const bool old=profile::enabled();profile::set_enabled(true);profile::reset();
  try {r.info=db.execute(sql,{{},[&](const Row& row){r.rows.push_back(row);}},mode);}
  catch(...) {profile::set_enabled(old);throw;}
  r.stats=profile::snapshot();profile::set_enabled(old);
  return r;
}
// Independent sorting and arithmetic oracle: no executor or exact_top_k calls.
std::vector<Row> expected(const std::vector<Item>& items=initial,int qx=0,int qy=0,
                          std::optional<int> group={},std::size_t limit=9) {
  std::vector<Row> rows;
  for(const auto& item:items) {
    if(group && item.group!=*group)continue;
    const double dx=double(item.x)-qx;
    const double dy=double(item.y)-qy;
    rows.push_back({item.id,std::sqrt(dx*dx+dy*dy),std::int64_t(item.group),item.label});
  }
  std::sort(rows.begin(),rows.end(),[](const Row& a,const Row& b) {
    const auto da=std::get<double>(a[1]),db=std::get<double>(b[1]);
    return da<db || (da==db && std::get<std::int64_t>(a[0])<std::get<std::int64_t>(b[0]));
  });
  if(rows.size()>limit)rows.resize(limit);
  return rows;
}
void full_validation(const Result& r,bool selective,std::size_t count) {
  CHECK(r.info.rows_examined==count);
  CHECK(r.info.affected_rows==r.rows.size());
  CHECK((selective?r.stats.row_validation.calls:r.stats.row_decode.calls)==count+r.rows.size());
}
std::size_t retained_bytes(std::size_t budget) {
  return reuse_arena && budget>=4096?std::min(budget,std::size_t(8*1024*1024)):0;
}
void warm_cycle(Database& db,const std::vector<Row>& answer,std::size_t count,std::size_t budget,
                bool selective,std::size_t expected_cold_bytes,const std::string& sql=statement()) {
  const bool fits=budget>=4096;
  const auto cold=query(db,sql);
  CHECK(cold.rows==answer);CHECK(cold.info.snapshot_bytes==expected_cold_bytes);CHECK(cold.info.snapshot_rows_examined==0);
  const auto build=query(db,sql);
  CHECK(build.rows==answer);CHECK(build.info.snapshot_rows_examined==0);
  const auto warm=query(db,sql);
  CHECK(warm.rows==answer);
  // This assertion is deliberately RED on the baseline: absent no-index reuse
  // is a missing performance feature, not an existing query-correctness defect.
  CHECK(warm.info.snapshot_rows_examined==(fits?count:0));
  CHECK(build.info.snapshot_bytes==(fits?std::min(budget,std::size_t(8*1024*1024)):0));
  CHECK(warm.info.snapshot_bytes==build.info.snapshot_bytes);
  CHECK(warm.info.vector_index_entries_examined==0);
  full_validation(cold,selective,count);full_validation(build,selective,count);full_validation(warm,selective,count);
}
template<class F> void expect_error(ErrorCode code,F&& action) {
  bool rejected=false;
  try {action();}catch(const Error& error) {CHECK(error.code()==code);rejected=true;}
  CHECK(rejected);
}
void reuse_after_write(const std::filesystem::path& path,bool packed,bool selective) {
  Database db(path,settings(packed,selective,4096));seed(db);
  warm_cycle(db,expected(),initial.size(),4096,selective,0);
  db.execute("DELETE FROM t WHERE id=99");
  auto remaining=initial;remaining.pop_back();
  const auto answer=expected(remaining);

  const auto cold=query(db);
  CHECK(cold.rows==answer);CHECK(cold.info.snapshot_rows_examined==0);
  // In reuse mode invalidate the proof while retaining the allocation.
  // The enabled-mode capacity assertion was RED before arena reuse.
  CHECK(cold.info.snapshot_bytes==retained_bytes(4096));
  full_validation(cold,selective,remaining.size());
  const auto build=query(db);
  CHECK(build.rows==answer);CHECK(build.info.snapshot_rows_examined==0);
  CHECK(build.info.snapshot_bytes==4096);
  full_validation(build,selective,remaining.size());
  const auto warm=query(db);
  CHECK(warm.rows==answer);CHECK(warm.info.snapshot_rows_examined==remaining.size());
  CHECK(warm.info.snapshot_bytes==4096);
  // The shorter rebuilt snapshot must not expose the removed final record.
  full_validation(warm,selective,remaining.size());
}
void lifecycle(const std::filesystem::path& path,bool packed,bool selective,std::size_t budget) {
  const auto o=settings(packed,selective,budget);
  const bool fits=budget>=4096;
  std::vector<Item> committed=initial;
  {
    Database db(path,o);seed(db);
    warm_cycle(db,expected(),initial.size(),budget,selective,0);
    const auto filtered=query(db,statement(0,0,1,2));
    CHECK(filtered.rows==expected(initial,0,0,1,2));
    CHECK(filtered.info.snapshot_rows_examined==(fits?initial.size():0));
    full_validation(filtered,selective,initial.size());
    CHECK(query(db,statement(0,0,100)).rows.empty());
    CHECK(query(db,statement(9,12,{},3)).rows==expected(initial,9,12,{},3));
    CHECK(query(db,statement(),SearchMode::Ann).rows==expected());

    db.execute("BEGIN");
    warm_cycle(db,expected(),initial.size(),budget,selective,retained_bytes(budget));
    db.execute("UPDATE t SET v='[0,0]',label='changed' WHERE id=99");
    auto changed=initial;changed.back().x=changed.back().y=0;changed.back().label="changed";
    warm_cycle(db,expected(changed),changed.size(),budget,selective,retained_bytes(budget));
    db.execute("DELETE FROM t WHERE id=-12");changed.erase(changed.begin());
    warm_cycle(db,expected(changed),changed.size(),budget,selective,retained_bytes(budget));
    db.execute("INSERT INTO t VALUES(-99,'[0,0]',1,'new')");
    changed.push_back({-99,0,0,1,"new"});
    warm_cycle(db,expected(changed),changed.size(),budget,selective,retained_bytes(budget));
    expect_error(ErrorCode::Constraint,[&]{db.execute("INSERT INTO t VALUES(100,'[0,0]',1,'partial'),(0,'[0,0]',1,'duplicate')");});
    CHECK(db.in_transaction());
    warm_cycle(db,expected(changed),changed.size(),budget,selective,retained_bytes(budget));
    db.execute("ROLLBACK");
    warm_cycle(db,expected(),initial.size(),budget,selective,retained_bytes(budget));

    expect_error(ErrorCode::Constraint,[&]{db.execute("INSERT INTO t VALUES(0,'[0,0]',1,'duplicate')");});
    CHECK(!db.in_transaction());
    warm_cycle(db,expected(),initial.size(),budget,selective,retained_bytes(budget));
    expect_error(ErrorCode::Schema,[&]{db.execute("UPDATE t SET missing=1 WHERE id=0");});
    warm_cycle(db,expected(),initial.size(),budget,selective,retained_bytes(budget));
    db.execute("BEGIN");
    db.execute("INSERT INTO t VALUES(-99,'[0,0]',1,'committed')");
    committed.push_back({-99,0,0,1,"committed"});
    warm_cycle(db,expected(committed),committed.size(),budget,selective,retained_bytes(budget));
    db.execute("COMMIT");
    warm_cycle(db,expected(committed),committed.size(),budget,selective,retained_bytes(budget));
  }
  // A new Database must allocate its own snapshot even for the same file.
  {Database db(path,o);warm_cycle(db,expected(committed),committed.size(),budget,selective,0);}
  {auto reference=o;reference.scan_snapshot_bytes=0;Database db(path,reference);
    CHECK(query(db).rows==expected(committed));CHECK(query(db).info.snapshot_rows_examined==0);}
  // Destruction rolls back both rows and any warmed uncommitted snapshot.
  {Database db(path,o);db.execute("BEGIN");db.execute("DELETE FROM t WHERE id=99");
    auto temporary=committed;temporary.erase(temporary.begin()+4);
    warm_cycle(db,expected(temporary),temporary.size(),budget,selective,0);}
  {Database db(path,o);warm_cycle(db,expected(committed),committed.size(),budget,selective,0);}
}
void eligibility(const std::filesystem::path& path,bool packed,bool selective) {
  Database db(path,settings(packed,selective,4096));seed(db);
  const std::vector<std::string> excluded{
    "SELECT id FROM t WHERE id=-2 ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 2",
    "SELECT id FROM t WHERE id=123456 ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 2",
    "SELECT id FROM t LIMIT 1",
    "SELECT id FROM t",
    "SELECT id FROM t ORDER BY id LIMIT 2",
    "SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]') DESC LIMIT 2",
    "SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]'),id LIMIT 2",
    "SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]')",
    "SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 0"};
  const std::vector<std::vector<Row>> answers{
    {{std::int64_t(-2)}},{},{{std::int64_t(-12)}},
    {{std::int64_t(-12)},{std::int64_t(-2)},{std::int64_t(0)},{std::int64_t(7)},{std::int64_t(99)}},
    {{std::int64_t(-12)},{std::int64_t(-2)}},
    {{std::int64_t(99)},{std::int64_t(-2)}},
    {{std::int64_t(-12)},{std::int64_t(0)}},
    {{std::int64_t(-12)},{std::int64_t(0)},{std::int64_t(-2)},{std::int64_t(7)},{std::int64_t(99)}},{}};
  for(std::size_t n=0;n<excluded.size();++n) {
    const auto cold_bytes=n==0?0:retained_bytes(4096);
    db.execute("UPDATE t SET g=0 WHERE id=-12");
    for(int attempt=0;attempt<3;++attempt) {
      const auto r=query(db,excluded[n]);CHECK(r.rows==answers[n]);
      CHECK(r.info.snapshot_rows_examined==0);
      CHECK(r.info.snapshot_bytes==(n<2?cold_bytes:0));
    }
    warm_cycle(db,expected(),initial.size(),4096,selective,cold_bytes);
    // Point lookup still bypasses an existing full snapshot.
    const auto point=query(db,excluded[0]);CHECK(point.rows==answers[0]);
    CHECK(point.info.rows_examined==1);CHECK(point.info.snapshot_rows_examined==0);
  }
}
void switching(const std::filesystem::path& path,bool packed,bool selective) {
  const auto o=settings(packed,selective,4096);
  Database db(path,o);seed(db);
  const std::vector<Item> other{{-77,12,9,1,"other"},{0,0,0,0,"zero"}};
  seed(db,other,"other");
  const auto unindexed=statement(3,4,{},9,"other");
  const auto answer=expected(other,3,4);
  warm_cycle(db,expected(),initial.size(),4096,selective,0);
  warm_cycle(db,answer,other.size(),4096,selective,retained_bytes(4096),unindexed);
  warm_cycle(db,expected(),initial.size(),4096,selective,retained_bytes(4096));
  // Creating the index invalidates the existing no-index snapshot for t.
  db.execute("CREATE VECTOR INDEX tv ON t(v)");
  const auto first_index=query(db);CHECK(first_index.rows==expected());
  CHECK(first_index.info.vector_index_entries_examined==initial.size());
  CHECK(first_index.info.snapshot_rows_examined==0);CHECK(first_index.info.snapshot_bytes==retained_bytes(4096));
  CHECK(query(db).info.snapshot_rows_examined==0);
  CHECK(query(db).info.snapshot_rows_examined==initial.size());
  // Indexed t and unindexed other share one arena, with distinct identities.
  warm_cycle(db,answer,other.size(),4096,selective,retained_bytes(4096),unindexed);
  const auto return_index=query(db);CHECK(return_index.rows==expected());
  CHECK(return_index.info.vector_index_entries_examined==initial.size());
  CHECK(return_index.info.snapshot_rows_examined==0);CHECK(return_index.info.snapshot_bytes==retained_bytes(4096));
  CHECK(query(db).info.snapshot_rows_examined==0);
  CHECK(query(db).info.snapshot_rows_examined==initial.size());
  warm_cycle(db,answer,other.size(),4096,selective,retained_bytes(4096),unindexed);

  Database independent(path.string()+"-independent",o);seed(independent,initial);
  warm_cycle(independent,expected(),initial.size(),4096,selective,0);
  CHECK(query(db,unindexed).rows==answer);
  CHECK(query(db,unindexed).info.snapshot_rows_examined==other.size());
}
void empty_and_cap(const std::filesystem::path& path,bool packed,bool selective) {
  {
    Database db(path,settings(packed,selective,4096));seed(db,{});
    warm_cycle(db,{},0,4096,selective,0);
    db.execute("INSERT INTO t VALUES(-12,'[0,0]',0,'a')");
    const std::vector<Item> one{initial.front()};
    warm_cycle(db,expected(one),1,4096,selective,retained_bytes(4096));
  }
  {Database db(path.string()+"-cap",settings(packed,selective,16*1024*1024));seed(db);
    warm_cycle(db,expected(),initial.size(),16*1024*1024,selective,0);}
}
void sink_failure(const std::filesystem::path& path,bool packed,bool selective) {
  Database db(path,settings(packed,selective,4096));seed(db);
  CHECK(query(db).rows==expected()); // probation
  for(int attempt=0;attempt<2;++attempt) {
    bool failed=false;
    try {db.execute(statement(),{{},[](const Row&){throw std::runtime_error("intentional sink failure");}});}
    catch(const std::runtime_error& error) {failed=std::string(error.what())=="intentional sink failure";}
    CHECK(failed);
    // The first failure happens after building, the second after warm reuse.
    const auto recovered=query(db);CHECK(recovered.rows==expected());
    CHECK(recovered.info.snapshot_rows_examined==initial.size());
    full_validation(recovered,selective,initial.size());
  }
}
void failed_build(const std::filesystem::path& path,bool packed,bool selective,std::size_t budget=65536) {
  auto o=settings(packed,selective,budget);
  o.index_cache_bytes=0;o.data_cache_bytes=0;o.packed_scan_cache_bytes=0;
  auto large=initial;
  for(auto& item:large)item.label=std::string(5000,char('a'+item.group));
  Database db(path,o);seed(db,large);
  CHECK(query(db).rows==expected(large)); // A healthy probation scan.
  failure_needle=encode_row(fixture_schema(),stored_row(large.back())).substr(0,64);
  fail_read=true;failed_reads=0;
  for(int attempt=0;attempt<3;++attempt) {
    bool failed=false;
    try {query(db);}catch(const std::system_error& error) {CHECK(error.code().value()==EIO);failed=true;}
    CHECK(failed);CHECK(failed_reads==static_cast<unsigned>(attempt+1));
    const auto stats=profile::snapshot();
    CHECK((selective?stats.row_validation.calls:stats.row_decode.calls)==large.size()-1);
  }
  fail_read=false;failure_needle.clear();
  // No complete or partially built snapshot survived any failed scan.
  warm_cycle(db,expected(large),large.size(),budget,selective,0);
}
void filtered_build(const std::filesystem::path& path,bool packed,bool selective) {
  for(int group:{100,1}) {
    Database db(path.string()+"-"+std::to_string(group),settings(packed,selective,4096));seed(db);
    const auto sql=statement(0,0,group,2);
    const auto answer=expected(initial,0,0,group,2);
    for(int phase=0;phase<3;++phase) {
      const auto r=query(db,sql);CHECK(r.rows==answer);
      CHECK(r.info.snapshot_rows_examined==(phase==2?initial.size():0));
      CHECK(r.info.snapshot_bytes==(phase==0?0:4096));
      CHECK(r.info.vector_index_entries_examined==0);
      full_validation(r,selective,initial.size());
    }
    const auto all=query(db);CHECK(all.rows==expected());
    CHECK(all.info.snapshot_rows_examined==initial.size());
    full_validation(all,selective,initial.size());
    const auto changed=query(db,statement(3,4,0,1));
    CHECK(changed.rows==expected(initial,3,4,0,1));
    CHECK(changed.info.snapshot_rows_examined==initial.size());
    full_validation(changed,selective,initial.size());
  }
}
void disabled_snapshot(const std::filesystem::path& path,bool packed,bool selective) {
  auto o=settings(packed,selective,4096);o.table_scan_snapshot=false;
  Database db(path,o);seed(db);
  for(int attempt=0;attempt<3;++attempt) {
    const auto r=query(db);
    CHECK(r.rows==expected());CHECK(r.info.snapshot_rows_examined==0);
    CHECK(r.info.snapshot_bytes==0);CHECK(r.info.vector_index_entries_examined==0);
    full_validation(r,selective,initial.size());
  }
}
void failed_update(const std::filesystem::path& path,bool packed,bool selective,bool commit) {
  auto o=settings(packed,selective,65536);o.batch_writes=false;
  o.index_cache_bytes=0;o.data_cache_bytes=0;o.packed_scan_cache_bytes=0;
  auto original=initial;
  for(auto& item:original)item.label=std::string(5000,char('a'+item.group));
  auto prior=original;prior.front().group=42;
  const auto& final_rows=commit?prior:original;
  {
    Database db(path,o);seed(db,original);
    warm_cycle(db,expected(original),original.size(),65536,selective,0);
    db.execute("BEGIN");
    db.execute("UPDATE t SET g=42 WHERE id=-12");
    warm_cycle(db,expected(prior),prior.size(),65536,selective,retained_bytes(65536));

    failure_needle=encode_row(fixture_schema(),stored_row(prior.back())).substr(0,64);
    fail_read=true;failed_reads=0;
    bool failed=false;
    try {query(db,"UPDATE t SET g=77,label='must roll back'");}
    catch(const std::system_error& error) {CHECK(error.code().value()==EIO);failed=true;}
    fail_read=false;failure_needle.clear();
    const auto stats=profile::snapshot();
    CHECK(failed);CHECK(failed_reads==1);CHECK(db.in_transaction());
    // DML eagerly decodes regardless of selective_decode. Four rows were
    // decoded and immediate writes reached the file before row five failed.
    CHECK(stats.row_decode.calls==prior.size()-1);
    CHECK(stats.pwrite.calls>0);CHECK(stats.pwrite.bytes>0);
    // Statement rollback restores all four modified rows, retains the earlier
    // transaction statement, and cannot retain the snapshot from before UPDATE.
    warm_cycle(db,expected(prior),prior.size(),65536,selective,retained_bytes(65536));
    db.execute(commit?"COMMIT":"ROLLBACK");
    CHECK(!db.in_transaction());
    warm_cycle(db,expected(final_rows),final_rows.size(),65536,selective,retained_bytes(65536));
  }
  Database reopened(path,o);
  warm_cycle(reopened,expected(final_rows),final_rows.size(),65536,selective,0);
}
void corrupt_hidden_row(const std::filesystem::path& path,bool packed,bool selective) {
  const auto o=settings(packed,selective,4096);
  {Database db(path,o);seed(db);warm_cycle(db,expected(),initial.size(),4096,selective,0);}
  const auto good=encode_row(fixture_schema(),stored_row(initial.back()));
  for(int kind=0;kind<3;++kind) {
    auto damaged=good;
    if(kind==0) {
      // Nonfinite vector in a filtered-out row: no distance should be
      // calculated for this row, but full row validation must still reject it.
      const std::size_t offset=21;
      damaged[offset]=0;damaged[offset+1]=0;damaged[offset+2]=char(0xc0);damaged[offset+3]=char(0x7f);
    } else if(kind==1) {
      // Valid encoded row but primary key no longer agrees with storage key 99.
      damaged=encode_row(fixture_schema(),stored_row({98,9,12,1,"e"}));
    } else damaged[37]=char(0x80); // Invalid UTF-8 in the unprojected label.
    // Store updates checksums normally; this isolates full SQL row validation.
    {Store store(path);store.begin();store.put_in(1,99,damaged);store.commit();}
    {Database db(path,o);
      for(int attempt=0;attempt<3;++attempt)
        expect_error(ErrorCode::Corruption,[&]{query(db,"SELECT id FROM t WHERE g=0 ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 1");});}
    {Store store(path);store.begin();store.put_in(1,99,good);store.commit();}
    {Database db(path,o);warm_cycle(db,expected(),initial.size(),4096,selective,0);}
  }
}
}

int main() {
  const char* env=std::getenv("MVDB_TEST_DIR");
  const auto root=std::filesystem::path(env?env:"/data")/("unindexed-snapshot-"+std::to_string(getpid()));
  std::filesystem::create_directories(root);
  try {
    for(bool packed:{false,true})for(bool selective:{false,true})for(bool reuse:{false,true}) {
      reuse_arena=reuse;
      const auto suffix=std::to_string(packed)+std::to_string(selective)+std::to_string(reuse);
      reuse_after_write(root/("reuse-after-write-"+suffix),packed,selective);
      for(std::size_t budget:{0U,1U,120U,4096U})
        lifecycle(root/("lifecycle-"+suffix+"-"+std::to_string(budget)),packed,selective,budget);
      filtered_build(root/("filtered-build-"+suffix),packed,selective);
      disabled_snapshot(root/("disabled-"+suffix),packed,selective);
      eligibility(root/("eligibility-"+suffix),packed,selective);
      switching(root/("switching-"+suffix),packed,selective);
      empty_and_cap(root/("empty-"+suffix),packed,selective);
      sink_failure(root/("sink-"+suffix),packed,selective);
      failed_build(root/("failure-"+suffix),packed,selective);
      failed_build(root/("refused-failure-"+suffix),packed,selective,1);
      for(bool commit:{false,true})
        failed_update(root/("update-failure-"+suffix+std::to_string(commit)),packed,selective,commit);
      corrupt_hidden_row(root/("corruption-"+suffix),packed,selective);
    }
  } catch(const std::exception& error) {
    fail_read=false;std::cerr<<error.what()<<'\n';return 1;
  }
  std::filesystem::remove_all(root);
  std::cout<<"unindexed snapshot tests passed\n";
}
