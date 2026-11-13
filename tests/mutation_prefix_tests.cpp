#include "minivectordb/store.hpp"
#include <array>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <vector>
#include <unistd.h>
using namespace minivectordb;
namespace fs=std::filesystem;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
using Rows=std::map<std::int64_t,std::string>;
using Model=std::map<std::uint64_t,Rows>;
void verify(Store& store,const Model& model) {
  for(const auto& [space,expected]:model) {
    Rows actual;
    store.scan_in(space,[&](auto key,auto value){CHECK(actual.emplace(key,value).second);});
    CHECK(actual==expected);
    for(const auto& [key,value]:expected)CHECK(store.get_in(space,key)==value);
  }
}
std::int64_t signed_key(std::uint64_t ordered) {
  return std::bit_cast<std::int64_t>(ordered^(UINT64_C(1)<<63));
}
void prefix_edges(Store& store) {
  constexpr std::uint64_t base_space=UINT64_C(0x31425364758697a8);
  constexpr std::uint64_t base_key=UINT64_C(0x23456789abcdef10);
  Model model;model[base_space]={};
  auto put=[&](std::uint64_t space,std::int64_t key,const std::string& value,bool batch) {
    if(batch) {
      const std::array<Store::Mutation,1> changes{{{key,value}}};
      store.apply_batch_in(space,changes);
    } else store.put_in(space,key,value);
    model[space][key]=value;
  };
  auto erase=[&](std::uint64_t space,std::int64_t key,bool batch) {
    CHECK(model[space].erase(key)==1);
    if(batch) {
      const std::array<Store::Mutation,1> changes{{{key,std::nullopt}}};
      store.apply_batch_in(space,changes);
      CHECK(!store.get_in(space,key));
    } else CHECK(store.erase_in(space,key));
  };
  store.begin();
  // XOR exactly one big-endian byte. Earlier bytes remain identical, so the
  // first fork visits each of the eight namespace and eight key layers.
  // Repeat with replace() and merge_batch(); every pair starts with an empty
  // tree and its last deletion is followed by insertion into that empty tree.
  for(bool batch:{false,true})for(unsigned layer=0;layer<16;++layer) {
    const auto mask=UINT64_C(0x80)<<((7-layer%8)*8);
    const auto other_space=layer<8?base_space^mask:base_space;
    const auto first=signed_key(base_key);
    const auto other=signed_key(layer<8?base_key:base_key^mask);
    model[other_space];verify(store,model);
    put(base_space,first,"first",batch);
    if(batch && other_space==base_space) {
      // Updating the retained row in the same unordered batch makes its first
      // and last keys constrain the skipped path at this exact key byte.
      const std::array<Store::Mutation,2> changes{{{other,"other"},{first,"retained"}}};
      store.apply_batch_in(base_space,changes);
      model[base_space][first]="retained";model[base_space][other]="other";
    } else put(other_space,other,"other",batch);
    verify(store,model);
    erase(base_space,first,batch);verify(store,model);
    erase(other_space,other,batch);verify(store,model);
    put(base_space,first,"after empty",batch);verify(store,model);
    erase(base_space,first,batch);verify(store,model);
  }
  // Remove *all* rows below an existing prefix while introducing another
  // prefix in the same batch. Restore must recover both original rows and
  // remove every new row, even though all newly written pages still exist.
  for(unsigned byte=0;byte<8;++byte) {
    const auto newer=base_key^(UINT64_C(0x80)<<((7-byte)*8));
    const unsigned count=byte==7?1:2;
    for(unsigned i=0;i<count;++i)put(base_space,signed_key(base_key+i),"before "+std::to_string(i),false);
    const auto before=model;const auto saved=store.savepoint();
    std::vector<Store::Mutation> changes;
    for(unsigned i=0;i<count;++i) {
      changes.push_back({signed_key(newer+i),"after "+std::to_string(i)});
      changes.push_back({signed_key(base_key+i),std::nullopt});
      model[base_space].erase(signed_key(base_key+i));
      model[base_space][signed_key(newer+i)]="after "+std::to_string(i);
    }
    store.apply_batch_in(base_space,changes);verify(store,model);
    for(unsigned i=0;i<count;++i)CHECK(!store.get_in(base_space,signed_key(base_key+i)));
    store.restore(saved);model=before;verify(store,model);
    for(unsigned i=0;i<count;++i)CHECK(!store.get_in(base_space,signed_key(newer+i)));
    for(unsigned i=0;i<count;++i)erase(base_space,signed_key(base_key+i),true);
    verify(store,model);
  }
  // The next trace opens this committed empty root, then reinserts rows.
  store.commit();verify(store,model);
}
void trace(const fs::path& path,bool packed,bool compressed,bool packed_nodes,bool compact_values=false,bool compact_refs=false) {
  const std::array<std::uint64_t,8> spaces{0,1,255,256,UINT64_C(1)<<24,UINT64_C(1)<<40,UINT64_C(1)<<56,UINT64_MAX};
  const std::array<std::int64_t,8> boundaries{INT64_MIN,INT64_MAX,-1,0,1,256,65536,INT64_C(1)<<40};
  std::mt19937_64 rng(0xd531e78);
  Model committed;for(auto space:spaces)committed[space]={};
  {
    Store store(path,0,packed,0,0,CommitSyncPolicy::ProcessCrash,compressed,packed_nodes,compact_values,false,compact_refs);
    prefix_edges(store);
  }
  for(unsigned round=0;round<6;++round) {
    Store store(path,round%2?65536:0,packed,0,round%2?65536:0,CommitSyncPolicy::ProcessCrash,compressed,packed_nodes,compact_values,false,compact_refs);
    verify(store,committed);store.begin();const auto saved=store.savepoint();auto working=committed;
    for(auto space:spaces) {
      // replace() must split old prefixes at namespace bytes and at either sign,
      // then collapse single-child branches when deleting a previously stored key.
      const auto single=boundaries[round];
      const auto text="single:"+std::to_string(round)+":"+std::to_string(space);
      store.put_in(space,single,text);working[space][single]=text;
      const auto absent=std::bit_cast<std::int64_t>(rng());
      CHECK(store.erase_in(space,absent)==(working[space].erase(absent)!=0));
      std::map<std::int64_t,std::optional<std::string>> changes;
      for(unsigned i=0;i<boundaries.size()+2;++i) {
        const auto key=i<boundaries.size()?boundaries[i]:std::bit_cast<std::int64_t>(rng());
        changes[key]=(round+i)%3?std::optional<std::string>(std::string(1+i*37,char('a'+round))):std::nullopt;
      }
      std::vector<Store::Mutation> batch;
      // Deliberately reverse caller order; apply_batch_in owns the ordering
      // proof used to skip only the prefix shared by *all* replacements.
      for(auto it=changes.rbegin();it!=changes.rend();++it) {
        batch.push_back({it->first,it->second});
        if(it->second)working[space][it->first]=*it->second;else working[space].erase(it->first);
      }
      store.apply_batch_in(space,batch);
      if(round%2) {
        CHECK(store.erase_in(space,single)==(working[space].erase(single)!=0));
      }
    }
    verify(store,working);
    if(round==2) {
      store.restore(saved);verify(store,committed);
      store.put_in(UINT64_MAX,INT64_MIN,"after restore");committed[UINT64_MAX][INT64_MIN]="after restore";
      store.commit();
    } else if(round==4) {
      store.rollback();verify(store,committed);
    } else {store.commit();committed=std::move(working);}
  }
  Store reopened(path,0,packed,0,0,CommitSyncPolicy::ProcessCrash,compressed,packed_nodes);
  verify(reopened,committed);
}
int main(int argc,char** argv) {
  try {
    CHECK(argc<=2);
    bool preserve=argc==2;fs::path dir;
    if(preserve) {dir=argv[1];CHECK(!fs::exists(dir));fs::create_directories(dir);}
    else {
      const char* env=std::getenv("MVDB_TEST_DIR");CHECK(env);fs::create_directories(env);
      auto tmp=(fs::path(env)/"mutation-prefix-XXXXXX").string();CHECK(mkdtemp(tmp.data()));dir=tmp;
    }
    trace(dir/"format02.db",false,false,false);
    trace(dir/"format03.db",true,false,false);
    trace(dir/"format04-unpacked.db",true,true,false);
    trace(dir/"format04-packed.db",true,true,true);
    trace(dir/"format05-packed.db",true,true,true,true);
    std::cout<<"PASS mutation-prefix model: four formats/layouts, all 16 first-fork bytes, empty/delete/reinsert, prefix replacement/restore, single and reversed batch writes, sign/namespace boundaries, restore/rollback/reopen\n";
    // An optional fresh directory preserves deterministic database bytes so the
    // same probe can be linked against the old core and compared with cmp.
    if(!preserve)fs::remove_all(dir);
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
