#include "journal_harness.hpp"
#include <map>
#include <random>
#include <iostream>
#include <fstream>
#include <sys/wait.h>
#include <unistd.h>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
using Model=std::map<std::int64_t,std::string>;
Model rows(Store&s){Model out;s.scan_in(7,[&](auto k,auto v){out.emplace(k,v);});return out;}
int main(){try{
  const auto root=std::filesystem::path(std::getenv("MVDB_TEST_DIR")?std::getenv("MVDB_TEST_DIR"):"/tmp")/("journal-"+std::to_string(getpid()));std::filesystem::create_directories(root);
  auto path=root/"main.db";Model model;std::mt19937 rng(20261112);
  {
    JournalHarnessStore db(path);db.begin();auto empty=db.savepoint();db.put_in(7,INT64_MIN,"");db.put_in(7,INT64_MAX,"max");auto future=db.savepoint();
    db.restore(empty);CHECK(rows(db).empty());db.restore(future);model={{INT64_MIN,""},{INT64_MAX,"max"}};CHECK(rows(db)==model);db.commit();
    for(int round=0;round<30;++round){auto before=model;db.begin();auto sp=db.savepoint();std::vector<Store::Mutation> batch;
      for(int j=0;j<100;++j){auto key=std::int64_t(round*100+j);std::string value(32+rng()%5000,'\0');for(auto&c:value)c=char(rng());batch.push_back({key,value});model[key]=std::move(value);}
      db.apply_batch_in(7,batch);CHECK(rows(db)==model);
      if(round%3==0){db.restore(sp);model=before;db.put_in(7,-round-1,"after restore");model[-round-1]="after restore";}
      db.commit();CHECK(rows(db)==model);
    }
    db.begin();db.put_in(7,-999,"uncommitted snapshot");auto expected=rows(db);Model observed;bool first=true;
    db.scan_in(7,[&](auto k,auto v){observed.emplace(k,v);if(first){first=false;db.rollback();db.begin();db.put_in(7,999999,"new branch");db.rollback();}});
    CHECK(observed==expected);CHECK(rows(db)==model);
    db.begin();db.put_in(8,1,"right");db.put_in(9,1,"left");db.commit();unsigned visits=0;
    db.scan_equal_in(8,9,[&](auto k,auto a,auto b){CHECK(k==1&&a=="right"&&b=="left");++visits;});CHECK(visits==1);
  }
  {Store reopened(path);CHECK(rows(reopened)==model);}
  const auto pid=fork();CHECK(pid>=0);if(!pid){JournalHarnessStore db(path);db.begin();db.put_in(7,333333,"not committed");_exit(0);}int status=0;CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  {Store reopened(path);CHECK(rows(reopened)==model);reopened.begin();reopened.put_in(7,444444,"committed after orphan");reopened.commit();model[444444]="committed after orphan";}
  {Store reopened(path);CHECK(rows(reopened)==model);}
  // Corrupt a referenced payload without touching its header/root: row read
  // must reject it. The default Store constructor must autodetect this format.
  auto damaged=root/"payload.db";{JournalHarnessStore db(damaged);db.begin();db.put_in(7,1,"payload");db.commit();}
  {std::fstream f(damaged,std::ios::in|std::ios::out|std::ios::binary);f.seekp(8192+48);f.put('X');}
  bool rejected=false;try{Store db(damaged);(void)db.get_in(7,1);}catch(const std::exception&){rejected=true;}CHECK(rejected);
  std::filesystem::remove_all(root);std::cout<<"journal random model, savepoint branches, snapshot mutation, orphan recovery and corruption passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
