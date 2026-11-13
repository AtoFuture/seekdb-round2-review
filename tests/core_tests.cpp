#include "minivectordb/value.hpp"
#include "minivectordb/exact.hpp"
#include "minivectordb/store.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <sys/resource.h>
#include <csignal>
#include <unistd.h>
using namespace minivectordb;
namespace fs = std::filesystem;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("check failed: ") + #x + " line " + std::to_string(__LINE__)); } while(false)
template<class F> void rejects(F f) { bool caught=false; try { f(); } catch (const std::exception&) { caught=true; } CHECK(caught); }
void types() {
  CHECK(parse_bigint("-9223372036854775808") == INT64_MIN);
  CHECK(parse_bigint("9223372036854775807") == INT64_MAX);
  CHECK(parse_bigint("+42")==42); CHECK(parse_bigint("-0")==0);
  for (auto s: {"", "+", "-", " 1", "1 ", "1x", "1.0", "9223372036854775808", "-9223372036854775809", "++1", "+-1"}) rejects([&]{parse_bigint(s);});
  CHECK(validate_text("A\xE4\xB8\xAD\xF0\x9F\x98\x80",3)==3);
  CHECK(validate_text(std::string("a\0b",3),3)==3);
  CHECK(validate_text("",0)==0);
  rejects([]{validate_text("\xF0\x9F\x98\x80",0);});
  for (auto s: {"\x80", "\xC0\xAF", "\xE0\x80\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xE4\xB8", "\xC2x"}) rejects([&]{validate_text(s,100);});
  CHECK(compare_text("a","aa")<0); CHECK(compare_text("\xEE\x80\x80","\xF0\x90\x80\x80")<0);
  CHECK(compare_text("same","same")==0); rejects([]{compare_text("\x80","a");});
  std::array<double,2> v{1.25,-2}; auto converted=make_vector(v,2);
  CHECK(converted[0]==1.25f && converted[1]==-2);
  rejects([&]{make_vector(v,3);});
  const double maximum=std::numeric_limits<float>::max();
  const double overflow=std::ldexp(1.0,128)-std::ldexp(1.0,103);
  for(double number:{maximum,3.40282347e38,3.4028235e38,std::nextafter(overflow,0.0)}) {
    const std::array<double,1> positive{number},negative{-number};
    CHECK(make_vector(positive,1)[0]==std::numeric_limits<float>::max());
    CHECK(make_vector(negative,1)[0]==-std::numeric_limits<float>::max());
  }
  for(double number:{overflow,std::nextafter(overflow,std::numeric_limits<double>::infinity()),3.4028236e38}) {
    rejects([&]{make_vector(std::array<double,1>{number},1);});
    rejects([&]{make_vector(std::array<double,1>{-number},1);});
  }
  CHECK(std::signbit(make_vector(std::array<double,1>{-0.0},1)[0]));
  CHECK(make_vector(std::array<double,1>{std::ldexp(1.0,-149)},1)[0]==std::numeric_limits<float>::denorm_min());

  for (double bad: {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), 1e100, -1e100}) {
    v[0]=bad; rejects([&]{make_vector(v,2);});
  }
}
void vectors() {
  std::array<float,2> a{0,0}, b{3,4}; CHECK(l2_distance(a,b)==5);
  float max=std::numeric_limits<float>::max();
  std::array<float,1> x{max}, y{-max}; CHECK(std::isfinite(l2_distance(x,y))); CHECK(l2_distance(x,y)==2.0*max);
  rejects([&]{l2_distance(a,x);});
  y[0]=std::numeric_limits<float>::infinity(); rejects([&]{l2_distance(x,y);});
  std::array<std::array<float,2>,5> rows{{{0,0},{1,0},{-1,0},{0,2},{0,1}}};
  std::array<std::int64_t,5> keys{99,INT64_MAX,INT64_MIN,3,4};
  VectorSource source=[&](const VectorVisitor& visit){for(std::size_t i=0;i<rows.size();++i) visit(keys[i],rows[i]);};
  auto top=exact_top_k(a,3,source,[](auto key){return key!=99;});
  CHECK(top.size()==3); CHECK(top[0].key==INT64_MIN && top[1].key==4 && top[2].key==INT64_MAX);
  CHECK(exact_top_k(a,100,source).size()==5);
  CHECK(exact_top_k(a,0,source).empty());
  CHECK(exact_top_k(a,3,[](const auto&){}).empty());
  // sqrt rounds these distinct double squared distances to the same output value.
  const std::array<float,2> near{1,0}, farther{1,std::ldexp(1.0f,-26)};
  VectorSource close_distances=[&](const VectorVisitor& visit) {visit(2,near); visit(1,farther);};
  auto closest=exact_top_k(a,1,close_distances);
  CHECK(closest.size()==1 && closest[0].key==1 && closest[0].distance==1);
  auto close_order=exact_top_k(a,2,close_distances);
  CHECK(close_order.size()==2 && close_order[0].key==1 && close_order[1].key==2);
  CHECK(close_order[0].distance==1 && close_order[1].distance==1);
}
void equal(Store& store,const std::map<std::int64_t,std::string>& expected) {
  auto it=expected.begin();
  store.scan([&](auto k,auto v){CHECK(it!=expected.end()); CHECK(it->first==k); CHECK(it->second==v); ++it;});
  CHECK(it==expected.end());
  for (auto& [k,v]:expected) CHECK(store.get(k)==v);
}
void flip(const fs::path& path,std::uint64_t offset) {
  std::fstream f(path,std::ios::in|std::ios::out|std::ios::binary);
  f.seekg(static_cast<std::streamoff>(offset)); char c=0; f.read(&c,1); CHECK(f.good());
  c^=0x55; f.seekp(static_cast<std::streamoff>(offset)); f.write(&c,1); f.flush(); CHECK(f.good());
}
void child_success(pid_t pid) { int status=0; CHECK(waitpid(pid,&status,0)==pid); CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0); }
fs::path make_test_directory(std::string_view prefix) {
  const auto* configured=std::getenv("MVDB_TEST_DIR");
  const auto base=configured?fs::path(configured):fs::temp_directory_path()/"minivectordb-tests";
  fs::create_directories(base);
  auto pattern=(base/(std::string(prefix)+"-XXXXXX")).string();
  if(::mkdtemp(pattern.data())==nullptr) throw std::runtime_error("cannot create unique test directory");
  return fs::path(pattern);
}
void storage() {
  const auto dir=make_test_directory("run");
  auto path=dir/"main.db";
  Store::Savepoint stale;
  {
    Store s(path); CHECK(!s.get(1)); rejects([&]{s.put(1,"x");}); rejects([&]{s.commit();});
    s.begin(); rejects([&]{s.begin();}); s.put(1,"first"); stale=s.savepoint();
    s.put(2,"second"); s.restore(stale); CHECK(s.get(1)=="first" && !s.get(2));
    s.put(INT64_MIN,std::string(17000,'z')); s.put(INT64_MAX,std::string("a\0b",3)); s.put(0,""); s.commit();
    s.begin(); rejects([&]{s.restore(stale);}); CHECK(s.erase(1)); CHECK(!s.erase(55)); s.rollback(); CHECK(s.get(1)=="first");
    s.begin(); s.put(22,"never published");
  }
  {
    Store s(path); CHECK(!s.get(22)); CHECK(s.get(INT64_MIN)==std::string(17000,'z'));
    CHECK(s.get(INT64_MAX)==std::string("a\0b",3)); CHECK(s.get(0)=="");
    rejects([&]{Store second(path);});
    std::map<std::int64_t,std::string> expected{{INT64_MIN,std::string(17000,'z')},{INT64_MAX,std::string("a\0b",3)},{0,""},{1,"first"}};
    std::mt19937_64 rng(1717);
    s.begin();
    for(int i=0;i<2200;++i) {
      auto key=static_cast<std::int64_t>(rng()%700)-350;
      if(rng()%3==0) { CHECK(s.erase(key)==(expected.erase(key)!=0)); }
      else { auto value=std::to_string(rng()); s.put(key,value); expected[key]=value; }
      if(i%73==0) equal(s,expected);
    }
    equal(s,expected); s.commit();
    s.begin(); for(auto& [key,value]:expected) { (void)value; CHECK(s.erase(key)); } equal(s,{}); s.rollback(); equal(s,expected);
  }
  // Actual process exit without running destructors: published root survives, work root does not.
  auto crash=dir/"crash.db";
  auto pid=fork(); CHECK(pid>=0);
  if(pid==0) { try { Store s(crash); s.begin(); s.put(7,"durable"); s.commit(); s.begin(); s.put(7,"uncommitted"); s.put(8,"absent"); _exit(0); } catch(...) {_exit(1);} }
  child_success(pid);
  { Store s(crash); CHECK(s.get(7)=="durable" && !s.get(8)); }
  fs::resize_file(crash,fs::file_size(crash)-123);
  { Store s(crash); CHECK(s.get(7)=="durable" && !s.get(8)); s.begin(); s.put(9,"after-tail"); s.commit(); }
  { Store s(crash); CHECK(s.get(9)=="after-tail"); }
  // Corrupt newest root slot: recover prior committed generation, never a rolled-back write.
  auto slots=dir/"slots.db";
  { Store s(slots); s.begin(); s.put(1,"old"); s.commit(); s.begin(); s.put(2,"rolled-back"); s.rollback(); s.begin(); s.put(3,"new"); s.commit(); }
  flip(slots,64); // generation 2 is in slot 0
  { Store s(slots); CHECK(s.get(1)=="old" && !s.get(2) && !s.get(3)); }
  flip(slots,4096+64);
  rejects([&]{Store s(slots);});
  auto corrupt=dir/"corrupt.db";
  { Store s(corrupt); s.begin(); s.put(1,"value"); s.commit(); }
  flip(corrupt,fs::file_size(corrupt)-4096+40);
  rejects([&]{Store s(corrupt);});
  auto truncated=dir/"truncated.db"; {std::ofstream f(truncated); f<<"broken";}
  rejects([&]{Store s(truncated);});
  fs::remove_all(dir);
}

void storage_failure() {
  const auto dir=make_test_directory("failure");
  const auto another=make_test_directory("failure");
  CHECK(dir!=another);
  CHECK(fs::is_directory(dir) && fs::is_directory(another));
  CHECK(fs::is_empty(dir) && fs::is_empty(another));
  fs::remove_all(another);
  auto path=dir/"failure.db";
  auto pid=fork(); CHECK(pid>=0);
  if(pid==0) {
    try {
      Store s(path); s.begin(); s.put(1,"earlier successful statement");
      const auto point=s.savepoint();
      struct rlimit original{}; CHECK(getrlimit(RLIMIT_FSIZE,&original)==0);
      struct rlimit limited=original; limited.rlim_cur=fs::file_size(path)+4096;
      CHECK(signal(SIGXFSZ,SIG_IGN)!=SIG_ERR); CHECK(setrlimit(RLIMIT_FSIZE,&limited)==0);
      rejects([&]{s.put(2,std::string(20000,'x'));});
      CHECK(s.get(1)=="earlier successful statement" && !s.get(2));
      CHECK(setrlimit(RLIMIT_FSIZE,&original)==0);
      s.restore(point); CHECK(s.get(1)=="earlier successful statement" && !s.get(2));
      s.commit(); _exit(0);
    } catch(...) {_exit(1);}
  }
  child_success(pid);
  {Store s(path); CHECK(s.get(1)=="earlier successful statement" && !s.get(2));}
  fs::remove_all(dir);
}

int main(int argc,char** argv) {
  try {
    CHECK(argc==2); std::string suite=argv[1];
    if(suite=="types") types(); else if(suite=="vector") vectors(); else if(suite=="storage") storage(); else if(suite=="storage_failure") storage_failure(); else throw std::runtime_error("unknown suite");
    std::cout<<"PASS "<<suite<<"\n"; return 0;
  } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<"\n"; return 1;}
}
