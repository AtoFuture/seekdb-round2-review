#pragma once
#include <cstdint>
#include <string_view>
#include <span>
#include <vector>
namespace minivectordb {
std::int64_t parse_bigint(std::string_view text);
std::size_t validate_text(std::string_view text, std::size_t max_scalars);
int compare_text(std::string_view a, std::string_view b);
std::vector<float> make_vector(std::span<const double> values, std::size_t dimension);
}
