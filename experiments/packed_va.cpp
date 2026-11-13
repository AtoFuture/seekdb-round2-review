// Standalone research prototype, deliberately excluded from production builds.
#include "../src/storage/page_checksum.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cfenv>
#ifdef __FAST_MATH__
#error Compile this prototype without fast-math
#endif
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
using namespace std;
namespace fs=std::filesystem;
constexpr size_t D=128,P=4096,H=32,ROW=536,PACK=(P-H)/ROW,SUM=72,SP=(P-H)/SUM;
using Page=array<unsigned char,P>;
using Vec=array<float,D>;
using Bounds=array<array<double,17>,D>;
struct Row  {
  uint64_t id;
  uint32_t group;
  array<unsigned char,12> payload{};
  Vec v;
};
struct Hit  {
  double distance;
  uint64_t id;
  bool operator==(const Hit&)const=default;
};
bool less_hit(const Hit&a,const Hit&b) {
  return a.distance<b.distance||(a.distance==b.distance&&a.id<b.id);
}
struct Stats  {
  uint64_t pages=0,checksum_bytes=0,distances=0,pruned=0;
};
void require(bool b,const string&m) {
  if(!b)throw runtime_error(m);
}
template<class T>void put(Page&p,size_t o,T x) {
  memcpy(p.data()+o,&x,sizeof x);
}
template<class T>T get(const Page&p,size_t o) {
  T x;
  memcpy(&x,p.data()+o,sizeof x);
  return x;
}
void validate(const Vec&v) {
  for(float x:v)require(isfinite(x),"nonfinite vector");
}
double distance(const Vec&a,const Vec&b) {
  double s=0;
  for(size_t d=0; d<D; ++d) {
    double x=double(a[d])-double(b[d]);
    s+=x*x;
  }
  return s;
}
void offer(vector<Hit>&top,Hit h,size_t k) {
  if(!k)return;
  top.insert(lower_bound(top.begin(),top.end(),h,less_hit),h);
  if(top.size()>k)top.pop_back();
}
vector<Hit>truth(const vector<Row>&a,const Vec&q,int filter,size_t k) {
  vector<Hit>r;
  for(auto&x:a)if(filter<0||int(x.group)==filter)r.push_back( {
    distance(x.v,q),x.id
  });
  sort(r.begin(),r.end(),less_hit);
  if(r.size()>k)r.resize(k);
  return r;
}
void writepage(ofstream&f,Page&p,uint32_t type,uint32_t n,uint64_t seq) {
  put(p,0,uint32_t(0x56415031));
  put(p,4,type);
  put(p,8,n);
  put(p,12,uint32_t(0));
  put(p,16,seq);
  put(p,12,minivectordb::detail::page_checksum(p,12));
  f.write(reinterpret_cast<char*>(p.data()),P);
  require(bool(f),"write failure");
}
struct Reader  {
  int fd=-1;
  uint64_t count=0;
  Stats&s;
  Reader(const fs::path&p,Stats&st):s(st) {
    auto z=fs::file_size(p);
    require(z%P==0,"truncated file");
    count=z/P;
    fd=open(p.c_str(),O_RDONLY);
    require(fd>=0,"open failure");
  }
  ~Reader() {
    if(fd>=0)close(fd);
  }
  void read(uint64_t i,Page&p,uint32_t type) {
    require(i<count,"missing page");
    require(pread(fd,p.data(),P,i*P)==ssize_t(P),"short read");
    ++s.pages;
    s.checksum_bytes+=P;
    require(get<uint32_t>(p,12)==minivectordb::detail::page_checksum(p,12),"CRC mismatch");
    require(get<uint32_t>(p,0)==0x56415031&&get<uint32_t>(p,4)==type&&get<uint64_t>(p,16)==i,"page identity mismatch");
  }
};
void encode(Page&p,size_t o,const Row&r) {
  put(p,o,r.id);
  put(p,o+8,r.group);
  memcpy(p.data()+o+12,r.payload.data(),12);
  memcpy(p.data()+o+24,r.v.data(),512);
}
Row decode(const Page&p,size_t o) {
  Row r;
  r.id=get<uint64_t>(p,o);
  r.group=get<uint32_t>(p,o+8);
  memcpy(r.payload.data(),p.data()+o+12,12);
  memcpy(r.v.data(),p.data()+o+24,512);
  validate(r.v);
  return r;
}
Bounds boundaries(const vector<Row>&a) {
  Bounds b{};
  for(size_t d=0; d<D; ++d) {
    double lo=a.empty()?0:a[0].v[d],hi=lo;
    for(auto&r:a) {
      lo=min(lo,double(r.v[d]));
      hi=max(hi,double(r.v[d]));
    }
    for(size_t i=0; i<=16; ++i)b[d][i]=lo+(hi-lo)*(double(i)/16);
    b[d][0]=lo;
    b[d][16]=hi;
  }
  return b;
}
unsigned code(float x,const array<double,17>&b) {
  unsigned c=0;
  while(c<15&&double(x)>b[c+1])++c;
  require(double(x)>=b[c]&&double(x)<=b[c+1],"quantizer containment");
  return c;
}
// All finite float32 differences/squares/128-term sums fit binary64.
// Round positive operations one binary64 step toward zero. Closed interval
// endpoints are expanded outward. Build without fast-math/FMA contraction.
double down(double x) {
  return x>0?nextafter(x,0.0):0.0;
}
double lowerbound(const unsigned char*c,const Bounds&b,const Vec&q) {
  double s=0;
  for(size_t d=0; d<D; ++d) {
    unsigned z=(c[d/2]>>((d%2)*4))&15;
    double lo=nextafter(b[d][z],-numeric_limits<double>::infinity()),hi=nextafter(b[d][z+1],numeric_limits<double>::infinity());
    double delta=q[d]<lo?down(lo-double(q[d])):q[d]>hi?down(double(q[d])-hi):0;
    s=down(s+down(delta*delta));
  }
  return s;
}
struct Files  {
  fs::path one,packed,summary;
};
constexpr size_t BC=(P-H)/8,BN=D*17,BP=(BN+BC-1)/BC;
Files build(const fs::path&dir,const vector<Row>&a) {
  fs::create_directories(dir);
  for(auto&r:a)validate(r.v);
  Files files {
    dir/"one.pages",dir/"packed.pages",dir/"summary.pages"
  };
  for(bool packed: {
    false,true
  }) {
    ofstream f(packed?files.packed:files.one,ios::binary|ios::trunc);
    size_t cap=packed?PACK:1;
    Page p{};
    put(p,H,uint64_t(a.size()));
    put(p,H+8,uint32_t(cap));
    writepage(f,p,1,0,0);
    uint64_t seq=1;
    for(size_t i=0; i<a.size(); i+=cap) {
      p={};
      size_t n=min(cap,a.size()-i);
      for(size_t j=0; j<n; ++j)encode(p,H+j*ROW,a[i+j]);
      writepage(f,p,2,n,seq++);
    }
  }
  Bounds b=boundaries(a);
  ofstream f(files.summary,ios::binary|ios::trunc);
  Page p{};
  put(p,H,uint64_t(a.size()));
  put(p,H+8,uint32_t(BP));
  writepage(f,p,3,0,0);
  uint64_t seq=1;
  for(size_t i=0; i<BN; i+=BC) {
    p={};
    size_t n=min(BC,BN-i);
    for(size_t j=0; j<n; ++j)put(p,H+j*8,b[(i+j)/17][(i+j)%17]);
    writepage(f,p,4,n,seq++);
  }
  for(size_t i=0; i<a.size(); i+=SP) {
    p={};
    size_t n=min(SP,a.size()-i);
    for(size_t j=0; j<n; ++j) {
      size_t o=H+j*SUM;
      put(p,o,uint32_t(i+j));
      put(p,o+4,a[i+j].group);
      for(size_t d=0; d<D; ++d)p[o+8+d/2]|=code(a[i+j].v[d],b[d])<<((d%2)*4);
    }
    writepage(f,p,5,n,seq++);
  }
  return files;
}
vector<Hit>query(const Files&f,int mode,const Vec&q,int filter,size_t k,Stats&s) {
  validate(q);
  vector<Hit>top;
  Reader data(mode==0?f.one:f.packed,s);
  Page page{};
  data.read(0,page,1);
  uint64_t rows=get<uint64_t>(page,H);
  size_t cap=get<uint32_t>(page,H+8);
  require(cap==(mode==0?1:PACK)&&data.count==1+(rows+cap-1)/cap,"data shape");
  if(!k)return top;
  if(mode<2) {
    for(uint64_t pi=1; pi<data.count; ++pi) {
      data.read(pi,page,2);
      size_t n=get<uint32_t>(page,8);
      require(n==min<uint64_t>(cap,rows-(pi-1)*cap),"row count");
      for(size_t j=0; j<n; ++j) {
        Row r=decode(page,H+j*ROW);
        if(filter>=0&&int(r.group)!=filter)continue;
        ++s.distances;
        offer(top, {
          distance(r.v,q),r.id
        },k);
      }
    }
    return top;
  }
  Reader summary(f.summary,s);
  Page sp{};
  summary.read(0,sp,3);
  require(get<uint64_t>(sp,H)==rows,"summary row count");
  require(get<uint32_t>(sp,H+8)==BP&&summary.count==1+BP+(rows+SP-1)/SP,"summary shape");
  Bounds b{};
  for(size_t i=0; i<BP; ++i) {
    summary.read(i+1,sp,4);
    size_t n=min(BC,BN-i*BC);
    require(get<uint32_t>(sp,8)==n,"boundary count");
    for(size_t j=0; j<n; ++j)b[(i*BC+j)/17][(i*BC+j)%17]=get<double>(sp,H+j*8);
  }
  for(auto&dim:b)for(size_t j=0; j<17; ++j)require(isfinite(dim[j])&&(!j||dim[j]>=dim[j-1]),"boundary values");
  uint64_t cached=0;
  for(uint64_t pi=1+BP; pi<summary.count; ++pi) {
    summary.read(pi,sp,5);
    size_t n=min<uint64_t>(SP,rows-(pi-1-BP)*SP);
    require(get<uint32_t>(sp,8)==n,"summary count");
    for(size_t j=0; j<n; ++j) {
      size_t o=H+j*SUM;
      uint32_t ri=get<uint32_t>(sp,o);
      require(ri==(pi-1-BP)*SP+j,"summary ordinal");
      if(filter>=0&&int(get<uint32_t>(sp,o+4))!=filter)continue;
      double lb=lowerbound(sp.data()+o+8,b,q);
      if(top.size()==k&&lb>top.back().distance) {
        ++s.pruned;
        continue;
      }
      uint64_t dp=1+ri/PACK;
      if(cached!=dp) {
        data.read(dp,page,2);
        require(get<uint32_t>(page,8)==min<uint64_t>(PACK,rows-(dp-1)*PACK),"packed row count");
        cached=dp;
      }
      Row r=decode(page,H+(ri%PACK)*ROW);
      require(r.group==get<uint32_t>(sp,o+4),"summary group mismatch");
      ++s.distances;
      offer(top, {
        distance(r.v,q),r.id
      },k);
    }
  }
  return top;
}
vector<Row>generate_rows(size_t n,uint32_t seed,bool clustered) {
  mt19937 rng(seed);
  vector<Row>a(n);
  for(size_t i=0; i<n; ++i) {
    auto&r=a[i];
    r.id=i+1;
    r.group=i%8;
    for(size_t j=0; j<12; ++j)r.payload[j]=(i+j)%251;
    for(float&x:r.v) {
      float u=float(rng()%1000000)/1000000.0f;
      x=clustered?float(i%8)*8+u*0.25f:u*2-1;
    }
  }
  return a;
}
void checkall(const vector<Row>&a,const vector<Vec>&qs,const fs::path&dir) {
  auto f=build(dir,a);
  // Untimed persisted-row roundtrip: ID, group, payload, all float bits.
  for (int mode = 0; mode < 2; ++mode) {
    Stats stats;
    Reader reader(mode == 0 ? f.one : f.packed, stats);
    const size_t capacity = mode == 0 ? 1 : PACK;
    Page page{};
    for (size_t i = 0; i < a.size(); ++i) {
      if (i % capacity == 0) reader.read(1 + i / capacity, page, 2);
      const Row decoded = decode(page, H + (i % capacity) * ROW);
      require(decoded.id == a[i].id && decoded.group == a[i].group &&
              decoded.payload == a[i].payload &&
              memcmp(decoded.v.data(), a[i].v.data(), sizeof(Vec)) == 0,
              "persisted row roundtrip mismatch");
    }
  }
  for(auto&q:qs)for(int filter: {
    -1,0,7,99
  })for(size_t k: {
    size_t(0),size_t(1),size_t(10),a.size()+1
  }) {
    auto expected=truth(a,q,filter,k);
    for(int mode=0; mode<3; ++mode) {
      Stats s;
      require(query(f,mode,q,filter,k,s)==expected,"exact mismatch mode "+to_string(mode));
    }
  }
}
void selftest(const fs::path&dir) {
  auto a=generate_rows(81,123,true);
  a[1].v=a[0].v;
  a[0].id=900;
  a[1].id=2;
  Vec zero{};
  checkall(a, {
    zero,a[0].v,a.back().v
  },dir/"normal");
  checkall({},  {
    zero
  },dir/"empty");
  vector<Row>edge(40);
  for(size_t i=0; i<edge.size(); ++i) {
    edge[i].id=40-i;
    edge[i].group=i%8;
    for(size_t d=0; d<D; ++d)edge[i].v[d]=float(int(i)-20)*0.125f;
  }
  edge[0].v.fill(-numeric_limits<float>::max());
  edge[1].v.fill(numeric_limits<float>::max());
  edge[2].v.fill(numeric_limits<float>::denorm_min());
  edge[3].v.fill(-0.0f);
  checkall(edge, {
    zero,edge[0].v,edge[1].v,edge[2].v
  },dir/"edge");
  auto b=boundaries(a);
  vector<Vec>qs {
    zero,a[0].v
  };
  for(size_t c=0; c<17; ++c) {
    Vec q{};
    for(size_t d=0; d<D; ++d)q[d]=float(b[d][c]);
    qs.push_back(q);
    for(auto&x:q)x=nextafter(x,numeric_limits<float>::infinity());
    qs.push_back(q);
    for(size_t d=0; d<D; ++d)q[d]=nextafter(float(b[d][c]),-numeric_limits<float>::infinity());
    qs.push_back(q);
  }
  for(auto&r:a) {
    array<unsigned char,64>codes{};
    for(size_t d=0; d<D; ++d)codes[d/2]|=code(r.v[d],b[d])<<((d%2)*4);
    for(auto&q:qs)require(lowerbound(codes.data(),b,q)<=distance(r.v,q),"nonconservative bound");
  }
  checkall(a,qs,dir/"boundaries");
  auto expect_throw=[](auto fn) {
    bool caught=false;
    try {
      fn();
    }
    catch(const exception&) {
      caught=true;
    }
    require(caught,"expected rejection");
  };
  for(float bad: {
    numeric_limits<float>::infinity(),numeric_limits<float>::quiet_NaN()
  }) {
    auto invalid=a;
    invalid[0].v[5]=bad;
    expect_throw([&] {
      build(dir/"invalid",invalid);
    });
    auto f=build(dir/"query-invalid",a);
    Vec q{};
    q[2]=bad;
    expect_throw([&] {
      Stats s; query(f,2,q,-1,10,s);
    });
  }
  for(int mode=0; mode<3; ++mode) {
    auto f=build(dir/("corrupt"+to_string(mode)),a);
    auto target=mode==0?f.one:mode==1?f.packed:f.summary;
    {
      fstream io(target,ios::in|ios::out|ios::binary);
      io.seekg(P+100);
      char c=0;
      io.read(&c,1);
      c^=87;
      io.seekp(P+100);
      io.write(&c,1);
    }
    expect_throw([&] {
      Stats s; query(f,mode,zero,-1,10,s);
    });
    f=build(dir/("truncated"+to_string(mode)),a);
    target=mode==0?f.one:mode==1?f.packed:f.summary;
    fs::resize_file(target,fs::file_size(target)-1);
    expect_throw([&] {
      Stats s; query(f,mode,zero,-1,10,s);
    });
  }
  cout<<"{\"self_test\":\"passed\",\"cases\":\"ties,duplicates,empty,filters,k,extremes,subnormal,NaN,infinity,boundaries,CRC,truncation\"}\n";
}
int main(int argc,char**argv) {
  try {
    require(numeric_limits<double>::is_iec559&&numeric_limits<float>::is_iec559,"IEEE floats required");
    require(fegetround()==FE_TONEAREST,"round-to-nearest required" );
    size_t n=2000,nq=40,rounds=3;
    uint32_t seed=20261109;
    bool test=false;
    fs::path dir;
    for(int i=1; i<argc; ++i) {
      string arg=argv[i];
      if(arg=="--self-test")test=true;
      else  {
        require(i+1<argc,"missing argument");
        string val=argv[++i];
        if(arg=="--dir")dir=val;
        else if(arg=="--rows")n=stoull(val);
        else if(arg=="--queries")nq=stoull(val);
        else if(arg=="--rounds")rounds=stoull(val);
        else if(arg=="--seed")seed=stoul(val);
        else throw runtime_error("unknown option");
      }
    }
    require(!dir.empty(),"--dir required (dedicated experiment directory)");
    require(n>0&&n<=10000000&&nq>0&&nq<=100000&&rounds>0&&rounds<=100,"invalid workload");
    fs::create_directories(dir);
    if(test) {
      selftest(dir);
      return 0;
    }
    for(bool clustered: {
      false,true
    }) {
      auto rows=generate_rows(n,seed,clustered);
      auto start=chrono::steady_clock::now();
      auto f=build(dir/(clustered?"clustered":"uniform"),rows);
      double buildms=chrono::duration<double,milli>(chrono::steady_clock::now()-start).count();
      vector<Vec>qs;
      auto generated=generate_rows(nq,seed+1,clustered);
      for(auto&r:generated)qs.push_back(r.v);
      vector<vector<Hit>>reference;
      for(size_t qi=0; qi<nq; ++qi)reference.push_back(truth(rows,qs[qi],qi%2?int(qi%8):-1,10));
      rows.clear();
      rows.shrink_to_fit();
      generated.clear();
      generated.shrink_to_fit();
      for(size_t round=0; round<rounds; ++round)for(int order=0; order<3; ++order) {
        int mode=(order+round)%3;
        Stats total;
        start=chrono::steady_clock::now();
        bool equal=true;
        for(size_t qi=0; qi<nq; ++qi) {
          auto got=query(f,mode,qs[qi],qi%2?int(qi%8):-1,10,total);
          equal=equal&&(got==reference[qi]);
        }
        double ms=chrono::duration<double,milli>(chrono::steady_clock::now()-start).count();
        cout<<"{\"dataset\":\""<<(clustered?"clustered":"uniform")<<"\",\"mode\":\""<<(mode==0?"one_row":mode==1?"packed":"packed_va")<<"\",\"rows\":"<<n<<",\"queries\":"<<nq<<",\"seed\":"<<seed<<",\"round\":"<<round<<",\"k\":10,\"pages_read\":"<<total.pages<<",\"checksum_bytes\":"<<total.checksum_bytes<<",\"distance_evaluations\":"<<total.distances<<",\"pruned_rows\":"<<total.pruned<<",\"query_ms\":"<<ms<<",\"all_formats_build_ms\":"<<buildms<<",\"file_bytes\":"<<(mode==0?fs::file_size(f.one):fs::file_size(f.packed)+(mode==2?fs::file_size(f.summary):0))<<",\"raw_results_equal\":"<<(equal?"true":"false")<<",\"cache_scope\":\"warm_uncontrolled_OS_cache_no_application_cache\"}\n";
        require(equal,"benchmark correctness failure");
      }
    }
  }
  catch(const exception&e) {
    cerr<<e.what()<<'\n';
    return 1;
  }
}
