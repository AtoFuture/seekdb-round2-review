#pragma once
#include "minivectordb/row_codec.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <numeric>

namespace minivectordb::sql {
// Owned numeric columns and an optional text predicate column from one fully
// validated table version. Text slots allow four UTF-8 bytes per schema scalar.
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
    bool transposed_=false;
    std::optional<std::size_t> text_column_;
    std::size_t text_width_=0;
    std::unique_ptr<char[]> texts_;
    std::unique_ptr<std::uint32_t[]> text_lengths_;

    void release() noexcept {
        vectors_.reset();scalars_.reset();columns_.reset();texts_.reset();text_lengths_.reset();
        capacity_=count_=bytes_=0;transposed_=false;
    }
    void refuse() noexcept { release();state_=State::refused; }
    void cluster() {
        // Bounded preparation only: <=64 KiB row permutation and <=64 KiB
        // single-row scratch. Allocate everything before changing any payload.
        if(count_<2 || count_>16384)return;
        if(dimension_*sizeof(float)+scalar_count_*sizeof(std::int64_t)+text_width_>65536)return;
        std::size_t slot=0;
        while(slot<scalar_count_ && slot==primary_slot_)++slot;
        if(!text_column_ && slot==scalar_count_)return;
        std::vector<std::uint32_t> order(count_);
        std::iota(order.begin(),order.end(),0U);
        std::vector<float> vector(dimension_);
        std::vector<std::int64_t> scalars(scalar_count_);
        std::vector<char> text_copy(text_width_);
        std::sort(order.begin(),order.end(),[&](auto a,auto b) {
            int cmp;
            if(text_column_)cmp=text(a).compare(text(b));
            else {const auto x=scalar(a,slot),y=scalar(b,slot);cmp=x<y?-1:(x>y?1:0);}
            return cmp<0 || (cmp==0 && key(a)<key(b));
        });
        const auto move_row=[&](std::size_t to,std::size_t from) {
            std::memcpy(vectors_.get()+to*dimension_,vectors_.get()+from*dimension_,dimension_*sizeof(float));
            std::memcpy(scalars_.get()+to*scalar_count_,scalars_.get()+from*scalar_count_,scalar_count_*sizeof(std::int64_t));
            if(text_column_) {
                text_lengths_[to]=text_lengths_[from];
                if(text_lengths_[to])std::memcpy(texts_.get()+to*text_width_,texts_.get()+from*text_width_,text_lengths_[to]);
            }
        };
        for(std::size_t i=0;i<count_;++i)if(order[i]!=i) {
            std::memcpy(vector.data(),vectors_.get()+i*dimension_,dimension_*sizeof(float));
            std::memcpy(scalars.data(),scalars_.get()+i*scalar_count_,scalar_count_*sizeof(std::int64_t));
            const auto length=text_column_?text_lengths_[i]:0;
            if(length)std::memcpy(text_copy.data(),texts_.get()+i*text_width_,length);
            auto row=i;
            while(order[row]!=i) {
                const auto next=order[row];move_row(row,next);
                order[row]=static_cast<std::uint32_t>(row);row=next;
            }
            std::memcpy(vectors_.get()+row*dimension_,vector.data(),dimension_*sizeof(float));
            std::memcpy(scalars_.get()+row*scalar_count_,scalars.data(),scalar_count_*sizeof(std::int64_t));
            if(text_column_) {
                text_lengths_[row]=length;
                if(length)std::memcpy(texts_.get()+row*text_width_,text_copy.data(),length);
            }
            order[row]=static_cast<std::uint32_t>(row);
        }
    }
public:
    void invalidate() noexcept {count_=0;table_=index_=0;state_=State::empty;transposed_=false;}
    void clear() noexcept {release();invalidate();budget_=dimension_=scalar_count_=text_width_=0;text_column_.reset();}
    bool building() const noexcept {return state_==State::building;}
    bool refused() const noexcept {return state_==State::refused;}
    bool ready(std::uint64_t table,std::uint64_t index,std::size_t column) const noexcept {
        return state_==State::ready && table_==table && index_==index && column_==column;
    }
    void start(const Schema& schema,std::uint64_t index,std::size_t column,
               std::size_t budget,bool reuse=true,std::optional<std::size_t> text_column={}) {
        budget=std::min(budget,std::size_t(8*1024*1024));
        const std::size_t dimension=schema.columns.at(column).extent;
        std::size_t scalar_count=0;
        for(const auto& field:schema.columns)if(field.type==ColumnType::Bigint)++scalar_count;
        const auto text_width=text_column?std::size_t(schema.columns.at(*text_column).extent)*4:0;
        if(budget_!=budget || dimension_!=dimension || scalar_count_!=scalar_count ||
           text_column_!=text_column || text_width_!=text_width)clear();
        if(state_==State::empty || table_!=schema.id || index_!=index || column_!=column) {
            if(reuse)invalidate();else clear();
            table_=schema.id;index_=index;column_=column;
            dimension_=dimension;scalar_count_=scalar_count;budget_=budget;
            text_column_=text_column;text_width_=text_width;
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
        auto row_bytes=vector_bytes+scalar_count*sizeof(std::int64_t);
        if(text_column_) {
            if(text_width_>budget-metadata-row_bytes ||
               sizeof(std::uint32_t)>budget-metadata-row_bytes-text_width_) {refuse();return;}
            row_bytes+=text_width_+sizeof(std::uint32_t);
        }
        const auto capacity=(budget-metadata)/row_bytes;
        if(!capacity) {refuse();return;}
        if(!vectors_) {
            try {
                vectors_.reset(new float[capacity*dimension]);
                scalars_.reset(new std::int64_t[capacity*scalar_count]);
                columns_.reset(new std::size_t[scalar_count]);
                if(text_column_) {
                    texts_.reset(new char[capacity*text_width_]);
                    text_lengths_.reset(new std::uint32_t[capacity]);
                }
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
    template<class ScalarGetter> void append(std::span<const float> vector,ScalarGetter&& scalar,
                                             std::string_view text={}) {
        if(!building())return;
        if(count_==capacity_) {refuse();return;}
        if(vector.size()!=dimension_)throw Error(ErrorCode::Corruption,"cached VECTOR dimension mismatch");
        if(text_column_) {
            if(text.size()>text_width_)throw Error(ErrorCode::Corruption,"cached VARCHAR length mismatch");
            if(!text.empty())std::memcpy(texts_.get()+count_*text_width_,text.data(),text.size());
            text_lengths_[count_]=static_cast<std::uint32_t>(text.size());
        }
        std::memcpy(vectors_.get()+count_*dimension_,vector.data(),dimension_*sizeof(float));
        for(std::size_t slot=0;slot<scalar_count_;++slot)
            scalars_[count_*scalar_count_+slot]=scalar(columns_[slot]);
        ++count_;
    }
    void finish(bool transpose=false,bool clustered=false) noexcept {
        if(!building())return;
        if(clustered)try {cluster();}catch(const std::bad_alloc&) {}
        // Only complete blocks are transposed; the final short block remains
        // AoS, so padding never grows the arena or reduces cache admission.
        if(transpose && dimension_>=16 && dimension_<=1024 && count_>=16) {
            try {
                auto scratch=std::make_unique<float[]>(16*dimension_);
                for(std::size_t row=0;row+16<=count_;row+=16) {
                    auto* block=vectors_.get()+row*dimension_;
                    for(std::size_t lane=0;lane<16;++lane)
                        for(std::size_t d=0;d<dimension_;++d)
                            scratch[d*16+lane]=block[lane*dimension_+d];
                    std::memcpy(block,scratch.get(),16*dimension_*sizeof(float));
                }
                transposed_=true;
            } catch(const std::bad_alloc&) {} // No block changed before allocation.
        }
        state_=State::ready;
    }
    bool transposed() const noexcept {return transposed_;}
    std::optional<std::size_t> text_column(std::uint64_t table,std::uint64_t index,std::size_t column) const noexcept {
        return table_==table && index_==index && column_==column?text_column_:std::nullopt;
    }
    std::string_view text(std::size_t row) const noexcept {
        return {texts_.get()+row*text_width_,text_lengths_[row]};
    }
    std::span<const float> block(std::size_t row) const noexcept {
        return {vectors_.get()+row*dimension_,16*dimension_};
    }
    std::span<const float> read_vector(std::size_t row,std::vector<float>& scratch) const {
        if(!transposed_ || row>=count_/16*16)return vector(row);
        scratch.resize(dimension_);
        const auto* values=vectors_.get()+(row/16*16)*dimension_;
        for(std::size_t d=0;d<dimension_;++d)scratch[d]=values[d*16+row%16];
        return scratch;
    }
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
