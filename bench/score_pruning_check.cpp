#include "minivectordb/exact.hpp"
#include "../src/vector/validated.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>

using namespace minivectordb;
void same(const std::vector<Neighbor>& a,const std::vector<Neighbor>& b) {
  if(a.size()!=b.size())throw std::runtime_error("size");
  for(std::size_t i=0;i<a.size();++i)
    if(a[i].key!=b[i].key || std::bit_cast<std::uint64_t>(a[i].distance)!=std::bit_cast<std::uint64_t>(b[i].distance))
      throw std::runtime_error("rank or bitwise distance");
}
int main() {
  std::mt19937 rng(20261111);
  std::size_t checks=0;
  for(auto dim:{0,1,2,31,32,33,64,127,128,129,512}) {
    std::vector<float> query(dim);
    std::vector<std::vector<float>> rows(200,std::vector<float>(dim));
    for(int style=0;style<5;++style) {
      auto value=[&]() {
        if(style==0)return static_cast<float>(static_cast<int>(rng()%4097)-2048)/128;
        if(style==1)return static_cast<float>(static_cast<int>(rng()%5)-2);
        if(style==2)return std::bit_cast<float>(std::uint32_t((rng()&0x807fffffU)|((rng()%254+1)<<23)));
        if(style==3)return std::bit_cast<float>(std::uint32_t(rng()&0x807fffffU));
        return 0.0f;
      };
      for(auto& v:query)v=value();
      for(auto& row:rows)for(auto& v:row)v=value();
      std::vector<std::int64_t> keys(200);std::iota(keys.begin(),keys.end(),-100);
      std::shuffle(keys.begin(),keys.end(),rng);
      VectorSource source=[&](const VectorVisitor& visit){for(std::size_t i=0;i<rows.size();++i)visit(keys[i],rows[i]);};
      for(auto k:{0,1,2,10,100,200,250})for(bool filter:{false,true}) {
        KeyPredicate predicate=filter?KeyPredicate([](auto key){return key%7==0;}):KeyPredicate{};
        same(exact_top_k(query,k,source,predicate),detail::exact_top_k_validated_candidates(query,k,source,predicate));++checks;
      }
    }
  }
  // Equal rounded Euclidean distances must still use signed-key ordering.
  const std::vector<float> query{0,0};
  const std::vector<std::vector<float>> rows{{1,0},{1,std::ldexp(1.0f,-27)},{1,std::ldexp(1.0f,-26)}};
  VectorSource ties=[&](const VectorVisitor& visit){visit(2,rows[0]);visit(-2,rows[1]);visit(-3,rows[2]);};
  same(exact_top_k(query,1,ties),detail::exact_top_k_validated_candidates(query,1,ties));++checks;
  std::vector<float> q(128);
  std::vector<std::vector<float>> data(10000,std::vector<float>(128));
  for(auto& row:data)for(auto& v:row)v=static_cast<float>(static_cast<int>(rng()%4097)-2048)/128;
  std::vector<std::vector<float>> queries(200,std::vector<float>(128));
  for(auto& row:queries)for(auto& v:row)v=static_cast<float>(static_cast<int>(rng()%4097)-2048)/128;
  for(std::size_t j=0;j<queries.size();++j) {
    VectorSource s=[&](const VectorVisitor& visit){for(std::size_t i=0;i<data.size();++i)if(j%2==0 || i%10==j%10)visit(static_cast<std::int64_t>(i),data[i]);};
    same(exact_top_k(queries[j],10,s),detail::exact_top_k_validated_candidates(queries[j],10,s));++checks;
  }
  std::cout<<"{\"bitwise_differential_cases\":"<<checks<<",\"passed\":true}\n";
}
