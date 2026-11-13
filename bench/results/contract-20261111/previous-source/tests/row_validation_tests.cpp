#include "minivectordb/row_codec.hpp"
#include "../src/storage/row_cpu.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>

using namespace minivectordb;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__)); } while(false)

namespace {
std::size_t comparisons=0;
constexpr std::array<std::size_t,15> dimensions{1,2,3,4,7,8,9,15,16,17,31,32,33,128,768};
constexpr std::array<std::uint32_t,22> special_bits{
  0x00000000u,0x80000000u,0x00000001u,0x80000001u,
  0x007fffffu,0x807fffffu,0x00800000u,0x80800000u,
  0x7f7fffffu,0xff7fffffu,0x3f800001u,0xbf800001u,
  0x7f800000u,0xff800000u,0x7f800001u,0xff800001u,
  0x7fbfffffu,0xffbfffffu,0x7fc00000u,0xffc00000u,
  0x7fffffffu,0xffffffffu};

// Construct the wire format independently of encode_row and binary::Reader.
// In particular, invalid float bit patterns must reach the decoder unchanged.
void append_le(std::string& bytes,std::uint64_t value,unsigned count) {
  for(unsigned n=0;n<count;++n)bytes.push_back(static_cast<char>((value>>(8*n))&255));
}
void set_le(std::string& bytes,std::size_t offset,std::uint32_t value) {
  CHECK(offset+4<=bytes.size());
  for(unsigned n=0;n<4;++n)bytes[offset+n]=static_cast<char>((value>>(8*n))&255);
}
Schema vector_schema(std::size_t dimension) {
  return {1,"vector_bits",{{"id",ColumnType::Bigint,0},{"padding",ColumnType::Varchar,64},
    {"v",ColumnType::Vector,static_cast<std::uint32_t>(dimension)}},0};
}
std::string raw_row(const std::vector<std::uint32_t>& bits,std::size_t padding=0) {
  std::string bytes;
  append_le(bytes,1,1);append_le(bytes,3,4);
  append_le(bytes,std::bit_cast<std::uint64_t>(std::int64_t(-17)),8);
  append_le(bytes,padding,4);bytes.append(padding,'x');
  append_le(bytes,bits.size(),4);
  for(auto value:bits)append_le(bytes,value,4);
  return bytes;
}
bool finite_oracle(const std::vector<std::uint32_t>& bits) {
  // Deliberately use floating classification, not the production exponent mask.
  return std::all_of(bits.begin(),bits.end(),[](auto value) {
    return std::isfinite(std::bit_cast<float>(value));
  });
}
void same_cell(const Cell& actual,const Cell& expected) {
  CHECK(actual.index()==expected.index());
  if(const auto* values=std::get_if<std::vector<float>>(&expected)) {
    const auto& decoded=std::get<std::vector<float>>(actual);
    CHECK(decoded.size()==values->size());
    for(std::size_t n=0;n<values->size();++n)
      CHECK(std::bit_cast<std::uint32_t>(decoded[n])==std::bit_cast<std::uint32_t>((*values)[n]));
  } else CHECK(actual==expected);
}
void same_bits(const Cell& cell,const std::vector<std::uint32_t>& bits) {
  const auto& values=std::get<std::vector<float>>(cell);
  CHECK(values.size()==bits.size());
  for(std::size_t n=0;n<bits.size();++n)CHECK(std::bit_cast<std::uint32_t>(values[n])==bits[n]);
}

// The eager decoder and the constructor must independently match the oracle.
// Rejected rows are never projected, which also checks immediate full validation.
void check_row(const Schema& schema,std::string_view bytes,bool expected_valid,
               const std::vector<std::uint32_t>* expected_bits=nullptr) {
  Row eager;
  bool eager_valid=false,lazy_valid=false;
  std::string eager_error,lazy_error;
  try {eager=decode_row(schema,bytes);eager_valid=true;}
  catch(const Error& error) {CHECK(error.code()==ErrorCode::Corruption);eager_error=error.what();}
  CHECK(eager_valid==expected_valid);
  ExecutionInfo info;
  std::optional<LazyRowView> lazy;
  try {lazy.emplace(schema,bytes,info);lazy_valid=true;}
  catch(const Error& error) {CHECK(error.code()==ErrorCode::Corruption);lazy_error=error.what();}
  CHECK(lazy_valid==expected_valid);
  CHECK(info.cells_materialized==0&&info.vector_values_materialized==0);
  if(expected_valid) {
    CHECK(eager.size()==schema.columns.size());
    if(expected_bits)same_bits(eager.back(),*expected_bits);
    for(std::size_t n=schema.columns.size();n>0;--n) {
      const auto index=n-1;
      const auto& cell=(*lazy)[index];
      same_cell(cell,eager[index]);
      CHECK(&(*lazy)[index]==&cell);
    }
    CHECK(info.cells_materialized==schema.columns.size());
    CHECK(info.vector_values_materialized==std::count_if(schema.columns.begin(),schema.columns.end(),
      [](const auto& column){return column.type==ColumnType::Vector;}));
    if(expected_bits)same_bits((*lazy)[schema.columns.size()-1],*expected_bits);
  } else CHECK(lazy_error==eager_error);
  ++comparisons;
}

void bit_patterns() {
  for(const auto dimension:dimensions) {
    const auto schema=vector_schema(dimension);
    std::vector<std::uint32_t> bits(dimension,0x3f800000u);
    for(std::size_t position=0;position<dimension;++position) {
      for(auto value:special_bits) {
        bits[position]=value;
        const auto bytes=raw_row(bits,position%17);
        check_row(schema,bytes,finite_oracle(bits),&bits);
      }
      bits[position]=0x3f800000u;
    }
  }
}
void random_bits() {
  std::mt19937 random(20261110);
  for(auto dimension:dimensions) {
    const auto schema=vector_schema(dimension);
    for(unsigned trial=0;trial<256;++trial) {
      std::vector<std::uint32_t> bits(dimension);
      for(auto& value:bits) {
        do {value=static_cast<std::uint32_t>(random());}
        while(trial%2&& !std::isfinite(std::bit_cast<float>(value)));
      }
      // Both unrestricted uint32 samples and guaranteed finite samples matter:
      // a large unrestricted vector will usually be rejected before its tail.
      const auto bytes=raw_row(bits,random()%33);
      check_row(schema,bytes,finite_oracle(bits),&bits);
    }
  }
}
void unaligned_inputs() {
  for(auto dimension:dimensions) {
    const auto schema=vector_schema(dimension);
    std::vector<std::uint32_t> bits(dimension);
    for(std::size_t n=0;n<dimension;++n)bits[n]=special_bits[n%12];
    for(std::size_t padding=0;padding<16;++padding) {
      auto bytes=raw_row(bits,padding);
      for(std::size_t offset=0;offset<32;++offset) {
        // Consecutive byte offsets exercise all alignments irrespective of the
        // allocator's starting alignment; padding also moves the vector payload.
        std::string storage(bytes.size()+64,'z');
        std::memcpy(storage.data()+offset,bytes.data(),bytes.size());
        check_row(schema,std::string_view(storage.data()+offset,bytes.size()),true,&bits);
        set_le(storage,offset+21+padding+4*(dimension-1),0xff800001u);
        check_row(schema,std::string_view(storage.data()+offset,bytes.size()),false);
      }
    }
  }
}
void malformed_rows() {
  for(auto dimension:std::array<std::size_t,8>{1,7,8,9,17,33,128,768}) {
    const auto schema=vector_schema(dimension);
    const std::vector<std::uint32_t> bits(dimension,0x3f800000u);
    const auto good=raw_row(bits,11);
    for(std::size_t length=0;length<good.size();++length)
      check_row(schema,std::string_view(good).substr(0,length),false);
    check_row(schema,good,true,&bits);
    check_row(schema,good+"x",false);
    check_row(schema,good+std::string(16,'\0'),false);
    for(auto count:{0u,static_cast<std::uint32_t>(dimension-1),
                   static_cast<std::uint32_t>(dimension+1),std::numeric_limits<std::uint32_t>::max()}) {
      auto bytes=good;set_le(bytes,28,count);check_row(schema,bytes,false);
    }
    for(auto count:{0u,2u,4u,std::numeric_limits<std::uint32_t>::max()}) {
      auto bytes=good;set_le(bytes,1,count);check_row(schema,bytes,false);
    }
    for(unsigned version:{0u,2u,255u}) {
      auto bytes=good;bytes[0]=static_cast<char>(version);check_row(schema,bytes,false);
    }
    auto invalid_text=good;invalid_text[17]=char(0x80);check_row(schema,invalid_text,false);
    auto invalid_length=good;set_le(invalid_length,13,UINT32_MAX);check_row(schema,invalid_length,false);
    auto short_text_schema=schema;short_text_schema.columns[1].extent=10;
    check_row(short_text_schema,good,false);
  }
}

void guard_pages() {
  const auto page_size=sysconf(_SC_PAGESIZE);CHECK(page_size>0);
  const auto page=static_cast<std::size_t>(page_size);
  auto* base=static_cast<char*>(mmap(nullptr,3*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
  CHECK(base!=MAP_FAILED);
  struct Mapping {char* base;std::size_t bytes;~Mapping(){munmap(base,bytes);}} mapping{base,3*page};
  auto* data=base+page;CHECK(mprotect(data,page,PROT_READ|PROT_WRITE)==0);
  auto check_guarded=[&](const Schema& schema,std::string_view bytes,bool valid,bool at_end,
                         const std::vector<std::uint32_t>* bits=nullptr) {
    CHECK(bytes.size()<=page);
    auto* start=at_end?data+page-bytes.size():data;
    if(!bytes.empty())std::memcpy(start,bytes.data(),bytes.size());
    check_row(schema,std::string_view(start,bytes.size()),valid,bits);
  };
  for(auto dimension:dimensions)for(std::size_t padding=0;padding<16;++padding) {
    const auto schema=vector_schema(dimension);
    std::vector<std::uint32_t> bits(dimension,0x80000001u);
    const auto good=raw_row(bits,padding);
    for(bool at_end:{false,true}) {
      check_guarded(schema,good,true,at_end,&bits);
      for(std::size_t position=0;position<dimension;++position) {
        auto invalid=good;set_le(invalid,21+padding+4*position,0x7f800001u);
        check_guarded(schema,invalid,false,at_end);
      }
    }
  }
  // Put every truncated span directly against an inaccessible page, including
  // an empty span. This catches speculative reads past a checked byte count.
  for(auto dimension:std::array<std::size_t,4>{1,9,33,768}) {
    const auto schema=vector_schema(dimension);
    const auto good=raw_row(std::vector<std::uint32_t>(dimension,0x3f800000u),7);
    for(std::size_t length=0;length<good.size();++length)
      for(bool at_end:{false,true})check_guarded(schema,std::string_view(good).substr(0,length),false,at_end);
  }
}

struct WideRow {Schema schema;Row row;};
WideRow wide_row(std::size_t count) {
  WideRow result{{1,"wide",{},count-1},{}};
  for(std::size_t n=0;n<count;++n) {
    const auto name="column_"+std::to_string(n);
    if(n==count-1||n%3==2) {
      result.schema.columns.push_back({name,ColumnType::Bigint,0});
      result.row.emplace_back(std::int64_t(-1000)-static_cast<std::int64_t>(n));
    } else if(n==0) {
      result.schema.columns.push_back({name,ColumnType::Vector,9});
      std::vector<float> values;
      for(std::size_t j=0;j<9;++j)values.push_back(std::bit_cast<float>(special_bits[j]));
      result.row.emplace_back(std::move(values));
    } else {
      result.schema.columns.push_back({name,ColumnType::Varchar,80});
      result.row.emplace_back(std::string(30+n%10,'a')+"\xe4\xb8\xad\xf0\x9f\x98\x80"+std::string("\0z",2));
    }
  }
  validate_schema(result.schema);
  return result;
}
void wide_schemas() {
  for(auto count:std::array<std::size_t,8>{1,4,5,7,8,9,17,64}) {
    const auto fixture=wide_row(count);
    const auto bytes=encode_row(fixture.schema,fixture.row);
    check_row(fixture.schema,bytes,true);
    ExecutionInfo info;
    LazyRowView view(fixture.schema,bytes,info);
    const auto& key=view[fixture.schema.primary];
    same_cell(key,fixture.row.back());CHECK(info.cells_materialized==1);
    CHECK(info.vector_values_materialized==0);
    std::vector<const Cell*> references(count);
    for(std::size_t n=0;n<count;++n) {
      references[n]=&view[n];same_cell(view[n],fixture.row[n]);
    }
    CHECK(&view[fixture.schema.primary]==&key);
    for(std::size_t n=count;n>0;--n)CHECK(&view[n-1]==references[n-1]);
    CHECK(info.cells_materialized==count);
    CHECK(info.vector_values_materialized==(count>1?1u:0u));
    bool rejected=false;
    try {(void)view[count];}catch(const std::out_of_range&){rejected=true;}
    CHECK(rejected);CHECK(info.cells_materialized==count);
    ++comparisons;
  }
}
void copy_move_lifetimes() {
  for(auto count:std::array<std::size_t,8>{1,4,5,7,8,9,17,64}) {
    // Schema, bytes and the referenced ExecutionInfo outlive every view. Cached
    // Cells belong to their view; no reference into a destroyed source is used.
    const auto fixture=wide_row(count);
    const auto bytes=encode_row(fixture.schema,fixture.row);
    for(bool move:{false,true})for(unsigned cached:{0u,1u,2u}) {
      ExecutionInfo info;
      std::optional<LazyRowView> destination;
      {
        LazyRowView source(fixture.schema,bytes,info);
        const auto initial=cached==2?count:std::min<std::size_t>(count,cached);
        for(std::size_t n=0;n<initial;++n)same_cell(source[n],fixture.row[n]);
        const auto before=info.cells_materialized;
        if(move)destination.emplace(std::move(source));
        else destination.emplace(source);
        if(move && (detail::row_metadata_heap || count>8)) {
          bool rejected=false;
          try {(void)source[0];}catch(const std::out_of_range&){rejected=true;}
          CHECK(rejected);
        }
        CHECK(info.cells_materialized==before);
        for(std::size_t n=0;n<initial;++n) {
          same_cell((*destination)[n],fixture.row[n]);
          if(!move)CHECK(&(*destination)[n]!=&source[n]);
        }
        CHECK(info.cells_materialized==before);
      }
      std::vector<const Cell*> references(count);
      for(std::size_t n=count;n>0;--n) {
        references[n-1]=&(*destination)[n-1];
        same_cell(*references[n-1],fixture.row[n-1]);
      }
      // Copy/move retained the same info reference; cache hits did not count a
      // second materialization and uncached cells remain usable after teardown.
      CHECK(info.cells_materialized==count);
      CHECK(info.vector_values_materialized==(count>1?1u:0u));
      for(std::size_t n=0;n<count;++n)CHECK(&(*destination)[n]==references[n]);
      CHECK(info.cells_materialized==count);
      ++comparisons;
    }
    // A copied view has independent lazy state and its own owned Cell objects.
    ExecutionInfo info;
    LazyRowView original(fixture.schema,bytes,info);
    LazyRowView copy(original);
    for(std::size_t n=0;n<count;++n) {
      same_cell(copy[n],fixture.row[n]);CHECK(info.cells_materialized==2*n+1);
      same_cell(original[n],fixture.row[n]);CHECK(info.cells_materialized==2*n+2);
      CHECK(&copy[n]!=&original[n]);
    }
    CHECK(info.vector_values_materialized==(count>1?2u:0u));
    ++comparisons;
  }
}
void mode_lifetimes() {
  const auto fixture=wide_row(4);
  const auto bytes=encode_row(fixture.schema,fixture.row);
  for(bool heap:{false,true}) {
    detail::row_metadata_heap=heap;
    ExecutionInfo info;
    LazyRowView view(fixture.schema,bytes,info);
    detail::row_metadata_heap=!heap;
    for(std::size_t n=0;n<fixture.row.size();++n)same_cell(view[n],fixture.row[n]);
    CHECK(info.cells_materialized==fixture.row.size());
  }
  detail::vector_validation_mode=detail::VectorValidationMode::scalar;
  detail::row_metadata_heap=true;
  bool isolated=false;
  std::thread worker([&] {
    isolated=detail::vector_validation_mode==detail::VectorValidationMode::batched && !detail::row_metadata_heap;
  });
  worker.join();
  CHECK(isolated);
  CHECK(detail::vector_validation_mode==detail::VectorValidationMode::scalar && detail::row_metadata_heap);
}
}

int main() {
  try {
    for(auto mode:{detail::VectorValidationMode::scalar,detail::VectorValidationMode::batched})for(bool heap:{true,false}) {
      detail::vector_validation_mode=mode;
      detail::row_metadata_heap=heap;
      bit_patterns();random_bits();unaligned_inputs();malformed_rows();guard_pages();
      wide_schemas();copy_move_lifetimes();
    }
    mode_lifetimes();
    std::cout<<"PASS row validation "<<comparisons<<" comparisons\n";
  } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
