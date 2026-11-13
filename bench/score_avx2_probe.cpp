#include "minivectordb/exact.hpp"
#include "../src/vector/validated.hpp"
#include <algorithm>
#include "minivectordb/profile.hpp"
#include <cmath>
#include <queue>
#include <array>
#include <new>
#include <stdexcept>
#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#endif
namespace minivectordb {
namespace {
#if defined(__x86_64__) && defined(__GNUC__)
// SIMD is across independent candidates. Each candidate still accumulates
// dimensions in order; no FMA or horizontal reassociation is permitted.
__attribute__((target("avx2,no-fma")))
void batch_distances_avx2(const float* batch,const float* query,std::size_t n,double* out) {
  const auto stride=static_cast<int>(n);
  const auto offsets=_mm256_setr_epi32(0,stride,2*stride,3*stride,4*stride,5*stride,6*stride,7*stride);
  auto s0=_mm256_setzero_pd(),s1=s0,s2=s0,s3=s0;
  for(std::size_t d=0;d<n;++d) {
    const auto q=_mm256_set1_pd(static_cast<double>(query[d]));
    const auto lo=_mm256_i32gather_ps(batch+d,offsets,4);
    const auto hi=_mm256_i32gather_ps(batch+8*n+d,offsets,4);
    const auto d0=_mm256_sub_pd(q,_mm256_cvtps_pd(_mm256_castps256_ps128(lo)));
    const auto d1=_mm256_sub_pd(q,_mm256_cvtps_pd(_mm256_extractf128_ps(lo,1)));
    const auto d2=_mm256_sub_pd(q,_mm256_cvtps_pd(_mm256_castps256_ps128(hi)));
    const auto d3=_mm256_sub_pd(q,_mm256_cvtps_pd(_mm256_extractf128_ps(hi,1)));
    s0=_mm256_add_pd(s0,_mm256_mul_pd(d0,d0));
    s1=_mm256_add_pd(s1,_mm256_mul_pd(d1,d1));
    s2=_mm256_add_pd(s2,_mm256_mul_pd(d2,d2));
    s3=_mm256_add_pd(s3,_mm256_mul_pd(d3,d3));
  }
  _mm256_storeu_pd(out,s0);_mm256_storeu_pd(out+4,s1);
  _mm256_storeu_pd(out+8,s2);_mm256_storeu_pd(out+12,s3);
}
#endif

template<bool CheckComponents>
double squared_l2(std::span<const float> a,std::span<const float> b) {
  profile::detail::Scope timer(&profile::Snapshot::vector_distance);
  if(a.size()!=b.size()) throw std::invalid_argument("VECTOR dimension mismatch");
  double squared=0;
  for(std::size_t i=0;i<a.size();++i) {
    if constexpr(CheckComponents)
      if(!std::isfinite(a[i]) || !std::isfinite(b[i])) throw std::invalid_argument("non-finite VECTOR component");
    const double difference=static_cast<double>(a[i])-static_cast<double>(b[i]);
    squared+=difference*difference;
  }
  return squared;
}
struct RankedNeighbor { std::int64_t key; double distance; };
template<bool CheckComponents>
std::vector<Neighbor> top_k(std::span<const float> query,std::size_t k,const VectorSource& source,const KeyPredicate& predicate) {
  for(float component:query) if(!std::isfinite(component)) throw std::invalid_argument("non-finite query component");
  // Rank the same rounded Euclidean value exposed to callers, then primary key.
  auto better=[](const RankedNeighbor& a,const RankedNeighbor& b) {
    return a.distance<b.distance || (a.distance==b.distance && a.key<b.key);
  };
  std::priority_queue<RankedNeighbor,std::vector<RankedNeighbor>,decltype(better)> heap(better);
  if(k==0) return {};
  source([&](std::int64_t key,std::span<const float> value) {
    if(predicate && !predicate(key)) return;
    RankedNeighbor candidate{key,std::sqrt(squared_l2<CheckComponents>(query,value))};
    if(heap.size()<k) heap.push(candidate);
    else if(better(candidate,heap.top())) {heap.pop(); heap.push(candidate);}
  });
  std::vector<Neighbor> result; result.reserve(heap.size());
  while(!heap.empty()) {
    const auto candidate=heap.top(); heap.pop();
    result.push_back({candidate.key,candidate.distance});
  }
  std::reverse(result.begin(),result.end());
  return result;
}
}
double l2_distance(std::span<const float> a,std::span<const float> b) {
  return std::sqrt(squared_l2<true>(a,b));
}
std::vector<Neighbor> exact_top_k(std::span<const float> query,std::size_t k,const VectorSource& source,const KeyPredicate& predicate) {
  return top_k<true>(query,k,source,predicate);
}
namespace detail {
std::vector<Neighbor> exact_top_k_validated_candidates(std::span<const float> query,std::size_t k,
                                                     const VectorSource& source,const KeyPredicate& predicate) {
  return top_k<false>(query,k,source,predicate);
}
// Interleave independent candidates while preserving each candidate's dimension
// accumulation order. Own each callback span: streaming sources may reuse it.
// No reduced precision, reassociation, or approximate candidate rejection.
std::vector<Neighbor> exact_top_k_batched_validated_candidates(
    std::span<const float> query,std::size_t k,const VectorSource& source,const KeyPredicate& predicate) {
  for(float component:query)if(!std::isfinite(component))throw std::invalid_argument("non-finite query component");
  if(k==0)return {};
  constexpr std::size_t width=16;
  constexpr std::size_t scratch_limit=64*1024;
  if(query.size()<16 || query.size()>scratch_limit/(width*sizeof(float)))
    return top_k<false>(query,k,source,predicate);
  auto better=[](const Neighbor& a,const Neighbor& b){return a.distance<b.distance || (a.distance==b.distance&&a.key<b.key);};
  std::priority_queue<Neighbor,std::vector<Neighbor>,decltype(better)> heap(better);
  std::array<std::int64_t,width> keys{};
  std::vector<float> batch;
  try {batch.resize(width*query.size());}
  catch(const std::bad_alloc&) {return top_k<false>(query,k,source,predicate);}
  std::size_t count=0;
  auto flush=[&]() {
    std::array<double,width> sums{};
    {
      profile::detail::Scope timer(&profile::Snapshot::vector_distance);
      for(std::size_t lane=1;lane<count;++lane)profile::detail::count(&profile::Snapshot::vector_distance);
      #if defined(__x86_64__) && defined(__GNUC__)
      if(count==width && __builtin_cpu_supports("avx2")) {
        batch_distances_avx2(batch.data(),query.data(),query.size(),sums.data());
      } else
#endif
      if(count==width) {
        const auto n=query.size();
        for(std::size_t d=0;d<n;++d) {
          const double q=query[d];
          const double d0=q-static_cast<double>(batch[0*n+d]);
          const double d1=q-static_cast<double>(batch[1*n+d]);
          const double d2=q-static_cast<double>(batch[2*n+d]);
          const double d3=q-static_cast<double>(batch[3*n+d]);
          const double d4=q-static_cast<double>(batch[4*n+d]);
          const double d5=q-static_cast<double>(batch[5*n+d]);
          const double d6=q-static_cast<double>(batch[6*n+d]);
          const double d7=q-static_cast<double>(batch[7*n+d]);
          const double d8=q-static_cast<double>(batch[8*n+d]);
          const double d9=q-static_cast<double>(batch[9*n+d]);
          const double d10=q-static_cast<double>(batch[10*n+d]);
          const double d11=q-static_cast<double>(batch[11*n+d]);
          const double d12=q-static_cast<double>(batch[12*n+d]);
          const double d13=q-static_cast<double>(batch[13*n+d]);
          const double d14=q-static_cast<double>(batch[14*n+d]);
          const double d15=q-static_cast<double>(batch[15*n+d]);
          sums[0]+=d0*d0;
          sums[1]+=d1*d1;
          sums[2]+=d2*d2;
          sums[3]+=d3*d3;
          sums[4]+=d4*d4;
          sums[5]+=d5*d5;
          sums[6]+=d6*d6;
          sums[7]+=d7*d7;
          sums[8]+=d8*d8;
          sums[9]+=d9*d9;
          sums[10]+=d10*d10;
          sums[11]+=d11*d11;
          sums[12]+=d12*d12;
          sums[13]+=d13*d13;
          sums[14]+=d14*d14;
          sums[15]+=d15*d15;
        }
      } else {
        for(std::size_t lane=0;lane<count;++lane)
          for(std::size_t d=0;d<query.size();++d) {
            const double delta=static_cast<double>(query[d])-static_cast<double>(batch[lane*query.size()+d]);
            sums[lane]+=delta*delta;
          }
      }
    }
    for(std::size_t lane=0;lane<count;++lane) {
      Neighbor candidate{keys[lane],std::sqrt(sums[lane])};
      if(heap.size()<k)heap.push(candidate);
      else if(better(candidate,heap.top())){heap.pop();heap.push(candidate);}
    }
    count=0;
  };
  source([&](std::int64_t key,std::span<const float> value) {
    if(predicate&&!predicate(key))return;
    if(value.size()!=query.size())throw std::invalid_argument("VECTOR dimension mismatch");
    keys[count]=key;
    std::copy(value.begin(),value.end(),batch.begin()+count*query.size());
    if(++count==width)flush();
  });
  if(count)flush();
  std::vector<Neighbor> result;result.reserve(heap.size());
  while(!heap.empty()){result.push_back(heap.top());heap.pop();}
  std::reverse(result.begin(),result.end());return result;
}
}
}
