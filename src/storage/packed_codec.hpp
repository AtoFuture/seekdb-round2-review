#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>
#include <string>
#include <string_view>
#include <stdexcept>
namespace minivectordb::detail {
// Bounded lossless byte runs, independent of SQL/types/workload contents.
// 4-byte decoded size; control <128 = 1..128 literals, else 3..130 repeats.
// Maximum encoded size is 4 + n + ceil(n/128), n <= 4064.
inline std::size_t pack_run_frame(std::string_view input,char* output) {
  auto* to=output;
  for(unsigned k=0;k<4;++k)*to++=static_cast<char>(input.size()>>(k*8));
  std::size_t i=0;
  while(i<input.size()) {
    auto end=i+1;
    while(end<input.size() && end-i<130 && input[end]==input[i])++end;
    if(end-i>=3) {
      *to++=static_cast<char>(128+end-i-3);*to++=input[i];i=end;
    } else {
      const auto begin=i;i=end;
      while(i<input.size() && i-begin<128) {
        end=i+1;while(end<input.size() && end-i<3 && input[end]==input[i])++end;
        if(end-i>=3)break;
        i=std::min(end,begin+128);
      }
      *to++=static_cast<char>(i-begin-1);
      std::memcpy(to,input.data()+begin,i-begin);to+=i-begin;
    }
  }
  return static_cast<std::size_t>(to-output);
}
inline bool pack_bytes(std::string_view input,std::string& output,bool planes=false) {
  output.clear();if(input.size()<64 || input.size()>4064)return false;
  std::array<char,4104> ordinary;
  const auto normal_size=pack_run_frame(input,ordinary.data());
  const bool normal=normal_size+8<=input.size();
  if(planes) {
    std::array<char,4064> reordered;std::size_t count=0;
    for(std::size_t lane=0;lane<4;++lane)
      for(std::size_t i=lane;i<input.size();i+=4)reordered[count++]=input[i];
    std::array<char,4104> candidate;
    const auto plane_size=pack_run_frame({reordered.data(),input.size()},candidate.data());
    if(plane_size+8<=input.size() && (!normal || plane_size<normal_size)) {
      candidate[3]=static_cast<char>(static_cast<unsigned char>(candidate[3])|128U);
      output.assign(candidate.data(),plane_size);return true;
    }
  }
  if(normal)output.assign(ordinary.data(),normal_size);
  return normal;
}
inline std::string unpack_bytes(std::string_view input,std::size_t maximum=4064,bool planes=false) {
  auto bad=[](){throw std::runtime_error("corrupt MiniVectorDB packed value");};
  if(input.size()<4)bad();
  std::size_t expected=0;
  for(unsigned k=0;k<4;++k)expected|=std::size_t(static_cast<unsigned char>(input[k]))<<(k*8);
  const bool shuffled=(expected&0x80000000U)!=0;
  if(shuffled&&!planes)bad();
  expected&=0x7fffffffU;
  if(expected<64 || expected>maximum || input.size()+8>expected)bad();
  std::string output;output.reserve(expected);std::size_t i=4;
  while(i<input.size()) {
    const auto control=static_cast<unsigned char>(input[i++]);
    const std::size_t length=control<128?std::size_t(control)+1:std::size_t(control)-128+3;
    if(length>expected-output.size())bad();
    if(control<128) {
      if(length>input.size()-i)bad();
      output.append(input.substr(i,length));i+=length;
    } else {
      if(i==input.size())bad();
      output.append(length,input[i++]);
    }
  }
  if(output.size()!=expected)bad();
  if(shuffled) {
    std::string original(expected,'\0');std::size_t offset=0;
    for(std::size_t lane=0;lane<4;++lane)
      for(std::size_t n=lane;n<expected;n+=4)original[n]=output[offset++];
    return original;
  }
  return output;
}
}
