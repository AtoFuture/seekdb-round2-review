#pragma once
#include "minivectordb/database.hpp"
#include <limits>
namespace minivectordb::binary {
  inline void put(std::string& out,std::uint64_t v,unsigned n=8) {
    for(unsigned i=0;i<n;++i)out.push_back(static_cast<char>(v>>(8*i)));
  }
  inline void text(std::string& out,std::string_view s) {
    if(s.size()>UINT32_MAX)throw Error(ErrorCode::Type,"value too long");
    put(out,s.size(),4);
    out.append(s);
  }
  class Reader {
    std::string_view data;
    std::size_t pos=0;
    public:
    explicit Reader(std::string_view s):data(s) {
    }
    std::size_t remaining() const {
      return data.size()-pos;
    }
    std::uint64_t get(unsigned n=8) {
      if(n>remaining())throw Error(ErrorCode::Corruption,"truncated binary value");
      std::uint64_t v=0;
      for(unsigned i=0;i<n;++i)v|=std::uint64_t(static_cast<unsigned char>(data[pos++]))<<(8*i);
      return v;
    }
    std::string_view take(std::size_t n) {
      if(n>remaining())throw Error(ErrorCode::Corruption,"truncated binary value");
      auto bytes=data.substr(pos,n);
      pos+=n;
      return bytes;
    }
    std::string_view text_view() {
      auto n=get(4);
      if(n>remaining())throw Error(ErrorCode::Corruption,"invalid binary string length");
      auto s=data.substr(pos,static_cast<std::size_t>(n));
      pos+=static_cast<std::size_t>(n);
      return s;
    }
    std::string text() { return std::string(text_view()); }
    void end() {
      if(remaining())throw Error(ErrorCode::Corruption,"trailing binary data");
    }
  };
}
