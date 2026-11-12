#include "minivectordb/exact.hpp"
#include "validated.hpp"
#include "batch_simd.hpp"
#include <algorithm>
#include "minivectordb/profile.hpp"
#include <cmath>
#include <queue>
#include <array>
#include <new>
#include <stdexcept>
#include <bit>
namespace minivectordb {
namespace {
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
bool has_avx2() noexcept {
#if defined(__x86_64__) && defined(__GNUC__)
  return __builtin_cpu_supports("avx2");
#else
  return false;
#endif
}
bool has_avx512() noexcept {
#if defined(__x86_64__) && defined(__GNUC__)
  return __builtin_cpu_supports("avx512f") && has_avx2();
#else
  return false;
#endif
}
std::vector<Neighbor> exact_top_k_transposed_validated_candidates(
    std::span<const float> query,std::size_t k,const BlockVectorSource& source,bool allow_wide) {
  for(float component:query)if(!std::isfinite(component))throw std::invalid_argument("non-finite query component");
  if(k==0)return {};
  auto better=[](const Neighbor& a,const Neighbor& b){return a.distance<b.distance || (a.distance==b.distance&&a.key<b.key);};
  std::priority_queue<Neighbor,std::vector<Neighbor>,decltype(better)> heap(better);
  [[maybe_unused]] const bool wide=allow_wide && has_avx512();
  source([&](std::span<const std::int64_t> keys,std::span<const float> values,std::uint16_t mask) {
    if(keys.size()>16 || values.size()/16!=query.size() || values.size()%16 ||
       (std::uint32_t(mask)>>keys.size()))throw std::invalid_argument("invalid transposed VECTOR block");
    if(!mask)return;
    std::array<double,16> sums{};
    {
      profile::detail::Scope timer(&profile::Snapshot::vector_distance);
      const auto count=std::popcount(mask);
      for(int i=1;i<count;++i)profile::detail::count(&profile::Snapshot::vector_distance);
#if defined(__x86_64__) && defined(__GNUC__)
      if(count>=4 && has_avx2()) {
        if(wide)batch16_transposed_avx512(values.data(),query.data(),query.size(),sums.data());
        else batch16_transposed_avx2(values.data(),query.data(),query.size(),sums.data());
      }
      else
#endif
      for(std::size_t lane=0;lane<keys.size();++lane)if(mask&(1U<<lane))
        for(std::size_t d=0;d<query.size();++d) {
          const double delta=static_cast<double>(query[d])-static_cast<double>(values[d*16+lane]);
          sums[lane]+=delta*delta;
        }
    }
    for(std::size_t lane=0;lane<keys.size();++lane)if(mask&(1U<<lane)) {
      Neighbor candidate{keys[lane],std::sqrt(sums[lane])};
      if(heap.size()<k)heap.push(candidate);
      else if(better(candidate,heap.top())){heap.pop();heap.push(candidate);}
    }
  });
  std::vector<Neighbor> result;result.reserve(heap.size());
  while(!heap.empty()){result.push_back(heap.top());heap.pop();}
  std::reverse(result.begin(),result.end());return result;
}
std::vector<Neighbor> exact_top_k_validated_candidates(std::span<const float> query,std::size_t k,
                                                     const VectorSource& source,const KeyPredicate& predicate) {
  return top_k<false>(query,k,source,predicate);
}
// Interleave independent candidates while preserving each candidate's dimension
// accumulation order. Own each callback span: streaming sources may reuse it.
// No reduced precision, reassociation, or approximate candidate rejection.
template<bool Simd,bool Wide=false>
static std::vector<Neighbor> batched_top_k_impl(
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
      if(count==width) {
#if defined(__x86_64__) && defined(__GNUC__)
        if constexpr(Simd) {
          if constexpr(Wide)batch16_transposed_avx512(batch.data(),query.data(),query.size(),sums.data());
          else batch16_transposed_avx2(batch.data(),query.data(),query.size(),sums.data());
        } else
#endif
        {
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
        }
      } else {
        for(std::size_t lane=0;lane<count;++lane)
          for(std::size_t d=0;d<query.size();++d) {
            const auto offset=Simd?d*width+lane:lane*query.size()+d;
            const double delta=static_cast<double>(query[d])-static_cast<double>(batch[offset]);
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
    if constexpr(Simd) {
      for(std::size_t d=0;d<query.size();++d)batch[d*width+count]=value[d];
    } else std::copy(value.begin(),value.end(),batch.begin()+count*query.size());
    if(++count==width)flush();
  });
  if(count)flush();
  std::vector<Neighbor> result;result.reserve(heap.size());
  while(!heap.empty()){result.push_back(heap.top());heap.pop();}
  std::reverse(result.begin(),result.end());return result;
}
std::vector<Neighbor> exact_top_k_batched_validated_candidates(
    std::span<const float> query,std::size_t k,const VectorSource& source,const KeyPredicate& predicate) {
  return batched_top_k_impl<false>(query,k,source,predicate);
}
std::vector<Neighbor> exact_top_k_simd_validated_candidates(
    std::span<const float> query,std::size_t k,const VectorSource& source,const KeyPredicate& predicate) {
#if defined(__x86_64__) && defined(__GNUC__)
  if(has_avx2())return batched_top_k_impl<true>(query,k,source,predicate);
#endif
  return batched_top_k_impl<false>(query,k,source,predicate);
}
std::vector<Neighbor> exact_top_k_wide_validated_candidates(
    std::span<const float> query,std::size_t k,const VectorSource& source,const KeyPredicate& predicate) {
#if defined(__x86_64__) && defined(__GNUC__)
  if(has_avx512())return batched_top_k_impl<true,true>(query,k,source,predicate);
#endif
  return exact_top_k_simd_validated_candidates(query,k,source,predicate);
}
}
}
