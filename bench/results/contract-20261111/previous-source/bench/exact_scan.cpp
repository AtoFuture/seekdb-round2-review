#include "minivectordb/exact.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
using namespace minivectordb;
using Vector=std::array<float,128>;
using Results=std::vector<std::vector<Neighbor>>;
bool better(const Neighbor& a,const Neighbor& b) {
  return a.distance<b.distance || (a.distance==b.distance && a.key<b.key);
}
int main() {
  try {
    constexpr std::size_t count=10000, queries=1000;
    std::mt19937 rng(20261109);
    std::uniform_real_distribution<float> uniform(-1.0f,1.0f);
    std::vector<Vector> rows(count), query(queries);
    for(auto& v:rows) for(auto& x:v) x=uniform(rng);
    for(auto& v:query) for(auto& x:v) x=uniform(rng);
    auto full_sort=[&]() {
      Results results; results.reserve(queries);
      for(std::size_t q=0;q<queries;++q) {
        std::vector<Neighbor> candidates;
        for(std::size_t i=0;i<count;++i) {
          if(q%2 && i%10!=q%10) continue;
          double sum=0;
          for(std::size_t j=0;j<128;++j) {
            double diff=static_cast<double>(rows[i][j])-static_cast<double>(query[q][j]);
            sum+=diff*diff;
          }
          candidates.push_back({static_cast<std::int64_t>(i),sum});
        }
        std::sort(candidates.begin(),candidates.end(),better);
        candidates.resize(std::min<std::size_t>(10,candidates.size()));
        for(auto& candidate:candidates) candidate.distance=std::sqrt(candidate.distance);
        results.push_back(std::move(candidates));
      }
      return results;
    };
    auto heap=[&]() {
      Results results; results.reserve(queries);
      const VectorSource source=[&](const VectorVisitor& visitor) {
        for(std::size_t i=0;i<count;++i) visitor(static_cast<std::int64_t>(i),rows[i]);
      };
      for(std::size_t q=0;q<queries;++q) {
        KeyPredicate filter;
        if(q%2) filter=[q](std::int64_t id) {return static_cast<std::size_t>(id)%10==q%10;};
        results.push_back(exact_top_k(query[q],10,source,filter));
      }
      return results;
    };
    auto measure=[](auto&& run,Results& output) {
      const auto start=std::chrono::steady_clock::now();
      output=run();
      return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    };
    std::array<double,3> full_ms{},heap_ms{};
    std::cout<<"{\"scope\":\"in-memory distance-selection microbenchmark, NOT SQL/storage or competition score\","
             <<"\"records\":10000,\"dimensions\":128,\"queries\":1000,\"filtered_queries\":500,"
             <<"\"filter_selectivity\":0.1,\"k\":10,\"rounds\":[";
    for(int round=0;round<3;++round) {
      Results truth,result;
      if(round%2==0) {full_ms[round]=measure(full_sort,truth);heap_ms[round]=measure(heap,result);}
      else {heap_ms[round]=measure(heap,result);full_ms[round]=measure(full_sort,truth);}
      if(result.size()!=truth.size()) throw std::runtime_error("query count mismatch");
      for(std::size_t q=0;q<queries;++q) {
        if(result[q].size()!=truth[q].size()) throw std::runtime_error("result count mismatch");
        for(std::size_t k=0;k<result[q].size();++k)
          if(result[q][k].key!=truth[q][k].key || std::abs(result[q][k].distance-truth[q][k].distance)>1e-12)
            throw std::runtime_error("independent exact truth mismatch");
      }
      if(round) std::cout<<",";
      std::cout<<"{\"full_sort_ms\":"<<full_ms[round]<<",\"streaming_heap_ms\":"<<heap_ms[round]<<",\"exact_matches\":1000}";
    }
    std::sort(full_ms.begin(),full_ms.end());std::sort(heap_ms.begin(),heap_ms.end());
    std::cout<<"],\"median_full_sort_ms\":"<<full_ms[1]<<",\"median_streaming_heap_ms\":"<<heap_ms[1]
             <<",\"median_ratio\":"<<full_ms[1]/heap_ms[1]<<"}\n";
  } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
