#include "minivectordb/row_codec.hpp"
#include "minivectordb/value.hpp"
#include "binary.hpp"
#include "row_cpu.hpp"
#include <bit>
#include "minivectordb/profile.hpp"
#include <cmath>
#include <cstring>
#include <set>
namespace minivectordb {
  namespace {
    void validate_vector_bytes(std::string_view bytes) {
      // The caller checked the dimension and took exactly count*4 bytes. Each
      // word is loaded with memcpy: row payloads need not be naturally aligned.
      // Test each exponent separately before reducing; OR-ing exponents first
      // would incorrectly reject some combinations of finite values.
      constexpr std::uint32_t exponent=std::endian::native==std::endian::little?0x7f800000u:0x0000807fu;
      std::uint32_t invalid=0;
      for(std::size_t i=0;i<bytes.size();i+=4) {
        std::uint32_t bits;
        std::memcpy(&bits,bytes.data()+i,sizeof(bits));
        invalid|=static_cast<std::uint32_t>((bits&exponent)==exponent);
      }
      if(invalid)throw Error(ErrorCode::Corruption,"non-finite stored VECTOR");
    }
  }
  void validate_schema(const Schema&s) {
    if(s.name.empty()||s.columns.empty()||s.columns.size()>UINT32_MAX||s.primary>=s.columns.size()||s.columns[s.primary].type!=ColumnType::Bigint)throw Error(ErrorCode::Schema,"table requires exactly one BIGINT primary key");
    std::set<std::string> names;
    unsigned vectors=0;
    for(const auto&c:s.columns) {
      if(c.name.empty()||!names.insert(c.name).second)throw Error(ErrorCode::Schema,"duplicate or empty column name");
      if(c.type==ColumnType::Vector)++vectors;
      if((c.type==ColumnType::Vector||c.type==ColumnType::Varchar)&&!c.extent)throw Error(ErrorCode::Schema,"type extent must be positive");
      if(c.type!=ColumnType::Bigint&&c.type!=ColumnType::Varchar&&c.type!=ColumnType::Vector)throw Error(ErrorCode::Schema,"unknown column type");
    }
    if(vectors>1)throw Error(ErrorCode::Schema,"at most one VECTOR column supported");
  }
  std::string encode_row(const Schema&s,const Row&r) {
    if(r.size()!=s.columns.size())throw Error(ErrorCode::Type,"row column count mismatch");
    std::string out;
    std::size_t bytes=5;
    auto add_bytes=[&](std::size_t count) {
      if(count>out.max_size()-bytes)throw std::length_error("encoded row exceeds string capacity");
      bytes+=count;
    };
    // Validate in the original column/component order before reserving storage.
    // The encoding pass only consumes this already validated, immutable row.
    for(std::size_t i=0;i<r.size();++i) {
      const auto&c=s.columns[i];
      const auto&v=r[i];
      try {
        switch(c.type) {
          case ColumnType::Bigint:if(!std::holds_alternative<std::int64_t>(v))throw Error(ErrorCode::Type,"expected BIGINT");
          add_bytes(8);
          break;
          case ColumnType::Varchar:if(!std::holds_alternative<std::string>(v))throw Error(ErrorCode::Type,"expected VARCHAR");
          validate_text(std::get<std::string>(v),c.extent);
          if(std::get<std::string>(v).size()>UINT32_MAX)throw Error(ErrorCode::Type,"value too long");
          add_bytes(4);
          add_bytes(std::get<std::string>(v).size());
          break;
          case ColumnType::Vector: {
            if(!std::holds_alternative<std::vector<float>>(v))throw Error(ErrorCode::Type,"expected VECTOR");
            const auto&a=std::get<std::vector<float>>(v);
            if(a.size()!=c.extent)throw Error(ErrorCode::Type,"VECTOR dimension mismatch");
            for(float f:a) {
              if(!std::isfinite(f))throw Error(ErrorCode::Type,"non-finite VECTOR component");
            }
            add_bytes(4);
            if(a.size()>(out.max_size()-bytes)/sizeof(float))throw std::length_error("encoded row exceeds string capacity");
            add_bytes(a.size()*sizeof(float));
            break;
          }
        }
      }
      catch(const std::invalid_argument&e) {
        throw Error(ErrorCode::Type,e.what());
      }
    }
    out.reserve(bytes);
    binary::put(out,1,1);
    binary::put(out,r.size(),4);
    for(std::size_t i=0;i<r.size();++i) {
      const auto&v=r[i];
      switch(s.columns[i].type) {
        case ColumnType::Bigint:
          binary::put(out,std::bit_cast<std::uint64_t>(std::get<std::int64_t>(v)));
          break;
        case ColumnType::Varchar:
          binary::text(out,std::get<std::string>(v));
          break;
        case ColumnType::Vector: {
          const auto&a=std::get<std::vector<float>>(v);
          binary::put(out,a.size(),4);
          if constexpr(std::endian::native==std::endian::little) {
            static_assert(sizeof(float)==4);
            if(!a.empty())out.append(reinterpret_cast<const char*>(a.data()),a.size()*sizeof(float));
          } else {
            for(float f:a)binary::put(out,std::bit_cast<std::uint32_t>(f),4);
          }
          break;
        }
      }
    }
    return out;
  }
  Row decode_row(const Schema&s,std::string_view bytes) {
    profile::detail::Scope timer(&profile::Snapshot::row_decode);
    binary::Reader in(bytes);
    if(in.get(1)!=1||in.get(4)!=s.columns.size())throw Error(ErrorCode::Corruption,"row version or column count mismatch");
    Row r;
    r.reserve(s.columns.size());
    for(const auto&c:s.columns) {
      switch(c.type) {
        case ColumnType::Bigint:r.emplace_back(std::bit_cast<std::int64_t>(in.get()));
        break;
        case ColumnType::Varchar: {
          auto text=in.text();
          try {
            validate_text(text,c.extent);
          }
          catch(const std::invalid_argument&e) {
            throw Error(ErrorCode::Corruption,e.what());
          }
          r.emplace_back(std::move(text));
          break;
        }
        case ColumnType::Vector: {
          auto n=in.get(4);
          if(n!=c.extent||n>in.remaining()/4)throw Error(ErrorCode::Corruption,"invalid VECTOR length");
          std::vector<float> v;
          v.reserve(static_cast<std::size_t>(n));
          for(std::uint64_t i=0;i<n;++i) {
            float f=std::bit_cast<float>(static_cast<std::uint32_t>(in.get(4)));
            if(!std::isfinite(f))throw Error(ErrorCode::Corruption,"non-finite stored VECTOR");
            v.push_back(f);
          }
          r.emplace_back(std::move(v));
          break;
        }
      }
    }
    in.end();
    return r;
  }
  LazyRowView::LazyRowView(const Schema& schema, std::string_view bytes, ExecutionInfo& info)
      : schema_(schema), bytes_(bytes),
        inline_metadata_(schema.columns.size()<=inline_capacity && !detail::row_metadata_heap),
        cells_(inline_metadata_?0:schema.columns.size()), info_(info) {
    profile::detail::Scope timer(&profile::Snapshot::row_validation);
    binary::Reader in(bytes);
    if(in.get(1)!=1 || in.get(4)!=schema.columns.size())
      throw Error(ErrorCode::Corruption,"row version or column count mismatch");
    if(!inline_metadata_)offsets_.reserve(schema.columns.size());
    std::size_t column_index=0;
    for(const auto& column:schema.columns) {
      const auto offset=bytes.size()-in.remaining();
      if(inline_metadata_)inline_offsets_[column_index]=offset;
      else offsets_.push_back(offset);
      ++column_index;
      switch(column.type) {
        case ColumnType::Bigint: (void)in.get(); break;
        case ColumnType::Varchar:
          try { validate_text(in.text_view(),column.extent); }
          catch(const std::invalid_argument& error) { throw Error(ErrorCode::Corruption,error.what()); }
          break;
        case ColumnType::Vector: {
          auto count=in.get(4);
          if(count!=column.extent || count>in.remaining()/4)
            throw Error(ErrorCode::Corruption,"invalid VECTOR length");
          bool batched=detail::vector_validation_mode==detail::VectorValidationMode::batched;
          if constexpr(std::endian::native!=std::endian::little && std::endian::native!=std::endian::big)
            batched=false;
          if(batched)validate_vector_bytes(in.take(static_cast<std::size_t>(count)*4));
          else {
            for(std::uint64_t i=0;i<count;++i)
              if(!std::isfinite(std::bit_cast<float>(static_cast<std::uint32_t>(in.get(4)))))
                throw Error(ErrorCode::Corruption,"non-finite stored VECTOR");
          }
          break;
        }
      }
    }
    in.end();
  }
  void LazyRowView::copy_vector(std::size_t index,std::vector<float>& scratch) const {
    profile::detail::Scope timer(&profile::Snapshot::vector_scratch_copy);
    if(index>=schema_.columns.size())throw std::out_of_range("row column index");
    if(schema_.columns[index].type!=ColumnType::Vector)
      throw Error(ErrorCode::Type,"expected VECTOR column");
    const auto offset=inline_metadata_?inline_offsets_[index]:offsets_.at(index);
    binary::Reader in(bytes_.substr(offset));
    const auto count=in.get(4);
    if(count!=schema_.columns[index].extent || count>in.remaining()/sizeof(float))
      throw Error(ErrorCode::Corruption,"invalid VECTOR length");
    const auto payload=in.take(static_cast<std::size_t>(count)*sizeof(float));
    // With an unchanged dimension, resize preserves the existing allocation and values.
    scratch.resize(static_cast<std::size_t>(count));
    if constexpr(std::endian::native==std::endian::little) {
      if(!scratch.empty())std::memcpy(scratch.data(),payload.data(),payload.size());
    } else {
      binary::Reader components(payload);
      for(auto& value:scratch)
        value=std::bit_cast<float>(static_cast<std::uint32_t>(components.get(4)));
    }
    timer.bytes(payload.size());
  }
  const Cell& LazyRowView::operator[](std::size_t index) const {
    if(index>=schema_.columns.size())throw std::out_of_range("row column index");
    auto& cell=inline_metadata_?inline_cells_[index]:cells_.at(index);
    if(!cell) {
      profile::detail::Scope timer(&profile::Snapshot::row_materialization);
      binary::Reader in(bytes_.substr(inline_metadata_?inline_offsets_[index]:offsets_[index]));
      switch(schema_.columns[index].type) {
        case ColumnType::Bigint: cell.emplace(std::bit_cast<std::int64_t>(in.get())); break;
        case ColumnType::Varchar: cell.emplace(in.text()); break;
        case ColumnType::Vector: {
          auto count=in.get(4);
          if(count!=schema_.columns[index].extent || count>in.remaining()/sizeof(float))
            throw Error(ErrorCode::Corruption,"invalid VECTOR length");
          std::vector<float> values(static_cast<std::size_t>(count));
          if constexpr(std::endian::native==std::endian::little) {
            // The constructor has already checked every component for finiteness.
            // Copy validated float32 bits without repeating the per-component decode.
            if(!values.empty())
              std::memcpy(values.data(),bytes_.data()+bytes_.size()-in.remaining(),values.size()*sizeof(float));
          } else {
            for(auto& value:values)
              value=std::bit_cast<float>(static_cast<std::uint32_t>(in.get(4)));
          }
          cell.emplace(std::move(values));
          ++info_.vector_values_materialized;
          break;
        }
      }
      ++info_.cells_materialized;
    }
    return *cell;
  }

}
