#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string_view>

namespace minivectordb::sql {
// Owns one complete, validated base-table scan. An index ID additionally records
// verified index/base consistency; index zero identifies an unindexed scan.
// Executor invalidates before every non-SELECT, so no database root or schema can
// change while the snapshot is reused. The executor can reuse these admission
// proofs on replay; structural bounds are still checked and winners are refetched.
// Invalidation may retain the arena, never old rows or their validation proof.
// A failed or oversized builder is never readable. Budget includes record headers.
class ScanSnapshot {
    enum class State { empty, probation, building, ready, refused } state_ = State::empty;
    std::uint64_t table_ = 0, index_ = 0;
    std::size_t capacity_ = 0, used_ = 0;
    std::unique_ptr<char[]> data_;
public:
    void invalidate() noexcept { used_=0; table_=index_=0; state_=State::empty; }
    void clear() noexcept { data_.reset(); capacity_=0; invalidate(); }
    bool building() const noexcept { return state_==State::building; }
    // Keep one representation for this table/index until invalidation. A raw
    // probation/refusal is a query-shape decision, not permission to read rows.
    bool observed(std::uint64_t table,std::uint64_t index) const noexcept {
        return state_!=State::empty && table_==table && index_==index;
    }
    bool ready(std::uint64_t table,std::uint64_t index) const noexcept {
        return state_==State::ready && table_==table && index_==index;
    }
    void start(std::uint64_t table,std::uint64_t index,std::size_t budget,bool reuse_buffer=true) {
        budget=std::min(budget,std::size_t(8*1024*1024));
        // Release before allocating a different size; at most one arena is owned.
        if(capacity_ && capacity_!=budget)clear();
        if(state_==State::empty || table_!=table || index_!=index) {
            if(reuse_buffer)invalidate();else clear();
            table_=table;index_=index;state_=State::probation;
            return; // The first eligible scan never allocates a new arena.
        }
        if(state_!=State::probation)return;
        if(!budget) {state_=State::refused;return;}
        if(!data_) {
            try { data_.reset(new char[budget]); }
            catch(const std::bad_alloc&) {state_=State::refused;return;}
            capacity_=budget;
        }
        used_=0;state_=State::building;
    }
    void append(std::int64_t key,std::string_view bytes) {
        if(state_!=State::building)return;
        constexpr std::size_t header=sizeof(key)+sizeof(std::uint64_t);
        if(capacity_-used_<header || bytes.size()>capacity_-used_-header) {
            data_.reset();capacity_=used_=0;state_=State::refused;return;
        }
        auto size=static_cast<std::uint64_t>(bytes.size());
        std::memcpy(data_.get()+used_,&key,sizeof(key));used_+=sizeof(key);
        std::memcpy(data_.get()+used_,&size,sizeof(size));used_+=sizeof(size);
        std::memcpy(data_.get()+used_,bytes.data(),bytes.size());used_+=bytes.size();
    }
    void finish() noexcept {if(state_==State::building)state_=State::ready;}
    std::size_t bytes() const noexcept {return capacity_;}
    template<class F> void scan(F&& visit) const {
        std::size_t offset=0;
        while(offset<used_) {
            std::int64_t key;std::uint64_t size;
            std::memcpy(&key,data_.get()+offset,sizeof(key));offset+=sizeof(key);
            std::memcpy(&size,data_.get()+offset,sizeof(size));offset+=sizeof(size);
            visit(key,std::string_view(data_.get()+offset,static_cast<std::size_t>(size)));
            offset+=static_cast<std::size_t>(size);
        }
    }
};
}
