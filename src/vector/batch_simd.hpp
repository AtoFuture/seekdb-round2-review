#pragma once
#include <cstddef>
#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
namespace minivectordb::detail {
// AVX-512F itself admits 512-bit FMA: disabling the separate FMA target bit
// does not prohibit contraction. Keep an independently compiled strict body.
#if !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
__attribute__((target("avx512f,no-fma"),noinline))
inline void batch16_transposed_avx512(const float* batch,const float* query,
                                    std::size_t n,double* out) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    auto s0=_mm512_setzero_pd(),s1=s0;
    for(std::size_t d=0;d<n;++d) {
        const auto q=_mm512_set1_pd(static_cast<double>(query[d]));
        const auto d0=_mm512_sub_pd(q,_mm512_cvtps_pd(_mm256_loadu_ps(batch+d*16)));
        const auto d1=_mm512_sub_pd(q,_mm512_cvtps_pd(_mm256_loadu_ps(batch+d*16+8)));
        s0=_mm512_add_pd(s0,_mm512_mul_pd(d0,d0));
        s1=_mm512_add_pd(s1,_mm512_mul_pd(d1,d1));
    }
    _mm512_storeu_pd(out,s0);_mm512_storeu_pd(out+8,s1);
}
// Call only after the AVX2 runtime check. Lanes are independent candidates;
// accumulation stays in dimension order with separate multiply and add.
__attribute__((target("avx2,no-fma")))
inline void batch16_transposed_avx2(const float* batch,const float* query,
                                  std::size_t n,double* out) {
    auto s0=_mm256_setzero_pd(),s1=s0,s2=s0,s3=s0;
    for(std::size_t d=0;d<n;++d) {
        const auto q=_mm256_set1_pd(static_cast<double>(query[d]));
        const auto lo=_mm256_loadu_ps(batch+d*16);
        const auto hi=_mm256_loadu_ps(batch+d*16+8);
        const auto d0=_mm256_sub_pd(q,_mm256_cvtps_pd(_mm256_castps256_ps128(lo)));
        const auto d1=_mm256_sub_pd(q,_mm256_cvtps_pd(_mm256_extractf128_ps(lo,1)));
        const auto d2=_mm256_sub_pd(q,_mm256_cvtps_pd(_mm256_castps256_ps128(hi)));
        const auto d3=_mm256_sub_pd(q,_mm256_cvtps_pd(_mm256_extractf128_ps(hi,1)));
        s0=_mm256_add_pd(s0,_mm256_mul_pd(d0,d0));
        s1=_mm256_add_pd(s1,_mm256_mul_pd(d1,d1));
        s2=_mm256_add_pd(s2,_mm256_mul_pd(d2,d2));
        s3=_mm256_add_pd(s3,_mm256_mul_pd(d3,d3));
    }
    _mm256_storeu_pd(out,s0);_mm256_storeu_pd(out+4,s1);
    _mm256_storeu_pd(out+8,s2);_mm256_storeu_pd(out+12,s3);
}
}
#endif
