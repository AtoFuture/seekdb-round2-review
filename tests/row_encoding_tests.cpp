#include "minivectordb/row_codec.hpp"
#include "minivectordb/value.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <vector>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)

// Frozen scalar format-1 encoder, independent of the production binary helpers.
// This is the byte-at-a-time contract used before the bulk-copy optimization.
std::string reference(const Schema& schema,const Row& row) {
  if(row.size()!=schema.columns.size())throw Error(ErrorCode::Type,"row column count mismatch");
  std::string bytes;
  auto put=[&](std::uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)bytes+=char((v>>(i*8))&255);};
  put(1,1);put(row.size(),4);
  for(std::size_t i=0;i<row.size();++i) {
    const auto& c=schema.columns[i];const auto& v=row[i];
    try {
      switch(c.type) {
      case ColumnType::Bigint:
        if(!std::holds_alternative<std::int64_t>(v))throw Error(ErrorCode::Type,"expected BIGINT");
        put(std::bit_cast<std::uint64_t>(std::get<std::int64_t>(v)),8);break;
      case ColumnType::Varchar: {
        if(!std::holds_alternative<std::string>(v))throw Error(ErrorCode::Type,"expected VARCHAR");
        const auto& text=std::get<std::string>(v);validate_text(text,c.extent);
        if(text.size()>UINT32_MAX)throw Error(ErrorCode::Type,"value too long");
        put(text.size(),4);bytes+=text;break;
      }
      case ColumnType::Vector: {
        if(!std::holds_alternative<std::vector<float>>(v))throw Error(ErrorCode::Type,"expected VECTOR");
        const auto& values=std::get<std::vector<float>>(v);
        if(values.size()!=c.extent)throw Error(ErrorCode::Type,"VECTOR dimension mismatch");
        put(values.size(),4);
        for(float f:values) {
          if(!std::isfinite(f))throw Error(ErrorCode::Type,"non-finite VECTOR component");
          put(std::bit_cast<std::uint32_t>(f),4);
        }
        break;
      }
      }
    } catch(const std::invalid_argument& e) {throw Error(ErrorCode::Type,e.what());}
  }
  return bytes;
}
struct Outcome {std::optional<ErrorCode> error;std::string message,bytes;};
template<class F> Outcome observe(F&& f) {
  Outcome out;
  try {out.bytes=f();}catch(const Error& e){out.error=e.code();out.message=e.what();}
  return out;
}
void compare(const Schema& s,const Row& r) {
  auto a=observe([&]{return reference(s,r);}),b=observe([&]{return encode_row(s,r);});
  CHECK(a.error==b.error);CHECK(a.message==b.message);CHECK(a.bytes==b.bytes);
  if(!a.error)CHECK(encode_row(s,decode_row(s,b.bytes))==a.bytes);
}
int main() {
  try {
    // A fixed golden row makes endian and signed-zero checks independent of
    // both encoders and the decoder. Vector bits: -0, min subnormal, FLT_MAX.
    Schema s{1,"gold",{{"id",ColumnType::Bigint,0},{"v",ColumnType::Vector,3},{"t",ColumnType::Varchar,3}},0};
    Row r{std::int64_t(-2),std::vector<float>{-0.f,std::bit_cast<float>(UINT32_C(1)),std::numeric_limits<float>::max()},std::string("A\0B",3)};
    const unsigned char expected[]={1,3,0,0,0,254,255,255,255,255,255,255,255,3,0,0,0,0,0,0,128,1,0,0,0,255,255,127,127,3,0,0,0,65,0,66};
    CHECK(encode_row(s,r)==std::string(reinterpret_cast<const char*>(expected),sizeof(expected)));
    compare(s,r);
    std::mt19937_64 rng(0x5bd38b17);
    std::size_t checks=1;
    for(unsigned n:{0u,1u,2u,3u,16u,127u,128u,129u,768u,1025u})for(unsigned order=0;order<3;++order)for(unsigned round=0;round<12;++round) {
      std::vector<float> values;values.reserve(n);
      for(unsigned i=0;i<n;++i) {
        auto bits=static_cast<std::uint32_t>(rng());
        if((bits&UINT32_C(0x7f800000))==UINT32_C(0x7f800000))bits^=UINT32_C(0x00800000);
        values.push_back(std::bit_cast<float>(bits));
      }
      std::string text=round%3==0?std::string{}:round%3==1?std::string("x\0\xf0\x9f\x98\x80",6):std::string(8192,'q');
      std::vector<Column> columns{{"id",ColumnType::Bigint,0},{"v",ColumnType::Vector,n},{"t",ColumnType::Varchar,8192}};
      Row row{std::bit_cast<std::int64_t>(rng()),values,text};
      std::rotate(columns.begin(),columns.begin()+order,columns.end());std::rotate(row.begin(),row.begin()+order,row.end());
      Schema schema{1,"random",columns,(3-order)%3};compare(schema,row);++checks;
      // A wrong variant, bad dimension, and invalid text in varied column
      // order independently exercise which error must win.
      auto bad=row;bad[0]=double(0);compare(schema,bad);++checks;
      bad=row;bad[(4-order)%3]=std::vector<float>{std::numeric_limits<float>::infinity()};compare(schema,bad);++checks;
      bad=row;bad[(5-order)%3]=std::string("\xc0\xaf",2);compare(schema,bad);++checks;
      bad[(4-order)%3]=std::vector<float>(n,std::numeric_limits<float>::quiet_NaN());compare(schema,bad);++checks;
    }
    compare(s,Row{});++checks;
    std::cout<<"PASS row encoding: "<<checks<<" golden/scalar byte and error-order comparisons\n";
    return 0;
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
