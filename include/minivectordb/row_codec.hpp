#pragma once
#include "minivectordb/database.hpp"
#include <array>
#include <optional>
namespace minivectordb {
  enum class ColumnType:std::uint8_t {
    Bigint=1,Varchar=2,Vector=3
  };
  struct Column {
    std::string name;
    ColumnType type;
    std::uint32_t extent=0;
  };
  struct Schema {
    std::uint64_t id=0;
    std::string name;
    std::vector<Column> columns;
    std::size_t primary=0;
  };
  void validate_schema(const Schema&);
  std::string encode_row(const Schema&,const Row&);
  Row decode_row(const Schema&,std::string_view);
  // Validates every field immediately, but owns only fields requested through operator[].
  // Schema and bytes must outlive the view; it must not escape a Store scan callback.
  // Cell references belong to this view and must not be kept across moving it.
  class LazyRowView {
    const Schema& schema_;
    std::string_view bytes_;
    static constexpr std::size_t inline_capacity=8;
    bool inline_metadata_;
    std::array<std::size_t,inline_capacity> inline_offsets_{};
    mutable std::array<std::optional<Cell>,inline_capacity> inline_cells_;
    std::vector<std::size_t> offsets_;
    mutable std::vector<std::optional<Cell>> cells_;
    ExecutionInfo& info_;
  public:
    LazyRowView(const Schema&, std::string_view, ExecutionInfo&);
    const Cell& operator[](std::size_t) const;
    // Copy a validated VECTOR without materializing a Cell. The view's bytes must
    // remain valid and must not alias the caller-owned scratch buffer.
    void copy_vector(std::size_t, std::vector<float>& scratch) const;
  };
}
