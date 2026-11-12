#include "minivectordb/store.hpp"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

// These wrappers affect this test process only. EIO injection models a returned
// error with an intact kernel page cache, NOT actual failed device writeback.
using namespace minivectordb;
namespace fs = std::filesystem;
namespace {
enum class Stop { None, BeforeRoot, ShortRoot, AfterRoot, BeforeFinalSync,
                  AfterFinalSync, Ack, Uncommitted };
enum class Fault { None, ShortRetry, ShortThenEio, Eintr, Zero, Eio };
struct Hooks {
  bool armed=false, root_complete=false, fault_used=false, pending_eio=false;
  int writes=0, syncs=0, fail_sync=0, event_fd=-1, commit_fd=-1;
  bool sync_error_after_real=false, sync_interrupt=false;
  Stop stop=Stop::None;
  Fault fault=Fault::None;
  std::size_t cut=17;
  std::string order;
} hooks;
int cases=0;
bool compressed_paths=false;
void check(bool value,const std::string& label) {
  if(!value)throw std::runtime_error(label);
}
template<class F> bool throws(F f) {
  try {f();return false;}catch(const std::exception&) {return true;}
}
void write_event(int fd,char value) {
  ssize_t count;
  do {count=::write(fd,&value,1);}while(count<0 && errno==EINTR);
  if(count!=1)::_exit(81);
}
void stop_at(Stop point) {
  if(hooks.stop!=point)return;
  write_event(hooks.event_fd,point==Stop::Ack?'A':'S');
  ::raise(SIGSTOP);
  // Parent must kill this child. Resuming it is a test error.
  ::_exit(82);
}
void arm(Stop stop=Stop::None,int event_fd=-1,std::size_t cut=17) {
  hooks=Hooks{};hooks.armed=true;hooks.stop=stop;
  hooks.event_fd=event_fd;hooks.cut=cut;
}
void disarm() {hooks.armed=false;}
std::unique_ptr<Store> open_store(const fs::path& path,bool process_crash,bool packed) {
#ifdef MVDB_COMMIT_POLICY_API
  return std::make_unique<Store>(path,65536,packed,16384,65536,
      process_crash?CommitSyncPolicy::ProcessCrash:CommitSyncPolicy::Ordered,compressed_paths);
#else
  // TDD bootstrap: old implementation must reach the one-sync assertion and
  // fail at runtime (2 != 1), rather than fail to compile on a missing enum.
  (void)process_crash;
  return std::make_unique<Store>(path,65536,packed,16384,65536);
#endif
}
using Records=std::map<std::int64_t,std::string>;
using State=std::map<std::uint64_t,Records>;
const State original{
  {7,{{INT64_MIN,"minimum"},{0,"zero"},{1,"one"},{INT64_MAX,"maximum"}}},
  {99,{{-1,"secondary-old"},{5,"other-five"}}}, {123,{}}
};
State changed() {
  State next=original;
  next[7].erase(0);next[7][1]="replacement";
  next[7][2]=std::string(9000,'v');next[7][3]="packed-three";next[7][4]="packed-four";
  next[99].erase(-1);next[99][5]="secondary-new";
  next[99][6]="six";next[123][42]="new namespace";
  return next;
}
void mutate(Store& store) {
  store.put_in(7,1,"replacement");store.erase_in(7,0);
  // Two short records share a real packed page in format03; the long record
  // simultaneously exercises overflow pages referenced by the same commit.
  std::vector<Store::Mutation> batch{{2,std::string(9000,'v')},{3,"packed-three"},{4,"packed-four"}};
  store.apply_batch_in(7,batch);
  store.erase_in(99,-1);store.put_in(99,5,"secondary-new");
  store.put_in(99,6,"six");store.put_in(123,42,"new namespace");
}
State observe(Store& store) {
  State got;
  for(auto space:{7ULL,99ULL,123ULL}) {
    auto& entries=got[space];
    store.scan_in(space,[&](auto key,auto bytes) {
      check(entries.emplace(key,std::string(bytes)).second,"duplicate key");
    });
    for(const auto& [key,value]:entries)
      check(store.get_in(space,key)==value,"scan/point disagreement");
  }
  return got;
}
void make_seed(const fs::path& path,bool packed,int extra_commits=0) {
  auto store=open_store(path,false,packed);store->begin();
  for(const auto& [space,entries]:original)
    for(const auto& [key,value]:entries)store->put_in(space,key,value);
  store->commit();
  for(int i=0;i<extra_commits;++i) {store->begin();store->commit();}
}
void check_reopens(const fs::path& path,bool packed,bool require_old,bool require_new) {
  State first;
  for(int i=0;i<2;++i) {
    auto store=open_store(path,i!=0,packed);
    auto got=observe(*store);
    check(got==original||got==changed(),"mixed or incomplete namespaces after reopen");
    if(require_old)check(got==original,"uncommitted transaction became visible");
    if(require_new)check(got==changed(),"confirmed or complete published transaction lost");
    if(i==0)first=got;else check(got==first,"repeated reopen changed state");
  }
}
void successful_commit(const fs::path& seed,const fs::path& path,bool policy,bool packed) {
  fs::copy_file(seed,path,fs::copy_options::overwrite_existing);
  {auto store=open_store(path,policy,packed);store->begin();mutate(*store);arm();
   store->commit();disarm();
   const int expected=policy?1:2;
   check(hooks.syncs==expected,"commit sync count: expected "+std::to_string(expected)+
         " got "+std::to_string(hooks.syncs));
   check(hooks.order==(policy?"RS":"SRS"),"commit publication/sync order");
   check(observe(*store)==changed(),"successful transaction contents");
   // Empty explicit transactions must still follow the selected commit policy.
   store->begin();arm();store->commit();disarm();
   check(hooks.syncs==expected,"empty commit sync count");}
  check_reopens(path,packed,false,true);++cases;
}
void kill_case(const fs::path& seed,const fs::path& path,bool policy,bool packed,
               Stop stage,std::size_t cut=17) {
  fs::copy_file(seed,path,fs::copy_options::overwrite_existing);
  int pipefd[2];check(::pipe(pipefd)==0,"pipe");
  const pid_t child=::fork();
  if(child<0) {::close(pipefd[0]);::close(pipefd[1]);throw std::runtime_error("fork");}
  if(child==0) {
    ::close(pipefd[0]);
    try {
      auto store=open_store(path,policy,packed);store->begin();mutate(*store);
      arm(stage,pipefd[1],cut);
      if(stage==Stop::Uncommitted)stop_at(stage);
      store->commit();stop_at(Stop::Ack);
      write_event(pipefd[1],'E');::_exit(83);
    }catch(...) {write_event(pipefd[1],'E');::_exit(84);}
  }
  ::close(pipefd[1]);
  char observed=0;
  pollfd ready{pipefd[0],POLLIN,0};int polled;
  do {polled=::poll(&ready,1,20000);}while(polled<0 && errno==EINTR);
  if(polled>0) {
    ssize_t size;
    do {size=::read(pipefd[0],&observed,1);}while(size<0 && errno==EINTR);
    if(size!=1)observed=0;
  }
  ::close(pipefd[0]);
  // Always reap the child, including unexpected-exit/timeout failure paths.
  const int killed=::kill(child,SIGKILL);const int kill_errno=errno;
  int status=0;pid_t waited;
  do {waited=::waitpid(child,&status,0);}while(waited<0 && errno==EINTR);
  check(polled>0 && observed==(stage==Stop::Ack?'A':'S'),"child failed to reach crash boundary");
  check(killed==0 || kill_errno==ESRCH,"kill child");
  check(waited==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL,"child not killed by SIGKILL");
  const bool old=stage==Stop::BeforeRoot||stage==Stop::Uncommitted;
  const bool next=stage==Stop::AfterRoot||stage==Stop::BeforeFinalSync||
      stage==Stop::AfterFinalSync||stage==Stop::Ack;
  check_reopens(path,packed,old,next);++cases;
}
void poison_checks(Store& store) {
  check(throws([&]{store.get_in(7,1);}),"poisoned get allowed");
  check(throws([&]{store.scan_in(7,[](auto,auto){});}),"poisoned scan allowed");
  check(throws([&]{store.begin();}),"poisoned begin allowed");
  check(throws([&]{store.rollback();}),"poisoned rollback allowed");
  check(throws([&]{store.commit();}),"poisoned commit allowed");
}
void fault_case(const fs::path& seed,const fs::path& path,bool policy,bool packed,
                Fault fault,int fail_sync=0,bool after_real=false,std::size_t cut=17,bool interrupt=false) {
  fs::copy_file(seed,path,fs::copy_options::overwrite_existing);
  const bool expected_error=(fail_sync!=0 && !interrupt) || fault==Fault::Eio ||
      fault==Fault::Zero || fault==Fault::ShortThenEio;
  {auto store=open_store(path,policy,packed);store->begin();mutate(*store);arm();
   hooks.fault=fault;hooks.fail_sync=fail_sync;
   hooks.sync_error_after_real=after_real;hooks.sync_interrupt=interrupt;hooks.cut=cut;
   const bool failed=throws([&]{store->commit();});disarm();
   check(failed==expected_error,"commit error expectation");
   if(failed)poison_checks(*store);
   else {check(observe(*store)==changed(),"short/EINTR success state");
     check(hooks.syncs==(policy?1:2)+(interrupt?1:0),"successful retry sync count");}}
  check_reopens(path,packed,false,!expected_error);++cases;
}
} // namespace

extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t n,off_t offset) {
  if(!hooks.armed)return __real_pwrite(fd,data,n,offset);
  if(hooks.commit_fd<0)hooks.commit_fd=fd;
  check(hooks.commit_fd==fd,"commit writes and sync must use the same fd");
  ++hooks.writes;
  const bool root=offset>=0 && offset<8192;
  if(root)stop_at(Stop::BeforeRoot);
  if(hooks.pending_eio) {hooks.pending_eio=false;errno=EIO;return -1;}
  if(root && hooks.stop==Stop::ShortRoot) {
    const auto size=__real_pwrite(fd,data,std::min(n,hooks.cut),offset);
    if(size>0)stop_at(Stop::ShortRoot);
    return size;
  }
  if(root && !hooks.fault_used && hooks.fault!=Fault::None) {
    hooks.fault_used=true;
    switch(hooks.fault) {
      case Fault::Eintr:errno=EINTR;return -1;
      case Fault::Zero:return 0;
      case Fault::Eio:errno=EIO;return -1;
      case Fault::ShortThenEio:hooks.pending_eio=true;[[fallthrough]];
      case Fault::ShortRetry:return __real_pwrite(fd,data,std::min(n,hooks.cut),offset);
      default:break;
    }
  }
  const auto size=__real_pwrite(fd,data,n,offset);
  if(root && size>0 && (offset%4096)+size==4096) {
    hooks.root_complete=true;hooks.order+='R';stop_at(Stop::AfterRoot);
  }
  return size;
}
extern "C" int __wrap_fsync(int fd) {
  if(!hooks.armed)return __real_fsync(fd);
  if(hooks.commit_fd<0)hooks.commit_fd=fd;
  check(hooks.commit_fd==fd,"commit writes and sync must use the same fd");
  ++hooks.syncs;
  if(hooks.root_complete)stop_at(Stop::BeforeFinalSync);
  const bool fail=hooks.syncs==hooks.fail_sync;
  if(fail && hooks.sync_interrupt) {hooks.fail_sync=0;errno=EINTR;return -1;}
  if(fail && !hooks.sync_error_after_real) {errno=EIO;return -1;}
  const int result=__real_fsync(fd);
  if(result==0) {
    hooks.order+='S';
    if(fail) {errno=EIO;return -1;}
    if(hooks.root_complete)stop_at(Stop::AfterFinalSync);
  }
  return result;
}
int main(int argc,char** argv) {
  compressed_paths=argc>1 && std::string(argv[1])=="--compressed";
  fs::path directory;
  try {
    const char* configured=std::getenv("MVDB_TEST_DIR");check(configured,"MVDB_TEST_DIR required");
    fs::create_directories(configured);
    std::string name=(fs::path(configured)/"commit-policy-XXXXXX").string();
    check(::mkdtemp(name.data())!=nullptr,"mkdtemp");directory=name;
    // The first assertion supplies a real behavioral RED against the old API.
    const auto first=directory/"red-seed";make_seed(first,true);
    successful_commit(first,directory/"red-case",true,true);
    if(argc>1 && std::string(argv[1])=="--red") {
      fs::remove_all(directory);std::cout<<"PASS single-sync contract\n";return 0;
    }
    for(bool packed:{false,true}) for(int parity:{0,1}) {
      const auto seed=directory/((packed?std::string("seed03-"):std::string("seed02-"))+std::to_string(parity));make_seed(seed,packed,parity);
      for(bool policy:{false,true}) {
        const auto path=directory/"case";
        successful_commit(seed,path,policy,packed);
        for(Stop point:{Stop::Uncommitted,Stop::BeforeRoot,Stop::AfterRoot,
            Stop::BeforeFinalSync,Stop::AfterFinalSync,Stop::Ack})
          kill_case(seed,path,policy,packed,point);
        for(std::size_t cut:{1u,7u,8u,9u,15u,16u,17u,23u,24u,25u,31u,
                            32u,33u,34u,35u,36u,37u,137u,512u,4095u}) {
          kill_case(seed,path,policy,packed,Stop::ShortRoot,cut);
          fault_case(seed,path,policy,packed,Fault::ShortThenEio,0,false,cut);
        }
        for(Fault fault:{Fault::ShortRetry,Fault::Eintr,Fault::Zero,Fault::Eio})
          fault_case(seed,path,policy,packed,fault);
        fault_case(seed,path,policy,packed,Fault::None,1,false,17,true);
        for(int index=1;index<=(policy?1:2);++index)
          for(bool after:{false,true})fault_case(seed,path,policy,packed,Fault::None,index,after);
      }
    }
    fs::remove_all(directory);
    std::cout<<"PASS commit policy cases="<<cases<<" (SIGKILL and injected return errors; not media-fault proof)\n";
    return 0;
  }catch(const std::exception& error) {
    disarm();std::cerr<<"FAIL "<<error.what()<<" after "<<cases<<" cases; evidence "<<directory<<'\n';return 1;
  }
}
