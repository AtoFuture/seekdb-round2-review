#include "minivectordb/value.hpp"
#include "text_validation.hpp"
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
namespace minivectordb {
std::int64_t parse_bigint(std::string_view text) {
  if (text.empty()) throw std::invalid_argument("empty BIGINT");
  if (text.front()=='+') {
    text.remove_prefix(1);
    if (text.empty() || text.front()<'0' || text.front()>'9') throw std::invalid_argument("invalid BIGINT");
  }
  std::int64_t result{};
  auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),result);
  if (error!=std::errc{} || end!=text.data()+text.size()) throw std::invalid_argument("BIGINT outside signed 64-bit range or invalid syntax");
  return result;
}
namespace {
template<bool AsciiBlocks>
std::size_t validate_text_impl(std::string_view text,std::size_t limit) {
  std::size_t count=0;
  for(std::size_t i=0;i<text.size();) {
    if constexpr(AsciiBlocks) {
      if(text.size()-i>=sizeof(std::uint64_t)) {
        std::uint64_t block;
        std::memcpy(&block,text.data()+i,sizeof(block));
        // This mask identifies non-ASCII bytes on either endian order. A mixed
        // block leaves i unchanged so the scalar parser validates it normally.
        if((block&UINT64_C(0x8080808080808080))==0) {
          // Every preceding increment checked the limit, so count<=limit.
          if(limit-count<sizeof(block))throw std::invalid_argument("text exceeds scalar length");
          count+=sizeof(block);i+=sizeof(block);
          continue;
        }
      }
    }
    auto lead=static_cast<unsigned char>(text[i++]);
    std::uint32_t cp=lead, minimum=0; unsigned remaining=0;
    if(lead<0x80) {}
    else if(lead>=0xc2 && lead<=0xdf) {cp=lead&0x1f; minimum=0x80; remaining=1;}
    else if(lead>=0xe0 && lead<=0xef) {cp=lead&0x0f; minimum=0x800; remaining=2;}
    else if(lead>=0xf0 && lead<=0xf4) {cp=lead&0x07; minimum=0x10000; remaining=3;}
    else throw std::invalid_argument("invalid UTF-8 leading byte");
    while(remaining--) {
      if(i==text.size()) throw std::invalid_argument("truncated UTF-8");
      auto next=static_cast<unsigned char>(text[i++]);
      if((next&0xc0)!=0x80) throw std::invalid_argument("invalid UTF-8 continuation");
      cp=(cp<<6)|(next&0x3f);
    }
    if(cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) throw std::invalid_argument("invalid Unicode scalar");
    if(count==limit) throw std::invalid_argument("text exceeds scalar length");
    ++count;
  }
  return count;
}
}
std::size_t validate_text(std::string_view text,std::size_t limit) {
  if(detail::text_validation_mode==detail::TextValidationMode::scalar)
    return validate_text_impl<false>(text,limit);
  return validate_text_impl<true>(text,limit);
}
int compare_text(std::string_view a,std::string_view b) {
  validate_text(a,std::numeric_limits<std::size_t>::max());
  validate_text(b,std::numeric_limits<std::size_t>::max());
  // Canonical UTF-8 preserves scalar ordering when compared as unsigned bytes.
  for(std::size_t i=0;i<a.size() && i<b.size();++i) {
    auto x=static_cast<unsigned char>(a[i]), y=static_cast<unsigned char>(b[i]);
    if(x!=y) return x<y?-1:1;
  }
  return a.size()==b.size()?0:(a.size()<b.size()?-1:1);
}
std::vector<float> make_vector(std::span<const double> values,std::size_t dimension) {
  if(values.size()!=dimension) throw std::invalid_argument("VECTOR dimension mismatch");
  std::vector<float> result; result.reserve(values.size());
  for(double value:values) {
    // IEEE binary32 round-to-nearest overflow begins half an ULP above
    // FLT_MAX. Decimal literals such as 3.4028235e38 still round to FLT_MAX.
    // Handle that interval explicitly rather than casting an out-of-range double.
    static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559 &&
                  std::numeric_limits<float>::digits==24);
    const double maximum=std::numeric_limits<float>::max();
    const double overflow=std::ldexp(1.0,128)-std::ldexp(1.0,103);
    const double magnitude=std::abs(value);
    if(!std::isfinite(value) || magnitude>=overflow) throw std::invalid_argument("VECTOR component is not finite float32");
    const float converted=magnitude>maximum
        ? (std::signbit(value)?-std::numeric_limits<float>::max():std::numeric_limits<float>::max())
        : static_cast<float>(value);
    if(!std::isfinite(converted)) throw std::invalid_argument("VECTOR component is not finite float32");
    result.push_back(converted);
  }
  return result;
}
}
