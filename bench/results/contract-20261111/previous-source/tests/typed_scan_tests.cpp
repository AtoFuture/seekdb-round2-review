#include "minivectordb/database.hpp"
#include "minivectordb/profile.hpp"
#include "minivectordb/row_codec.hpp"
#include "../src/sql/typed_scan_cache.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <limits>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>

using namespace minivectordb;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__)); } while(false)

namespace {
bool fail_read=false;
std::string failure_needle;
unsigned failed_reads=0;
}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* buffer,size_t bytes,off_t offset) {
  const auto result=__real_pread(fd,buffer,bytes,offset);
  if(fail_read&&result>0&&!failure_needle.empty()) {
    const auto* begin=static_cast<const char*>(buffer);const auto* end=begin+result;
    if(std::search(begin,end,failure_needle.begin(),failure_needle.end())!=end) {
      ++failed_reads;errno=EIO;return -1;
    }
  }
  return result;
}

namespace {
struct Item {std::int64_t id,g,h;std::vector<float> v;std::string label;};
struct Result {std::vector<Row> rows;ExecutionInfo info;profile::Snapshot stats;};
using Filter=std::function<bool(const Item&)>;
Options settings(std::size_t budget=65536,bool packed=true,bool selective=true) {
  Options o;o.typed_scan_cache=true;o.scan_snapshot_bytes=budget;
  o.packed_values=packed;o.selective_decode=selective;return o;
}
std::string literal(const std::vector<float>& values) {
  std::ostringstream out;out<<std::setprecision(9)<<'[';
  for(std::size_t i=0;i<values.size();++i) {if(i)out<<',';out<<values[i];}
  out<<']';return out.str();
}
std::vector<Item> fixture(std::size_t dimension=3) {
  std::vector<Item> items;
  for(int i=0;i<24;++i) {
    std::vector<float> v(dimension);
    for(std::size_t d=0;d<dimension;++d)v[d]=float((i%7)-3)*float(1+d%3)/4.0f;
    items.push_back({std::int64_t(i)*100003-1200031,i%4,(i%5)-2,std::move(v),"body"+std::to_string(i)});
  }
  std::reverse(items.begin(),items.end());return items;
}
void seed(Database& db,const std::vector<Item>& items,const std::string& table="t") {
  CHECK(!items.empty());
  db.execute("CREATE TABLE "+table+"(id BIGINT PRIMARY KEY,v VECTOR("+
    std::to_string(items.front().v.size())+"),g BIGINT,h BIGINT,label VARCHAR(6000))");
  std::string sql="INSERT INTO "+table+" VALUES ";
  for(std::size_t n=0;n<items.size();++n) {
    if(n)sql+=',';const auto& i=items[n];
    sql+='('+std::to_string(i.id)+",'"+literal(i.v)+"',"+std::to_string(i.g)+','+
      std::to_string(i.h)+",'"+i.label+"')";
  }
  db.execute(sql);
}
std::string statement(const std::vector<float>& q,const std::string& filter="",std::size_t k=5,
                      const std::string& table="t") {
  return "SELECT id,L2_DISTANCE(v,'"+literal(q)+"') AS d,g,h,label FROM "+table+
    (filter.empty()?"":" WHERE "+filter)+" ORDER BY d LIMIT "+std::to_string(k);
}
Result query(Database& db,const std::string& sql,SearchMode mode=SearchMode::Exact) {
  Result result;const bool old=profile::enabled();profile::set_enabled(true);profile::reset();
  try {result.info=db.execute(sql,{{},[&](const Row& row){result.rows.push_back(row);}},mode);}
  catch(...) {profile::set_enabled(old);throw;}
  result.stats=profile::snapshot();profile::set_enabled(old);return result;
}
// Independent oracle: arithmetic and full sorting do not call engine TopK or codecs.
std::vector<Row> oracle(const std::vector<Item>& items,const std::vector<float>& q,
                        const Filter& filter={},std::size_t k=5) {
  std::vector<Row> rows;
  for(const auto& item:items) {
    if(filter&&!filter(item))continue;
    CHECK(q.size()==item.v.size());double squared=0;
    for(std::size_t n=0;n<q.size();++n) {const double delta=double(q[n])-double(item.v[n]);squared+=delta*delta;}
    rows.push_back({item.id,std::sqrt(squared),item.g,item.h,item.label});
  }
  std::sort(rows.begin(),rows.end(),[](const Row& a,const Row& b) {
    const auto da=std::get<double>(a[1]),db=std::get<double>(b[1]);
    return da<db || (da==db&&std::get<std::int64_t>(a[0])<std::get<std::int64_t>(b[0]));
  });
  if(rows.size()>k)rows.resize(k);return rows;
}
void basic(const std::filesystem::path& path,bool indexed) {
  Database db(path,settings());const auto items=fixture();seed(db,items);
  if(indexed)db.execute("CREATE VECTOR INDEX tv ON t(v)");
  const std::vector<float> q(3,0);
  // Both initial reads reject every row. Publishing only the rows that match the
  // builder's predicate would make the subsequent different filter incorrect.
  const auto cold=query(db,statement(q,"g=99"));CHECK(cold.rows.empty());
  const auto build=query(db,statement(q,"g=99"));CHECK(build.rows.empty());
  CHECK(cold.stats.row_validation.calls==items.size());
  CHECK(build.stats.row_validation.calls==items.size());
  const auto warm=query(db,statement(q,"g=1"));
  CHECK(warm.rows==oracle(items,q,[](const Item& i){return i.g==1;}));
  CHECK(warm.info.affected_rows==warm.rows.size());
  // Intended RED on 524133: repeated full validation is a missing optimization,
  // not an incorrect SQL answer. Only projected winners need row decoding here.
  CHECK(warm.stats.row_validation.calls<=warm.rows.size());
  CHECK(warm.stats.predicate_evaluation.calls==items.size());
  CHECK(warm.stats.scan_consume.calls==items.size());
  CHECK(warm.info.typed_rows_examined==items.size());
  CHECK(warm.info.typed_cache_bytes>0&&warm.info.typed_cache_bytes<=65536);
  CHECK(warm.info.snapshot_bytes==0);
  CHECK(query(db,statement(q)).rows==oracle(items,q));
}
std::uint64_t validations(const Result& r) {return r.stats.row_validation.calls+r.stats.row_decode.calls;}
template<class F> void expect_error(ErrorCode code,F&& f) {
  bool rejected=false;
  try {f();}catch(const Error& error){CHECK(error.code()==code);rejected=true;}
  CHECK(rejected);
}
Result warm(Database& db,const std::vector<Item>& items,const std::vector<float>& q,
            const std::string& filter="",const Filter& predicate={},bool cached=true,
            const std::string& table="t",std::size_t k=5) {
  Result r;
  for(int phase=0;phase<3;++phase) {
    r=query(db,statement(q,filter,k,table));CHECK(r.rows==oracle(items,q,predicate,k));
    CHECK(r.info.affected_rows==r.rows.size());
  }
  if(cached) {
    CHECK(r.info.typed_rows_examined==items.size());
    CHECK(validations(r)<=r.rows.size());CHECK(r.info.snapshot_bytes==0);
  } else {
    CHECK(r.info.typed_rows_examined==0);
    CHECK(validations(r)==items.size()+r.rows.size());
  }
  return r;
}
void predicate_matrix(const std::filesystem::path& path,bool indexed,bool selective) {
  Database db(path,settings(65536,true,selective));const auto items=fixture();seed(db,items);
  if(indexed)db.execute("CREATE VECTOR INDEX tv ON t(v)");
  const std::vector<float> q{0.5f,-1.0f,0.25f};warm(db,items,q);
  const std::vector<std::pair<std::string,Filter>> cases{
    {"g=2",[](const Item& i){return i.g==2;}},{"g<2",[](const Item& i){return i.g<2;}},
    {"g<=2",[](const Item& i){return i.g<=2;}},{"g>2",[](const Item& i){return i.g>2;}},
    {"g>=2",[](const Item& i){return i.g>=2;}},{"g=99",[](const Item& i){return i.g==99;}},
    {"h>=-1 AND g<3",[](const Item& i){return i.h>=-1&&i.g<3;}},
    {"id<0 AND h<=0",[](const Item& i){return i.id<0&&i.h<=0;}},
    {"g>=-9223372036854775808 AND h<=9223372036854775807",[](const Item&){return true;}}};
  for(const auto& [sql,predicate]:cases) {
    const auto r=query(db,statement(q,sql));CHECK(r.rows==oracle(items,q,predicate));
    CHECK(r.info.typed_rows_examined==items.size());CHECK(validations(r)<=r.rows.size());
  }
  CHECK(query(db,statement(q),SearchMode::Ann).rows==oracle(items,q));
  const std::vector<float> changed{-3,2,1};
  const auto change=query(db,statement(changed,"",17));CHECK(change.rows==oracle(items,changed,{},17));
  CHECK(change.info.typed_rows_examined==items.size());
}
void lifecycle(const std::filesystem::path& path,bool indexed,bool packed) {
  const auto original=fixture();auto committed=original;const std::vector<float> q(3,0);
  const auto o=settings(65536,packed);
  {
    Database db(path,o);seed(db,original);
    if(indexed)db.execute("CREATE VECTOR INDEX tv ON t(v)");
    warm(db,original,q);
    db.execute("BEGIN");
    auto changed=original;
    const auto id=changed.front().id;
    db.execute("UPDATE t SET v='[0,0,0]',g=99,label='updated' WHERE id="+std::to_string(id));
    changed.front().v={0,0,0};changed.front().g=99;changed.front().label="updated";
    const auto after_write=query(db,statement(q));CHECK(after_write.info.typed_rows_examined==0);
    CHECK(after_write.rows==oracle(changed,q));CHECK(validations(after_write)==changed.size()+after_write.rows.size());
    warm(db,changed,q);
    db.execute("DELETE FROM t WHERE id="+std::to_string(changed.back().id));changed.pop_back();
    warm(db,changed,q);
    db.execute("INSERT INTO t VALUES(-9000000,'[0,0,0]',1,-1,'new')");
    changed.push_back({-9000000,1,-1,{0,0,0},"new"});warm(db,changed,q);
    expect_error(ErrorCode::Constraint,[&]{db.execute("INSERT INTO t VALUES(9000000,'[0,0,0]',1,1,'partial'),("+
      std::to_string(id)+",'[0,0,0]',1,1,'duplicate')");});
    CHECK(db.in_transaction());CHECK(query(db,statement(q)).info.typed_rows_examined==0);warm(db,changed,q);
    expect_error(ErrorCode::Schema,[&]{db.execute("UPDATE t SET missing=1");});
    CHECK(query(db,statement(q)).info.typed_rows_examined==0);warm(db,changed,q);
    db.execute("ROLLBACK");CHECK(!db.in_transaction());warm(db,original,q);
    expect_error(ErrorCode::Transaction,[&]{db.execute("ROLLBACK");});
    CHECK(query(db,statement(q)).info.typed_rows_examined==0);warm(db,original,q);
    db.execute("BEGIN");db.execute("INSERT INTO t VALUES(-9000000,'[0,0,0]',1,-1,'committed')");
    committed.push_back({-9000000,1,-1,{0,0,0},"committed"});warm(db,committed,q);
    db.execute("COMMIT");CHECK(query(db,statement(q)).info.typed_rows_examined==0);warm(db,committed,q);
    if(!indexed) {db.execute("CREATE VECTOR INDEX tv ON t(v)");warm(db,committed,q);}
    expect_error(ErrorCode::Schema,[&]{db.execute("CREATE VECTOR INDEX bad ON t(label)");});
    CHECK(query(db,statement(q)).info.typed_rows_examined==0);warm(db,committed,q);
  }
  {Database db(path,o);CHECK(query(db,statement(q)).info.typed_rows_examined==0);warm(db,committed,q);}
  {Database db(path,o);db.execute("BEGIN");db.execute("DELETE FROM t");CHECK(query(db,statement(q)).rows.empty());}
  {Database db(path,o);warm(db,committed,q);}
}
void budgets(const std::filesystem::path& directory) {
  const auto items=fixture();const std::vector<float> q(3,0);
  for(std::size_t budget:{0U,1U,300U,65536U,16U*1024U*1024U})for(bool indexed:{false,true}) {
    const bool fits=budget>=65536;
    Database db(directory/(std::to_string(budget)+"-"+std::to_string(indexed)),settings(budget));seed(db,items);
    if(indexed)db.execute("CREATE VECTOR INDEX tv ON t(v)");
    const auto r=warm(db,items,q,"",{},fits);
    CHECK(r.info.typed_cache_bytes<=std::min(budget,std::size_t(8*1024*1024)));
    CHECK(r.info.typed_cache_bytes+r.info.snapshot_bytes<=std::min(budget,std::size_t(8*1024*1024)));
    if(!fits)CHECK(r.info.typed_cache_bytes==0);
  }
  auto o=settings();o.typed_scan_cache=false;
  Database reference(directory/"disabled",o);seed(reference,items);warm(reference,items,q,"",{},false);
}
void compatibility(const std::filesystem::path& path) {
  Database db(path,settings());const auto items=fixture();seed(db,items);
  const std::vector<float> q(3,0);warm(db,items,q);
  const auto string_filter=query(db,statement(q,"label>='body20'"));
  CHECK(string_filter.rows==oracle(items,q,[](const Item& i){return i.label>="body20";}));
  CHECK(string_filter.info.typed_rows_examined==0);CHECK(string_filter.info.typed_cache_bytes==0);
  CHECK(validations(string_filter)==items.size()+string_filter.rows.size());
  warm(db,items,q);
  const auto key=items.front().id;
  const auto point=query(db,statement(q,"id="+std::to_string(key)));
  CHECK(point.rows==oracle(items,q,[&](const Item&i){return i.id==key;}));
  CHECK(point.info.typed_rows_examined==0);
  CHECK(query(db,"SELECT id FROM t LIMIT 1").rows.size()==1);
  CHECK(query(db,statement(q,"",0)).rows.empty());
  auto other=items;for(auto& i:other){i.g=42;i.label="other";i.v[0]+=5;}
  seed(db,other,"u");warm(db,other,q,"",{},true,"u");
  warm(db,items,q);warm(db,other,q,"g=42",[](const Item&i){return i.g==42;},true,"u");
  const auto smaller=fixture(1);seed(db,smaller,"one");warm(db,smaller,{0},"",{},true,"one");
  warm(db,items,q);
}
void dimensions_and_extremes(const std::filesystem::path& directory) {
  for(std::size_t dimension:{1U,2U,3U,7U,8U,16U,128U,768U}) {
    auto items=fixture(dimension);const std::vector<float> q(dimension,0);
    Database db(directory/std::to_string(dimension),settings(262144));seed(db,items);warm(db,items,q);
    db.execute("CREATE VECTOR INDEX tv ON t(v)");warm(db,items,q);
  }
  const float max=std::numeric_limits<float>::max(),tiny=std::numeric_limits<float>::denorm_min();
  std::vector<Item> items{
    {4,1,0,{max,-max},"max"},{-9,1,0,{-max,max},"neg"},
    {10,1,0,{tiny,-tiny},"tiny"},{11,1,0,{-0.0f,0.0f},"zero"},
    {-1,1,0,{1.0f,std::ldexp(1.0f,-26)},"rounded"},{2,1,0,{1.0f,0},"one"},
    {-2,1,0,{1.0f,std::ldexp(1.0f,-25)},"adjacent"}};
  Database db(directory/"extremes",settings());seed(db,items);
  for(bool indexed:{false,true}) {
    if(indexed)db.execute("CREATE VECTOR INDEX tv ON t(v)");
    warm(db,items,{0,0},"",{},true,"t",7);
    const auto r=query(db,statement({max,max},"",7));CHECK(r.rows==oracle(items,{max,max},{},7));
    const auto zero=query(db,"SELECT v FROM t WHERE id=11");
    CHECK(std::signbit(std::get<std::vector<float>>(zero.rows[0][0])[0]));
  }
}
void wide_schema(const std::filesystem::path& path) {
  Database db(path,settings());
  std::string create="CREATE TABLE wide(v VECTOR(3),label VARCHAR(40),id BIGINT PRIMARY KEY";
  for(int c=0;c<60;++c)create+=",c"+std::to_string(c)+" BIGINT";db.execute(create+")");
  std::string insert="INSERT INTO wide VALUES ";
  for(int i=0;i<12;++i) {
    if(i)insert+=',';
    insert+="('["+std::to_string(i)+",0,0]','wide',"+std::to_string(i-6);
    for(int c=0;c<60;++c)insert+=','+std::to_string(i+c);
    insert+=')';
  }
  db.execute(insert);
  const std::string sql="SELECT id,L2_DISTANCE(v,'[0,0,0]') AS d,c59,label FROM wide WHERE c59>=65 AND c30<40 ORDER BY d LIMIT 3";
  const std::vector<Row> expected{{std::int64_t(0),6.0,std::int64_t(65),std::string("wide")},
    {std::int64_t(1),7.0,std::int64_t(66),std::string("wide")},{std::int64_t(2),8.0,std::int64_t(67),std::string("wide")}};
  for(int phase=0;phase<4;++phase) {
    const auto r=query(db,sql);CHECK(r.rows==expected);
    if(phase>=2){CHECK(r.info.typed_rows_examined==12);CHECK(validations(r)==3);}
  }
}
void sink_and_empty(const std::filesystem::path& path) {
  Database db(path,settings());const auto items=fixture();seed(db,items);const std::vector<float> q(3,0);
  query(db,statement(q));
  for(int attempt=0;attempt<2;++attempt) {
    bool threw=false;
    try {db.execute(statement(q),{{},[](const Row&){throw std::runtime_error("sink stopped");}});}
    catch(const std::runtime_error& error){threw=std::string(error.what())=="sink stopped";}
    CHECK(threw);warm(db,items,q);
  }
  db.execute("DELETE FROM t");
  for(int i=0;i<4;++i)CHECK(query(db,statement(q)).rows.empty());
  db.execute("INSERT INTO t VALUES(-1,'[1,0,0]',2,-2,'after empty')");
  warm(db,{{-1,2,-2,{1,0,0},"after empty"}},q);
}
void corruption_before_load(const std::filesystem::path& path) {
  const auto items=fixture();const auto& damaged_item=items.front();const auto o=settings();
  {Database db(path,o);seed(db,items);warm(db,items,{0,0,0});}
  const Schema schema{1,"t",{{"id",ColumnType::Bigint,0},{"v",ColumnType::Vector,3},
    {"g",ColumnType::Bigint,0},{"h",ColumnType::Bigint,0},{"label",ColumnType::Varchar,6000}},0};
  const Row row{damaged_item.id,damaged_item.v,damaged_item.g,damaged_item.h,damaged_item.label};
  const auto good=encode_row(schema,row);
  for(int kind=0;kind<3;++kind) {
    auto bad=good;
    if(kind==0){bad[17]=0;bad[18]=0;bad[19]=char(0xc0);bad[20]=char(0x7f);}
    else if(kind==1)bad.back()=char(0x80);
    else {auto wrong=row;wrong[0]=damaged_item.id+1;bad=encode_row(schema,wrong);}
    {Store store(path);store.begin();store.put_in(1,damaged_item.id,bad);store.commit();}
    {Database db(path,o);
      for(int attempt=0;attempt<3;++attempt)expect_error(ErrorCode::Corruption,[&]{query(db,statement({0,0,0},"g=99"));});}
    {Store store(path);store.begin();store.put_in(1,damaged_item.id,good);store.commit();}
    {Database db(path,o);warm(db,items,{0,0,0});}
  }
}
Schema fixture_schema() {
  return {1,"t",{{"id",ColumnType::Bigint,0},{"v",ColumnType::Vector,3},
    {"g",ColumnType::Bigint,0},{"h",ColumnType::Bigint,0},{"label",ColumnType::Varchar,6000}},0};
}
Row stored_row(const Item& i) {return {i.id,i.v,i.g,i.h,i.label};}
Options fault_settings() {
  auto o=settings(65536,false);o.index_cache_bytes=0;o.data_cache_bytes=0;
  o.packed_scan_cache_bytes=0;o.batch_writes=false;return o;
}
std::vector<Item> fault_fixture() {
  auto rows=fixture();for(auto& row:rows)row.label=std::string(5000,char('a'+row.g));return rows;
}
void fail_on_last(const std::vector<Item>& items) {
  const auto last=std::max_element(items.begin(),items.end(),[](const Item& a,const Item& b){return a.id<b.id;});
  failure_needle=encode_row(fixture_schema(),stored_row(*last)).substr(0,64);
  fail_read=true;failed_reads=0;
}
void failed_build(const std::filesystem::path& path,bool indexed) {
  Database db(path,fault_settings());const auto items=fault_fixture();seed(db,items);
  if(indexed)db.execute("CREATE VECTOR INDEX tv ON t(v)");
  CHECK(query(db,statement({0,0,0})).rows==oracle(items,{0,0,0})); // probation
  fail_on_last(items);
  for(unsigned attempt=0;attempt<2;++attempt) {
    bool failed=false;
    try {query(db,statement({0,0,0}));}
    catch(const std::system_error& error){CHECK(error.code().value()==EIO);failed=true;}
    CHECK(failed);CHECK(failed_reads==attempt+1);
    CHECK(profile::snapshot().row_validation.calls==items.size()-1);
  }
  fail_read=false;failure_needle.clear();
  const auto recovered=query(db,statement({0,0,0}));
  CHECK(recovered.rows==oracle(items,{0,0,0}));CHECK(recovered.info.typed_rows_examined==0);
  warm(db,items,{0,0,0});
}
void partially_failed_update(const std::filesystem::path& path,bool indexed,bool commit) {
  const auto original=fault_fixture();auto prior=original;
  const auto changed_id=prior.back().id;prior.back().g=42;
  const auto final=commit?prior:original;
  {
    Database db(path,fault_settings());seed(db,original);
    if(indexed)db.execute("CREATE VECTOR INDEX tv ON t(v)");
    warm(db,original,{0,0,0});db.execute("BEGIN");
    db.execute("UPDATE t SET g=42 WHERE id="+std::to_string(changed_id));warm(db,prior,{0,0,0});
    fail_on_last(prior);
    bool failed=false;
    try {query(db,"UPDATE t SET g=88,label='must roll back'");}
    catch(const std::system_error& error){CHECK(error.code().value()==EIO);failed=true;}
    fail_read=false;failure_needle.clear();
    const auto stats=profile::snapshot();
    CHECK(failed);CHECK(failed_reads==1);CHECK(db.in_transaction());
    CHECK(stats.row_decode.calls==prior.size()-1);
    CHECK(stats.pwrite.calls>0&&stats.pwrite.bytes>0);
    const auto after=query(db,statement({0,0,0}));CHECK(after.rows==oracle(prior,{0,0,0}));
    CHECK(after.info.typed_rows_examined==0);warm(db,prior,{0,0,0});
    db.execute(commit?"COMMIT":"ROLLBACK");warm(db,final,{0,0,0});
  }
  Database reopened(path,fault_settings());warm(reopened,final,{0,0,0});
}
void cache_unit_contract() {
  using sql::TypedScanCache;
  const Schema a{17,"a",{{"v",ColumnType::Vector,2},{"g",ColumnType::Bigint,0},{"id",ColumnType::Bigint,0}},2};
  const Schema b{18,"b",{{"id",ColumnType::Bigint,0},{"v",ColumnType::Vector,2},{"g",ColumnType::Bigint,0}},0};
  const std::vector<float> v{1.25f,-0.0f};
  TypedScanCache cache;
  cache.start(a,0,0,512);CHECK(!cache.building());CHECK(cache.bytes()==0);
  cache.start(a,0,0,512);CHECK(cache.building());
  cache.append(v,[](std::size_t c){return std::int64_t(c==2?-11:99);});
  CHECK(!cache.ready(a.id,0,0));cache.finish();
  CHECK(cache.ready(a.id,0,0));CHECK(!cache.ready(b.id,0,0));CHECK(!cache.ready(a.id,1,0));
  CHECK(cache.size()==1);CHECK(cache.key(0)==-11);
  CHECK(cache.scalar(0,cache.scalar_slot(1))==99);CHECK(cache.vector(0)[0]==1.25f);
  CHECK(std::signbit(cache.vector(0)[1]));CHECK(cache.bytes()<=512);
  const auto retained=cache.bytes();cache.invalidate();CHECK(!cache.ready(a.id,0,0));
  CHECK(cache.size()==0);CHECK(cache.bytes()==retained);
  // Same counts/dimension, different column order and primary location: retained
  // allocation is permitted, retained column interpretation is not.
  cache.start(b,77,1,512);CHECK(!cache.building());
  cache.start(b,77,1,512);CHECK(cache.building());
  cache.append(v,[](std::size_t c){return std::int64_t(c==0?123:-77);});cache.finish();
  CHECK(cache.ready(b.id,77,1));CHECK(cache.key(0)==123);
  CHECK(cache.scalar(0,cache.scalar_slot(2))==-77);CHECK(cache.size()==1);
  cache.clear();CHECK(cache.bytes()==0);CHECK(!cache.ready(b.id,77,1));
  cache.start(a,0,0,512);cache.start(a,0,0,512);
  for(int i=0;i<100;++i)cache.append(v,[&](std::size_t){return std::int64_t(i);});
  cache.finish();CHECK(!cache.ready(a.id,0,0));CHECK(cache.bytes()==0);CHECK(cache.size()==0);
  cache.start(a,0,0,512);CHECK(!cache.building()); // refused until identity invalidates
  cache.invalidate();cache.start(a,0,0,512);cache.start(a,0,0,512);cache.finish();
  CHECK(cache.ready(a.id,0,0));CHECK(cache.size()==0); // complete empty snapshot
  auto huge=a;huge.id=19;huge.columns[0].extent=std::numeric_limits<std::uint32_t>::max();
  cache.start(huge,0,0,512);CHECK(cache.bytes()==0);
  cache.start(huge,0,0,512);CHECK(!cache.building());CHECK(cache.bytes()==0);
  cache.start(a,0,0,1);cache.start(a,0,0,1);cache.finish();
  CHECK(!cache.ready(a.id,0,0));CHECK(cache.bytes()==0);

  // The exact two-row boundary counts both numeric arrays AND column metadata.
  // One byte less must never admit the second row or publish its partial prefix.
  constexpr std::size_t metadata=2*sizeof(std::size_t);
  constexpr std::size_t row_bytes=2*sizeof(float)+2*sizeof(std::int64_t);
  for(std::size_t budget:{metadata+2*row_bytes-1,metadata+2*row_bytes}) {
    cache.clear();cache.start(a,0,0,budget);cache.start(a,0,0,budget);
    CHECK(cache.building());CHECK(cache.bytes()<=budget);
    cache.append(v,[](std::size_t c){return std::int64_t(c==2?1:10);});
    cache.append(v,[](std::size_t c){return std::int64_t(c==2?2:20);});
    cache.finish();
    if(budget==metadata+2*row_bytes) {
      CHECK(cache.ready(a.id,0,0));CHECK(cache.size()==2);CHECK(cache.bytes()==budget);
      CHECK(cache.key(0)==1&&cache.key(1)==2);
    } else {
      CHECK(!cache.ready(a.id,0,0));CHECK(cache.size()==0&&cache.bytes()==0);
      for(int repeat=0;repeat<5;++repeat) {
        cache.start(a,0,0,budget);cache.finish();
        CHECK(!cache.building()&&!cache.ready(a.id,0,0)&&cache.bytes()==0);
      }
    }
  }
  // A partially appended row whose scalar getter throws cannot survive the
  // executor's clear-on-build-failure. Later publication must contain only the
  // new complete scan, not stale metadata/counts from the aborted builder.
  cache.invalidate();cache.start(a,0,0,512);cache.start(a,0,0,512);
  cache.append(v,[](std::size_t c){return std::int64_t(c==2?-50:10);});
  bool stopped=false;
  try {
    cache.append(v,[](std::size_t c)->std::int64_t {
      if(c==2)throw std::runtime_error("scalar construction failed");
      return 55;
    });
  } catch(const std::runtime_error& error) {
    CHECK(std::string(error.what())=="scalar construction failed");
    cache.clear();stopped=true;
  }
  CHECK(stopped);cache.finish();CHECK(!cache.ready(a.id,0,0));
  CHECK(cache.bytes()==0&&cache.size()==0);
  cache.start(a,0,0,512);cache.start(a,0,0,512);
  cache.append(v,[](std::size_t c){return std::int64_t(c==2?900:70);});cache.finish();
  CHECK(cache.ready(a.id,0,0));CHECK(cache.size()==1);CHECK(cache.key(0)==900);
}
}
int main() {
  try {
    const char* configured=std::getenv("MVDB_TEST_DIR");
    const auto parent=std::filesystem::path(configured?configured:"/data/core-tests");
    std::filesystem::create_directories(parent);
    std::string pattern=(parent/"typed-scan-XXXXXX").string();
    CHECK(::mkdtemp(pattern.data()));const std::filesystem::path root(pattern);
    cache_unit_contract();
    basic(root/"unindexed.db",false);basic(root/"indexed.db",true);
    for(bool indexed:{false,true})for(bool selective:{false,true})
      predicate_matrix(root/("predicates-"+std::to_string(indexed)+std::to_string(selective)),indexed,selective);
    for(bool indexed:{false,true})for(bool packed:{false,true})
      lifecycle(root/("lifecycle-"+std::to_string(indexed)+std::to_string(packed)),indexed,packed);
    std::filesystem::create_directories(root/"budgets");budgets(root/"budgets");
    compatibility(root/"compatibility");wide_schema(root/"wide");sink_and_empty(root/"sink");
    std::filesystem::create_directories(root/"dimensions");dimensions_and_extremes(root/"dimensions");
    corruption_before_load(root/"corruption");
    for(bool indexed:{false,true}) {
      failed_build(root/("failed-build-"+std::to_string(indexed)),indexed);
      for(bool commit:{false,true})partially_failed_update(root/("failed-update-"+
        std::to_string(indexed)+std::to_string(commit)),indexed,commit);
    }
    std::filesystem::remove_all(root);
    std::cout<<"typed scan tests passed\n";return 0;
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
