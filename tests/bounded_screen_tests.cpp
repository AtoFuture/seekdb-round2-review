#define main original_validated_test_main
#include "validated_vector_tests.cpp"
#undef main
#include "../src/vector/batch_simd.hpp"
#include <array>

namespace {
std::uint64_t rejected=0,screened_count=0,lower_checks=0;
detail::BlockVectorSource source_for(const std::vector<Item>& data,const std::vector<float>& q) {
  return [&data,&q](const detail::BlockVectorVisitor& visit) {
    std::vector<float> block(16*q.size(),0);
    std::array<std::int64_t,16> keys{};
    for(std::size_t base=0;base<data.size();base+=16) {
      const auto n=std::min(std::size_t(16),data.size()-base);
      std::fill(block.begin(),block.end(),0);
      for(std::size_t lane=0;lane<n;++lane) {
        keys[lane]=data[base+lane].key;
        for(std::size_t d=0;d<q.size();++d)block[d*16+lane]=data[base+lane].value[d];
      }
      visit(std::span<const std::int64_t>(keys.data(),n),block,
            static_cast<std::uint16_t>((1u<<n)-1));
    }
  };
}
void dataset(bool equal,std::size_t k,unsigned seed,bool wide) {
  std::mt19937 random(seed);std::vector<float> q(128,0);
  std::vector<Item> data;
  for(int i=0;i<1027;++i) {
    std::vector<float> v(128);
    for(auto& x:v)x=equal?1.f:float(int(random()%100003)-50000)/16384.f;
    data.push_back({1026-i,std::move(v)});
  }
  const auto blocks=source_for(data,q);
  detail::BoundScreenStats stats;
  const auto expected=oracle(data,q,k,{});
  same(detail::exact_top_k_transposed_validated_candidates(q,k,blocks,wide,true,&stats),expected);
  rejected+=stats.rejected_lanes;screened_count+=stats.screened_lanes;
  if(stats.environment_supported && k==10) {
    CHECK(stats.screened_lanes>0);
    if(equal){CHECK(stats.rejected_lanes==0);CHECK(stats.adaptive_disabled);}
    else CHECK(stats.rejected_lanes>500);
  }
}
void direct_lower_bounds() {
#if defined(__x86_64__) && defined(__GNUC__)
  if(!detail::has_avx2()||!__builtin_cpu_supports("fma"))return;
  std::mt19937 random(0xb017d5u);
  for(std::size_t n:{1u,2u,31u,32u,127u,128u,129u,1024u}) {
    for(unsigned mode=0;mode<6;++mode)for(unsigned trial=0;trial<24;++trial) {
      auto next=[&]() {
        if(mode==0)return arbitrary_finite(random);
        if(mode==1)return std::bit_cast<float>((static_cast<std::uint32_t>(random())&0x807fffffu)|0x3f000000u);
        if(mode==2)return std::bit_cast<float>(static_cast<std::uint32_t>(random())&0x807fffffu);
        if(mode==3)return std::bit_cast<float>((static_cast<std::uint32_t>(random())&0x807fffffu)|0x7f000000u);
        if(mode==4)return float(int(random()%4097)-2048)/128.f;
        return (random()&1)?0.f:-0.f;
      };
      std::vector<float> q(n),block(n*16);
      for(auto& x:q)x=next();for(auto& x:block)x=next();
      std::array<float,16> lower{};
      for(bool wide:{false,true}) {
      if(wide&&!detail::has_avx512())continue;
      if(wide)detail::batch16_lower_bound_avx512(block.data(),q.data(),n,lower.data());
      else detail::batch16_estimate_avx2(block.data(),q.data(),n,lower.data());
      for(unsigned lane=0;lane<16;++lane) {
        double exact=0;
        for(std::size_t d=0;d<n;++d){const double delta=double(q[d])-double(block[d*16+lane]);exact+=delta*delta;}
        CHECK(!std::isnan(lower[lane]));CHECK(lower[lane]>=0);
        // A nonfinite RN32 estimate must be retained for exact evaluation.
        if(std::isfinite(lower[lane])) {
          const double factor=1+std::ldexp(double(n+8),wide?-49:-20);
          const double allowance=wide?0:double(n+8)*std::numeric_limits<float>::min();
          const double upper=std::nextafter(std::nextafter(exact*factor,INFINITY)+allowance,INFINITY);
          CHECK(double(lower[lane])<=upper);
        }
        ++lower_checks;
      }
      }
    }
  }
#endif
}
void environment_fallback() {
#if defined(__x86_64__) && defined(__GNUC__)
  const auto saved=_mm_getcsr();
  struct Restore {unsigned saved;~Restore(){_mm_setcsr(saved);}} restore{saved};
  std::vector<float> q(128,std::numeric_limits<float>::denorm_min());
  std::vector<Item> data;
  for(int i=0;i<160;++i)data.push_back({i,std::vector<float>(128,float(i)/17)});
  const auto blocks=source_for(data,q);
  for(unsigned flag:{0x40u,0x8000u,0x2000u,0x4000u,0x6000u}) {
    _mm_setcsr((saved&~0xe040u)|flag);
    detail::BoundScreenStats stats;
    const auto baseline=detail::exact_top_k_transposed_validated_candidates(q,10,blocks,true,false);
    same(detail::exact_top_k_transposed_validated_candidates(q,10,blocks,true,true,&stats),baseline);
    CHECK(!stats.environment_supported);CHECK(stats.screened_lanes==0);
  }
#endif
}
}
int main() {
  try {
    direct_lower_bounds();
    for(bool wide:{false,true})for(auto k:{0u,1u,10u,128u,1027u,2048u})for(bool equal:{false,true})dataset(equal,k,0x5ba13u+k,wide);
    // Adjacent sqrt outputs/ties, tails, mixed masks and all exponent classes
    // also run the independent full-sort comparison in the shared fixture.
    extremes_and_rounding();
    environment_fallback();
    std::cout<<"{\"avx2_supported\":"<<(detail::has_avx2()?"true":"false")<<",\"avx512_supported\":"<<(detail::has_avx512()?"true":"false")
      <<",\"lower_bound_lane_checks\":"<<lower_checks<<",\"screened_lanes\":"<<screened_count
      <<",\"rejected_lanes\":"<<rejected<<"}\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
