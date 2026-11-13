#pragma once
#include "minivectordb/store.hpp"
#include "packed_codec.hpp"
#include "page_checksum.hpp"
#include <map>
#include <vector>
#include <limits>
#include <bit>
#include <cerrno>
#include <cstring>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace minivectordb::detail {
// Format J08: immutable records linked to a previous logical head, plus two
// checksummed commit roots. Rollback changes reachability, never truncates a
// record that an in-flight scan or savepoint may still reference. The in-memory
// ordered directory stores offsets only; no second copy of row payloads.
class JournalStore {
  using Key=std::pair<std::uint64_t,std::int64_t>;
  using Page=std::array<unsigned char,4096>;
  struct Undo {Key key;std::uint64_t value,head;};
  struct Cached {Page bytes{};std::uint64_t page=UINT64_MAX;std::size_t size=0;};
  int fd_;
  CommitSyncPolicy policy_;
  std::uint64_t head_=0,durable_=0,end_=8192,generation_=0;
  std::map<Key,std::uint64_t> index_;
  std::size_t empty_entries_=0;
  std::vector<Undo> undo_;
  mutable std::vector<Cached> cache_;
  static constexpr std::size_t header_size=48;
  static void fail(){throw std::runtime_error("corrupt MiniVectorDB journal");}
  static void io(const char* what){throw std::system_error(errno,std::generic_category(),what);}
  static std::uint64_t get(const unsigned char* p,unsigned n=8){std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(p[i])<<(8*i);return v;}
  static void put(unsigned char* p,std::uint64_t v,unsigned n=8){for(unsigned i=0;i<n;++i)p[i]=static_cast<unsigned char>(v>>(8*i));}
  static std::uint32_t crc(std::string_view s){return ~page_crc32_span(0xffffffffU,reinterpret_cast<const unsigned char*>(s.data()),s.size());}
  void sync(){for(;;){profile::detail::Scope timer(&profile::Snapshot::fsync);if(::fsync(fd_)==0)return;if(errno!=EINTR)io("journal fsync");}}
  void write(std::uint64_t offset,std::string_view bytes){
    if(offset>std::uint64_t(std::numeric_limits<off_t>::max()) || bytes.size()>std::uint64_t(std::numeric_limits<off_t>::max())-offset)throw std::length_error("journal offset exhausted");
    if(!bytes.empty())for(auto& c:cache_)if(c.page>=offset/4096 && c.page<=(offset+bytes.size()-1)/4096)c.page=UINT64_MAX;
    std::size_t done=0;
    while(done<bytes.size()){
      ssize_t n;{profile::detail::Scope timer(&profile::Snapshot::pwrite);n=::pwrite(fd_,bytes.data()+done,bytes.size()-done,off_t(offset+done));if(n>0)timer.bytes(std::uint64_t(n));}
      if(n<0){if(errno==EINTR)continue;io("journal pwrite");}if(!n)throw std::runtime_error("zero-length journal pwrite");done+=std::size_t(n);
    }
  }
  std::size_t read_some(std::uint64_t offset,unsigned char* output,std::size_t size)const{
    std::size_t done=0;
    while(done<size){ssize_t n;{profile::detail::Scope timer(&profile::Snapshot::pread);n=::pread(fd_,output+done,size-done,off_t(offset+done));if(n>0)timer.bytes(std::uint64_t(n));}
      if(n<0){if(errno==EINTR)continue;io("journal pread");}if(!n)break;done+=std::size_t(n);
    }return done;
  }
  std::string read(std::uint64_t offset,std::size_t size)const{
    if(offset>end_ || size>end_-offset)fail();std::string out(size,'\0');std::size_t done=0;
    if(cache_.empty()){if(read_some(offset,reinterpret_cast<unsigned char*>(out.data()),size)!=size)fail();return out;}
    while(done<size){const auto at=offset+done,page=at/4096;auto& entry=cache_[page%cache_.size()];
      if(entry.page!=page){entry.page=UINT64_MAX;entry.size=read_some(page*4096,entry.bytes.data(),4096);entry.page=page;}
      const auto inside=std::size_t(at%4096),n=std::min(size-done,4096-inside);if(inside+n>entry.size)fail();
      std::memcpy(out.data()+done,entry.bytes.data()+inside,n);done+=n;
    }return out;
  }
  struct Record {Key key;std::uint64_t previous,length;std::uint32_t flags,checksum;};
  Record record(std::uint64_t at,std::uint64_t extent)const{
    if(at<8192 || at>extent || header_size>extent-at)fail();const auto bytes=read(at,header_size);const auto* p=reinterpret_cast<const unsigned char*>(bytes.data());
    if(std::memcmp(p,"JREC",4) || get(p+44,4)!=crc(std::string_view(bytes).substr(0,44)))fail();
    Record r{{get(p+8),std::bit_cast<std::int64_t>(get(p+16))},get(p+24),get(p+32),std::uint32_t(get(p+4,4)),std::uint32_t(get(p+40,4))};
    if(r.flags>2 || (r.previous && (r.previous<8192 || r.previous>=at)) || r.length>extent-at-header_size || (r.flags==2 && (r.length||r.checksum)))fail();return r;
  }
  auto reconstruct(std::uint64_t head,std::uint64_t extent)const{
    std::vector<std::uint64_t> positions;
    for(auto at=head;at;){positions.push_back(at);at=record(at,extent).previous;}
    std::map<Key,std::uint64_t> result;
    for(auto i=positions.rbegin();i!=positions.rend();++i){auto r=record(*i,extent);if(r.flags==2)result.erase(r.key);else result[r.key]=*i;}
    return result;
  }
  Page root(std::uint64_t generation)const{
    Page p{};std::memcpy(p.data(),"MVDBJL08",8);put(p.data()+8,generation);put(p.data()+16,head_);put(p.data()+24,end_);put(p.data()+32,page_checksum(p,32),4);return p;
  }
  static bool valid(const Page&p,std::uint64_t bytes){
    if(std::memcmp(p.data(),"MVDBJL08",8) || get(p.data()+32,4)!=page_checksum(p,32))return false;
    const auto head=get(p.data()+16),end=get(p.data()+24);return end>=8192 && end<=bytes && (!head || (head>=8192 && head<=end && header_size<=end-head));
  }
  void assign(std::uint64_t& slot,std::uint64_t value){if(!slot&&value)--empty_entries_;else if(slot&&!value)++empty_entries_;slot=value;}
  void purge_empty(){if(!empty_entries_)return;for(auto i=index_.begin();i!=index_.end();)if(!i->second){i=index_.erase(i);--empty_entries_;}else ++i;}
public:
  static bool magic(const unsigned char* p){return std::memcmp(p,"MVDBJL08",8)==0;}
  JournalStore(int fd,bool created,std::size_t cache_budget,CommitSyncPolicy policy):fd_(fd),policy_(policy){
    cache_.resize(std::min(cache_budget,std::size_t(512*1024))/sizeof(Cached));
    if(created){Page zero{};write(4096,{reinterpret_cast<const char*>(zero.data()),zero.size()});auto p=root(0);write(0,{reinterpret_cast<const char*>(p.data()),p.size()});sync();return;}
    struct stat stat{};if(::fstat(fd_,&stat))io("journal fstat");if(stat.st_size<8192)fail();end_=std::uint64_t(stat.st_size);
    Page a{},b{};if(read_some(0,a.data(),a.size())!=a.size()||read_some(4096,b.data(),b.size())!=b.size())fail();
    const bool av=valid(a,end_),bv=valid(b,end_);if(!av&&!bv)fail();const auto& p=!av||(bv&&get(b.data()+8)>get(a.data()+8))?b:a;
    generation_=get(p.data()+8);head_=durable_=get(p.data()+16);index_=reconstruct(head_,get(p.data()+24));
  }
  std::uint64_t head()const noexcept{return head_;}
  void begin(){undo_.clear();}
  std::optional<std::string> value(std::uint64_t space,std::int64_t key)const{
    const auto it=index_.find({space,key});if(it==index_.end()||!it->second)return {};return value_at(space,key,it->second);
  }
  std::string value_at(std::uint64_t space,std::int64_t key,std::uint64_t at)const{
    const auto r=record(at,end_);if(r.key!=Key{space,key}||r.flags==2)fail();auto payload=read(at+header_size,std::size_t(r.length));
    if(crc(payload)!=r.checksum)fail();return r.flags==1?unpack_bytes(payload,4064,true):payload;
  }
  void apply(std::uint64_t space,std::span<const Store::Mutation> changes){
    if(changes.size()>Store::max_batch_mutations)throw std::length_error("too many batch mutations");
    std::vector<const Store::Mutation*> order;order.reserve(changes.size());std::size_t bytes=0;
    for(const auto& c:changes){if(c.value&&changes.size()>1){if(c.value->size()>Store::max_batch_bytes-bytes)throw std::length_error("batch payload exceeds byte budget");bytes+=c.value->size();}order.push_back(&c);}
    std::sort(order.begin(),order.end(),[](auto a,auto b){return a->key<b->key;});for(std::size_t i=1;i<order.size();++i)if(order[i-1]->key==order[i]->key)throw std::invalid_argument("duplicate batch key");
    if(order.empty())return;
    std::string encoded,packed;std::vector<std::pair<Key,std::uint64_t>> updates;updates.reserve(order.size());auto previous=head_;
    for(const auto* c:order){std::string_view payload=c->value?std::string_view(*c->value):std::string_view{};unsigned flags=c->value?0:2;
      if(c->value&&pack_bytes(payload,packed,true)){payload=packed;flags=1;}
      if(encoded.size()>UINT64_MAX-end_)throw std::length_error("journal size");const auto at=end_+encoded.size();std::array<unsigned char,header_size> h{};
      std::memcpy(h.data(),"JREC",4);put(h.data()+4,flags,4);put(h.data()+8,space);put(h.data()+16,std::bit_cast<std::uint64_t>(c->key));put(h.data()+24,previous);put(h.data()+32,payload.size());put(h.data()+40,crc(payload),4);put(h.data()+44,crc({reinterpret_cast<const char*>(h.data()),44}),4);
      encoded.append(reinterpret_cast<const char*>(h.data()),h.size());encoded.append(payload);updates.push_back({{space,c->key},at});previous=at;
    }
    // Allocate all directory/undo storage before touching disk or live offsets.
    undo_.reserve(undo_.size()+updates.size());for(const auto& u:updates)if(index_.try_emplace(u.first,0).second)++empty_entries_;
    write(end_,encoded);end_+=encoded.size();
    for(std::size_t i=0;i<updates.size();++i){auto& current=index_.find(updates[i].first)->second;undo_.push_back({updates[i].first,current,head_});assign(current,order[i]->value?updates[i].second:0);head_=updates[i].second;}
  }
  void restore(std::uint64_t target){
    if(target==head_)return;
    const bool ancestor=std::any_of(undo_.begin(),undo_.end(),[&](auto&u){return u.head==target;});
    if(!ancestor){auto rebuilt=reconstruct(target,end_);index_.swap(rebuilt);empty_entries_=0;head_=target;undo_.clear();return;}
    while(head_!=target){auto u=undo_.back();undo_.pop_back();assign(index_.find(u.key)->second,u.value);head_=u.head;}
  }
  void rollback(){restore(durable_);undo_.clear();purge_empty();}
  void commit(){
    if(generation_==UINT64_MAX)throw std::overflow_error("generation exhausted");const auto next=generation_+1;
    if(policy_==CommitSyncPolicy::Ordered)sync();auto p=root(next);write((next%2)*4096,{reinterpret_cast<const char*>(p.data()),p.size()});sync();
    generation_=next;durable_=head_;undo_.clear();purge_empty();
  }
  auto snapshot(std::uint64_t space)const{
    std::vector<std::pair<std::int64_t,std::uint64_t>> rows;
    for(auto it=index_.lower_bound({space,INT64_MIN});it!=index_.end()&&it->first.first==space;++it)if(it->second)rows.push_back({it->first.second,it->second});return rows;
  }
};
}
