#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <vector>
namespace minivectordb {
struct Neighbor { std::int64_t key; double distance; };
using VectorVisitor = std::function<void(std::int64_t, std::span<const float>)>;
using VectorSource = std::function<void(const VectorVisitor&)>;
using KeyPredicate = std::function<bool(std::int64_t)>;
double l2_distance(std::span<const float> a, std::span<const float> b);
// Rank by the returned double Euclidean L2 value, then signed primary key for ties.
// Square-root rounding is applied before comparison, matching SQL distance ordering.
std::vector<Neighbor> exact_top_k(std::span<const float> query, std::size_t k,
                                const VectorSource& source, const KeyPredicate& predicate = {});
}
