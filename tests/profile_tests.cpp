#include "minivectordb/profile.hpp"
#include "minivectordb/database.hpp"
#include "minivectordb/exact.hpp"
#include "minivectordb/row_codec.hpp"
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
using namespace minivectordb;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(#x); } while(false)
std::vector<Row> query(Database& db,std::string_view filter="") {
  std::vector<Row> rows;
  db.execute("SELECT id,L2_DISTANCE(v,[0,0]) FROM t "+std::string(filter)+" ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 2",{{},[&](const Row& r){rows.push_back(r);}});
  return rows;
}
int main() {
 try {
  auto dir=std::filesystem::temp_directory_path()/("mvdb-profile-"+std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  {
   CHECK(!profile::enabled());
   Options options; options.typed_scan_cache=false; // Raw path stage-count contract.
   Database db(dir/"data",options);
   db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2))");
   db.execute("INSERT INTO t VALUES(1,[1,2]),(2,[3,4]),(3,[5,6])");
   auto expected=query(db,"WHERE id>=1");
   CHECK(profile::snapshot()==profile::Snapshot{});
   profile::set_enabled(true); profile::reset();
   db.execute("INSERT INTO t VALUES(4,[7,8])");
   auto write=profile::snapshot();
   CHECK(write.pwrite.calls>0 && write.pwrite.bytes==write.pwrite.calls*4096);
   CHECK(write.fsync.calls>0 && write.page_checksum.calls>0);
   profile::reset(); CHECK(profile::enabled() && profile::snapshot()==profile::Snapshot{});
   CHECK(query(db)==expected);
   auto read=profile::snapshot();
   CHECK(read.pread.calls>0 && read.pread.bytes==read.pread.calls*4096);
   CHECK(read.pwrite.calls==0 && read.fsync.calls==0);
   CHECK(read.page_checksum.calls>0 && read.row_validation.calls==6);
   CHECK(read.row_materialization.calls==6 && read.vector_distance.calls==4);
   CHECK(read.vector_scratch_copy.calls==4 && read.vector_scratch_copy.bytes==32);
   CHECK(read.vector_scratch_copy.nanoseconds>0);
   CHECK(read.page_read_branch.calls>0 && read.page_read_overflow.calls>0);
   CHECK(read.table_scan.calls==1 && read.scan_consume.calls==4);
   CHECK(read.predicate_evaluation.calls==0);
   CHECK(read.table_scan.nanoseconds>=read.scan_consume.nanoseconds);
   CHECK(read.scan_consume.nanoseconds>0);
   bool isolated=false;
   std::thread other([&]{isolated=!profile::enabled() && profile::snapshot()==profile::Snapshot{}; profile::set_enabled(true); profile::reset();});
   other.join(); CHECK(isolated && profile::snapshot()==read);
   profile::set_enabled(false); CHECK(query(db)==expected && profile::snapshot()==read);
   profile::reset(); CHECK(profile::snapshot()==profile::Snapshot{});
   profile::set_enabled(true);
   CHECK(query(db,"WHERE id<4 AND id>=1")==expected);
   const auto filtered=profile::snapshot();
   CHECK(filtered.table_scan.calls==1 && filtered.scan_consume.calls==4);
   // Three rows evaluate both predicates; the fourth short-circuits after one.
   CHECK(filtered.predicate_evaluation.calls==7);
   // This third scan reuses the complete snapshot's validation proof. Only
   // the two projected winners are fetched and validated again.
   CHECK(filtered.row_validation.calls==2 && filtered.vector_distance.calls==3);
   CHECK(filtered.row_materialization.calls==2);
   CHECK(filtered.vector_scratch_copy.calls==3 && filtered.vector_scratch_copy.bytes==24);
   CHECK(filtered.table_scan.nanoseconds>=filtered.scan_consume.nanoseconds);
   CHECK(filtered.predicate_evaluation.nanoseconds>0);
   profile::reset();
   std::vector<float> a{0,0},b{3,4}; CHECK(l2_distance(a,b)==5);
   CHECK(profile::snapshot().vector_distance.calls==1);
   profile::reset();
   Schema schema{1,"t",{{"id",ColumnType::Bigint,0}},0};
   auto encoded=encode_row(schema,{std::int64_t(7)});
   CHECK(decode_row(schema,encoded)==Row{std::int64_t(7)});
   bool rejected=false;
   try { (void)decode_row(schema,encoded+"x"); }
   catch(const Error& e) {rejected=e.code()==ErrorCode::Corruption;}
   CHECK(rejected && profile::snapshot().row_decode.calls==2);
   CHECK(profile::snapshot().row_decode.nanoseconds>0);
   errno=EINTR;
   { profile::detail::Scope timer(&profile::Snapshot::fsync); CHECK(errno==EINTR); errno=EIO; }
   CHECK(errno==EIO);
   profile::set_enabled(false);
  }
  std::filesystem::remove_all(dir);
  std::cout<<"profile tests passed\n";
 } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
