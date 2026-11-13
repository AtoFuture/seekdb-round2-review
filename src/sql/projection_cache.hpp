#pragma once
#include "minivectordb/row_codec.hpp"
#include "../storage/binary.hpp"
#include "../storage/packed_codec.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <memory>
#include <optional>

namespace minivectordb::sql {
// A complete immutable projection of validated rows, keyed independently from
// the clustered vector layout. It owns no vector payload. Raw strings retain
// their exact bytes; optional RLE is lossless and independent of SQL contents.
// Every non-SELECT invalidates the cache before the statement can run.
class ProjectionCache {
  struct Entry {std::int64_t key;std::uint32_t offset,length;};
  enum class State {empty,probation,building,ready,refused} state_=State::empty;
  std::uint64_t table_=0,index_=0;
  std::size_t budget_=0,expected_rows_=0,expected_bytes_=0,count_=0,used_=0;
  bool complete_=false;
  std::unique_ptr<Entry[]> entries_;
  std::unique_ptr<char[]> data_;
  void refuse() noexcept {entries_.reset();data_.reset();count_=used_=0;state_=State::refused;}
public:
  void clear() noexcept {entries_.reset();data_.reset();state_=State::empty;table_=index_=0;budget_=expected_rows_=expected_bytes_=count_=used_=0;complete_=false;}
  bool ready(std::uint64_t table,std::uint64_t index) const noexcept {return state_==State::ready&&table_==table&&index_==index;}
  std::size_t bytes() const noexcept {return entries_?expected_rows_*sizeof(Entry)+expected_bytes_:0;}
  std::size_t planned_bytes() const noexcept {return bytes()?bytes():state_==State::probation&&complete_?expected_rows_*sizeof(Entry)+expected_bytes_:0;}
  void start(std::uint64_t table,std::uint64_t index,std::size_t budget,bool allocate=true) {
    budget=std::min(budget,std::size_t(8*1024*1024));
    if(table_!=table||index_!=index||budget_!=budget)clear();
    if(state_==State::empty){table_=table;index_=index;budget_=budget;state_=State::probation;return;}
    if(state_!=State::probation)return;
    if(!complete_||!budget||!expected_rows_||expected_rows_>budget/sizeof(Entry)||expected_bytes_>budget-expected_rows_*sizeof(Entry)){refuse();return;}
    if(!allocate)return; // The vector cache must pass admission first.
    try {entries_=std::make_unique<Entry[]>(std::max(std::size_t(1),expected_rows_));data_=std::make_unique<char[]>(std::max(std::size_t(1),expected_bytes_));}
    catch(const std::bad_alloc&){refuse();return;}
    count_=used_=0;state_=State::building;
  }
  void append(const Schema& schema,std::int64_t key,std::string_view validated) {
    if(state_!=State::probation&&state_!=State::building)return;
    // An already completed sizing scan belongs to this immutable table/index.
    // Another vector-cache probation (for example when a VARCHAR predicate is
    // first seen) must not count every projection row a second time.
    if(state_==State::probation&&complete_)return;
    // Bound scratch before copying even when a valid row has very large text.
    binary::Reader scan(validated);if(scan.get(1)!=1||scan.get(4)!=schema.columns.size())throw Error(ErrorCode::Corruption,"projection row header");
    std::size_t size=0;
    for(const auto& col:schema.columns) {
      if(col.type==ColumnType::Vector){auto n=scan.get(4);if(n!=col.extent||n>scan.remaining()/4)throw Error(ErrorCode::Corruption,"projection vector length");scan.take(std::size_t(n)*4);}
      else if(col.type==ColumnType::Bigint){scan.take(8);size+=8;}
      else {auto text=scan.text_view();if(text.size()>65536-size||size+text.size()>65532){refuse();return;}size+=4+text.size();}
      if(size>65536){refuse();return;}
    }
    scan.end();std::string raw;raw.reserve(size);binary::Reader in(validated);in.take(5);
    for(const auto& col:schema.columns) {
      if(col.type==ColumnType::Vector){auto n=in.get(4);in.take(std::size_t(n)*4);}
      else if(col.type==ColumnType::Bigint)raw.append(in.take(8));
      else binary::text(raw,in.text_view());
    }
    std::string packed;const bool compressed=detail::pack_bytes(raw,packed);const std::string_view value=compressed?packed:raw;
    if(state_==State::probation) {
      if(expected_rows_>=budget_/sizeof(Entry)||value.size()>budget_-expected_rows_*sizeof(Entry)||
          expected_bytes_>budget_-expected_rows_*sizeof(Entry)-value.size()||
          sizeof(Entry)>budget_-expected_rows_*sizeof(Entry)-value.size()-expected_bytes_){refuse();return;}
      ++expected_rows_;expected_bytes_+=value.size();return;
    }
    if(count_>=expected_rows_||value.size()>expected_bytes_-used_){refuse();return;}
    entries_[count_++]={key,std::uint32_t(used_),std::uint32_t(value.size())|(compressed?0x80000000U:0)};
    if(!value.empty())std::memcpy(data_.get()+used_,value.data(),value.size());used_+=value.size();
  }
  void finish() {
    if(state_==State::probation){complete_=true;return;}
    if(state_!=State::building)return;
    if(count_!=expected_rows_||used_!=expected_bytes_){refuse();return;}
    std::sort(entries_.get(),entries_.get()+count_,[](auto a,auto b){return a.key<b.key;});
    for(std::size_t i=1;i<count_;++i)if(entries_[i-1].key==entries_[i].key){clear();throw Error(ErrorCode::Corruption,"duplicate projection key");}
    state_=State::ready;
  }
  std::optional<std::string> get(std::int64_t key) const {
    if(state_!=State::ready)return {};
    const auto end=entries_.get()+count_;
    const auto at=std::lower_bound(entries_.get(),end,key,[](auto row,auto k){return row.key<k;});
    if(at==end||at->key!=key)return {};
    const auto value=std::string_view(data_.get()+at->offset,at->length&0x7fffffffU);
    return at->length&0x80000000U?detail::unpack_bytes(value):std::string(value);
  }
};
}
