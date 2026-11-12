#include "minivectordb/database.hpp"
#include "minivectordb/row_codec.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <sys/wait.h>
#include <sys/resource.h>
#include <csignal>
#include <unistd.h>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
template<class F> void rejects(F f) {
  bool failed=false;
  try {
    f();
  }
  catch(const std::exception&) {
    failed=true;
  }
  CHECK(failed);
}
struct Result {
  std::vector<std::string> columns;
  std::vector<Row> rows;
  ExecutionInfo info;
};
Result q(Database&d,std::string_view sql) {
  Result r;
  r.info=d.execute(sql, {
    [&](const auto& c) {
      r.columns=c;
    },[&](const Row& row) {
      r.rows.push_back(row);
    }
  });
  return r;
}
std::int64_t id(const Result&r,std::size_t n=0) {
  return std::get<std::int64_t>(r.rows.at(n).at(0));
}
void codec() {
  Schema s {
    1,"t", {
      {
        "id",ColumnType::Bigint,0
      }, {
        "name",ColumnType::Varchar,3
      }, {
        "v",ColumnType::Vector,2
      }
    },0
  };
  Row r {
    INT64_MIN,std::string("a\0b",3),std::vector<float> {
      1.25f,-2
    }
  };
  auto bytes=encode_row(s,r);
  CHECK(decode_row(s,bytes)==r);
  for(std::size_t n=0;n<bytes.size();++n)rejects([&] {
    decode_row(s,std::string_view(bytes).substr(0,n));
  });
  rejects([&] {
    decode_row(s,bytes+"x");
  });
  auto bad=bytes;
  for(std::size_t n=13;n<17&&n<bad.size();++n)bad[n]=char(255);
  rejects([&] {
    decode_row(s,bad);
  });
  r[1]=std::string("中😀");
  CHECK(decode_row(s,encode_row(s,r))==r);
  r[1]=std::string("abcd");
  rejects([&] {
    encode_row(s,r);
  });
  r[1]=std::string("ok");
  r[2]=std::vector<float> {
    INFINITY,1
  };
  rejects([&] {
    encode_row(s,r);
  });
}
void sql(const std::filesystem::path&p) {
  {
    Database d(p, {
      p.parent_path()/"sort",4096
    });
    d.execute("/* hello */ CREATE TABLE T (id BIGINT PRIMARY KEY, name VARCHAR(4), v VECTOR(2), score BIGINT); --tail");
    d.execute("CREATE TABLE u (id BIGINT, s VARCHAR(2), PRIMARY KEY(id))");
    CHECK(d.execute("INSERT INTO t VALUES (1,'one',[3,4],10),(2,'two','[0,1]',20),(3,'三',[0,-1],20)").affected_rows==3);
    d.execute("INSERT INTO u(s,id) VALUES ('好',1)");
    CHECK(id(q(d,"SELECT id FROM u"))==1);
    auto all=q(d,"select name,id from T where score>=20 AND id<3");
    CHECK(all.rows.size()==1&&std::get<std::string>(all.rows[0][0])=="two");
    auto near=q(d,"SELECT id,L2_DISTANCE(v,[0,0]) AS distance FROM t WHERE score>=20 ORDER BY distance LIMIT 10");
    CHECK(near.rows.size()==2&&id(near)==2&&id(near,1)==3);
    CHECK(std::get<double>(near.rows[0][1])==1);
    CHECK(near.info.distances_computed==2);
    CHECK(q(d,"select * from t limit 0").rows.empty());
    CHECK(q(d,"select id from t where id=9223372036854775807").rows.empty());
    CHECK(q(d,"select id from t where id=2").info.rows_examined==1);
    CHECK(id(q(d,"select id from t order by score desc, name desc limit 1"))==3);
    for(auto bad: {
      "SELECT * FROM t OR id=1","SELECT NULL FROM t","SELECT count(*) FROM t","SELECT * FROM t; SELECT * FROM u","UPDATE t SET id=9","INSERT INTO t VALUES (4,'abcde',[1,2],1)","INSERT INTO t VALUES (4,'four',[1e100,2],1)","INSERT INTO t VALUES (4,'four',[1],1)","CREATE TABLE bad(id BIGINT)","CREATE TABLE bad(id BIGINT PRIMARY KEY,id BIGINT)","CREATE TABLE bad(id BIGINT PRIMARY KEY,v VECTOR(2),w VECTOR(2))","COMMIT","ROLLBACK"
    })rejects([&] {
      d.execute(bad);
    });
    CHECK(d.execute("UPDATE t SET name='O''K',v=[0,0] WHERE id=1").affected_rows==1);
    CHECK(id(q(d,"select id from t order by L2_DISTANCE(v,[0,0]) limit 1"))==1);
    d.execute("BEGIN");
    rejects([&] {
      d.execute("BEGIN");
    });
    d.execute("INSERT INTO t VALUES (4,'four',[1,1],0)");
    rejects([&] {
      d.execute("INSERT INTO t VALUES (5,'five',[1,1],0),(2,'dup',[0,0],0)");
    });
    CHECK(q(d,"select * from t where id=5").rows.empty());
    CHECK(q(d,"select * from t where id=4").rows.size()==1);
    d.execute("CREATE TABLE provisional(id BIGINT PRIMARY KEY)");
    d.execute("INSERT INTO provisional VALUES(1)");
    d.execute("DELETE FROM t WHERE id=1");
    d.execute("UPDATE t SET v=[9,9] WHERE id=2");
    CHECK(id(q(d,"select id from t order by L2_DISTANCE(v,[0,0]) limit 1"))==3);
    d.execute("ROLLBACK");
    rejects([&] {
      d.execute("SELECT * FROM provisional");
    });
    CHECK(id(q(d,"select id from t order by L2_DISTANCE(v,[0,0]) limit 1"))==1);
    d.execute("BEGIN");
    d.execute("DELETE FROM u");
    d.execute("UPDATE t SET score=-1 WHERE score>=20");
    d.execute("COMMIT");
    CHECK(q(d,"select * from u").rows.empty());
    CHECK(q(d,"select id from t where score=-1").rows.size()==2);
    d.execute("BEGIN");
    d.execute("INSERT INTO u VALUES(9,'no')");
  }
  {
    Database d(p);
    CHECK(q(d,"select * from u").rows.empty());
    CHECK(q(d,"select * from t").rows.size()==3);
  }
}
void sorting(const std::filesystem::path&p) {
  Database d(p, {
    p.parent_path()/"sort",4096
  });
  d.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(50),n BIGINT)");
  d.execute("BEGIN");
  std::vector<std::pair<int,int>> expected;
  std::mt19937 rng(7);
  for(int i=0;i<180;++i) {
    int n=int(rng()%9);
    expected.emplace_back(n,i);
    d.execute("INSERT INTO t VALUES("+std::to_string(i)+",'abcdefghijklmnopqrstuvwxyz',"+std::to_string(n)+")");
  }
  d.execute("COMMIT");
  std::sort(expected.begin(),expected.end());
  auto r=q(d,"SELECT id FROM t ORDER BY n ASC");
  CHECK(r.info.spill_runs>1);
  CHECK(r.rows.size()==expected.size());
  for(std::size_t i=0;i<expected.size();++i)CHECK(id(r,i)==expected[i].second);
  auto limited=q(d,"SELECT id FROM t ORDER BY n LIMIT 100");
  CHECK(limited.rows.size()==100);
  for(std::size_t i=0;i<100;++i)CHECK(id(limited,i)==expected[i].second);
  CHECK(std::filesystem::is_empty(p.parent_path()/"sort"));
  rejects([&] { d.execute("SELECT id FROM t ORDER BY n", {{}, [](const Row&) { throw std::runtime_error("sink stopped"); }}); });
  CHECK(std::filesystem::is_empty(p.parent_path()/"sort"));
}
void crash(const std::filesystem::path&p) {
  auto pid=fork();
  CHECK(pid>=0);
  if(pid==0) {
    try {
      Database d(p);
      d.execute("BEGIN");
      d.execute("CREATE TABLE a(id BIGINT PRIMARY KEY)");
      d.execute("CREATE TABLE b(id BIGINT PRIMARY KEY)");
      d.execute("INSERT INTO a VALUES(1)");
      d.execute("INSERT INTO b VALUES(1)");
      d.execute("COMMIT");
      d.execute("BEGIN");
      d.execute("DELETE FROM a");
      d.execute("INSERT INTO b VALUES(2)");
      _exit(0);
    }
    catch(...) {
      _exit(1);
    }
  }
  int status=0;
  CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  Database d(p);
  CHECK(q(d,"SELECT * FROM a").rows.size()==1&&q(d,"SELECT * FROM b").rows.size()==1);
}
void varchar_predicates(Database& d) {
  d.execute("CREATE TABLE narrow(id BIGINT PRIMARY KEY,s VARCHAR(1))");
  d.execute("INSERT INTO narrow VALUES(1,'a'),(2,'b'),(3,'中'),(4,'😀')");
  CHECK(id(q(d,"SELECT id FROM narrow WHERE s<'aa'"))==1);
  CHECK(q(d,"SELECT id FROM narrow WHERE s='aa'").rows.empty());
  CHECK(q(d,"SELECT id FROM narrow WHERE s<='aa'").rows.size()==1);
  CHECK(q(d,"SELECT id FROM narrow WHERE s>'aa'").rows.size()==3);
  CHECK(q(d,"SELECT id FROM narrow WHERE s>='aa'").rows.size()==3);
  auto unicode=q(d,"SELECT id FROM narrow WHERE s<'中😀'");
  CHECK(unicode.rows.size()==3 && id(unicode,2)==3);
  auto zero=q(d,"SELECT id FROM narrow WHERE s<'中😀' LIMIT 0");
  CHECK(zero.rows.empty() && zero.info.rows_examined==0);
  CHECK(d.execute("UPDATE narrow SET s='z' WHERE s<'aa'").affected_rows==1);
  CHECK(d.execute("DELETE FROM narrow WHERE s>'中中'").affected_rows==1);
  CHECK(q(d,"SELECT id FROM narrow WHERE id=4").rows.empty());
  for(auto invalid:{"INSERT INTO narrow VALUES(5,'aa')", "INSERT INTO narrow VALUES(5,'中😀')",
                    "UPDATE narrow SET s='aa' WHERE id=1", "UPDATE narrow SET s='中😀' WHERE id=99",
                    "SELECT id FROM narrow WHERE s=1 LIMIT 0"}) {
    rejects([&] { d.execute(invalid); });
  }
  const auto bad=std::string("'")+char(0x80)+"'";
  for(const auto& invalid: std::vector<std::string>{
        "SELECT id FROM narrow WHERE s="+bad+" LIMIT 0",
        "UPDATE narrow SET s='x' WHERE s="+bad,
        "DELETE FROM narrow WHERE s="+bad,
        "INSERT INTO narrow VALUES(5,"+bad+")",
        "UPDATE narrow SET s="+bad+" WHERE id=1"}) {
    rejects([&] { d.execute(invalid); });
  }
  CHECK(q(d,"SELECT id FROM narrow").rows.size()==3);
  CHECK(std::get<std::string>(q(d,"SELECT s FROM narrow WHERE id=1").rows[0][0])=="z");
}
void edges(const std::filesystem::path&p) {
  Database d(p, {
    p.parent_path()/"sort",4096
  });
  varchar_predicates(d);
  d.execute("CREATE TABLE `T` (`Id` BIGINT PRIMARY KEY,v VECTOR(2),s VARCHAR(4))");
  d.execute("INSERT INTO t VALUES(1,[1,1.490116119384765625e-8],'a'),(2,[1,0],'b'),(-9223372036854775808,[3,4],'中😀'),(9223372036854775807,[-3,-4],'')");
  auto near=q(d,"SELECT id,L2_DISTANCE(v,[0,0]) AS d FROM t ORDER BY d LIMIT 2");
  CHECK(id(near)==1&&id(near,1)==2);
  CHECK(std::get<double>(near.rows[0][1])==1&&std::get<double>(near.rows[1][1])==1);
  CHECK(near.info.distances_computed==4);
  auto sorted=q(d,"SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d");
  CHECK(id(sorted)==1&&id(sorted,1)==2&&id(sorted,2)==INT64_MIN&&id(sorted,3)==INT64_MAX);
  CHECK(sorted.info.distances_computed==4);
  CHECK(q(d,"SELECT * FROM t WHERE id=0 ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 8").rows.empty());
  CHECK(id(q(d,"SELECT id FROM t WHERE id=-9223372036854775808"))==INT64_MIN);
  for(auto bad: {
    "INSERT INTO t VALUES(7,[0,0],'x'),(7,[1,1],'y')","UPDATE t SET s='a',s='b'","SELECT * FROM t LIMIT -1","SELECT * FROM t ORDER BY v","SELECT * FROM t WHERE v=[0,0]","SELECT id FROM t ORDER BY L2_DISTANCE(v,[0]) LIMIT 0","SELECT * FROM t /*","INSERT INTO t VALUES(9,[0,0],'unterminated)","CREATE TABLE bad(id VARCHAR(2) PRIMARY KEY)","CREATE TABLE bad(id BIGINT PRIMARY KEY,x BIGINT PRIMARY KEY)","CREATE TABLE bad(id BIGINT PRIMARY KEY,x VARCHAR(0))"
  })rejects([&] {
    d.execute(bad);
  });
  CHECK(q(d,"SELECT * FROM t WHERE id=7").rows.empty());
  d.execute("BEGIN");
  d.execute("CREATE TABLE provisional(id BIGINT PRIMARY KEY)");
  rejects([&] {
    d.execute("CREATE TABLE provisional(id BIGINT PRIMARY KEY)");
  });
  d.execute("ROLLBACK");
  d.execute("CREATE TABLE provisional(id BIGINT PRIMARY KEY)");
  d.execute("INSERT INTO provisional VALUES(1)");
  rejects([&] {
    d.execute("SELECT id FROM t ORDER BY s", {
      {
      },[&](const Row&) {
        throw std::runtime_error("sink failure");
      }
    });
  });
  CHECK(!std::filesystem::exists(p.parent_path()/"sort")||std::filesystem::is_empty(p.parent_path()/"sort"));
  rejects([&] {
    d.execute("SELECT id FROM t", {
      {
      },[&](const Row&) {
        d.execute("DELETE FROM t");
      }
    });
  });
  CHECK(q(d,"SELECT * FROM t").rows.size()==4);
}
void failure(const std::filesystem::path&p,bool batch,int initial=3) {
  auto pid=fork();
  CHECK(pid>=0);
  if(pid==0) {
    try {
      Database d(p,{p.parent_path()/"sort",4096,true,batch});
      d.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(4))");
      d.execute("CREATE TABLE u(id BIGINT PRIMARY KEY)");
      std::string insert="INSERT INTO t VALUES";
      for(int i=1;i<=initial;++i)insert+=(i>1?",":"")+std::string("(")+std::to_string(i)+",'a')";
      d.execute(insert);
      const auto earlier="INSERT INTO t VALUES("+std::to_string(initial+1)+",'d')";
      // Measure this mode's exact statement extent before imposing a partial-page
      // limit. This keeps failure coverage when batch sharing reduces page count.
      d.execute("BEGIN"); d.execute(earlier); d.execute("INSERT INTO u VALUES(9)");
      auto before=std::filesystem::file_size(p);
      d.execute("UPDATE t SET s='oops'");
      auto growth=std::filesystem::file_size(p)-before;
      CHECK(growth>=4096);
      d.execute("ROLLBACK");
      d.execute("BEGIN"); d.execute(earlier); d.execute("INSERT INTO u VALUES(9)");
      auto expected=q(d,"SELECT * FROM t").rows;
      struct rlimit original{};
      CHECK(getrlimit(RLIMIT_FSIZE,&original)==0);
      auto limited=original;
      limited.rlim_cur=std::filesystem::file_size(p)+growth-4096+1000;
      CHECK(signal(SIGXFSZ,SIG_IGN)!=SIG_ERR);
      CHECK(setrlimit(RLIMIT_FSIZE,&limited)==0);
      rejects([&] { d.execute("UPDATE t SET s='oops'"); });
      CHECK(setrlimit(RLIMIT_FSIZE,&original)==0);
      CHECK(q(d,"SELECT * FROM t").rows==expected);
      CHECK(q(d,"SELECT * FROM u").rows.size()==1);
      d.execute("COMMIT");
      _exit(0);
    }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; _exit(1); }
  }
  int status=0;
  CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  Database d(p);
  CHECK(q(d,"SELECT * FROM t").rows.size()==static_cast<std::size_t>(initial+1));
  CHECK(q(d,"SELECT * FROM t WHERE s='a'").rows.size()==static_cast<std::size_t>(initial));
  CHECK(q(d,"SELECT * FROM t WHERE s='d'").rows.size()==1);
  CHECK(q(d,"SELECT * FROM u").rows.size()==1);
}
void batch_sql(const std::filesystem::path& dir) {
  std::uintmax_t growth[2]{};
  for(bool batch:{false,true}) {
    auto path=dir/(batch?"batch.db":"single.db");
    Database d(path,{dir/"sort",4096,true,batch});
    d.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT)");
    auto before=std::filesystem::file_size(path);
    std::string insert="INSERT INTO t VALUES";
    for(int i=0;i<300;++i)insert+=(i?",":"")+std::string("(")+std::to_string(i)+",0)";
    CHECK(d.execute(insert).affected_rows==300);
    growth[batch]=std::filesystem::file_size(path)-before;
    d.execute("BEGIN");
    CHECK(d.execute("UPDATE t SET n=1").affected_rows==300);
    CHECK(d.execute("UPDATE t SET n=1").affected_rows==300);
    CHECK(q(d,"SELECT id FROM t WHERE n=1").rows.size()==300);
    CHECK(d.execute("DELETE FROM t WHERE id>=128").affected_rows==172);
    CHECK(q(d,"SELECT id FROM t").rows.size()==128);
    d.execute("ROLLBACK");
    CHECK(q(d,"SELECT id FROM t WHERE n=0").rows.size()==300);
    d.execute("BEGIN");
    CHECK(d.execute("UPDATE t SET n=2").affected_rows==300);
    CHECK(d.execute("DELETE FROM t WHERE id<150").affected_rows==150);
    d.execute("COMMIT");
  }
  CHECK(growth[1]*4<growth[0]);
  for(auto name:{"batch.db","single.db"}) {
    Database d(dir/name);
    CHECK(q(d,"SELECT id FROM t WHERE n=2").rows.size()==150);
  }
  {
    Database d(dir/"wide.db",{dir/"sort",4096,true,true});
    d.execute("CREATE TABLE wide(id BIGINT PRIMARY KEY,s VARCHAR(700000))");
    const std::string medium(300000,'m'),large(600000,'x');
    CHECK(d.execute("INSERT INTO wide VALUES(1,'"+medium+"'),(2,'"+medium+"'),(3,'"+large+"')").affected_rows==3);
    CHECK(q(d,"SELECT id FROM wide").rows.size()==3);
    CHECK(d.execute("UPDATE wide SET s='"+medium+"'").affected_rows==3);
    CHECK(d.execute("DELETE FROM wide").affected_rows==3);
  }
}
int main(int argc,char**argv) {
  try {
    CHECK(argc==2);
    const auto* configured=std::getenv("MVDB_TEST_DIR");
    auto base=configured?std::filesystem::path(configured):std::filesystem::path("/data/core-tests");
    std::filesystem::create_directories(base);
    auto pattern=(base/(std::string(argv[1])+"-XXXXXX")).string();
    CHECK(::mkdtemp(pattern.data())!=nullptr);
    auto dir=std::filesystem::path(pattern);
    std::string suite=argv[1];
    if(suite=="codec")codec();
    else if(suite=="sql")sql(dir/"db");
    else if(suite=="sort")sorting(dir/"db");
    else if(suite=="sql_crash")crash(dir/"db");
    else if(suite=="sql_edges")edges(dir/"db");
    else if(suite=="sql_failure") {
      failure(dir/"single.db",false);
      failure(dir/"batch.db",true);
      failure(dir/"batch-chunks.db",true,130);
    }
    else if(suite=="sql_batch")batch_sql(dir);
    else throw std::runtime_error("unknown suite");
    std::filesystem::remove_all(dir);
    std::cout<<"PASS "<<suite<<'\n';
  }
  catch(const std::exception&e) {
    std::cerr<<"FAIL "<<e.what()<<'\n';
    return 1;
  }
}
