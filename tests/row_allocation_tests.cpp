#include "minivectordb/row_codec.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {
thread_local bool tracking=false;
thread_local std::size_t allocations=0;
}
void* operator new(std::size_t size) {
  if(auto* result=std::malloc(size?size:1)) {
    if(tracking)++allocations;
    return result;
  }
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }

using namespace minivectordb;
int main() {
  try {
    for(std::size_t columns=1;columns<=8;++columns) {
      Schema schema{1,"allocation",{},0};
      Row values;
      for(std::size_t i=0;i<columns;++i) {
        schema.columns.push_back({"c"+std::to_string(i),ColumnType::Bigint,0});
        values.emplace_back(std::int64_t(i+1));
      }
      const auto bytes=encode_row(schema,values);
      ExecutionInfo info;
      allocations=0;tracking=true;
      {
        LazyRowView row(schema,bytes,info);
        for(std::size_t i=0;i<columns;++i)
          if(std::get<std::int64_t>(row[i])!=std::int64_t(i+1))std::abort();
      }
      tracking=false;
      if(allocations!=0)throw std::runtime_error("short-schema row allocated "+std::to_string(allocations)+" times; expected zero metadata allocations");
      if(info.cells_materialized!=columns)throw std::runtime_error("wrong materialization count");
    }
    std::cout<<"PASS short-schema row metadata: zero allocations for 1..8 columns\n";
  } catch(const std::exception& error) {
    tracking=false;
    std::cerr<<error.what()<<'\n';return 1;
  }
}
