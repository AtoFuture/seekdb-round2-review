#pragma once
#include "minivectordb/database.hpp"
#include "minivectordb/store.hpp"
// Reuse the unchanged SQL/fault oracles against an explicitly selected backend.
class JournalHarnessDatabase:public minivectordb::Database {
  static minivectordb::Options select(minivectordb::Options o){o.journal_store=true;return o;}
public:
  explicit JournalHarnessDatabase(const std::filesystem::path& p,minivectordb::Options o={}):Database(p,select(std::move(o))){}
};
class JournalHarnessStore:public minivectordb::Store {
public:
  template<class... Args> explicit JournalHarnessStore(const std::filesystem::path& p,Args&&...):Store(p,0,true,0,65536,minivectordb::CommitSyncPolicy::Ordered,true,true,true,false,true,true,true){}
};
