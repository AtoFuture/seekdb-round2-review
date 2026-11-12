#pragma once
#include "minivectordb/exact.hpp"

namespace minivectordb::detail {
// Internal execution contract: each candidate span must contain finite float32
// values already validated by the row/index decoder or an immutable owned cache
// published after a complete validated scan. The source must keep those values
// unchanged throughout its synchronous visitor call. Do not use for raw input.
// Query finiteness is still checked once per call; each visited candidate's
// dimension is checked. Arithmetic/order/ties match the checked public API.
std::vector<Neighbor> exact_top_k_validated_candidates(
    std::span<const float> query,std::size_t k,const VectorSource& source,
    const KeyPredicate& predicate = {});
}
