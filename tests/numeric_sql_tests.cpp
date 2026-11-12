#include "minivectordb/database.hpp"
#include "minivectordb/value.hpp"
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <unistd.h>
using namespace minivectordb;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__)); } while(false)
template<class F> void rejects(F f) { bool caught=false; try { f(); } catch(const std::exception&) { caught=true; } CHECK(caught); }
std::vector<Row> query(Database& d,std::string_view sql) {
  std::vector<Row> result; d.execute(sql,{{},[&](const Row& row){result.push_back(row);}}); return result;
}
void maximum(Database& d) {
  const double max=std::numeric_limits<float>::max(), threshold=max+std::ldexp(1.0,103);
  for(double v:{3.4028235e38,std::nextafter(threshold,0.0)}) {
    CHECK(make_vector(std::array<double,2>{v,-v},2)==std::vector<float>({float(max),-float(max)}));
  }
  for(double v:{threshold,-threshold,std::nextafter(threshold,INFINITY)}) rejects([&]{make_vector(std::array<double,1>{v},1);});
  d.execute("CREATE TABLE m (id BIGINT PRIMARY KEY, v VECTOR(2))");
  d.execute("INSERT INTO m VALUES (1,[3.4028235e38,-3.4028235e38])");
  auto original=std::get<std::vector<float>>(query(d,"SELECT v FROM m WHERE id=1")[0][0]);
  std::string literal="[";
  for(std::size_t i=0;i<original.size();++i) {
    std::array<char,64> buffer{}; auto[end,ec]=std::to_chars(buffer.data(),buffer.data()+buffer.size(),original[i],std::chars_format::general);
    CHECK(ec==std::errc{}); if(i)literal+=","; literal.append(buffer.data(),end);
  }
  literal+="]"; d.execute("INSERT INTO m VALUES (2,'"+literal+"')");
  CHECK(std::get<std::vector<float>>(query(d,"SELECT v FROM m WHERE id=2")[0][0])==original);
  d.execute("BEGIN"); d.execute("INSERT INTO m VALUES (3,[1.401298464324817e-45,-1e-50])");
  auto tiny=std::get<std::vector<float>>(query(d,"SELECT v FROM m WHERE id=3")[0][0]);
  CHECK(tiny[0]==std::numeric_limits<float>::denorm_min() && tiny[1]==0 && std::signbit(tiny[1]));
  // Retain the remote parser repair: extreme underflow becomes signed zero.
  d.execute("INSERT INTO m VALUES (4,'[1e-400,-1e-400]')");
  auto zeros=std::get<std::vector<float>>(query(d,"SELECT v FROM m WHERE id=4")[0][0]);
  CHECK(zeros[0]==0 && !std::signbit(zeros[0]) && zeros[1]==0 && std::signbit(zeros[1]));
  for(auto bad:{"[1e400,0]","[3.4028236e38,0]","[nan,0]","[0x1p0,0]","[1x,0]"})
    rejects([&]{d.execute("INSERT INTO m VALUES (5,'"+std::string(bad)+"')");});
  CHECK(query(d,"SELECT id FROM m").size()==4); d.execute("ROLLBACK"); CHECK(query(d,"SELECT id FROM m").size()==2);
  d.execute("BEGIN"); d.execute("UPDATE m SET v=[-3.4028235e38,3.4028235e38] WHERE id=2"); d.execute("COMMIT");
  CHECK(std::get<std::vector<float>>(query(d,"SELECT v FROM m WHERE id=2")[0][0])[0]==-max);
}
void ties(Database& d) {
  d.execute("CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2), tag BIGINT)");
  d.execute("INSERT INTO t VALUES (1,[1,1.490116119384765625e-8],20),(2,[1,0],10),(3,[2,0],30)");
  auto check=[&](std::string_view order,std::initializer_list<std::int64_t> ids) {
    auto rows=query(d,"SELECT id,L2_DISTANCE(v,[0,0]) AS distance FROM t ORDER BY "+std::string(order));
    CHECK(rows.size()==ids.size()); std::size_t i=0;
    for(auto id:ids) {CHECK(std::get<std::int64_t>(rows[i][0])==id); CHECK(std::get<double>(rows[i][1])==(id==3?2:1)); ++i;}
  };
  for(int indexed=0;indexed<2;++indexed) {
    if(indexed)d.execute("CREATE VECTOR INDEX idx ON t(v)");
    check("distance LIMIT 1",{1}); check("distance LIMIT 2",{1,2});
    check("distance",{1,2,3}); check("distance DESC",{3,1,2});
    check("distance, tag DESC",{1,2,3}); check("distance, tag ASC LIMIT 2",{2,1}); check("distance, id DESC",{2,1,3});
  }
  // Force run serialization/merge and check exact returned-distance ties there too.
  std::string batch="INSERT INTO t VALUES ";
  for(int i=10;i<110;++i) { if(i!=10)batch+=","; batch+="("+std::to_string(i)+",[2,0],30)"; }
  d.execute(batch);
  std::vector<Row> spilled;
  auto info=d.execute("SELECT id FROM t ORDER BY L2_DISTANCE(v,[0,0]), tag DESC LIMIT 2",{{},[&](const Row&r){spilled.push_back(r);}});
  CHECK(info.spill_runs>0 && spilled.size()==2);
  CHECK(std::get<std::int64_t>(spilled[0][0])==1 && std::get<std::int64_t>(spilled[1][0])==2);
  d.execute("DELETE FROM t WHERE id>=10");
  // Distinct adjacent outputs stay distinct: no epsilon tie grouping.
  d.execute("INSERT INTO t VALUES (0,[1,2.98023223876953125e-8],0)");
  auto strict=query(d,"SELECT id,L2_DISTANCE(v,[0,0]) AS distance FROM t ORDER BY distance LIMIT 3");
  CHECK(std::get<std::int64_t>(strict[0][0])==1 && std::get<std::int64_t>(strict[1][0])==2 && std::get<std::int64_t>(strict[2][0])==0);
  CHECK(std::get<double>(strict[2][1])>std::get<double>(strict[1][1]));
  d.execute("DELETE FROM t WHERE id=0");
  d.execute("BEGIN"); d.execute("UPDATE t SET v=[0,0] WHERE id=2");
  CHECK(std::get<std::int64_t>(query(d,"SELECT id FROM t ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 1")[0][0])==2);
  d.execute("ROLLBACK"); check("distance LIMIT 1",{1});
  d.execute("BEGIN"); d.execute("UPDATE t SET tag=40 WHERE id=2"); d.execute("COMMIT"); check("distance, tag DESC",{2,1,3});
}
int main(int argc,char** argv) {
  try {
    CHECK(argc==2); std::string mode=argv[1];
    const char* configured=std::getenv("MVDB_TEST_DIR");
    if(!configured)configured=std::getenv("TMPDIR");
    auto base=std::filesystem::path(configured?configured:"/tmp");
    auto dir=base/("mvdb-numeric-"+std::to_string(getpid())+"-"+mode); std::filesystem::create_directories(dir);
    for(bool selective:{false,true}) { Options options; options.sort_directory=dir/"sort"; options.memory_budget_bytes=4096; options.selective_decode=selective;
      Database d(dir/(selective?"selective.db":"full.db"),options);
      if(mode=="maximum")maximum(d); else if(mode=="ties")ties(d); else throw std::runtime_error("unknown mode");
    }
    std::filesystem::remove_all(dir); std::cout<<"PASS numeric "<<mode<<"\n";
  } catch(const std::exception& e) {std::cerr<<e.what()<<"\n"; return 1;}
}
