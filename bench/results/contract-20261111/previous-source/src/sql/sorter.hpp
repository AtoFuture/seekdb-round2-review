#pragma once
#include "sql.hpp"
namespace minivectordb::sql {
  struct SortRecord {
    std::int64_t key=0;
    std::vector<Cell> values;
  };
  class Sorter {
    public:
    Sorter(const Options&,const std::vector<BoundOrder>&,ExecutionInfo&);
    ~Sorter();
    void add(SortRecord);
    void finish(std::uint64_t limit,const std::function<void(const SortRecord&)>&);
    private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
}
