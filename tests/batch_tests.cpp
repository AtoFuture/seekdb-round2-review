#include "minivectordb/store.hpp"
#include <algorithm>
#include <bit>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace minivectordb;
namespace fs=std::filesystem;
#define CHECK(x) do {if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
template<class F> void rejects(F f) { bool failed=false; try {f();}catch(const std::exception&){failed=true;} CHECK(failed); }
using Mutation=Store::Mutation;
using Snapshot=std::map<std::int64_t,std::string>;
Snapshot snapshot(Store& store,std::uint64_t space) {
  Snapshot result; store.scan_in(space,[&](auto key,auto bytes){result.emplace(key,bytes);}); return result;
}
void operations(const fs::path& dir) {
  std::mt19937_64 random(713);
  std::vector<Mutation> changes{{INT64_MIN,"min"},{INT64_MAX,"max"},{0,"zero"}};
  Snapshot expected;
  for(int i=0;i<90;++i)changes.push_back({std::bit_cast<std::int64_t>(random()),std::to_string(i)});
  for(const auto& change:changes)expected[change.key]=*change.value;
  auto path=dir/"mixed.db";
  {
    Store store(path); store.begin(); store.put_in(1,7,"existing"); store.put_in(2,7,"other"); store.commit();
    expected[7]="existing";
    store.begin(); store.apply_batch_in(1,changes); CHECK(snapshot(store,1)==expected);
    auto point=store.savepoint();
    std::vector<Mutation> second{{INT64_MIN,std::nullopt},{42,"new"},{0,"changed"},{99,std::nullopt}};
    store.apply_batch_in(1,second); CHECK(store.get_in(1,0)=="changed"&&!store.get_in(1,INT64_MIN));
    CHECK(store.get_in(2,7)=="other");
    store.restore(point); CHECK(snapshot(store,1)==expected);
    auto size=fs::file_size(path);
    std::vector<Mutation> duplicate{{1,"a"},{1,std::nullopt}};
    rejects([&]{store.apply_batch_in(1,duplicate);}); CHECK(fs::file_size(path)==size);
    CHECK(snapshot(store,1)==expected);
    std::vector<Mutation> excessive(Store::max_batch_mutations+1);
    rejects([&]{store.apply_batch_in(1,excessive);}); CHECK(fs::file_size(path)==size);
    std::vector<Mutation> large{{1,std::string(Store::max_batch_bytes,'x')},{2,"x"}};
    rejects([&]{store.apply_batch_in(1,large);}); CHECK(fs::file_size(path)==size);
    store.apply_batch_in(1,{}); CHECK(fs::file_size(path)==size);
    std::vector<Mutation> missing{{123456,std::nullopt}};
    store.apply_batch_in(1,missing); CHECK(fs::file_size(path)==size);
    std::vector<Mutation> oversized{{123456,std::string(Store::max_batch_bytes+1,'x')}};
    store.apply_batch_in(3,oversized); CHECK(store.get_in(3,123456)==oversized[0].value);
    store.commit();
  }
  {
    Store store(path); CHECK(snapshot(store,1)==expected); CHECK(store.get_in(2,7)=="other");
    store.begin(); std::vector<Mutation> erase;
    for(const auto& [key,value]:expected)erase.push_back({key,std::nullopt});
    store.apply_batch_in(1,erase); CHECK(snapshot(store,1).empty());
    store.rollback(); CHECK(snapshot(store,1)==expected);
    store.begin(); store.apply_batch_in(1,erase); store.commit();
  }
  {Store store(path); CHECK(snapshot(store,1).empty()); CHECK(store.get_in(2,7)=="other");}
}
void scan_batches(const fs::path& path) {
  Store store(path); store.begin();
  std::vector<Mutation> batch;
  for(int i=0;i<300;++i) {
    batch.push_back({i,"old"});
    if(batch.size()==Store::max_batch_mutations){store.apply_batch_in(1,batch);batch.clear();}
  }
  store.apply_batch_in(1,batch); batch.clear(); auto point=store.savepoint();
  std::size_t visited=0;
  store.scan_in(1,[&](auto key,auto bytes){
    CHECK(bytes=="old"); ++visited; batch.push_back({key,key%2?std::optional<std::string>("new"):std::nullopt});
    if(batch.size()==Store::max_batch_mutations){store.apply_batch_in(1,batch);batch.clear();}
  });
  store.apply_batch_in(1,batch); CHECK(visited==300); CHECK(snapshot(store,1).size()==150);
  store.restore(point); CHECK(snapshot(store,1).size()==300);
  store.commit();
}
void reduction(const fs::path& dir) {
  std::uintmax_t growth[2]{};
  for(bool batch:{false,true}) {
    auto path=dir/(batch?"compact.db":"reference.db"); Store store(path); store.begin();
    auto before=fs::file_size(path); std::vector<Mutation> changes;
    for(int i=0;i<100;++i)changes.push_back({i,std::string(100,'x')});
    if(batch)store.apply_batch_in(1,changes);
    else for(const auto& change:changes)store.put_in(1,change.key,*change.value);
    store.commit(); growth[batch]=fs::file_size(path)-before;
    CHECK(snapshot(store,1).size()==100);
  }
  CHECK(growth[1]*4<growth[0]);
  std::cout<<"growth bytes single="<<growth[0]<<" batch="<<growth[1]<<'\n';
}
void partial_failure(const fs::path& path) {
  auto pid=fork(); CHECK(pid>=0);
  if(pid==0) {
    try {
      Store store(path); store.begin(); store.put_in(1,-1,"previous"); store.put_in(2,5,"other");
      auto before=snapshot(store,1); std::vector<Mutation> changes;
      for(int i=0;i<100;++i)changes.push_back({i,"new"});
      struct rlimit original{}; CHECK(getrlimit(RLIMIT_FSIZE,&original)==0);
      auto limited=original; limited.rlim_cur=fs::file_size(path)+103*4096+1000;
      CHECK(signal(SIGXFSZ,SIG_IGN)!=SIG_ERR); CHECK(setrlimit(RLIMIT_FSIZE,&limited)==0);
      rejects([&]{store.apply_batch_in(1,changes);});
      CHECK(snapshot(store,1)==before); CHECK(store.get_in(2,5)=="other");
      CHECK(setrlimit(RLIMIT_FSIZE,&original)==0);
      // No restore: a raw failed batch must itself leave the prior root available.
      store.commit(); _exit(0);
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';_exit(1);}
  }
  int status=0; CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  Store store(path); CHECK(store.get_in(1,-1)=="previous"&&snapshot(store,1).size()==1); CHECK(store.get_in(2,5)=="other");
}
void crash(const fs::path& path) {
  auto pid=fork(); CHECK(pid>=0);
  if(pid==0) {
    try {
      Store store(path); store.begin(); std::vector<Mutation> changes{{1,"a"},{2,"b"}};
      store.apply_batch_in(1,changes); store.apply_batch_in(2,changes); store.commit();
      store.begin(); std::vector<Mutation> changed{{1,std::nullopt},{2,"new"},{3,"lost"}};
      store.apply_batch_in(1,changed); store.apply_batch_in(2,changed); _exit(0);
    }catch(...){_exit(1);}
  }
  int status=0; CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  Store store(path); for(int space:{1,2}){CHECK(store.get_in(space,1)=="a"&&store.get_in(space,2)=="b"&&!store.get_in(space,3));}
}
int main() {
  try {
    const auto* root=std::getenv("MVDB_TEST_DIR"); CHECK(root); fs::create_directories(root);
    auto name=(fs::path(root)/"batches-XXXXXX").string(); CHECK(mkdtemp(name.data())); fs::path dir(name);
    operations(dir); scan_batches(dir/"scan.db"); reduction(dir); partial_failure(dir/"failure.db"); crash(dir/"crash.db");
    fs::remove_all(dir); std::cout<<"PASS batches\n";
  } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
