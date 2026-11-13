#include "minivectordb/store.hpp"
#include "page_checksum.hpp"
#include "packed_codec.hpp"
#include "packed_pointer.hpp"
#include "journal_store.hpp"
#include <array>
#include <algorithm>
#include <vector>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace minivectordb {
namespace {
constexpr std::size_t page_size=4096, payload=32, chunk_size=page_size-payload;
using Page=std::array<unsigned char,page_size>;
constexpr std::uint64_t packed_tag=1ULL<<63;
constexpr std::uint64_t branch_tag=1ULL<<62;
constexpr std::size_t max_slots=508;
constexpr std::uint64_t compressed_slot=1ULL<<31;
constexpr std::size_t prefix_offset=payload+256*8;
const char* page_magic(bool v3,bool v4=false,bool v5=false,bool v6=false,bool v7=false) {return v7?"MVDBPG07":v6?"MVDBPG06":v5?"MVDBPG05":v4?"MVDBPG04":v3?"MVDBPG03":"MVDBPG02";}
const char* root_magic(bool v3,bool v4=false,bool v5=false,bool v6=false,bool v7=false) {return v7?"MVDBRT07":v6?"MVDBRT06":v5?"MVDBRT05":v4?"MVDBRT04":v3?"MVDBRT03":"MVDBRT02";}
enum : std::uint32_t { branch=1, overflow=2, packed=3 };
std::atomic<std::uint64_t> next_owner{1};
[[noreturn]] void io_error(const char* action) {throw std::system_error(errno,std::generic_category(),action);}
[[noreturn]] void corrupt() {throw std::runtime_error("corrupt MiniVectorDB file");}
void encode(Page& p,std::size_t offset,std::uint64_t value,unsigned bytes=8) {
  for(unsigned i=0;i<bytes;++i) p[offset+i]=static_cast<unsigned char>(value>>(i*8));
}
std::uint64_t decode(const Page& p,std::size_t offset,unsigned bytes=8) {
  std::uint64_t value=0;
  for(unsigned i=0;i<bytes;++i) value|=static_cast<std::uint64_t>(p[offset+i])<<(i*8);
  return value;
}
using detail::page_checksum;
off_t byte_offset(std::uint64_t id) {
  if(id>static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())/page_size) throw std::length_error("database exceeds file offset range");
  return static_cast<off_t>(id*page_size);
}
void write_page(int fd,const Page& p,std::uint64_t id) {
  std::size_t done=0; auto start=byte_offset(id);
  while(done<p.size()) {
    ssize_t n;
    {
      profile::detail::Scope timer(&profile::Snapshot::pwrite);
      n=::pwrite(fd,p.data()+done,p.size()-done,start+static_cast<off_t>(done));
      if(n>0) timer.bytes(static_cast<std::uint64_t>(n));
    }
    if(n<0) {if(errno==EINTR) continue; io_error("pwrite");}
    if(n==0) throw std::runtime_error("zero-length pwrite");
    done+=static_cast<std::size_t>(n);
  }
}
void discard_clean_pages(int fd,off_t offset,off_t length) noexcept {
#if defined(POSIX_FADV_DONTNEED)
  // Advisory: failure/ignore changes only performance. Owned page copies and
  // CRC checks remain authoritative; dirty pages are never relied on as freed.
  (void)::posix_fadvise(fd,offset,length,POSIX_FADV_DONTNEED);
#else
  (void)fd;(void)offset;(void)length;
#endif
}
Page read_raw(int fd,std::uint64_t id,bool discard=false) {
  Page p{}; std::size_t done=0; auto start=byte_offset(id);
  while(done<p.size()) {
    ssize_t n;
    {
      profile::detail::Scope timer(&profile::Snapshot::pread);
      n=::pread(fd,p.data()+done,p.size()-done,start+static_cast<off_t>(done));
      if(n>0) timer.bytes(static_cast<std::uint64_t>(n));
    }
    if(n<0) {if(errno==EINTR) continue; io_error("pread");}
    if(n==0) corrupt();
    done+=static_cast<std::size_t>(n);
  }
  if(discard)discard_clean_pages(fd,start,page_size);
  return p;
}
void sync_fd(int fd) {
  for(;;) {
    int result;
    {
      profile::detail::Scope timer(&profile::Snapshot::fsync);
      result=::fsync(fd);
    }
    if(result==0) return;
    if(errno!=EINTR) io_error("fsync");
  }
}
Page make_root(std::uint64_t generation,std::uint64_t root,std::uint64_t extent,bool v3,bool v4=false,bool v5=false,bool v6=false,bool v7=false) {
  Page p{}; std::memcpy(p.data(),root_magic(v3,v4,v5,v6,v7),8);
  encode(p,8,generation); encode(p,16,root); encode(p,24,extent);
  encode(p,32,page_checksum(p,32),4); return p;
}
bool valid_root(const Page& p,std::uint64_t complete_pages) {
  if((std::memcmp(p.data(),root_magic(false),8)!=0 && std::memcmp(p.data(),root_magic(true),8)!=0 && std::memcmp(p.data(),root_magic(true,true),8)!=0 && std::memcmp(p.data(),root_magic(true,true,true),8)!=0 && std::memcmp(p.data(),root_magic(true,true,true,true),8)!=0 && std::memcmp(p.data(),root_magic(true,true,true,true,true),8)!=0) || decode(p,32,4)!=page_checksum(p,32)) return false;
  auto root=decode(p,16), extent=decode(p,24);
  if((root&branch_tag) && !(root&packed_tag)) {
    if((p[7]!='4' && p[7]!='5' && p[7]!='6' && p[7]!='7') || (root&511)>=max_slots)return false;
    root=(root&~branch_tag)>>9;
  }
  return extent>=2 && extent<=complete_pages && (root==0 || (root>=2 && root<extent));
}
std::uint64_t ordered_key(std::int64_t key) {return std::bit_cast<std::uint64_t>(key)^(1ULL<<63);}
std::int64_t signed_key(std::uint64_t key) {return std::bit_cast<std::int64_t>(key^(1ULL<<63));}
unsigned key_index(std::uint64_t space,std::uint64_t key,unsigned depth) {return static_cast<unsigned>(((depth<8?space:key)>>((7-depth%8)*8))&255);}
}

struct Store::Impl {
  std::unique_ptr<detail::JournalStore> journal;
  struct CacheEntry {
    Page page{};
    std::uint64_t id=0;
    unsigned depth=0;
  };
  // Direct mapped: a fixed allocation, with no per-read allocation or eviction list.
  std::unique_ptr<CacheEntry[]> cache;
  std::size_t cache_count=0;
  mutable CacheStats stats;
  void initialize_cache(std::size_t budget) {
    cache_count=budget/sizeof(CacheEntry);
    if(cache_count) cache=std::make_unique<CacheEntry[]>(cache_count);
    stats.capacity_bytes=cache_count*sizeof(CacheEntry);
  }
  struct DataEntry {
    Page page{};
    std::uint64_t id=0;
    unsigned char rank=0;
  };
  struct DataSet {std::array<DataEntry,4> ways{};};
  mutable std::unique_ptr<DataSet[]> data_cache;
  std::size_t data_set_count=0;
  mutable std::size_t allocated_data_sets=0;
  bool write_cache_hint=false;
  mutable CacheStats data_stats;
  DataSet* data_set(std::uint64_t id) const {
    if(!data_set_count)return nullptr;
    // Write-heavy startup needs only a small working set of recently read
    // metadata pages. Grow once for standalone reads; never exceed the caller's
    // existing budget or retain old/new arenas together during that growth.
    const bool writing=active||write_cache_hint;
    const auto target=writing?std::min(data_set_count,std::size_t(512*1024)/sizeof(DataSet)):data_set_count;
    if(!data_cache || (!writing && allocated_data_sets<target)) {
      data_cache.reset();allocated_data_sets=0;
      data_stats.evictions+=data_stats.resident_pages;
      data_stats.resident_pages=data_stats.capacity_bytes=0;
      data_cache=std::make_unique<DataSet[]>(target);
      allocated_data_sets=target;
      data_stats.capacity_bytes=allocated_data_sets*sizeof(DataSet);
    }
    return &data_cache[id%allocated_data_sets];
  }
  // Ranks are a permutation of 0..resident-1, with 0 most recently used.
  // Updating only ranks younger than the selected entry avoids timestamp overflow.
  static void touch(DataSet& set,DataEntry& entry) {
    const auto previous=entry.id?entry.rank:4;
    for(auto& other:set.ways)
      if(&other!=&entry && other.id && other.rank<previous)++other.rank;
    entry.rank=0;
  }
  int fd=-1;
  std::uint64_t durable_root=0, root=0, generation=0, next_page=2, epoch=0;
  const std::uint64_t owner=next_owner.fetch_add(1);
  bool active=false, poisoned=false, format3=false, packing=false, compressed_paths=false, packed_branches=true, compact_values=false;
  bool discard_file_cache=false,compact_node_refs=false,byteplane_values=false;
  CommitSyncPolicy commit_sync_policy=CommitSyncPolicy::Ordered;
  std::size_t scan_budget=0;
  // Only the entry array is allocated, lazily on the first packed reference.
  // Each scan owns its array, including recursive scans from a visitor.
  struct ScanCache {
    struct Entry {Page page{}; std::uint64_t id=0;};
    std::unique_ptr<Entry[]> entries;
    std::size_t count=0, budget=0;
    explicit ScanCache(std::size_t bytes):budget(bytes) {}
    Entry* slot(std::uint64_t id) {
      if(!entries) {
        count=budget/sizeof(Entry);
        if(!count)return nullptr;
        entries=std::make_unique<Entry[]>(count);
      }
      return &entries[id%count];
    }
  };
  std::uint64_t physical(std::uint64_t ref,bool allow_packed=false,bool allow_branch=false) const {
    auto id=ref;
    if(ref&packed_tag) {
      if(!format3 || !allow_packed || (ref&511)>=max_slots)corrupt();
      id=(ref&~packed_tag)>>9;
    } else if(ref&branch_tag) {
      if(!compressed_paths || !allow_branch || (ref&511)>=max_slots)corrupt();
      id=(ref&~branch_tag)>>9;
    }
    if(id<2 || id>=next_page)corrupt();
    return id;
  }
  ~Impl() {if(fd>=0) ::close(fd);}
  void usable() const {if(poisoned) throw std::runtime_error("commit outcome uncertain; close and reopen Store");}
  void transaction() const {usable(); if(!active) throw std::logic_error("no active transaction");}
  Page branch_view(const Page& page,std::uint64_t id,unsigned depth) const {
    if(!compressed_paths || decode(page,12,4)==depth)return page;
    // Expand only in memory. Persisted edges remain strictly backward by page
    // address; a virtual unary edge may point to this same immutable node.
    auto result=empty_page(branch,depth);
    std::copy_n(page.data()+prefix_offset,16,result.data()+prefix_offset);
    encode(result,payload+page[prefix_offset+depth]*8,id);
    return result;
  }
  Page branch_at(std::uint64_t id,unsigned depth,std::uint64_t space,std::uint64_t key,bool expand=true) const {
    auto page=read(id,branch,depth,expand);
    if(compressed_paths)for(unsigned i=0;i<depth;++i)
      if(page[prefix_offset+i]!=key_index(space,key,i))corrupt();
    return page;
  }
  Page unpack_branch(std::uint64_t ref) const {
    const auto page=read(physical(ref,false,true),packed);
    const auto slot=ref&511;
    if(slot>=decode(page,12,4))corrupt();
    const auto start=decode(page,payload+slot*8,4),encoded_length=decode(page,payload+slot*8+4,4);
    const auto stored_length=encoded_length&~compressed_slot;
    std::string decoded;
    const unsigned char* bytes=page.data()+start;std::size_t length=stored_length;
    if(encoded_length&compressed_slot) {
      if(!compact_values)corrupt();
      decoded=detail::unpack_bytes({reinterpret_cast<const char*>(bytes),length},2098);
      bytes=reinterpret_cast<const unsigned char*>(decoded.data());length=decoded.size();
    }
    if(length<50 || (bytes[0]!=1 && !(compact_node_refs && bytes[0]==2)))corrupt();
    const auto depth=bytes[1];if(depth>15)corrupt();
    auto result=empty_page(branch,depth);
    std::copy_n(bytes+2,16,result.data()+prefix_offset);
    std::size_t offset=50,count=0;
    for(unsigned i=0;i<256;++i)if(bytes[18+i/8]&(1u<<(i%8))) {
      std::uint64_t child=0;
      if(bytes[0]==2)child=detail::read_packed_pointer({reinterpret_cast<const char*>(bytes),length},offset);
      else {
        if(offset+8>length)corrupt();
        for(unsigned n=0;n<8;++n)child|=std::uint64_t(bytes[offset+n])<<(8*n);
        offset+=8;
      }
      if(!child)corrupt();
      encode(result,payload+i*8,child);++count;
    }
    if(!count || offset!=length)corrupt();
    return result;
  }
  Page read(std::uint64_t id,std::uint32_t type,unsigned depth=0,bool expand=true) const {
    profile::detail::count(type==branch?&profile::Snapshot::page_read_branch:
        type==packed?&profile::Snapshot::page_read_packed:&profile::Snapshot::page_read_overflow);
    const auto physical_id=physical(id,false,type==branch);
    CacheEntry* slot=nullptr;
    if(type==branch && cache_count) {
      auto cache_key=id;
      if(id&branch_tag) {
        // Packed addresses combine page<<9 with a slot. Direct modulo aliases
        // neighbouring page/slot ranges after denser packing. Mix those bits
        // before choosing a cache entry; full immutable IDs still guard hits.
        cache_key^=cache_key>>30;cache_key*=UINT64_C(0xbf58476d1ce4e5b9);
        cache_key^=cache_key>>27;cache_key*=UINT64_C(0x94d049bb133111eb);
        cache_key^=cache_key>>31;
      }
      slot=&cache[cache_key%cache_count];
      if(slot->id==id) {
        // Range was checked above; a validated page is only reusable at its depth/type.
        if((compressed_paths?slot->depth<depth:slot->depth!=depth) || decode(slot->page,8,4)!=type) corrupt();
        ++stats.hits;
        return expand?branch_view(slot->page,id,depth):slot->page;
      }
      ++stats.misses;
    }
    DataSet* set=nullptr;
    DataEntry* data_slot=nullptr;
    if(type==packed || type==overflow) {
      set=data_set(id);
      if(set) {
        for(auto& entry:set->ways)if(entry.id==id) {
          if(decode(entry.page,8,4)!=type)corrupt();
          touch(*set,entry); ++data_stats.hits;
          return entry.page;
        }
        ++data_stats.misses;
        for(auto& entry:set->ways) {
          if(!entry.id) {data_slot=&entry; break;}
          if(!data_slot || entry.rank>data_slot->rank)data_slot=&entry;
        }
      }
    }
    const bool packed_node=type==branch && (id&branch_tag)!=0;
    auto p=packed_node?unpack_branch(id):read_raw(fd,id,discard_file_cache);
    if(std::memcmp(p.data(),page_magic(format3,compressed_paths,compact_values,compact_node_refs,byteplane_values),8)!=0 || (!packed_node && decode(p,24,4)!=page_checksum(p,24)) || decode(p,8,4)!=type) corrupt();
    if(type==branch) {
      const auto stored_depth=decode(p,12,4);
      if(stored_depth>15 || (compressed_paths?stored_depth<depth:stored_depth!=depth)) corrupt();
      if(compressed_paths)for(auto i=stored_depth;i<16;++i)
        if(p[prefix_offset+static_cast<std::size_t>(i)]!=0)corrupt();
      for(unsigned i=0;i<256;++i) {
        auto child=decode(p,payload+i*8);
        if(child) {
          const auto target=physical(child,stored_depth==15,stored_depth<15);
          if(target>physical_id)corrupt();
          if(target==physical_id && !(stored_depth<15 && packed_node &&
              (child&branch_tag) && !(child&packed_tag) && (child&511)<(id&511)))corrupt();
        }
      }
    } else if(type==overflow) {
      auto next=decode(p,16);
      if(decode(p,12,4)>chunk_size || (next!=0 && physical(next)>=id)) corrupt();
    } else {
      if(!format3)corrupt();
      const auto count=decode(p,12,4);
      if(!count || count>max_slots || decode(p,16)!=0)corrupt();
      auto end=page_size;
      const auto directory_end=payload+count*8;
      for(std::size_t i=0;i<count;++i) {
        const auto offset=decode(p,payload+i*8,4), encoded_length=decode(p,payload+i*8+4,4);
        if((encoded_length&compressed_slot) && !compact_values)corrupt();
        const auto length=encoded_length&~compressed_slot;
        // Canonical tightly packed reverse payload: validates every slot before admission.
        if(offset<directory_end || offset>end || length!=end-offset)corrupt();
        end=offset;
      }
    }
    if(slot) {
      if(slot->id) ++stats.evictions;
      else ++stats.resident_pages;
      slot->page=p; slot->depth=static_cast<unsigned>(decode(p,12,4)); slot->id=id;
    }
    if(data_slot) {
      if(data_slot->id)++data_stats.evictions;
      else ++data_stats.resident_pages;
      touch(*set,*data_slot);
      data_slot->page=p; data_slot->id=id;
    }
    return type==branch&&expand?branch_view(p,id,depth):p;
  }
  std::uint64_t append(Page p) {
    // Publish an address only after its complete write. A failed page has no root references.
    auto id=next_page; if(id&(packed_tag|branch_tag))throw std::length_error("database page address exhausted"); encode(p,24,page_checksum(p,24),4); write_page(fd,p,id); ++next_page; return id;
  }
  Page empty_page(std::uint32_t type,unsigned used) const {
    Page p{}; std::memcpy(p.data(),page_magic(format3,compressed_paths,compact_values,compact_node_refs,byteplane_values),8); encode(p,8,type,4); encode(p,12,used,4); return p;
  }
  struct NodeBatch {
    Impl& store;
    Page page;
    std::size_t slots=0,end=page_size;
    std::string compressed,compact;
    explicit NodeBatch(Impl& owner):store(owner),page(owner.empty_page(packed,0)) {}
    bool enabled() const {return store.compressed_paths && store.packed_branches;}
    void flush() {
      if(!slots)return;
      encode(page,12,slots,4);store.append(page);
      page=store.empty_page(packed,0);slots=0;end=page_size;
    }
    std::uint64_t add(const Page& node) {
      std::array<unsigned char,50+256*8> bytes{};
      bytes[0]=1;bytes[1]=static_cast<unsigned char>(decode(node,12,4));
      std::copy_n(node.data()+prefix_offset,16,bytes.data()+2);
      std::size_t length=50;
      for(unsigned i=0;i<256;++i)if(auto child=decode(node,payload+i*8)) {
        bytes[18+i/8]|=static_cast<unsigned char>(1u<<(i%8));
        for(unsigned n=0;n<8;++n)bytes[length+n]=static_cast<unsigned char>(child>>(8*n));
        length+=8;
      }
      std::string_view contents(reinterpret_cast<const char*>(bytes.data()),length);
      if(store.compact_node_refs) {
        compact.assign(contents.substr(0,50));compact[0]=2;
        for(unsigned i=0;i<256;++i)if(auto child=decode(node,payload+i*8))
          detail::append_packed_pointer(compact,child);
        // Large addresses retain version1 if canonical varints do not shrink.
        if(compact.size()<contents.size())contents=compact;
      }
      const bool encoded=store.compact_values && detail::pack_bytes(contents,compressed);
      if(encoded)contents=compressed;
      length=contents.size();
      if(slots==max_slots || end<payload+(slots+1)*8+length)flush();
      if(store.next_page>((branch_tag-1)>>9))throw std::length_error("node page address exhausted");
      const auto reference=branch_tag|(store.next_page<<9)|slots;
      end-=length;std::memcpy(page.data()+end,contents.data(),length);
      encode(page,payload+slots*8,end,4);encode(page,payload+slots*8+4,length|(encoded?compressed_slot:0),4);++slots;
      return reference;
    }
  };
  std::uint64_t publish_branch(Page page,unsigned depth,std::uint64_t space,std::uint64_t key,NodeBatch& nodes) {
    unsigned count=0; std::uint64_t only=0;
    for(unsigned i=0;i<256;++i)if(auto child=decode(page,payload+i*8)) {++count;only=child;}
    if(!count)return 0;
    if(compressed_paths) {
      // Values at depth15 are not nodes. Only intermediate unary nodes can
      // disappear; their prefix lives in the first retained descendant.
      if(count==1 && depth<15)return only;
      for(unsigned i=0;i<16;++i)
        page[prefix_offset+i]=i<depth?static_cast<unsigned char>(key_index(space,key,i)):0;
    }
    return nodes.enabled()?nodes.add(page):append(page);
  }
  std::uint64_t write_value(std::string_view value) {
    std::uint64_t next=0; std::size_t end=value.size();
    do {
      const auto start=end>chunk_size?end-chunk_size:0, length=end-start;
      auto p=empty_page(overflow,static_cast<unsigned>(length)); encode(p,16,next);
      if(length) std::memcpy(p.data()+payload,value.data()+start,length);
      next=append(p); end=start;
    } while(end);
    return next;
  }
  std::string value(std::uint64_t id,ScanCache* memo=nullptr) const {
    if(id&packed_tag) {
      const auto page_id=physical(id,true);
      auto* slot=memo?memo->slot(page_id):nullptr;
      const auto index=id&511;
      auto contents=[&](const Page& page) {
        if(index>=decode(page,12,4))corrupt();
        // Return an owned string before the visitor can mutate or recursively scan.
        const auto length=decode(page,payload+index*8+4,4);
        const std::string_view encoded(reinterpret_cast<const char*>(page.data()+decode(page,payload+index*8,4)),
                                       static_cast<std::size_t>(length&~compressed_slot));
        if(length&compressed_slot) {
          if(!compact_values)corrupt();
          return detail::unpack_bytes(encoded,4064,byteplane_values);
        }
        return std::string(encoded);
      };
      if(slot && slot->id==page_id) {
        profile::detail::count(&profile::Snapshot::page_read_packed);
        profile::detail::count(&profile::Snapshot::packed_cache_hit);
        return contents(slot->page);
      }
      const auto page=read(page_id,packed);
      if(slot) {slot->page=page; slot->id=page_id;}
      return contents(page);
    }
    std::string result;
    while(id) {auto p=read(id,overflow); result.append(reinterpret_cast<const char*>(p.data()+payload),static_cast<std::size_t>(decode(p,12,4))); id=decode(p,16);}
    return result;
  }
  std::uint64_t find(std::uint64_t space,std::uint64_t key) const {
    auto id=root;
    for(unsigned depth=0;depth<16 && id;) {
      auto p=branch_at(id,depth,space,key,false);
      const auto terminal=static_cast<unsigned>(decode(p,12,4));
      if(compressed_paths)for(unsigned i=depth;i<terminal;++i)
        if(p[prefix_offset+i]!=key_index(space,key,i))return 0;
      id=decode(p,payload+key_index(space,key,terminal)*8);depth=terminal+1;
    }
    return id;
  }
  std::uint64_t locate_namespace(std::uint64_t snapshot,std::uint64_t space) const {
    auto id=snapshot;
    for(unsigned depth=0;depth<8 && id;) {
      const auto page=branch_at(id,depth,space,0,false);
      const auto terminal=static_cast<unsigned>(decode(page,12,4));
      if(compressed_paths)for(unsigned i=depth;i<std::min(terminal,8u);++i)
        if(page[prefix_offset+i]!=key_index(space,0,i))return 0;
      if(terminal>=8)return id;
      id=decode(page,payload+key_index(space,0,terminal)*8);depth=terminal+1;
    }
    return id;
  }
  Page mutation_branch(std::uint64_t id,unsigned& depth,std::uint64_t space,
                       std::uint64_t first,std::uint64_t last) const {
    if(!compressed_paths)return id?branch_at(id,depth,space,first):empty_page(branch,depth);
    // The caller's ordered batch shares every prefix byte common to its first
    // and last key. Missing subtrees need no virtual unary ancestors at all.
    if(!id) {
      while(depth<15 && key_index(space,first,depth)==key_index(space,last,depth))++depth;
      return empty_page(branch,depth);
    }
    auto page=branch_at(id,depth,space,first,false);
    const auto terminal=static_cast<unsigned>(decode(page,12,4));
    // Existing nodes additionally constrain that common prefix. Stop at the
    // first divergence, so branch_view retains the old subtree as a sibling;
    // never skip the actual branching layer or a replacement's different byte.
    while(depth<terminal) {
      const auto index=key_index(space,first,depth);
      if(index!=key_index(space,last,depth) || index!=page[prefix_offset+depth])break;
      ++depth;
    }
    return branch_view(page,id,depth);
  }
  std::uint64_t replace(std::uint64_t id,unsigned depth,std::uint64_t space,std::uint64_t key,std::uint64_t new_value,NodeBatch& nodes) {
    auto p=mutation_branch(id,depth,space,key,key);
    auto offset=payload+key_index(space,key,depth)*8;
    auto child=depth==15?new_value:replace(decode(p,offset),depth+1,space,key,new_value,nodes);
    encode(p,offset,child);
    return publish_branch(p,depth,space,key,nodes);
  }
  struct Replacement { std::uint64_t key, value; };
  std::uint64_t merge_batch(std::uint64_t id,unsigned depth,std::uint64_t space,
                            std::span<const Replacement> replacements,NodeBatch& nodes) {
    auto page=mutation_branch(id,depth,space,replacements.front().key,replacements.back().key);
    bool changed=false;
    for(std::size_t first=0;first<replacements.size();) {
      const auto index=key_index(space,replacements[first].key,depth);
      auto end=first+1;
      while(end<replacements.size() && key_index(space,replacements[end].key,depth)==index)++end;
      const auto offset=payload+index*8;
      const auto old_child=decode(page,offset);
      const auto child=depth==15?replacements[first].value:
          merge_batch(old_child,depth+1,space,replacements.subspan(first,end-first),nodes);
      if(child!=old_child) { encode(page,offset,child); changed=true; }
      first=end;
    }
    if(!changed)return id;
    return publish_branch(page,depth,space,replacements.front().key,nodes);
  }
  void walk_equal(std::uint64_t left,std::uint64_t right,unsigned depth,std::uint64_t prefix,
                  std::uint64_t left_space,std::uint64_t right_space,
                  const std::function<void(std::int64_t,std::string_view,std::string_view)>& visitor,
                  ScanCache& memo) const {
    if(!left && !right)return;
    if(!left || !right)throw KeySetMismatch();
    // Copies remain valid if visitor mutations evict cache entries or change root.
    const auto a=branch_at(left,depth,left_space,prefix);
    const auto b=branch_at(right,depth,right_space,prefix);
    for(unsigned i=0;i<256;++i) {
      const auto ac=decode(a,payload+i*8),bc=decode(b,payload+i*8);
      if(!ac && !bc)continue;
      if(!ac || !bc)throw KeySetMismatch();
      const auto key=prefix|(static_cast<std::uint64_t>(i)<<((15-depth)*8));
      if(depth==15) {
        const auto av=value(ac,&memo),bv=value(bc,&memo);
        visitor(signed_key(key),av,bv);
      } else walk_equal(ac,bc,depth+1,key,left_space,right_space,visitor,memo);
    }
  }
  void walk(std::uint64_t id,unsigned depth,std::uint64_t prefix,std::uint64_t space,const std::function<void(std::int64_t,std::string_view)>& visitor,ScanCache& memo) const {
    if(!id) return;
    auto p=branch_at(id,depth,space,prefix,false);
    const auto terminal=static_cast<unsigned>(decode(p,12,4));
    if(compressed_paths)for(unsigned i=depth;i<terminal;++i)
      prefix|=static_cast<std::uint64_t>(p[prefix_offset+i])<<((15-i)*8);
    depth=terminal;
    for(unsigned i=0;i<256;++i) {
      auto child=decode(p,payload+i*8); if(!child) continue;
      auto key=prefix|(static_cast<std::uint64_t>(i)<<((15-depth)*8));
      if(depth==15) {auto contents=value(child,&memo); visitor(signed_key(key),contents);}
      else walk(child,depth+1,key,space,visitor,memo);
    }
  }
};

Store::Store(const std::filesystem::path& path,std::size_t index_cache_bytes,bool packed_values,std::size_t packed_scan_cache_bytes,std::size_t data_cache_bytes,CommitSyncPolicy commit_sync_policy,bool compressed_paths,bool packed_branches,bool compact_values,bool discard_file_cache,bool compact_node_refs,bool byteplane_values,bool journal_format):impl_(std::make_unique<Impl>()) {
  if(commit_sync_policy!=CommitSyncPolicy::Ordered && commit_sync_policy!=CommitSyncPolicy::ProcessCrash)
    throw std::invalid_argument("invalid commit synchronization policy");
  bool created=false;
  impl_->fd=::open(path.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600);
  if(impl_->fd>=0) created=true;
  else {
    if(errno!=EEXIST) io_error("create database");
    impl_->fd=::open(path.c_str(),O_RDWR|O_CLOEXEC);
    if(impl_->fd<0) io_error("open database");
  }
  auto& s=*impl_;
  s.commit_sync_policy=commit_sync_policy;
  s.compressed_paths=compressed_paths;
  s.packed_branches=packed_branches;
  s.compact_values=compact_values && compressed_paths && packed_values;
  s.discard_file_cache=discard_file_cache;
  s.compact_node_refs=compact_node_refs && s.compact_values;
  s.byteplane_values=byteplane_values && s.compact_node_refs;
  s.packing=packed_values; s.format3=packed_values||compressed_paths; s.scan_budget=packed_scan_cache_bytes;
  s.data_set_count=data_cache_bytes/sizeof(Impl::DataSet);
  if(::flock(s.fd,LOCK_EX|LOCK_NB)!=0) io_error("lock database");
  if(created && journal_format) {
    s.journal=std::make_unique<detail::JournalStore>(s.fd,true,data_cache_bytes,commit_sync_policy);
    auto parent=path.parent_path();if(parent.empty())parent=".";
    int directory=::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(directory<0)io_error("open database directory");
    try{sync_fd(directory);}catch(...){::close(directory);throw;}::close(directory);return;
  }
  if(created) {
    write_page(s.fd,Page{},1); write_page(s.fd,make_root(0,0,2,s.format3,s.compressed_paths,s.compact_values,s.compact_node_refs,s.byteplane_values),0); sync_fd(s.fd);
    auto parent=path.parent_path(); if(parent.empty()) parent=".";
    int directory=::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(directory<0) io_error("open database directory");
    try {sync_fd(directory);} catch(...) {::close(directory); throw;}
    ::close(directory); s.initialize_cache(index_cache_bytes); return;
  }
  struct stat info{}; if(::fstat(s.fd,&info)!=0) io_error("stat database");
  if(info.st_size<static_cast<off_t>(2*page_size)) corrupt();
  auto bytes=static_cast<std::uint64_t>(info.st_size), complete=bytes/page_size;
  auto a=read_raw(s.fd,0), b=read_raw(s.fd,1);
  if(detail::JournalStore::magic(a.data())||detail::JournalStore::magic(b.data())){s.journal=std::make_unique<detail::JournalStore>(s.fd,false,data_cache_bytes,commit_sync_policy);return;}
  if(std::memcmp(a.data(),"MVDBRT01",8)==0 || std::memcmp(b.data(),"MVDBRT01",8)==0) throw std::runtime_error("unsupported MiniVectorDB format 01; format 02 required");
  bool av=valid_root(a,complete), bv=valid_root(b,complete);
  if(!av && !bv) corrupt();
  if(av && bv && a[7]!=b[7])corrupt();
  const auto& selected=(!av || (bv && decode(b,8)>decode(a,8)))?b:a;
  s.byteplane_values=selected[7]=='7';
  s.compact_node_refs=selected[7]=='6'||s.byteplane_values;
  s.compact_values=selected[7]=='5'||s.compact_node_refs;
  s.compressed_paths=selected[7]=='4'||s.compact_values;
  s.format3=selected[7]=='3'||s.compressed_paths;
  s.generation=decode(selected,8); s.root=s.durable_root=decode(selected,16);
  // Validate the published root against its durable extent. Trailing uncommitted pages are ignored.
  s.next_page=decode(selected,24);
  if(s.root) (void)s.read(s.root,branch,0);
  s.next_page=complete+(bytes%page_size!=0);
  // Allocate after durable-root validation: opening never populates the page cache.
  s.initialize_cache(index_cache_bytes);
}
Store::~Store()=default;
void Store::set_write_cache_hint(bool enabled) noexcept {impl_->write_cache_hint=enabled;}
CacheStats Store::data_cache_stats() const noexcept {return impl_->data_stats;}
void Store::reset_data_cache_stats() noexcept {
  impl_->data_stats.hits=impl_->data_stats.misses=impl_->data_stats.evictions=0;
}
CacheStats Store::cache_stats() const noexcept {return impl_->stats;}
void Store::reset_cache_stats() noexcept {
  impl_->stats.hits=impl_->stats.misses=impl_->stats.evictions=0;
}
void Store::begin() {
  auto& s=*impl_; s.usable(); if(s.active) throw std::logic_error("transaction already active");
  if(s.epoch==UINT64_MAX) throw std::overflow_error("transaction epoch exhausted");
  ++s.epoch; s.root=s.durable_root; s.active=true;if(s.journal)s.journal->begin();
}
std::optional<std::string> Store::get(std::int64_t key) const {return get_in(0,key);}
void Store::put(std::int64_t key,std::string_view value) {put_in(0,key,value);}
bool Store::erase(std::int64_t key) {return erase_in(0,key);}
std::optional<std::string> Store::get_in(std::uint64_t space,std::int64_t key) const {
  auto& s=*impl_; s.usable();if(s.journal)return s.journal->value(space,key); auto id=s.find(space,ordered_key(key));
  if(!id) return std::nullopt;
  return s.value(id);
}
void Store::put_in(std::uint64_t space,std::int64_t key,std::string_view value) {
  auto& s=*impl_; s.transaction();
  if(s.journal){Mutation change{key,std::string(value)};s.journal->apply(space,{&change,1});return;}
  const auto data=s.write_value(value);
  Impl::NodeBatch nodes(s);
  const auto next=s.replace(s.root,0,space,ordered_key(key),data,nodes);
  nodes.flush();
  s.root=next;
}
bool Store::erase_in(std::uint64_t space,std::int64_t key) {
  auto& s=*impl_; s.transaction();if(s.journal){if(!s.journal->value(space,key))return false;Mutation change{key,std::nullopt};s.journal->apply(space,{&change,1});return true;} auto ordered=ordered_key(key);
  if(!s.find(space,ordered)) return false;
  Impl::NodeBatch nodes(s);
  const auto next=s.replace(s.root,0,space,ordered,0,nodes);nodes.flush();s.root=next;return true;
}
void Store::apply_batch_in(std::uint64_t space,std::span<const Mutation> mutations) {
  auto& state=*impl_; state.transaction();
  if(state.journal){state.journal->apply(space,mutations);return;}
  if(mutations.size()>max_batch_mutations)throw std::length_error("too many batch mutations");
  std::vector<const Mutation*> sorted;
  sorted.reserve(mutations.size());
  std::size_t bytes=0;
  for(const auto& mutation:mutations) {
    if(mutation.value && mutations.size()>1) {
      if(mutation.value->size()>max_batch_bytes-bytes)throw std::length_error("batch payload exceeds byte budget");
      bytes+=mutation.value->size();
    }
    sorted.push_back(&mutation);
  }
  std::sort(sorted.begin(),sorted.end(),[](auto a,auto b){return ordered_key(a->key)<ordered_key(b->key);});
  for(std::size_t i=1;i<sorted.size();++i)
    if(sorted[i-1]->key==sorted[i]->key)throw std::invalid_argument("duplicate batch key");
  if(sorted.empty())return;
  std::vector<Impl::Replacement> replacements;
  replacements.reserve(sorted.size());
  Page packed_page=state.empty_page(packed,0);
  std::size_t slot_count=0, payload_start=page_size;
  std::vector<std::size_t> pending;
  pending.reserve(sorted.size());
  std::string compressed;
  auto flush=[&] {
    if(!slot_count)return;
    if(slot_count==1 && !(decode(packed_page,payload+4,4)&compressed_slot)) {
      // Packing one record saves no pages; retain the simpler ordinary read path.
      const std::string_view contents(reinterpret_cast<const char*>(packed_page.data()+payload_start),
                                      page_size-payload_start);
      replacements[pending.front()].value=state.write_value(contents);
    } else {
      if(state.next_page>((packed_tag-1)>>9))throw std::length_error("packed page address exhausted");
      encode(packed_page,12,slot_count,4);
      const auto id=state.append(packed_page);
      for(std::size_t slot=0;slot<pending.size();++slot)
        replacements[pending[slot]].value=packed_tag|(id<<9)|slot;
    }
    packed_page=state.empty_page(packed,0); slot_count=0; payload_start=page_size; pending.clear();
  };
  for(const auto* mutation:sorted) {
    replacements.push_back({ordered_key(mutation->key),0});
    if(!mutation->value)continue;
    std::string_view contents=*mutation->value;
    if(!state.format3 || !state.packing || contents.size()>chunk_size-8) {
      replacements.back().value=state.write_value(contents); continue;
    }
    const bool encoded=state.compact_values && detail::pack_bytes(contents,compressed,state.byteplane_values);
    if(encoded)contents=compressed;
    const auto directory_end=payload+(slot_count+1)*8;
    if(slot_count==max_slots || payload_start<directory_end || contents.size()>payload_start-directory_end)flush();
    payload_start-=contents.size();
    encode(packed_page,payload+slot_count*8,payload_start,4);
    encode(packed_page,payload+slot_count*8+4,contents.size()|(encoded?compressed_slot:0),4);
    if(!contents.empty())std::memcpy(packed_page.data()+payload_start,contents.data(),contents.size());
    pending.push_back(replacements.size()-1); ++slot_count;
  }
  flush();
  // Values and every changed child precede their immutable parent. The working root
  // is assigned only when the whole merge succeeds; failed appends are unreachable.
  Impl::NodeBatch nodes(state);
  const auto next=state.merge_batch(state.root,0,space,replacements,nodes);
  nodes.flush();
  state.root=next;
}
void Store::commit() {
  auto& s=*impl_; s.transaction();
  if(s.journal){try{s.journal->commit();}catch(...){s.poisoned=true;throw;}s.active=false;return;}
  if(s.generation==UINT64_MAX) throw std::overflow_error("generation exhausted");
  auto next=s.generation+1;
  try {
    // Every reachable COW page has already completed its write loop. Ordered
    // additionally persists these dependencies before publishing the root.
    if(s.commit_sync_policy==CommitSyncPolicy::Ordered)sync_fd(s.fd);
    write_page(s.fd,make_root(next,s.root,s.next_page,s.format3,s.compressed_paths,s.compact_values,s.compact_node_refs,s.byteplane_values),next%2);
    // Both modes acknowledge only after the root AND its data have been synced.
    sync_fd(s.fd);
    if(s.discard_file_cache)discard_clean_pages(s.fd,0,0);
  } catch(...) {s.poisoned=true; throw;}
  s.generation=next; s.durable_root=s.root; s.active=false;
}
void Store::rollback() {auto& s=*impl_; s.transaction(); if(s.journal)s.journal->rollback();s.root=s.durable_root; s.active=false;}
Store::Savepoint Store::savepoint() const {
  auto& s=*impl_; s.transaction(); Savepoint point;
  point.root_=s.journal?s.journal->head():s.root; point.epoch_=s.epoch; point.owner_=s.owner; return point;
}
void Store::restore(const Savepoint& point) {
  auto& s=*impl_; s.transaction();
  if(point.owner_!=s.owner || point.epoch_!=s.epoch) throw std::invalid_argument("savepoint belongs to another transaction");
  if(s.journal){s.journal->restore(point.root_);return;}
  s.root=point.root_;
}
void Store::scan(const std::function<void(std::int64_t,std::string_view)>& visitor) const {
  scan_in(0,visitor);
}
void Store::scan_in(std::uint64_t space,const std::function<void(std::int64_t,std::string_view)>& visitor) const {
  auto& s=*impl_; s.usable();if(s.journal){const auto rows=s.journal->snapshot(space);for(auto [key,at]:rows){auto value=s.journal->value_at(space,key,at);visitor(key,value);}return;} auto id=s.locate_namespace(s.root,space);
  Impl::ScanCache memo(s.scan_budget);
  s.walk(id,8,0,space,visitor,memo);
}
void Store::scan_equal_in(std::uint64_t left_space,std::uint64_t right_space,
                          const std::function<void(std::int64_t,std::string_view,std::string_view)>& visitor) const {
  auto& s=*impl_; s.usable();
  if(s.journal){const auto left=s.journal->snapshot(left_space),right=s.journal->snapshot(right_space);if(left.size()!=right.size())throw KeySetMismatch();for(std::size_t i=0;i<left.size();++i){if(left[i].first!=right[i].first)throw KeySetMismatch();auto a=s.journal->value_at(left_space,left[i].first,left[i].second),b=s.journal->value_at(right_space,right[i].first,right[i].second);visitor(left[i].first,a,b);}return;}
  const auto root=s.root;
  const auto locate=[&](std::uint64_t space) {
    return s.locate_namespace(root,space);
  };
  const auto left=locate(left_space),right=locate(right_space);
  Impl::ScanCache memo(s.scan_budget);
  s.walk_equal(left,right,8,0,left_space,right_space,visitor,memo);
}
}
