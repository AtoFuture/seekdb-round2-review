#pragma once
#include "minivectordb/exact.hpp"

namespace minivectordb::detail {
class Int8BoundCache;
bool has_avx2() noexcept;
bool has_avx512() noexcept;
// Internal fixed-stride layout: 16 independent lanes per dimension, finite
// values admitted by a complete scan; keys.size() <= 16 and mask selects lanes.
using BlockVectorVisitor = std::function<void(std::span<const std::int64_t>,
                                             std::span<const float>,std::uint16_t)>;
using BlockVectorSource = std::function<void(const BlockVectorVisitor&)>;
struct BoundScreenStats {
  std::uint64_t integer_screened=0,integer_rejected=0;
  std::uint64_t screened_lanes=0,rejected_lanes=0,exact_lanes=0;
  bool environment_supported=false,adaptive_disabled=false;
};
std::vector<Neighbor> exact_top_k_transposed_validated_candidates(
    std::span<const float> query,std::size_t k,const BlockVectorSource& source,bool allow_wide=true,
    bool allow_lower_bound=false,BoundScreenStats* screen_stats=nullptr,const Int8BoundCache* integer_cache=nullptr);
// Internal execution contract: each candidate span must contain finite float32
// values already validated by the row/index decoder or an immutable owned cache
// published after a complete validated scan. The source must keep those values
// unchanged throughout its synchronous visitor call. Do not use for raw input.
// Query finiteness is still checked once per call; each visited candidate's
// dimension is checked. Arithmetic/order/ties match the checked public API.
std::vector<Neighbor> exact_top_k_validated_candidates(
    std::span<const float> query,std::size_t k,const VectorSource& source,
    const KeyPredicate& predicate = {});
// Same contract and bitwise distance/order as the scalar validated path.
// Batches independent candidates using at most 64 KiB of owned float scratch;
// tiny/large dimensions and allocation failure use the scalar implementation.
std::vector<Neighbor> exact_top_k_batched_validated_candidates(
    std::span<const float> query,std::size_t k,const VectorSource& source,
    const KeyPredicate& predicate = {});
// Runtime-dispatched AVX2 across independent candidates. Unsupported CPUs and
// non-x86 builds use the portable batch path, with the same scratch bound.
std::vector<Neighbor> exact_top_k_simd_validated_candidates(
    std::span<const float> query,std::size_t k,const VectorSource& source,
    const KeyPredicate& predicate = {});
std::vector<Neighbor> exact_top_k_wide_validated_candidates(
    std::span<const float> query,std::size_t k,const VectorSource& source,
    const KeyPredicate& predicate = {});
}
