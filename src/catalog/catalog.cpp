#include "catalog.hpp"
#include "../storage/binary.hpp"
#include <optional>
namespace minivectordb {
  namespace {
    std::string encode(const Schema&s) {
      std::string out;
      binary::put(out,1,1);
      binary::text(out,s.name);
      binary::put(out,s.primary,4);
      binary::put(out,s.columns.size(),4);
      for(const auto&c:s.columns) {
        binary::text(out,c.name);
        binary::put(out,static_cast<unsigned>(c.type),1);
        binary::put(out,c.extent,4);
      }
      return out;
    }
    Schema decode(std::int64_t id,std::string_view bytes) {
      binary::Reader in(bytes);
      if(in.get(1)!=1)throw Error(ErrorCode::Corruption,"unsupported catalog version");
      Schema s;
      s.id=static_cast<std::uint64_t>(id);
      s.name=in.text();
      s.primary=static_cast<std::size_t>(in.get(4));
      auto n=in.get(4);
      if(n>in.remaining()/9)throw Error(ErrorCode::Corruption,"invalid catalog column count");
      for(std::uint64_t i=0;i<n;++i) {
        Column c;
        c.name=in.text();
        c.type=static_cast<ColumnType>(in.get(1));
        c.extent=static_cast<std::uint32_t>(in.get(4));
        s.columns.push_back(std::move(c));
      }
      in.end();
      try {
        validate_schema(s);
      }
      catch(const Error&e) {
        throw Error(ErrorCode::Corruption,e.what());
      }
      return s;
    }
    std::optional<Schema> find(const Store&store,std::string_view name) {
      std::optional<Schema> found;
      store.scan([&](auto id,auto bytes) {
        if(id>0) {
          auto s=decode(id,bytes);
          if(s.name==name) {
            if(found)throw Error(ErrorCode::Corruption,"duplicate catalog name");
            found=std::move(s);
          }
        }
      });
      return found;
    }
  }
  // Positive catalog keys remain table records. Negative keys describe vector
  // namespaces, allocated from the same counter, so legacy format02/03 tables
  // and future tables cannot collide with index entries.
  std::vector<VectorIndex> vector_indexes(const Store& store,std::uint64_t table) {
    std::vector<VectorIndex> out;
    store.scan([&](auto key,auto bytes) {
      if(key>=0)return;
      if(key==INT64_MIN)throw Error(ErrorCode::Corruption,"invalid vector index identifier");
      binary::Reader in(bytes);
      const auto version=in.get(1);
      if(version!=1&&version!=2)throw Error(ErrorCode::Corruption,"unsupported vector index version");
      VectorIndex index{static_cast<std::uint64_t>(-key),in.get(),static_cast<std::size_t>(in.get(4)),in.text(),version==2};
      in.end();
      if(index.table==table)out.push_back(std::move(index));
    });
    return out;
  }
  VectorIndex create_vector_index(Store& store,const Schema& schema,std::size_t column,std::string_view name,bool referenced) {
    if(column>=schema.columns.size()||schema.columns[column].type!=ColumnType::Vector)
      throw Error(ErrorCode::Schema,"vector index requires VECTOR column");
    for(const auto& index:vector_indexes(store,schema.id))
      if(index.name==name)throw Error(ErrorCode::Schema,"index already exists: "+std::string(name));
    auto raw=store.get(0);
    if(!raw)throw Error(ErrorCode::Corruption,"missing catalog counter");
    binary::Reader in(*raw);
    if(in.get(1)!=1)throw Error(ErrorCode::Corruption,"invalid catalog counter version");
    auto next=in.get(); in.end();
    if(next==0||next>INT64_MAX)throw Error(ErrorCode::Constraint,"namespace identifier exhausted");
    std::string bytes; binary::put(bytes,referenced?2:1,1); binary::put(bytes,schema.id);
    binary::put(bytes,column,4); binary::text(bytes,name);
    store.put(-static_cast<std::int64_t>(next),bytes);
    std::string counter; binary::put(counter,1,1); binary::put(counter,next+1); store.put(0,counter);
    return {next,schema.id,column,std::string(name),referenced};
  }
  Schema lookup_table(const Store&store,std::string_view name) {
    auto s=find(store,name);
    if(!s)throw Error(ErrorCode::Schema,"unknown table: "+std::string(name));
    return std::move(*s);
  }
  void create_table(Store&store,Schema s) {
    validate_schema(s);
    if(find(store,s.name))throw Error(ErrorCode::Schema,"table already exists: "+s.name);
    std::uint64_t next=1;
    if(auto raw=store.get(0)) {
      binary::Reader in(*raw);
      if(in.get(1)!=1)throw Error(ErrorCode::Corruption,"invalid catalog counter version");
      next=in.get();
      in.end();
    }
    if(next==0||next>INT64_MAX)throw Error(ErrorCode::Constraint,"table identifier exhausted");
    s.id=next;
    store.put(static_cast<std::int64_t>(next),encode(s));
    std::string counter;
    binary::put(counter,1,1);
    binary::put(counter,next+1);
    store.put(0,counter);
  }
}
