#pragma once
#include "minivectordb/row_codec.hpp"
#include "minivectordb/store.hpp"
namespace minivectordb {
  struct VectorIndex { std::uint64_t id,table; std::size_t column; std::string name; };
  std::vector<VectorIndex> vector_indexes(const Store&,std::uint64_t table);
  VectorIndex create_vector_index(Store&,const Schema&,std::size_t,std::string_view);
  Schema lookup_table(const Store&,std::string_view);
  void create_table(Store&,Schema);
}
