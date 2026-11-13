#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>
#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#endif

namespace minivectordb::detail {
// Auxiliary rejection index only. Original float32 vectors remain authoritative;
// every survivor uses the unchanged sequential-double distance and tie order.
class Int8BoundCache {
  std::unique_ptr<std::int8_t[]> codes_;
  std::unique_ptr<std::int32_t[]> norms_,sums_;
  std::unique_ptr<double[]> errors_;
  const float* source_=nullptr;
  std::size_t rows_=0,dimension_=0,padded_=0;
  double scale_=1;
  static double up(double v) {return std::nextafter(v,std::numeric_limits<double>::infinity());}
  static int quantize(double value,double scale) {
    return static_cast<int>(std::clamp(std::round(value/scale),-127.0,127.0));
  }
  static double error_component(double value,double represented) {
    // Covers the multiply and subtraction rounding before squaring. Float32
    // inputs are exact in double; all intermediate magnitudes stay < 1e84.
    return up(std::abs(value-represented)+std::abs(represented)*0x1p-48+
              std::numeric_limits<double>::min());
  }
  static double finish_error(double square,std::size_t n) {
    return up(std::sqrt(square)*(1+static_cast<double>(n+8)*0x1p-48));
  }
#if defined(__x86_64__) && defined(__GNUC__)
  __attribute__((target("avx2"),noinline))
  static void dots_avx2(const std::int8_t* codes,const std::uint32_t* query,
                       const std::int32_t* sums,std::size_t groups,std::int32_t* output) {
    auto a=_mm256_setzero_si256(),b=a,c=a,d=a;
    const auto ones=_mm256_set1_epi16(1),bias=_mm256_set1_epi8(char(-128));
    for(std::size_t g=0;g<groups;++g) {
      // Restore signed query codes. Neither operand contains -128; therefore
      // abs(query)*sign(code,query) has adjacent sums bounded by 32258.
      // VPMADDUBSW cannot saturate, unlike using the biased unsigned query.
      const auto q=_mm256_xor_si256(_mm256_set1_epi32(std::bit_cast<std::int32_t>(query[g])),bias);
      const auto magnitude=_mm256_abs_epi8(q);
      const auto lo=_mm256_sign_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(codes+g*64)),q);
      const auto hi=_mm256_sign_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(codes+g*64+32)),q);
      const auto x=_mm256_madd_epi16(_mm256_maddubs_epi16(magnitude,lo),ones);
      const auto y=_mm256_madd_epi16(_mm256_maddubs_epi16(magnitude,hi),ones);
      if(g&1){c=_mm256_add_epi32(c,x);d=_mm256_add_epi32(d,y);}
      else {a=_mm256_add_epi32(a,x);b=_mm256_add_epi32(b,y);}
    }
    // Match VNNI's unsigned-query dot product for the shared bound evaluator.
    a=_mm256_add_epi32(_mm256_add_epi32(a,c),_mm256_slli_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(sums)),7));
    b=_mm256_add_epi32(_mm256_add_epi32(b,d),_mm256_slli_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(sums+8)),7));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(output),a);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(output+8),b);
  }
  __attribute__((target("avx512f,avx512vnni"),noinline))
  static void dots_native(const std::int8_t* codes,const std::uint32_t* query,
                          std::size_t groups,std::int32_t* output) {
    auto a=_mm512_setzero_si512(),b=a,c=a,d=a;std::size_t g=0;
    for(;g+4<=groups;g+=4) {
      a=_mm512_dpbusd_epi32(a,_mm512_set1_epi32(std::bit_cast<std::int32_t>(query[g])),_mm512_loadu_si512(codes+g*64));
      b=_mm512_dpbusd_epi32(b,_mm512_set1_epi32(std::bit_cast<std::int32_t>(query[g+1])),_mm512_loadu_si512(codes+(g+1)*64));
      c=_mm512_dpbusd_epi32(c,_mm512_set1_epi32(std::bit_cast<std::int32_t>(query[g+2])),_mm512_loadu_si512(codes+(g+2)*64));
      d=_mm512_dpbusd_epi32(d,_mm512_set1_epi32(std::bit_cast<std::int32_t>(query[g+3])),_mm512_loadu_si512(codes+(g+3)*64));
    }
    for(;g<groups;++g)a=_mm512_dpbusd_epi32(a,_mm512_set1_epi32(std::bit_cast<std::int32_t>(query[g])),_mm512_loadu_si512(codes+g*64));
    const auto total=_mm512_add_epi32(_mm512_add_epi32(a,b),_mm512_add_epi32(c,d));
    _mm512_storeu_si512(output,total);
  }
#if !defined(__clang__)
  __attribute__((optimize("fp-contract=off")))
#endif
  __attribute__((target("avx512f,avx2"),noinline))
  static std::uint16_t reject_native(const std::int32_t* dots,const std::int32_t* norms,
      const std::int32_t* sums,const double* errors,double scale_square,double cross,
      double a,double expanded) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    const auto one=_mm512_set1_epi64(1);const auto zero=_mm512_setzero_pd();std::uint16_t rejected=0;
    for(unsigned lane=0;lane<16;lane+=8) {
      const auto dot=_mm256_sub_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(dots+lane)),
                     _mm256_slli_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(sums+lane)),7));
      const auto b=_mm512_mul_pd(_mm512_cvtepi32_pd(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(norms+lane))),_mm512_set1_pd(scale_square));
      const auto c=_mm512_mul_pd(_mm512_cvtepi32_pd(dot),_mm512_set1_pd(cross));
      const auto abs_c=_mm512_max_pd(c,_mm512_sub_pd(zero,c));
      const auto ab=_mm512_add_pd(_mm512_set1_pd(a),b);
      const auto allowance=_mm512_mul_pd(_mm512_add_pd(ab,abs_c),_mm512_set1_pd(0x1p-45));
      const auto lower=_mm512_sub_pd(_mm512_sub_pd(ab,c),allowance);
      // All upper-bound operands are finite nonnegative doubles. Incrementing
      // their IEEE bits is nextafter(+infinity), including positive zero.
      auto bound=_mm512_add_pd(_mm512_set1_pd(expanded),_mm512_loadu_pd(errors+lane));
      bound=_mm512_castsi512_pd(_mm512_add_epi64(_mm512_castpd_si512(bound),one));
      auto upper=_mm512_mul_pd(_mm512_mul_pd(bound,bound),_mm512_set1_pd(1+0x1p-45));
      upper=_mm512_castsi512_pd(_mm512_add_epi64(_mm512_castpd_si512(upper),one));
      rejected|=std::uint16_t(std::uint16_t(_mm512_cmp_pd_mask(lower,upper,_CMP_GT_OQ))<<lane);
    }
    return rejected;
  }
#if !defined(__clang__)
  __attribute__((optimize("fp-contract=off")))
#endif
  __attribute__((target("avx2"),noinline))
  static std::uint16_t reject_avx2(const std::int32_t* dots,const std::int32_t* norms,
      const std::int32_t* sums,const double* errors,double scale_square,double cross,
      double a,double expanded) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    const auto one=_mm256_set1_epi64x(1);const auto zero=_mm256_setzero_pd();std::uint16_t rejected=0;
    for(unsigned lane=0;lane<16;lane+=4) {
      const auto dot=_mm_sub_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(dots+lane)),
                     _mm_slli_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(sums+lane)),7));
      const auto b=_mm256_mul_pd(_mm256_cvtepi32_pd(_mm_loadu_si128(reinterpret_cast<const __m128i*>(norms+lane))),_mm256_set1_pd(scale_square));
      const auto c=_mm256_mul_pd(_mm256_cvtepi32_pd(dot),_mm256_set1_pd(cross));
      const auto abs_c=_mm256_max_pd(c,_mm256_sub_pd(zero,c));
      const auto ab=_mm256_add_pd(_mm256_set1_pd(a),b);
      const auto allowance=_mm256_mul_pd(_mm256_add_pd(ab,abs_c),_mm256_set1_pd(0x1p-45));
      const auto lower=_mm256_sub_pd(_mm256_sub_pd(ab,c),allowance);
      auto bound=_mm256_add_pd(_mm256_set1_pd(expanded),_mm256_loadu_pd(errors+lane));
      bound=_mm256_castsi256_pd(_mm256_add_epi64(_mm256_castpd_si256(bound),one));
      auto upper=_mm256_mul_pd(_mm256_mul_pd(bound,bound),_mm256_set1_pd(1+0x1p-45));
      upper=_mm256_castsi256_pd(_mm256_add_epi64(_mm256_castpd_si256(upper),one));
      rejected|=std::uint16_t(unsigned(_mm256_movemask_pd(_mm256_cmp_pd(lower,upper,_CMP_GT_OQ)))<<lane);
    }
    return rejected;
  }
#endif
public:
  struct Query {
    std::vector<std::uint32_t> words;
    double scale=1,error=0,norm_scaled=0;
    bool valid=false;
  };
  static bool available() noexcept {
#if defined(__x86_64__) && defined(__GNUC__)
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
  }
  static bool native_vnni() noexcept {
#if defined(__x86_64__) && defined(__GNUC__)
    return available() && __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512vnni");
#else
    return false;
#endif
  }
  static bool environment() noexcept {
    if(std::fegetround()!=FE_TONEAREST)return false;
#if defined(__x86_64__) && defined(__GNUC__)
    return (_mm_getcsr()&0xffc0u)==0x1f80u;
#else
    return true;
#endif
  }
  void clear() noexcept {
    codes_.reset();norms_.reset();sums_.reset();errors_.reset();
    source_=nullptr;rows_=dimension_=padded_=0;scale_=1;
  }
  std::size_t bytes() const noexcept {return rows_*(padded_+16);}
  bool ready() const noexcept {return source_!=nullptr;}
  bool build(std::span<const float> source,std::size_t rows,std::size_t dimension,
             std::size_t budget,bool portable_test=false) {
    clear();
    if((!portable_test&&!available()) || !environment() || dimension<32 || dimension>4096 || rows<16)return false;
    const auto count=rows/16*16,padded=(dimension+3)/4*4;
    if(count>budget/(padded+16) || count>source.size()/dimension)return false;
    double maximum=0;
    for(std::size_t i=0;i<count*dimension;++i) {
      if(!std::isfinite(source[i]))return false;
      maximum=std::max(maximum,std::abs(static_cast<double>(source[i])));
    }
    const double scale=maximum>0?maximum/127:1;
    try {
      codes_=std::make_unique<std::int8_t[]>(count*padded);
      norms_=std::make_unique<std::int32_t[]>(count);
      sums_=std::make_unique<std::int32_t[]>(count);
      errors_=std::make_unique<double[]>(count);
    }catch(const std::bad_alloc&){clear();return false;}
    for(std::size_t block=0;block<count;block+=16)for(std::size_t lane=0;lane<16;++lane) {
      std::int32_t norm=0,sum=0;double error=0;
      for(std::size_t d=0;d<padded;++d) {
        const double value=d<dimension?source[block*dimension+d*16+lane]:0;
        const int code=quantize(value,scale);
        codes_[block*padded+d/4*64+lane*4+d%4]=static_cast<std::int8_t>(code);
        norm+=code*code;sum+=code;
        const double e=error_component(value,scale*code);error+=e*e;
      }
      norms_[block+lane]=norm;sums_[block+lane]=sum;
      errors_[block+lane]=finish_error(error,padded);
    }
    rows_=count;dimension_=dimension;padded_=padded;scale_=scale;source_=source.data();return true;
  }
  Query query(std::span<const float> vector) const {
    Query result;
    if(!ready() || vector.size()!=dimension_ || !environment())return result;
    double maximum=0;
    for(float x:vector){if(!std::isfinite(x))return result;maximum=std::max(maximum,std::abs(static_cast<double>(x)));}
    result.scale=maximum>0?maximum/127:1;
    result.words.assign(padded_/4,0);std::int32_t norm=0;double square=0;
    for(std::size_t d=0;d<padded_;++d) {
      const double value=d<dimension_?vector[d]:0;
      const int code=quantize(value,result.scale);norm+=code*code;
      result.words[d/4]|=std::uint32_t(code+128)<<(8*(d%4));
      const double e=error_component(value,result.scale*code);square+=e*e;
    }
    result.error=finish_error(square,padded_);
    result.norm_scaled=static_cast<double>(norm)*result.scale*result.scale;
    result.valid=true;return result;
  }
  std::uint16_t survivors(const Query& query,double radius,std::span<const float> original,
                          std::uint16_t mask,bool native=true,bool force_avx2=false) const {
    if(!query.valid || !std::isfinite(radius) || radius<0 || original.size()!=16*dimension_)return mask;
    const auto begin=reinterpret_cast<std::uintptr_t>(source_);
    const auto address=reinterpret_cast<std::uintptr_t>(original.data());
    if(address<begin || address-begin>=rows_*dimension_*sizeof(float))return mask;
    const auto offset=address-begin;
    if(offset%(16*dimension_*sizeof(float)))return mask;
    const auto row=offset/(dimension_*sizeof(float));
    std::array<std::int32_t,16> dots{};
#if defined(__x86_64__) && defined(__GNUC__)
    if(native&&native_vnni()&&!force_avx2)dots_native(codes_.get()+row*padded_,query.words.data(),padded_/4,dots.data());
    else if(native&&available())dots_avx2(codes_.get()+row*padded_,query.words.data(),sums_.get()+row,padded_/4,dots.data());
    else
#else
    (void)native;(void)force_avx2;
#endif
    for(std::size_t group=0;group<padded_/4;++group)for(std::size_t lane=0;lane<16;++lane)
      for(std::size_t k=0;k<4;++k)
        dots[lane]+=int((query.words[group]>>(k*8))&255U)*int(codes_[row*padded_+group*64+lane*4+k]);
    const double scale_square=scale_*scale_,cross=2*scale_*query.scale;
    // Upward radius expansion covers original sequential-double subtraction,
    // square/add, sqrt rounding and equality/tie ordering, n <= 4096.
    const double expanded=up(up(radius)*(1+double(dimension_+8)*0x1p-48))+query.error;
#if defined(__x86_64__) && defined(__GNUC__)
    if(native&&native_vnni()&&!force_avx2)return mask&std::uint16_t(~reject_native(dots.data(),norms_.get()+row,sums_.get()+row,
                      errors_.get()+row,scale_square,cross,query.norm_scaled,expanded));
    if(native&&available())return mask&std::uint16_t(~reject_avx2(dots.data(),norms_.get()+row,sums_.get()+row,
                      errors_.get()+row,scale_square,cross,query.norm_scaled,expanded));
#endif
    for(std::size_t lane=0;lane<16;++lane)if(mask&(1U<<lane)) {
      const auto dot=dots[lane]-128*sums_[row+lane];
      const double a=query.norm_scaled,b=double(norms_[row+lane])*scale_square,c=cross*double(dot);
      // Three-term cancellation may make the estimate negative; the outward
      // envelope remains valid and simply declines to reject that candidate.
      const double lower=(a+b-c)-(std::abs(a)+std::abs(b)+std::abs(c))*0x1p-45;
      const double bound=up(expanded+errors_[row+lane]);
      const double upper=up(bound*bound*(1+0x1p-45));
      if(lower>upper)mask&=static_cast<std::uint16_t>(~(1U<<lane));
    }
    return mask;
  }
};
}
