// Same failure/recovery oracles as adversarial_storage_tests.cpp (f62ce8a7).
// A short-write injection must write fewer than n bytes even for a 48-byte
// tombstone; the legacy fixture assumes page-sized calls.
#include "minivectordb/database.hpp"
#include "minivectordb/store.hpp"
#include <filesystem>
#include <iostream>
#include <map>
#include <functional>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <cstdlib>
#include <algorithm>
// Test-only linker wrappers inject bounded failures into this process alone.
using namespace minivectordb;
namespace fs=std::filesystem;
static int fail_write=0, fail_sync=0, writes=0, syncs=0, mode=0;
static bool armed=false, pending=false;
static size_t partial_size=137;
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t off) {
 if(armed) {
  if(pending) {pending=false;errno=EIO;return -1;}
  if(++writes==fail_write) {
   if(mode==1) {pending=true;return __real_pwrite(fd,p,std::min(n/2,partial_size),off);}
   if(mode==2) {errno=EINTR;return -1;}
   if(mode==3)return 0;
   errno=EIO;return -1;
  }
 }
 return __real_pwrite(fd,p,n,off);
}
extern "C" int __wrap_fsync(int fd) {
 if(armed && ++syncs==fail_sync) {errno=EIO;return -1;}
 return __real_fsync(fd);
}
void arm(int w=0,int s=0,int m=0,size_t cut=137) {partial_size=cut;writes=syncs=0;fail_write=w;fail_sync=s;mode=m;pending=false;armed=true;}
void disarm() {armed=false;pending=false;}
void check(bool ok,const char* where) {if(!ok)throw std::runtime_error(where);}
template<class F> bool throws(F f) {try{f();return false;}catch(const std::exception&){return true;}}
using Map=std::map<int64_t,std::string>;
void equal(Store& s,const Map& m) {
 Map got;s.scan_in(7,[&](auto k,auto v){check(got.emplace(k,v).second,"duplicate key");});check(got==m,"store oracle mismatch");
 for(const auto& [k,v]:m)check(s.get_in(7,k)==v,"point oracle mismatch");
}
int cases=0;
void storage(const fs::path& d) {
 const auto seed=d/"store-seed";
 const Map base{{INT64_MIN,"min"},{0,"zero"},{1,"one"},{INT64_MAX,"max"}};
 {Store s(seed,65536,true,16384,65536);s.begin();for(auto& [k,v]:base)s.put_in(7,k,v);s.commit();}
 std::vector<Store::Mutation> batch{{0,std::string(9000,'x')},{1,"replacement"},{2,"two"},{INT64_MIN,std::nullopt}};
 auto operation=[&](Store& s,int kind){if(kind==0)s.put_in(7,2,std::string(9000,'z'));if(kind==1)s.apply_batch_in(7,batch);if(kind==2)s.erase_in(7,0);};
 for(int kind=0;kind<3;++kind) {
  int count=0;
  auto measure=d/"measure";fs::copy_file(seed,measure,fs::copy_options::overwrite_existing);
  {Store s(measure,65536,true,16384,65536);s.begin();arm();operation(s,kind);disarm();count=writes;s.rollback();}
  std::cout<<"store kind="<<kind<<" writepoints="<<count<<std::endl;
  for(int m:{0,1,2,3})for(int n=1;n<=count;++n) {
   auto path=d/"case";fs::copy_file(seed,path,fs::copy_options::overwrite_existing);
   auto prior=base;prior[-100]="earlier transaction write";
   auto changed=prior;
   if(kind==0)changed[2]=std::string(9000,'z');
   else if(kind==1) {
    changed[0]=std::string(9000,'x');changed[1]="replacement";
    changed[2]="two";changed.erase(INT64_MIN);
   } else changed.erase(0);
   {Store s(path,65536,true,16384,65536);s.begin();s.put_in(7,-100,prior[-100]);auto sp=s.savepoint();
    arm(n,0,m);bool failed=throws([&]{operation(s,kind);});disarm();
    check(failed==(m!=2),"mutation failure expectation");
    if(failed)equal(s,prior);
    else equal(s,changed);
    s.restore(sp);equal(s,prior);
    // Reuse partially written address, then restore and commit earlier statement.
    operation(s,kind);equal(s,changed);s.restore(sp);equal(s,prior);s.commit();}
   {Store s(path,65536,true,16384,65536);equal(s,prior);}
   ++cases;
  }
 }
 // Failed commit is uncertain; all later data operations must be rejected.
 for(int w:{0,1})for(int sy:{0,1,2})for(int m:{0,1,3}) {
  if((w==0)==(sy==0))continue;
  for(size_t cut:{17u,25u,33u,137u}) {
   if(cut!=137 && !(w==1 && m==1))continue;
  auto path=d/"commit-case";fs::copy_file(seed,path,fs::copy_options::overwrite_existing);
  {Store s(path);s.begin();s.put_in(7,2,"new");s.erase_in(7,0);arm(w,sy,m,cut);bool failed=throws([&]{s.commit();});disarm();check(failed,"commit must fail");
   check(throws([&]{s.get_in(7,0);}),"poison get");check(throws([&]{s.rollback();}),"poison rollback");check(throws([&]{s.begin();}),"poison begin");}
  {Store s(path);auto next=base;next.erase(0);next[2]="new";Map got;s.scan_in(7,[&](auto k,auto v){got.emplace(k,v);});check(got==base||got==next,"commit reopened mixed generations");}
  ++cases;
  }
 }
}
using SqlMap=std::map<int64_t,std::pair<std::string,std::vector<float>>>;
void sql_equal(Database& db,const SqlMap& oracle) {
 SqlMap got;db.execute("SELECT id,n,v FROM t",{.row=[&](const Row& r){check(r.size()==3,"SQL row width");check(got.emplace(std::get<int64_t>(r[0]),std::make_pair(std::get<std::string>(r[1]),std::get<std::vector<float>>(r[2]))).second,"duplicate SQL row");}});check(got==oracle,"SQL row oracle mismatch");
 std::vector<int64_t> result;db.execute("SELECT id FROM t ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 200",{.row=[&](const Row& r){result.push_back(std::get<int64_t>(r[0]));}});
 std::vector<int64_t> expected;for(auto& [key,value]:oracle)expected.push_back(key);
 // Every seed uses v=[key,0]; nonnegative distinct keys order by distance exactly.
 check(result==expected,"SQL vector index oracle mismatch");
}
void sql(const fs::path& d) {
 const auto seed=d/"sql-seed";SqlMap original;
 Options opt;opt.data_cache_bytes=65536;opt.index_cache_bytes=65536;opt.scan_snapshot_bytes=65536;
 {Database db(seed,opt);db.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,n VARCHAR(20),v VECTOR(2))");
  std::string q="INSERT INTO t VALUES ";for(int i=0;i<140;++i){if(i)q+=",";q+="("+std::to_string(i)+",'old',["+std::to_string(i)+",0])";original[i]={"old",{float(i),0}};}db.execute(q);
  db.execute("CREATE VECTOR INDEX first ON t(v)");db.execute("CREATE VECTOR INDEX second ON t(v)");}
 for(auto statement:{"UPDATE t SET n='new',v=[999,0]", "DELETE FROM t", "CREATE VECTOR INDEX third ON t(v)"}) {
  auto measure=d/"sql-measure";fs::copy_file(seed,measure,fs::copy_options::overwrite_existing);int count=0;
  {Database db(measure,opt);db.execute("BEGIN");db.execute("INSERT INTO t VALUES (200,'prior',[200,0])");arm();db.execute(statement);disarm();count=writes;db.execute("ROLLBACK");}
  std::cout<<statement<<" writepoints="<<count<<std::endl;
  // Check every write boundary with partial pwrite followed by EIO, inside explicit tx.
  for(int n=1;n<=count;++n) {
   auto path=d/"sql-case";fs::copy_file(seed,path,fs::copy_options::overwrite_existing);auto prior=original;prior[200]={"prior",{200,0}};
   {Database db(path,opt);db.execute("BEGIN");db.execute("INSERT INTO t VALUES (200,'prior',[200,0])");sql_equal(db,prior);
    arm(n,0,1);bool failed=throws([&]{db.execute(statement);});disarm();check(failed,"SQL statement must fail");check(db.in_transaction(),"SQL prior tx retained");sql_equal(db,prior);
    // Repeat another failed statement; the earlier transaction write must survive both.
    arm(1,0,0);check(throws([&]{db.execute("DELETE FROM t");}),"nested failure");disarm();sql_equal(db,prior);db.execute("COMMIT");}
   {Database db(path,opt);sql_equal(db,prior);}
   ++cases;
  }
 }
}
int main() {
 try {
  const char* configured=std::getenv("MVDB_TEST_DIR");check(configured,"MVDB_TEST_DIR required");
  const fs::path base(configured);fs::create_directories(base);
  auto name=(base/"adversarial-storage-XXXXXX").string();check(mkdtemp(name.data()),"unique test directory");
  const fs::path directory(name);
  storage(directory);sql(directory);fs::remove_all(directory);
  std::cout<<"PASS adversarial storage cases="<<cases<<std::endl;
 } catch(const std::exception& e) {
  disarm();std::cerr<<"FAIL "<<e.what()<<" after cases="<<cases<<std::endl;return 1;
 }
}
