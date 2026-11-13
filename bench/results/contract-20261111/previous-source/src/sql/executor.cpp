#include "sql.hpp"
#include "sorter.hpp"
#include "scan_cache.hpp"
#include "typed_scan_cache.hpp"
#include "../catalog/catalog.hpp"
#include "minivectordb/exact.hpp"
#include "../vector/validated.hpp"
#include "minivectordb/profile.hpp"
#include <cmath>
#include <bit>
#include "../storage/binary.hpp"
#include <limits>
#include <span>
namespace minivectordb {
  namespace {
    std::string index_vector(const Cell& cell) {
      std::string bytes;
      for(float value:std::get<std::vector<float>>(cell))binary::put(bytes,std::bit_cast<std::uint32_t>(value),4);
      return bytes;
    }
    std::vector<float> read_index_vector(std::string_view bytes,std::size_t dimension) {
      if(bytes.size()!=dimension*4)throw Error(ErrorCode::Corruption,"invalid vector index entry length");
      binary::Reader in(bytes);
      std::vector<float> result; result.reserve(dimension);
      for(std::size_t i=0;i<dimension;++i) {
        float value=std::bit_cast<float>(static_cast<std::uint32_t>(in.get(4)));
        if(!std::isfinite(value))throw Error(ErrorCode::Corruption,"invalid vector index component");
        result.push_back(value);
      }
      return result;
    }
    struct StopScan {
    };
    void count_decoded(const Row& row,ExecutionInfo& info) {
      info.cells_materialized+=row.size();
      for(const auto& cell:row)if(std::holds_alternative<std::vector<float>>(cell))++info.vector_values_materialized;
    }
    class MutationBatch {
      Store& store_;
      std::uint64_t space_;
      bool enabled_;
      std::size_t bytes_=0;
      std::vector<Store::Mutation> mutations_;
    public:
      MutationBatch(Store& store,std::uint64_t space,bool enabled):store_(store),space_(space),enabled_(enabled) {}
      void flush() {
        if(mutations_.empty())return;
        store_.apply_batch_in(space_,mutations_);
        mutations_.clear(); bytes_=0;
      }
      void add(std::int64_t key,std::optional<std::string> value) {
        if(!enabled_) {
          if(value)store_.put_in(space_,key,*value);
          else store_.erase_in(space_,key);
          return;
        }
        const auto size=value?value->size():0;
        if(!mutations_.empty() && (mutations_.size()==Store::max_batch_mutations ||
            size>Store::max_batch_bytes-bytes_))flush();
        mutations_.push_back({key,std::move(value)});
        bytes_+=size;
        if(bytes_>=Store::max_batch_bytes)flush();
      }
    };
    class SelectRow {
      Row eager_;
      std::optional<LazyRowView> lazy_;
    public:
      SelectRow(const Schema& schema,std::int64_t key,std::string_view bytes,bool selective,ExecutionInfo& info) {
        if(selective)lazy_.emplace(schema,bytes,info);
        else { eager_=decode_row(schema,bytes); count_decoded(eager_,info); }
        if(std::get<std::int64_t>((*this)[schema.primary])!=key)
          throw Error(ErrorCode::Corruption,"row primary key disagrees with index");
      }
      const Cell& operator[](std::size_t index) const { return lazy_?(*lazy_)[index]:eager_[index]; }
      std::span<const float> vector_for_distance(std::size_t index,std::vector<float>& scratch) const {
        if(lazy_) {
          lazy_->copy_vector(index,scratch);
          return scratch;
        }
        return std::get<std::vector<float>>(eager_[index]);
      }
      bool matches(const std::vector<sql::BoundPredicate>& predicates) const {
        for(const auto& p:predicates) {
          const Cell& cell=(*this)[p.column];
          profile::detail::Scope timer(&profile::Snapshot::predicate_evaluation);
          int cmp=sql::compare_cells(cell,p.value);
          if((p.op=="="&&cmp!=0)||(p.op=="<"&&cmp>=0)||(p.op=="<="&&cmp>0)||(p.op==">"&&cmp<=0)||(p.op==">="&&cmp<0))return false;
        }
        return true;
      }
    };
    struct DistanceCache {
      std::vector<std::pair<const sql::BoundExpr*,double>> values;
      bool same(const sql::BoundExpr&a,const sql::BoundExpr&b)const {
        return a.column==b.column&&a.query==b.query;
      }
      double distance(const sql::BoundExpr&e,const SelectRow&r,ExecutionInfo&info) {
        for(const auto&[expr,value]:values)if(same(*expr,e))return value;
        auto value=std::sqrt(sql::squared_distance(std::get<std::vector<float>>(r[e.column]),*e.query));
        ++info.distances_computed;
        values.emplace_back(&e,value);
        return value;
      }
    };
    Row project(const sql::Bound&b,const SelectRow&r,DistanceCache&cache,ExecutionInfo&info) {
      Row out;
      out.reserve(b.projection.size());
      for(const auto&e:b.projection) {
        if(e.query)out.emplace_back(cache.distance(e,r,info));
        else out.push_back(r[e.column]);
      }
      return out;
    }
  }
  struct Database::Impl {
    Store store;
    Options options;
    sql::ScanSnapshot snapshot;
    sql::TypedScanCache typed_snapshot;
    bool active=false,poisoned=false,busy=false;
    Impl(const std::filesystem::path&path,Options settings):store(path,settings.index_cache_bytes,settings.packed_values,settings.packed_scan_cache_bytes,settings.data_cache_bytes,settings.commit_sync_policy),options(std::move(settings)) {
      if(options.sort_directory.empty())options.sort_directory=path.parent_path()/"tmp";
      if(options.memory_budget_bytes<4096)throw Error(ErrorCode::Constraint,"sort memory budget must be at least 4096 bytes");
    }
    ~Impl() {
      if(active&&!poisoned) {
        try {
          store.rollback();
        }
        catch(...) {
        }
      }
    }
    Row decode(const Schema&s,std::int64_t key,std::string_view bytes) {
      auto row=decode_row(s,bytes);
      if(std::get<std::int64_t>(row[s.primary])!=key)throw Error(ErrorCode::Corruption,"row primary key disagrees with index");
      return row;
    }
    void scan(const Schema&s,const sql::Bound&b,ExecutionInfo&info,const std::function<void(std::int64_t,const Row&)>&visit) {
      auto callback=[&](auto key,auto bytes) {
        ++info.rows_examined;
        auto row=decode(s,key,bytes);
        count_decoded(row,info);
        if(sql::matches(row,b.predicates))visit(key,row);
      };
      if(b.primary_lookup) {
        if(auto raw=store.get_in(s.id,*b.primary_lookup))callback(*b.primary_lookup,*raw);
      }
      else store.scan_in(s.id,callback);
    }
    void scan_select(const Schema& s,const sql::Bound& b,ExecutionInfo& info,
                     const std::function<void(std::int64_t,const SelectRow&)>& visit,
                     bool allow_snapshot=false) {
      profile::detail::Scope scan_timer(&profile::Snapshot::table_scan);
      auto callback=[&](auto key,auto bytes) {
        profile::detail::Scope consume_timer(&profile::Snapshot::scan_consume);
        ++info.rows_examined;
        SelectRow row(s,key,bytes,options.selective_decode,info);
        if(row.matches(b.predicates))visit(key,row);
      };
      if(allow_snapshot && !b.primary_lookup && options.table_scan_snapshot && options.scan_snapshot_bytes>0) {
        try {
          if(snapshot.ready(s.id,0)) {
            snapshot.scan([&](auto key,auto bytes) {
              ++info.snapshot_rows_examined;
              callback(key,bytes);
            });
          } else {
            snapshot.start(s.id,0,options.scan_snapshot_bytes,options.reuse_scan_snapshot_buffer);
            if(snapshot.building()) {
              store.scan_in(s.id,[&](auto key,auto bytes) {
                callback(key,bytes);
                // Retain every validated row, including rows rejected by this filter.
                snapshot.append(key,bytes);
              });
              snapshot.finish();
            } else store.scan_in(s.id,callback);
          }
        } catch(...) {
          snapshot.clear();throw;
        }
        return;
      }
      if(b.primary_lookup) {
        if(auto raw=store.get_in(s.id,*b.primary_lookup))callback(*b.primary_lookup,*raw);
      } else store.scan_in(s.id,callback);
    }
    void fetch_select(const Schema& s,std::int64_t key,ExecutionInfo& info,
                      const std::function<void(const SelectRow&)>& visit) {
      auto raw=store.get_in(s.id,key);
      if(!raw)throw Error(ErrorCode::Corruption,"selected row disappeared");
      SelectRow row(s,key,*raw,options.selective_decode,info);
      visit(row);
    }
    bool typed_source(const Schema& s,const sql::Bound& b,const sql::BoundExpr& e,
                      const std::optional<VectorIndex>& index,ExecutionInfo& info,
                      const VectorVisitor& visit,std::vector<float>& scratch) {
      bool eligible=options.typed_scan_cache && options.scan_snapshot_bytes>0 && !b.primary_lookup &&
          (index?options.paired_index_scan:options.table_scan_snapshot);
      for(const auto& p:b.predicates)
        eligible=eligible && s.columns[p.column].type==ColumnType::Bigint &&
            std::holds_alternative<std::int64_t>(p.value);
      if(!eligible) {typed_snapshot.clear();return false;}
      // Two representations never own their bounded payloads simultaneously.
      snapshot.clear();
      const auto index_id=index?index->id:0;
      profile::detail::Scope scan_timer(&profile::Snapshot::table_scan);
      try {
        if(typed_snapshot.ready(s.id,index_id,e.column)) {
          struct Predicate {std::size_t slot;std::int64_t value;std::string_view op;};
          std::vector<Predicate> predicates;
          predicates.reserve(b.predicates.size());
          for(const auto& p:b.predicates)
            predicates.push_back({typed_snapshot.scalar_slot(p.column),std::get<std::int64_t>(p.value),p.op});
          for(std::size_t r=0;r<typed_snapshot.size();++r) {
            profile::detail::Scope consume_timer(&profile::Snapshot::scan_consume);
            ++info.rows_examined;++info.typed_rows_examined;
            bool matches=true;
            for(const auto& p:predicates) {
              profile::detail::Scope predicate_timer(&profile::Snapshot::predicate_evaluation);
              const auto value=typed_snapshot.scalar(r,p.slot);
              if((p.op=="="&&value!=p.value)||(p.op=="<"&&value>=p.value)||
                 (p.op=="<="&&value>p.value)||(p.op==">"&&value<=p.value)||
                 (p.op==">="&&value<p.value)) {matches=false;break;}
            }
            if(matches) {++info.distances_computed;visit(typed_snapshot.key(r),typed_snapshot.vector(r));}
          }
        } else {
          typed_snapshot.start(s,index_id,e.column,options.scan_snapshot_bytes,options.reuse_scan_snapshot_buffer);
          auto consume=[&](std::int64_t key,const SelectRow& row,std::span<const float> vector) {
            // Capture every row before applying this query's predicate. The cache
            // must also answer a different filter, including after a zero-match build.
            if(typed_snapshot.building()) {
              if(vector.empty())vector=row.vector_for_distance(e.column,scratch);
              typed_snapshot.append(vector,[&](std::size_t column) {return std::get<std::int64_t>(row[column]);});
            }
            if(row.matches(b.predicates)) {
              if(vector.empty())vector=row.vector_for_distance(e.column,scratch);
              ++info.distances_computed;visit(key,vector);
            }
          };
          if(index) {
            store.scan_equal_in(index->id,s.id,[&](auto key,auto bytes,auto base) {
              profile::detail::Scope consume_timer(&profile::Snapshot::scan_consume);
              ++info.vector_index_entries_examined;++info.rows_examined;
              auto vector=read_index_vector(bytes,s.columns[e.column].extent);
              SelectRow row(s,key,base,options.selective_decode,info);
              if(vector!=std::get<std::vector<float>>(row[e.column]))
                throw Error(ErrorCode::Corruption,"vector index disagrees with row");
              consume(key,row,vector);
            });
          } else {
            store.scan_in(s.id,[&](auto key,auto bytes) {
              profile::detail::Scope consume_timer(&profile::Snapshot::scan_consume);
              ++info.rows_examined;
              SelectRow row(s,key,bytes,options.selective_decode,info);
              consume(key,row,{});
            });
          }
          typed_snapshot.finish();
        }
      } catch(const Store::KeySetMismatch& error) {
        typed_snapshot.clear();throw Error(ErrorCode::Corruption,error.what());
      } catch(...) {typed_snapshot.clear();throw;}
      info.typed_cache_bytes=typed_snapshot.bytes();
      return true;
    }
    ExecutionInfo select(const sql::Statement&statement,const Schema&s,const sql::Bound&b,const QuerySink&sink) {
      ExecutionInfo info;
      std::vector<std::string> labels;
      for(const auto&e:b.projection)labels.push_back(e.label);
      if(sink.columns)sink.columns(labels);
      auto limit=statement.limit.value_or(UINT64_MAX);
      if(!limit)return info;
      auto emit=[&](const SelectRow&r,DistanceCache&cache) {
        auto output=project(b,r,cache,info);
        if(sink.row)sink.row(output);
        ++info.affected_rows;
      };
      if(b.order.empty()) {
        typed_snapshot.clear();
        try {
          scan_select(s,b,info,[&](auto,const SelectRow&r) {
            DistanceCache cache;
            emit(r,cache);
            if(--limit==0)throw StopScan {
            };
          });
        }
        catch(const StopScan&) {
        }
        return info;
      }
      if(b.order.size()==1&&!b.order[0].descending&&b.order[0].expression.query&&statement.limit&&limit<=options.memory_budget_bytes/(4*sizeof(Neighbor))) {
        const auto&e=b.order[0].expression;
        std::vector<float> vector_scratch;
        // Every source below validates the candidate through SelectRow/index
        // decoding, or reuses owned typed columns from a complete validated scan.
        const auto top_k=options.validated_vector_kernel?detail::exact_top_k_validated_candidates:exact_top_k;
        auto winners=top_k(*e.query,static_cast<std::size_t>(limit),[&](const VectorVisitor&visit) {
          auto consume_row=[&](std::int64_t key,const SelectRow& row) {
            ++info.distances_computed;
            if(options.reuse_vector_scratch)visit(key,row.vector_for_distance(e.column,vector_scratch));
            else visit(key,std::get<std::vector<float>>(row[e.column]));
          };
          std::optional<VectorIndex> index;
          if(!b.primary_lookup)for(const auto& candidate:vector_indexes(store,s.id))
            if(candidate.column==e.column) { index=candidate; break; }
          if(typed_source(s,b,e,index,info,visit,vector_scratch))return;
          if(index) {
            // Exhaustive flat index: exact and ANN mode share recall 1. Read and
            // validate every base row, retaining the established corruption contract.
            auto consume=[&](auto key,const std::vector<float>& vector,const SelectRow& row) {
              if(vector!=std::get<std::vector<float>>(row[e.column]))
                throw Error(ErrorCode::Corruption,"vector index disagrees with row");
              if(row.matches(b.predicates)) { ++info.distances_computed; visit(key,vector); }
            };
            if(options.paired_index_scan && snapshot.ready(s.id,index->id)) {
              snapshot.scan([&](auto key,auto bytes) {
                ++info.rows_examined;++info.snapshot_rows_examined;
                SelectRow row(s,key,bytes,options.selective_decode,info);
                if(row.matches(b.predicates))consume_row(key,row);
              });
            } else if(options.paired_index_scan) {
              snapshot.start(s.id,index->id,options.scan_snapshot_bytes,options.reuse_scan_snapshot_buffer);
              try {
                store.scan_equal_in(index->id,s.id,[&](auto key,auto bytes,auto base) {
                  ++info.vector_index_entries_examined;
                  auto vector=read_index_vector(bytes,s.columns[e.column].extent);
                  ++info.rows_examined;
                  SelectRow row(s,key,base,options.selective_decode,info);
                  consume(key,vector,row);
                  snapshot.append(key,base);
                });
                snapshot.finish();
              } catch(const Store::KeySetMismatch& error) {
                snapshot.clear();
                throw Error(ErrorCode::Corruption,error.what());
              } catch(...) {
                snapshot.clear();throw;
              }
            } else store.scan_in(index->id,[&](auto key,auto bytes) {
              ++info.vector_index_entries_examined;
              auto vector=read_index_vector(bytes,s.columns[e.column].extent);
              ++info.rows_examined;
              fetch_select(s,key,info,[&](const SelectRow& row) {consume(key,vector,row);});
            });
          } else scan_select(s,b,info,consume_row,true);
        },{});
        info.snapshot_bytes=snapshot.bytes();
        for(const auto&winner:winners) {
          fetch_select(s,winner.key,info,[&](const SelectRow& row) {
            DistanceCache cache;
            Row output;
            output.reserve(b.projection.size());
            for(const auto&projection:b.projection) {
              if(projection.query&&cache.same(projection,e))output.emplace_back(winner.distance);
              else if(projection.query)output.emplace_back(cache.distance(projection,row,info));
              else output.push_back(row[projection.column]);
            }
            if(sink.row)sink.row(output);
            ++info.affected_rows;
          });
        }
        return info;
      }
      typed_snapshot.clear();
      sql::Sorter sorter(options,b.order,info);
      scan_select(s,b,info,[&](auto key,const SelectRow&r) {
        DistanceCache cache;
        sql::SortRecord record;
        record.key=key;
        record.values.reserve(b.order.size());
        for(const auto&o:b.order) {
          if(o.expression.query)record.values.emplace_back(cache.distance(o.expression,r,info));
          else record.values.push_back(r[o.expression.column]);
        }
        sorter.add(std::move(record));
      });
      sorter.finish(limit,[&](const sql::SortRecord&record) {
        fetch_select(s,record.key,info,[&](const SelectRow& row) {
          DistanceCache cache;
          // Sort and projection share the same rounded Euclidean values.
          for(std::size_t i=0;i<b.order.size();++i)if(b.order[i].expression.query)cache.values.emplace_back(&b.order[i].expression,std::get<double>(record.values[i]));
          emit(row,cache);
        });
      });
      return info;
    }
    ExecutionInfo execute(std::string_view text,const QuerySink&sink) {
      if(busy)throw Error(ErrorCode::Transaction,"Database callbacks cannot reenter execute");
      if(poisoned)throw Error(ErrorCode::Transaction,"commit outcome uncertain; close and reopen Database");
      busy=true;
      struct Reset {
        bool&flag;
        ~Reset() {
          flag=false;
        }
      }
      reset {
        busy
      };
      auto statement=sql::parse(text);
      // Includes failed DML/DDL, BEGIN, COMMIT and ROLLBACK. Conservative invalidation
      // avoids transaction/root ABA and does not require persisted cache metadata.
      if(statement.kind!=sql::Statement::Select) {
        if(options.reuse_scan_snapshot_buffer) {snapshot.invalidate();typed_snapshot.invalidate();}
        else {snapshot.clear();typed_snapshot.clear();}
      }
      ExecutionInfo info;
      if(statement.kind==sql::Statement::Begin) {
        if(active)throw Error(ErrorCode::Transaction,"transaction already active");
        store.begin();
        active=true;
        return info;
      }
      if(statement.kind==sql::Statement::Commit) {
        if(!active)throw Error(ErrorCode::Transaction,"no active transaction");
        try {
          store.commit();
        }
        catch(...) {
          poisoned=true;
          throw;
        }
        active=false;
        return info;
      }
      if(statement.kind==sql::Statement::Rollback) {
        if(!active)throw Error(ErrorCode::Transaction,"no active transaction");
        store.rollback();
        active=false;
        return info;
      }
      Schema schema;
      sql::Bound bound;
      if(statement.kind!=sql::Statement::Create) {
        schema=lookup_table(store,statement.table);
        if(statement.kind!=sql::Statement::CreateIndex)bound=sql::bind(statement,schema);
      }
      if(statement.kind==sql::Statement::Select)return select(statement,schema,bound,sink);
      // Binding and existing-key checks finish before any statement mutation.
      if(statement.kind==sql::Statement::Insert)for(const auto&r:bound.insert_rows)if(store.get_in(schema.id,std::get<std::int64_t>(r[schema.primary])))throw Error(ErrorCode::Constraint,"duplicate primary key");
      const bool automatic=!active;
      if(automatic) {
        store.begin();
        active=true;
      }
      const auto savepoint=store.savepoint();
      try {
        MutationBatch batch(store,schema.id,options.batch_writes);
        const auto indexes=vector_indexes(store,schema.id);
        std::vector<std::unique_ptr<MutationBatch>> index_batches;
        for(const auto& index:indexes) {
          if(index.column>=schema.columns.size()||schema.columns[index.column].type!=ColumnType::Vector)
            throw Error(ErrorCode::Corruption,"invalid indexed column");
          index_batches.push_back(std::make_unique<MutationBatch>(store,index.id,options.batch_writes));
        }
        auto maintain=[&](std::int64_t key,const Row* row) {
          for(std::size_t i=0;i<indexes.size();++i)
            index_batches[i]->add(key,row?std::optional<std::string>(index_vector((*row)[indexes[i].column])):std::nullopt);
        };
        if(statement.kind==sql::Statement::Create)create_table(store,statement.schema);
        else if(statement.kind==sql::Statement::CreateIndex) {
          auto column=sql::column_index(schema,statement.index_column);
          auto index=create_vector_index(store,schema,column,statement.index_name);
          MutationBatch entries(store,index.id,options.batch_writes);
          store.scan_in(schema.id,[&](auto key,auto bytes) {
            auto row=decode(schema,key,bytes);
            entries.add(key,index_vector(row[column]));
          });
          entries.flush();
        }
        else if(statement.kind==sql::Statement::Insert) {
          for(const auto&r:bound.insert_rows) {
            batch.add(std::get<std::int64_t>(r[schema.primary]),encode_row(schema,r));
            maintain(std::get<std::int64_t>(r[schema.primary]),&r);
            ++info.affected_rows;
          }
        }
        else if(statement.kind==sql::Statement::Update) {
          scan(schema,bound,info,[&](auto key,const Row&old) {
            auto r=old;
            for(const auto&[column,value]:bound.assignments)r[column]=value;
            batch.add(key,encode_row(schema,r));
            maintain(key,&r);
            ++info.affected_rows;
          });
        }
        else if(statement.kind==sql::Statement::Delete) {
          scan(schema,bound,info,[&](auto key,const Row&) {
            batch.add(key,std::nullopt);
            maintain(key,nullptr);
            ++info.affected_rows;
          });
        }
        batch.flush();
        for(auto& entries:index_batches)entries->flush();
      }
      catch(...) {
        store.restore(savepoint);
        if(automatic) {
          store.rollback();
          active=false;
        }
        throw;
      }
      // Never restore/rollback after a commit error: root publication may already be durable.
      if(automatic) {
        try {
          store.commit();
        }
        catch(...) {
          poisoned=true;
          throw;
        }
        active=false;
      }
      return info;
    }
  };
  Database::Database(const std::filesystem::path&path,Options options):impl_(std::make_unique<Impl>(path,std::move(options))) {
  }
  Database::~Database()=default;
  CacheStats Database::data_cache_stats() const noexcept {return impl_->store.data_cache_stats();}
  void Database::reset_data_cache_stats() noexcept {impl_->store.reset_data_cache_stats();}
  CacheStats Database::cache_stats() const noexcept {return impl_->store.cache_stats();}
  void Database::reset_cache_stats() noexcept {impl_->store.reset_cache_stats();}
  ExecutionInfo Database::execute(std::string_view sql,const QuerySink&sink,SearchMode mode) {
    if(mode!=SearchMode::Exact&&mode!=SearchMode::Ann)throw Error(ErrorCode::Type,"invalid search mode");
    // Flat index enumerates all vectors in either mode; no approximate pruning.
    return impl_->execute(sql,sink);
  }
  bool Database::in_transaction()const noexcept {
    return impl_->active;
  }
}
