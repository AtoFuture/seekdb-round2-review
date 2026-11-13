#pragma once
#include <cstddef>
#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
namespace minivectordb::detail {
// Embedded rounding is part of each instruction, not a process rounding-mode
// change. This bounds the mathematical square sum below. A conservative
// factor at the caller also protects the sequential-double rounding error.
#if !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
__attribute__((target("avx512f"),noinline))
inline void batch16_lower_bound_avx512(const float* batch,const float* query,
                                      std::size_t n,float* out) {
    constexpr int rz=_MM_FROUND_TO_ZERO|_MM_FROUND_NO_EXC;
    auto s0=_mm512_setzero_ps(),s1=s0,s2=s0,s3=s0;
    std::size_t d=0;
    for(;d+4<=n;d+=4) {
        const auto a=_mm512_sub_round_ps(_mm512_set1_ps(query[d]),_mm512_loadu_ps(batch+d*16),rz);
        const auto b=_mm512_sub_round_ps(_mm512_set1_ps(query[d+1]),_mm512_loadu_ps(batch+(d+1)*16),rz);
        const auto c=_mm512_sub_round_ps(_mm512_set1_ps(query[d+2]),_mm512_loadu_ps(batch+(d+2)*16),rz);
        const auto e=_mm512_sub_round_ps(_mm512_set1_ps(query[d+3]),_mm512_loadu_ps(batch+(d+3)*16),rz);
        s0=_mm512_fmadd_round_ps(a,a,s0,rz);s1=_mm512_fmadd_round_ps(b,b,s1,rz);
        s2=_mm512_fmadd_round_ps(c,c,s2,rz);s3=_mm512_fmadd_round_ps(e,e,s3,rz);
    }
    for(;d<n;++d) {
        const auto delta=_mm512_sub_round_ps(_mm512_set1_ps(query[d]),
                                            _mm512_loadu_ps(batch+d*16),rz);
        s0=_mm512_fmadd_round_ps(delta,delta,s0,rz);
    }
    const auto sum=_mm512_add_round_ps(_mm512_add_round_ps(s0,s1,rz),_mm512_add_round_ps(s2,s3,rz),rz);
    _mm512_storeu_ps(out,sum);
}

// RN float estimate, corrected by an outward error envelope at the caller.
// Eight independent chains hide FMA latency without changing returned values.
__attribute__((target("avx2,fma"),noinline))
inline void batch16_estimate_avx2(const float* batch,const float* query,
                                 std::size_t n,float* out) {
    auto a0=_mm256_setzero_ps(),a1=a0,a2=a0,a3=a0,b0=a0,b1=a0,b2=a0,b3=a0;
    std::size_t d=0;
    for(;d+4<=n;d+=4) {
        const auto q0=_mm256_set1_ps(query[d]),q1=_mm256_set1_ps(query[d+1]);
        const auto q2=_mm256_set1_ps(query[d+2]),q3=_mm256_set1_ps(query[d+3]);
        const auto x0=_mm256_sub_ps(q0,_mm256_loadu_ps(batch+d*16));
        const auto y0=_mm256_sub_ps(q0,_mm256_loadu_ps(batch+d*16+8));
        const auto x1=_mm256_sub_ps(q1,_mm256_loadu_ps(batch+(d+1)*16));
        const auto y1=_mm256_sub_ps(q1,_mm256_loadu_ps(batch+(d+1)*16+8));
        const auto x2=_mm256_sub_ps(q2,_mm256_loadu_ps(batch+(d+2)*16));
        const auto y2=_mm256_sub_ps(q2,_mm256_loadu_ps(batch+(d+2)*16+8));
        const auto x3=_mm256_sub_ps(q3,_mm256_loadu_ps(batch+(d+3)*16));
        const auto y3=_mm256_sub_ps(q3,_mm256_loadu_ps(batch+(d+3)*16+8));
        a0=_mm256_fmadd_ps(x0,x0,a0);b0=_mm256_fmadd_ps(y0,y0,b0);
        a1=_mm256_fmadd_ps(x1,x1,a1);b1=_mm256_fmadd_ps(y1,y1,b1);
        a2=_mm256_fmadd_ps(x2,x2,a2);b2=_mm256_fmadd_ps(y2,y2,b2);
        a3=_mm256_fmadd_ps(x3,x3,a3);b3=_mm256_fmadd_ps(y3,y3,b3);
    }
    for(;d<n;++d) {
        const auto q=_mm256_set1_ps(query[d]);
        const auto x=_mm256_sub_ps(q,_mm256_loadu_ps(batch+d*16));
        const auto y=_mm256_sub_ps(q,_mm256_loadu_ps(batch+d*16+8));
        a0=_mm256_fmadd_ps(x,x,a0);b0=_mm256_fmadd_ps(y,y,b0);
    }
    _mm256_storeu_ps(out,_mm256_add_ps(_mm256_add_ps(a0,a1),_mm256_add_ps(a2,a3)));
    _mm256_storeu_ps(out+8,_mm256_add_ps(_mm256_add_ps(b0,b1),_mm256_add_ps(b2,b3)));
}

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
