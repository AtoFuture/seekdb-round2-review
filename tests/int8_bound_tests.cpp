#define main original_validated_test_main
#include "validated_vector_tests.cpp"
#undef main
#include "../src/vector/int8_bound.hpp"
#include <cfenv>
namespace {
std::uint64_t compared=0,rejections=0,native_masks=0;
float component(std::mt19937& gen,unsigned mode) {
  if(mode==0)return float(int(gen()%131071)-65535)/4096.f;
  if(mode==1)return arbitrary_finite(gen);
  if(mode==2)return std::bit_cast<float>((std::uint32_t(gen())&0x807fffffu)|0x7f000000u);
  if(mode==3)return std::bit_cast<float>(std::uint32_t(gen())&0x807fffffu);
  if(mode==4)return gen()%2?1.f:std::nextafter(1.f,2.f);
  return gen()%2?0.f:-0.f;
}
void run_case(std::size_t dimension,unsigned mode) {
  const std::size_t rows=mode==0?1051:139;
  std::mt19937 gen(unsigned(20261112+dimension+mode));
  std::vector<Item> data;
  for(std::size_t i=0;i<rows;++i) {
    std::vector<float> values(dimension);
    for(auto& v:values)v=component(gen,mode);
    data.push_back({std::int64_t(rows-i)-20,std::move(values)});
  }
  const auto full=rows/16*16;
  std::vector<float> storage(full*dimension);
  for(std::size_t r=0;r<full;r+=16)for(std::size_t d=0;d<dimension;++d)for(std::size_t lane=0;lane<16;++lane)
    storage[r*dimension+d*16+lane]=data[r+lane].value[d];
  detail::Int8BoundCache cache;
  CHECK(!cache.build(storage,rows,dimension,1,true));
  CHECK(cache.build(storage,rows,dimension,16*1024*1024,true));
  CHECK(cache.bytes()<=16*1024*1024);
  for(unsigned trial=0;trial<4;++trial) {
    std::vector<float> q(dimension);
    if(trial==0)q=data[0].value;
    else for(auto& v:q)v=component(gen,mode);
    auto encoded=cache.query(q);CHECK(encoded.valid);
    auto predicate=[&](std::int64_t key){return trial%2==0 || key%3!=0;};
    detail::BlockVectorSource blocks=[&](const detail::BlockVectorVisitor& visit) {
      std::array<std::int64_t,16> keys{};std::vector<float> tail(dimension*16,0);
      for(std::size_t r=0;r<rows;r+=16) {
        const auto n=std::min(std::size_t(16),rows-r);std::uint16_t mask=0;
        for(std::size_t lane=0;lane<n;++lane){keys[lane]=data[r+lane].key;if(predicate(keys[lane]))mask|=std::uint16_t(1U<<lane);}
        if(n==16)visit({keys.data(),n},{storage.data()+r*dimension,dimension*16},mask);
        else {for(std::size_t lane=0;lane<n;++lane)for(std::size_t d=0;d<dimension;++d)tail[d*16+lane]=data[r+lane].value[d];visit({keys.data(),n},tail,mask);}
      }
    };
    for(std::size_t k:{std::size_t(0),std::size_t(1),std::size_t(10),rows+2}) {
      detail::BoundScreenStats stats;
      const auto expected=oracle(data,q,k,predicate);
      const auto unfiltered=detail::exact_top_k_transposed_validated_candidates(q,k,blocks,true,false);
      const auto filtered=detail::exact_top_k_transposed_validated_candidates(q,k,blocks,true,true,&stats,&cache);
      try {same(unfiltered,expected);same(filtered,expected);}
      catch(...) {
        std::cerr<<"dimension="<<dimension<<" mode="<<mode<<" trial="<<trial<<" k="<<k<<" integer_rejected="<<stats.integer_rejected<<'\n';
        for(std::size_t i=0;i<expected.size();++i)if(std::bit_cast<std::uint64_t>(filtered[i].distance)!=std::bit_cast<std::uint64_t>(expected[i].distance)) {
          std::cerr<<"rank="<<i<<" key="<<filtered[i].key<<" expected_key="<<expected[i].key<<" filtered="<<std::hexfloat<<filtered[i].distance<<" no_filter="<<unfiltered[i].distance<<" expected="<<expected[i].distance<<'\n';break;
        }
        throw;
      }
      compared+=expected.size();rejections+=stats.integer_rejected;
    }
    // Every candidate at its exact radius, and one ULP above, must survive.
    // Independently compare SIMD and scalar integer dot paths where available.
    for(std::size_t r=0;r<full;r+=16) {
      const auto values=std::span<const float>(storage.data()+r*dimension,dimension*16);
      for(std::size_t lane=0;lane<16;++lane) {
        double sum=0;for(std::size_t d=0;d<dimension;++d){double delta=double(q[d])-double(data[r+lane].value[d]);sum+=delta*delta;}
        const double distance=std::sqrt(sum);
        for(double radius:{distance,std::nextafter(distance,INFINITY)}) {
          const auto portable=cache.survivors(encoded,radius,values,65535,false);
          CHECK(portable&(1U<<lane));
          if(detail::Int8BoundCache::available()){
            CHECK(cache.survivors(encoded,radius,values,65535,true)==portable);
            CHECK(cache.survivors(encoded,radius,values,65535,true,true)==portable);
            ++native_masks;
          }
          ++compared;
        }
      }
    }
    // Unowned and misaligned buffers cannot use the source's rejection proof.
    std::vector<float> copy(storage.begin(),storage.begin()+16*dimension);
    CHECK(cache.survivors(encoded,0,copy,65535)==65535);
    CHECK(cache.survivors(encoded,0,{storage.data()+1,16*dimension},65535)==65535);
    const auto round=std::fegetround();CHECK(std::fesetround(FE_DOWNWARD)==0);
    CHECK(!cache.query(q).valid);CHECK(std::fesetround(round)==0);
  }
  cache.clear();CHECK(!cache.ready()&&cache.bytes()==0);
}
}
int main(){try{
  for(std::size_t dimension:{32u,127u,128u,129u,1024u,4096u})
    for(unsigned mode=0;mode<6;++mode)run_case(dimension,mode);
  CHECK(rejections>1000);
  std::cout<<"{\"native_vnni\":"<<(detail::Int8BoundCache::native_vnni()?"true":"false")
           <<",\"native_avx2\":"<<(detail::Int8BoundCache::available()?"true":"false")
           <<",\"compared\":"<<compared<<",\"rejected\":"<<rejections<<",\"native_mask_checks\":"<<native_masks<<"}\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
