#include "minivectordb/database.hpp"
#include "minivectordb/row_codec.hpp"
#include "../src/storage/row_cpu.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
thread_local bool track_allocations=false;
thread_local std::size_t allocation_count=0;
}
void* operator new(std::size_t size) {
  if(auto* result=std::malloc(size?size:1)) {
    if(track_allocations)++allocation_count;
    return result;
  }
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) {return ::operator new(size);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}

using namespace minivectordb;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__)); } while(false)

namespace {
struct Item { std::int64_t id; std::vector<float> value; };
struct Result { std::vector<Row> rows; ExecutionInfo info; };
struct Hit { const Item* item; double distance; };
constexpr std::size_t top_k=3;

std::string literal(const std::vector<float>& values) {
  std::ostringstream out;
  out<<std::setprecision(9)<<'[';
  for(std::size_t n=0;n<values.size();++n) {if(n)out<<',';out<<values[n];}
  out<<']';return out.str();
}
std::vector<Item> fixture(std::size_t dimensions) {
  // Opposite vectors have equal returned distances; signed primary keys decide
  // their order. Insertion order deliberately differs from primary-key order.
  const std::vector<std::pair<std::int64_t,int>> specs{
    {99,3},{-2,-1},{7,-2},{-99,1},{203,-3},{0,2},{-12,0}};
  std::vector<Item> items;
  for(const auto& [id,scale]:specs) {
    std::vector<float> value(dimensions);
    for(std::size_t n=0;n<dimensions;++n)
      value[n]=float(scale)*float(1+n%3)/4.0f;
    items.push_back({id,std::move(value)});
  }
  return items;
}
double distance(const std::vector<float>& a,const std::vector<float>& b) {
  CHECK(a.size()==b.size());
  double sum=0;
  for(std::size_t n=0;n<a.size();++n) {
    const double delta=double(a[n])-double(b[n]);sum+=delta*delta;
  }
  return std::sqrt(sum);
}
// Enumerate and sort independently; no executor, row decoder or TopK helpers.
std::vector<Hit> oracle(const std::vector<Item>& items,const std::vector<float>& q) {
  std::vector<Hit> answer;
  for(const auto& item:items)answer.push_back({&item,distance(item.value,q)});
  std::sort(answer.begin(),answer.end(),[](const Hit& a,const Hit& b) {
    return a.distance<b.distance || (a.distance==b.distance && a.item->id<b.item->id);
  });
  if(answer.size()>top_k)answer.resize(top_k);
  return answer;
}
Result query(Database& db,const std::string& sql) {
  Result r;r.info=db.execute(sql,{{},[&](const Row& row){r.rows.push_back(row);}});return r;
}
void check_answer(const Result& result,const std::vector<Item>& items,
                  const std::vector<float>& q,const std::vector<float>& other,
                  bool project_vector,bool project_other) {
  const auto answer=oracle(items,q);
  CHECK(result.rows.size()==answer.size());
  for(std::size_t n=0;n<answer.size();++n) {
    const auto& actual=result.rows[n];const auto& wanted=answer[n];
    CHECK(actual.size()==2+std::size_t(project_vector)+std::size_t(project_other));
    CHECK(std::get<std::int64_t>(actual[0])==wanted.item->id);
    CHECK(std::get<double>(actual[1])==wanted.distance);
    std::size_t column=2;
    if(project_vector)CHECK(std::get<std::vector<float>>(actual[column++])==wanted.item->value);
    if(project_other)CHECK(std::get<double>(actual[column])==distance(wanted.item->value,other));
  }
  CHECK(result.info.rows_examined==items.size());
  CHECK(result.info.affected_rows==answer.size());
  CHECK(result.info.distances_computed==items.size()+(project_other?answer.size():0));
}
void sql_case(const std::filesystem::path& path,std::size_t dimensions,
              bool packed,bool lazy,bool reuse,bool indexed) {
  const auto items=fixture(dimensions);
  const std::vector<float> q(dimensions,0.0f),other(dimensions,0.5f);
  Options options;options.typed_scan_cache=false; // Exercise the vector-copy reference path.
  options.referenced_vector_index=false; // Keep legacy index/base copy-count assertions.
  options.packed_values=packed;options.selective_decode=lazy;
  options.reuse_vector_scratch=reuse;options.scan_snapshot_bytes=8192;
  Database db(path,options);
  db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR("+std::to_string(dimensions)+"))");
  std::string insert="INSERT INTO t VALUES ";
  for(std::size_t n=0;n<items.size();++n) {
    if(n)insert+=',';
    insert+="("+std::to_string(items[n].id)+",'"+literal(items[n].value)+"')";
  }
  db.execute(insert);
  if(indexed)db.execute("CREATE VECTOR INDEX tv ON t(v)");
  const std::string base="SELECT id,L2_DISTANCE(v,'"+literal(q)+"') AS d";
  const std::string tail=" FROM t ORDER BY d LIMIT "+std::to_string(top_k);
  // Cover an initial scan, a snapshot-building scan and warm reuse when enabled.
  for(int phase=0;phase<3;++phase) {
    const auto r=query(db,base+tail);
    check_answer(r,items,q,other,false,false);
    // Cold indexed scans retain base/index equality checks. Eager decoding also
    // owns vectors in winner refetch, even if output needs only id and distance.
    const bool scratch_scan=lazy && reuse && (!indexed || phase==2);
    const auto scan_cells=scratch_scan?0:items.size()-r.info.validated_snapshot_rows_examined;
    CHECK(r.info.vector_values_materialized==scan_cells+(lazy?0:r.rows.size()));
    CHECK(r.info.vector_index_entries_examined==((indexed && phase<2)?items.size():0));
    CHECK(r.info.snapshot_rows_examined==(phase==2?items.size():0));
  }
  for(const auto& [project_vector,project_other]:
      std::vector<std::pair<bool,bool>>{{true,false},{false,true},{true,true}}) {
    std::string sql=base;
    if(project_vector)sql+=",v";
    if(project_other)sql+=",L2_DISTANCE(v,'"+literal(other)+"') AS other_d";
    const auto r=query(db,sql+tail);
    check_answer(r,items,q,other,project_vector,project_other);
    // Winner refetch owns its projected/evaluated vector Cell. Projecting both
    // a vector and another distance must still materialize it only once.
    const auto scan_cells=(lazy && reuse)?0:items.size()-r.info.validated_snapshot_rows_examined;
    CHECK(r.info.vector_values_materialized==scan_cells+r.rows.size());
    CHECK(r.info.vector_index_entries_examined==0);
    CHECK(r.info.snapshot_rows_examined==items.size());
  }
}

constexpr std::array<std::size_t,10> dimensions_list{1,2,7,8,9,31,32,33,128,768};
constexpr std::array<std::uint32_t,12> finite_bits{
  0x00000000u,0x80000000u,0x00000001u,0x80000001u,
  0x007fffffu,0x807fffffu,0x00800000u,0x80800000u,
  0x7f7fffffu,0xff7fffffu,0x3f800001u,0xbf800001u};
void append_le(std::string& bytes,std::uint64_t value,unsigned count) {
  for(unsigned n=0;n<count;++n)bytes.push_back(static_cast<char>((value>>(8*n))&255));
}
struct Encoded {Schema schema;std::string bytes;std::vector<std::uint32_t> bits;};
Encoded encoded(std::size_t dimension,std::size_t columns,std::size_t padding,std::size_t shift) {
  Encoded result{{1,"copy_bits",{},0},{},{}};
  append_le(result.bytes,1,1);append_le(result.bytes,columns,4);
  for(std::size_t n=0;n<columns;++n) {
    const auto name="c"+std::to_string(n);
    if(n==columns-1) {
      result.schema.columns.push_back({name,ColumnType::Vector,static_cast<std::uint32_t>(dimension)});
      append_le(result.bytes,dimension,4);
      for(std::size_t d=0;d<dimension;++d) {
        const auto bits=finite_bits[(d+shift)%finite_bits.size()];
        result.bits.push_back(bits);append_le(result.bytes,bits,4);
      }
    } else if(n==1) {
      result.schema.columns.push_back({name,ColumnType::Varchar,64});
      append_le(result.bytes,padding,4);result.bytes.append(padding,'a');
    } else {
      result.schema.columns.push_back({name,ColumnType::Bigint,0});
      append_le(result.bytes,std::bit_cast<std::uint64_t>(-std::int64_t(n)-17),8);
    }
  }
  validate_schema(result.schema);
  return result;
}
void same_bits(const std::vector<float>& actual,const std::vector<std::uint32_t>& wanted) {
  CHECK(actual.size()==wanted.size());
  for(std::size_t n=0;n<wanted.size();++n)
    CHECK(std::bit_cast<std::uint32_t>(actual[n])==wanted[n]);
}
template<class F> void out_of_range(F&& action) {
  bool failed=false;try {action();}catch(const std::out_of_range&) {failed=true;}CHECK(failed);
}
void copy_bits_and_bounds() {
  std::vector<float> scratch;scratch.reserve(dimensions_list.back());
  for(bool heap:{false,true})for(auto validation:{detail::VectorValidationMode::scalar,detail::VectorValidationMode::batched}) {
    detail::row_metadata_heap=heap;detail::vector_validation_mode=validation;
    for(const auto dimension:dimensions_list)for(std::size_t columns:{2U,8U,9U,64U}) {
      for(std::size_t offset=0;offset<16;++offset) {
        // Independent wire encoding, plus byte-wise placement/padding, exercises
        // every alignment and signed zero/subnormal/extreme float bit patterns.
        const auto fixture=encoded(dimension,columns,offset,offset);
        std::string storage(offset,'x');storage+=fixture.bytes;
        ExecutionInfo info;
        LazyRowView view(fixture.schema,std::string_view(storage).substr(offset),info);
        // A view captures its metadata layout; later benchmark-mode changes
        // cannot change the layout used by copy_vector.
        detail::row_metadata_heap=!heap;
        view.copy_vector(columns-1,scratch);same_bits(scratch,fixture.bits);
        CHECK(info.cells_materialized==0 && info.vector_values_materialized==0);
        const auto& cell=std::get<std::vector<float>>(view[columns-1]);
        same_bits(cell,fixture.bits);
        view.copy_vector(columns-1,scratch);same_bits(scratch,fixture.bits);
        CHECK(&std::get<std::vector<float>>(view[columns-1])==&cell);
        CHECK(info.cells_materialized==1 && info.vector_values_materialized==1);
        bool wrong_type=false;
        try {view.copy_vector(0,scratch);}catch(const Error& error) {CHECK(error.code()==ErrorCode::Type);wrong_type=true;}
        CHECK(wrong_type);
        out_of_range([&]{view.copy_vector(columns,scratch);});
        out_of_range([&]{view.copy_vector(std::size_t(-1),scratch);});
        detail::row_metadata_heap=heap;
      }
    }
  }
  detail::row_metadata_heap=false;detail::vector_validation_mode=detail::VectorValidationMode::batched;
}
void copy_move_lifetimes() {
  std::vector<float> scratch;scratch.reserve(33);
  for(bool heap:{false,true})for(std::size_t columns:{2U,8U,9U,64U})
    for(bool move:{false,true})for(bool cached:{false,true}) {
      detail::row_metadata_heap=heap;
      const auto fixture=encoded(33,columns,7,3);
      ExecutionInfo info;std::optional<LazyRowView> destination;
      {
        LazyRowView source(fixture.schema,fixture.bytes,info);
        if(cached)(void)source[columns-1];
        if(move)destination.emplace(std::move(source));else destination.emplace(source);
        if(move && (heap || columns>8))out_of_range([&]{source.copy_vector(columns-1,scratch);});
      }
      const auto cells=info.cells_materialized;
      destination->copy_vector(columns-1,scratch);same_bits(scratch,fixture.bits);
      CHECK(info.cells_materialized==cells && info.vector_values_materialized==cells);
      same_bits(std::get<std::vector<float>>((*destination)[columns-1]),fixture.bits);
      CHECK(info.cells_materialized==1 && info.vector_values_materialized==1);
    }
  detail::row_metadata_heap=false;
}
void reserved_buffer_allocations() {
  std::vector<float> scratch;scratch.reserve(768);
  const auto* buffer=scratch.data();
  // Reuse the same buffer across rows, dimensions and metadata paths. Both grow
  // within existing capacity and shrink must avoid a new allocation.
  for(bool heap:{false,true})for(std::size_t columns:{2U,8U,9U,64U})
    for(std::size_t dimension:{768U,1U,128U,33U,2U,768U}) {
      detail::row_metadata_heap=heap;
      const auto fixture=encoded(dimension,columns,5,dimension%finite_bits.size());
      ExecutionInfo info;LazyRowView view(fixture.schema,fixture.bytes,info);
      allocation_count=0;track_allocations=true;
      try {for(unsigned repeat=0;repeat<32;++repeat)view.copy_vector(columns-1,scratch);}
      catch(...) {track_allocations=false;throw;}
      track_allocations=false;
      CHECK(allocation_count==0);CHECK(scratch.data()==buffer);
      same_bits(scratch,fixture.bits);
      CHECK(info.cells_materialized==0 && info.vector_values_materialized==0);
    }
  detail::row_metadata_heap=false;
}
}
int main() {
  const char* env=std::getenv("MVDB_TEST_DIR");
  const auto root=std::filesystem::path(env?env:"/data")/("vector-scratch-"+std::to_string(getpid()));
  std::filesystem::create_directories(root);
  try {
    copy_bits_and_bounds();copy_move_lifetimes();reserved_buffer_allocations();
    for(std::size_t dimensions:{2U,128U})for(bool packed:{false,true})for(bool lazy:{false,true})
      for(bool reuse:{false,true})for(bool indexed:{false,true}) {
        const auto suffix=std::to_string(dimensions)+"-"+std::to_string(packed)+std::to_string(lazy)+std::to_string(reuse)+std::to_string(indexed);
        sql_case(root/("sql-"+suffix),dimensions,packed,lazy,reuse,indexed);
      }
  } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
  std::filesystem::remove_all(root);
  std::cout<<"vector scratch tests passed\n";
}
