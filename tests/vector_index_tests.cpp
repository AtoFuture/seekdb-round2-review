#include "minivectordb/database.hpp"
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>
using namespace minivectordb;
#define CHECK(x) do {if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
template<class F> void rejects(F f) {bool failed=false;try{f();}catch(const Error&){failed=true;}CHECK(failed);}
struct Result {std::vector<Row> rows; ExecutionInfo info;};
Result query(Database& d,SearchMode mode=SearchMode::Exact,std::string_view filter="") {
 Result r;
 r.info=d.execute("select id,l2_distance(v,[0,0]) as distance from t "+std::string(filter)+" order by distance limit 3",{{},[&](const Row& row){r.rows.push_back(row);}},mode);
 return r;
}
std::int64_t first(const Result& r){return std::get<std::int64_t>(r.rows.at(0).at(0));}
void exercise(const std::filesystem::path& dir,bool packed) {
 Options options; options.packed_values=packed; options.memory_budget_bytes=4096;
 options.scan_snapshot_bytes=0; // This suite asserts physical index traversal counts.
 auto file=dir/(packed?"packed":"plain");
 {
 Database d(file,options);
 d.execute("create table t (id bigint primary key, v vector(2), tag bigint)");
 d.execute("begin");
 for(int i=1;i<=300;++i)d.execute("insert into t values ("+std::to_string(i)+",["+std::to_string(i)+",0],1)");
 d.execute("commit");
 auto baseline=query(d); CHECK(first(baseline)==1); CHECK(baseline.info.vector_index_entries_examined==0);
 d.execute("begin");
 d.execute("create vector index rolled_back on t(v)");
 CHECK(query(d).info.vector_index_entries_examined==300);
 d.execute("rollback");
 CHECK(query(d).info.vector_index_entries_examined==0);
 d.execute("create vector index ix on t(v)");
 auto indexed=query(d); CHECK(indexed.rows==baseline.rows); CHECK(indexed.info.vector_index_entries_examined==300);
 CHECK(query(d,SearchMode::Ann).rows==baseline.rows);
 rejects([&]{d.execute("create vector index ix on t(v)");});
 rejects([&]{d.execute("create vector index bad on t(tag)");});
 rejects([&]{d.execute("create vector index bad on t(missing)");});
 rejects([&]{d.execute("create vector index bad on missing(v)");});
 d.execute("create table later (id bigint primary key, v vector(2))");
 d.execute("insert into later values(1,[9,9])");
 d.execute("begin");
 d.execute("update t set v=[0,0] where id=300"); CHECK(first(query(d))==300);
 d.execute("delete from t where id=300"); CHECK(first(query(d))==1); CHECK(query(d).info.vector_index_entries_examined==299);
 d.execute("insert into t values (-1,[0,0],2)"); CHECK(first(query(d,SearchMode::Ann))==-1);
 CHECK(first(query(d,SearchMode::Exact,"where tag=1"))==1);
 rejects([&]{d.execute("insert into t values (-2,[0,0],2),(1,[0,0],2)");});
 CHECK(query(d).info.vector_index_entries_examined==300);
 d.execute("rollback"); CHECK(query(d).rows==baseline.rows);
 d.execute("update t set v=[1,0]");
 CHECK(first(query(d))==1); // deterministic tie by primary key
 d.execute("delete from t where id>298");
 d.execute("insert into t values(-7,[0,0],2)");
 CHECK(first(query(d))==-7); CHECK(query(d).info.vector_index_entries_examined==299);
 }
 {
 Database d(file,options); CHECK(first(query(d,SearchMode::Ann))==-7); CHECK(query(d).info.vector_index_entries_examined==299);
 }
 auto child=fork(); CHECK(child>=0);
 if(child==0) {
  Database d(file,options);d.execute("begin");d.execute("delete from t");d.execute("create vector index crash_index on t(v)");_exit(0);
 }
 int status=0;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
 {Database d(file,options);CHECK(first(query(d))==-7);CHECK(query(d).info.vector_index_entries_examined==299);d.execute("create vector index crash_index on t(v)");}
}
void failed_build(const std::filesystem::path& dir) {
 auto file=dir/"failed-build";
 {Database d(file); d.execute("create table t(id bigint primary key,v vector(2),tag bigint)");d.execute("begin");for(int i=0;i<260;++i)d.execute("insert into t values("+std::to_string(i)+",[1,1],0)");d.execute("commit");}
 std::string original;
 {Store store(file);original=*store.get_in(1,259);store.begin();store.put_in(1,259,"bad");store.commit();}
 {Database d(file);d.execute("begin");rejects([&]{d.execute("create vector index ix on t(v)");});CHECK(d.in_transaction());d.execute("create table survives(id bigint primary key)");d.execute("commit");}
 {Store store(file);store.begin();store.put_in(1,259,original);store.commit();}
 {Database d(file);CHECK(query(d).info.vector_index_entries_examined==0);d.execute("create vector index ix on t(v)");CHECK(query(d).info.vector_index_entries_examined==260);}
 // A DML error after flushed row/index batches must restore both namespaces.
 {Store store(file);store.begin();store.put_in(1,259,"bad");store.commit();}
 {Database d(file);d.execute("begin");rejects([&]{d.execute("update t set v=[7,7]");});CHECK(d.in_transaction());d.execute("commit");}
 {Store store(file);store.begin();store.put_in(1,259,original);store.commit();}
 {Database d(file);auto r=query(d);CHECK(r.info.vector_index_entries_examined==260);CHECK(std::get<double>(r.rows[0][1])<2.0);}

}
int main(){
 const char* root=std::getenv("MVDB_TEST_DIR");
 auto dir=std::filesystem::path(root?root:"/data")/("vector-index-"+std::to_string(getpid()));
 std::filesystem::create_directories(dir);
 try {exercise(dir,false);exercise(dir,true);failed_build(dir);}
 catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
 std::filesystem::remove_all(dir);
 std::cout<<"vector index tests passed\n";
}
