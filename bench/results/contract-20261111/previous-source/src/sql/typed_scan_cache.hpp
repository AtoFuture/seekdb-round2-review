#pragma once
#include "minivectordb/row_codec.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>

namespace minivectordb::sql {
// Owned, aligned numeric columns from one completely validated table version.
// The executor invalidates before every non-SELECT and never retains the raw
// snapshot beside this cache. Only finish() publishes a successful full scan.
// All owned arrays, including the scalar-column map, share the bounded budget.
class TypedScanCache {
    enum class State { empty, probation, building, ready, refused } state_=State::empty;
    std::uint64_t table_=0,index_=0;
    std::size_t column_=0,dimension_=0,scalar_count_=0,primary_slot_=0;
    std::size_t budget_=0,capacity_=0,count_=0,bytes_=0;
    std::unique_ptr<float[]> vectors_;
    std::unique_ptr<std::int64_t[]> scalars_;
    std::unique_ptr<std::size_t[]> columns_;

    void release() noexcept {
        vectors_.reset();scalars_.reset();columns_.reset();
        capacity_=count_=bytes_=0;
    }
    void refuse() noexcept { release();state_=State::refused; }
public:
    void invalidate() noexcept {count_=0;table_=index_=0;state_=State::empty;}
    void clear() noexcept {release();invalidate();budget_=dimension_=scalar_count_=0;}
    bool building() const noexcept {return state_==State::building;}
    bool ready(std::uint64_t table,std::uint64_t index,std::size_t column) const noexcept {
        return state_==State::ready && table_==table && index_==index && column_==column;
    }
    void start(const Schema& schema,std::uint64_t index,std::size_t column,
               std::size_t budget,bool reuse=true) {
        budget=std::min(budget,std::size_t(8*1024*1024));
        const std::size_t dimension=schema.columns.at(column).extent;
        std::size_t scalar_count=0;
        for(const auto& field:schema.columns)if(field.type==ColumnType::Bigint)++scalar_count;
        if(budget_!=budget || dimension_!=dimension || scalar_count_!=scalar_count)clear();
        if(state_==State::empty || table_!=schema.id || index_!=index || column_!=column) {
            if(reuse)invalidate();else clear();
            table_=schema.id;index_=index;column_=column;
            dimension_=dimension;scalar_count_=scalar_count;budget_=budget;
            state_=State::probation;return;
        }
        if(state_!=State::probation)return;
        // Check using division/subtraction before multiplication; a huge schema
        // or vector dimension must refuse caching, not wrap the allocation size.
        if(!dimension || !scalar_count || scalar_count>budget/sizeof(std::size_t)) {refuse();return;}
        const auto metadata=scalar_count*sizeof(std::size_t);
        if(dimension>(budget-metadata)/sizeof(float)) {refuse();return;}
        const auto vector_bytes=dimension*sizeof(float);
        if(scalar_count>(budget-metadata-vector_bytes)/sizeof(std::int64_t)) {refuse();return;}
        const auto row_bytes=vector_bytes+scalar_count*sizeof(std::int64_t);
        const auto capacity=(budget-metadata)/row_bytes;
        if(!capacity) {refuse();return;}
        if(!vectors_) {
            try {
                vectors_.reset(new float[capacity*dimension]);
                scalars_.reset(new std::int64_t[capacity*scalar_count]);
                columns_.reset(new std::size_t[scalar_count]);
            } catch(const std::bad_alloc&) {refuse();return;}
            capacity_=capacity;bytes_=metadata+capacity*row_bytes;
        }
        std::size_t slot=0;
        for(std::size_t c=0;c<schema.columns.size();++c)if(schema.columns[c].type==ColumnType::Bigint) {
            columns_[slot]=c;
            if(c==schema.primary)primary_slot_=slot;
            ++slot;
        }
        count_=0;state_=State::building;
    }
    template<class ScalarGetter> void append(std::span<const float> vector,ScalarGetter&& scalar) {
        if(!building())return;
        if(count_==capacity_) {refuse();return;}
        if(vector.size()!=dimension_)throw Error(ErrorCode::Corruption,"cached VECTOR dimension mismatch");
        std::memcpy(vectors_.get()+count_*dimension_,vector.data(),dimension_*sizeof(float));
        for(std::size_t slot=0;slot<scalar_count_;++slot)
            scalars_[count_*scalar_count_+slot]=scalar(columns_[slot]);
        ++count_;
    }
    void finish() noexcept {if(building())state_=State::ready;}
    std::size_t size() const noexcept {return count_;}
    std::size_t bytes() const noexcept {return bytes_;}
    std::size_t scalar_slot(std::size_t column) const {
        for(std::size_t slot=0;slot<scalar_count_;++slot)if(columns_[slot]==column)return slot;
        throw Error(ErrorCode::Schema,"typed cache requires BIGINT predicate");
    }
    std::int64_t scalar(std::size_t row,std::size_t slot) const noexcept {
        return scalars_[row*scalar_count_+slot];
    }
    std::int64_t key(std::size_t row) const noexcept {return scalar(row,primary_slot_);}
    std::span<const float> vector(std::size_t row) const noexcept {
        return {vectors_.get()+row*dimension_,dimension_};
    }
};
}
