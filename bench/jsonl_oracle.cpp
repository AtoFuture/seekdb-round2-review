#include <fstream>
#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
int main(int argc,char**argv) {
 if(argc!=2)return 1;
 std::ifstream in(argv[1],std::ios::binary);uint32_t n,q,d;
 in.read((char*)&n,4);in.read((char*)&q,4);in.read((char*)&d,4);
 std::vector<float> rows(size_t(n)*d),queries(size_t(q)*d);
 in.read((char*)rows.data(),rows.size()*4);in.read((char*)queries.data(),queries.size()*4);
 if(!in)return 2;
 std::cout<<std::setprecision(17);
 for(uint32_t j=0;j<q;++j) {
  std::vector<std::pair<double,uint32_t>> rank;
  for(uint32_t i=0;i<n;++i) {
   if(j%2&&i%10!=(j/2)%10)continue;
   double sum=0;
   for(uint32_t k=0;k<d;++k) {double delta=double(rows[size_t(i)*d+k])-double(queries[size_t(j)*d+k]);sum+=delta*delta;}
   rank.emplace_back(std::sqrt(sum),i);
  }
  std::sort(rank.begin(),rank.end());
  for(size_t k=0;k<std::min(size_t(10),rank.size());++k)std::cout<<rank[k].second<<":"<<rank[k].first<<" ";
  std::cout<<"\n";
 }
}
