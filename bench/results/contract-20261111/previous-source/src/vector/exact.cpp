#include "minivectordb/exact.hpp"
#include "validated.hpp"
#include <algorithm>
#include "minivectordb/profile.hpp"
#include <cmath>
#include <queue>
#include <stdexcept>
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
std::vector<Neighbor> exact_top_k_validated_candidates(std::span<const float> query,std::size_t k,
                                                     const VectorSource& source,const KeyPredicate& predicate) {
  return top_k<false>(query,k,source,predicate);
}
}
}
