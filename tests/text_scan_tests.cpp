// Reuse the independent full-sort oracle and checked query fixture helpers.
#define main typed_scan_reference_main
#include "typed_scan_tests.cpp"
#undef main

namespace {
std::vector<Item> text_items() {
  const std::string labels[]{"","a","中","😀","é","aa","ä",std::string("a\0b",3)};
  std::vector<Item> items;
  for(int i=0;i<51;++i) {
    std::vector<float> v(128);
    for(std::size_t d=0;d<v.size();++d)v[d]=float(i%11-5)*float(d%3+1)/4;
    items.push_back({i*700003-17000051,i%5,i%3,std::move(v),labels[i%8]});
  }
  return items;
}
void text_seed(Database& db,const std::vector<Item>& items) {
  db.execute("CREATE TABLE t(v VECTOR(128),g BIGINT,label VARCHAR(16),h BIGINT,id BIGINT PRIMARY KEY,tag VARCHAR(16))");
  std::string sql="INSERT INTO t(v,g,label,h,id,tag) VALUES ";
  for(std::size_t n=0;n<items.size();++n) {
    if(n)sql+=',';const auto& r=items[n];
    sql+="('"+literal(r.v)+"',"+std::to_string(r.g)+",'"+r.label+"',"+
      std::to_string(r.h)+','+std::to_string(r.id)+",'z')";
  }
  db.execute(sql);
}
void text_lifecycle(const std::filesystem::path& path,bool indexed,bool selective) {
  auto o=settings();o.selective_decode=selective;
  const auto items=text_items();const std::vector<float> q(128,0.25f);
  {
    Database db(path,o);text_seed(db,items);
    if(indexed)db.execute("CREATE VECTOR INDEX tv ON t(v)");
    warm(db,items,q);
    warm(db,items,q,"label='absent'",[](const Item&){return false;});
    for(const auto& [sql,predicate]:std::vector<std::pair<std::string,Filter>>{
      {"label=''",[](const Item&i){return i.label.empty();}},
      {"label='😀'",[](const Item&i){return i.label=="😀";}},
      {"label='"+std::string("a\0b",3)+"'",[](const Item&i){return i.label==std::string("a\0b",3);}},
      {"label<'中'",[](const Item&i){return i.label<"中";}},
      {"label<='中'",[](const Item&i){return i.label<="中";}},
      {"label>'a'",[](const Item&i){return i.label>"a";}},
      {"label>='é' AND g<4",[](const Item&i){return i.label>="é"&&i.g<4;}},
      {"label>='a' AND label<'😀'",[](const Item&i){return i.label>="a"&&i.label<"😀";}}}) {
      auto r=query(db,statement(q,sql,51));CHECK(r.rows==oracle(items,q,predicate,51));
      CHECK(r.info.typed_rows_examined==items.size());CHECK(r.info.snapshot_bytes==0);
      CHECK(r.info.typed_cache_bytes<=o.scan_snapshot_bytes);
      CHECK(query(db,statement(q)).rows==oracle(items,q));
    }
    db.execute("BEGIN");auto changed=items;changed[0].label="😀";changed[0].v=std::vector<float>(128,0);
    db.execute("UPDATE t SET label='😀',v='"+literal(changed[0].v)+"' WHERE id="+std::to_string(changed[0].id));
    CHECK(query(db,statement(q)).info.typed_rows_examined==0);
    warm(db,changed,q,"label='😀'",[](const Item&i){return i.label=="😀";});
    db.execute("DELETE FROM t WHERE id="+std::to_string(changed.back().id));changed.pop_back();
    warm(db,changed,q,"label>='a'",[](const Item&i){return i.label>="a";});
    db.execute("ROLLBACK");warm(db,items,q,"label>='a'",[](const Item&i){return i.label>="a";});
    warm(db,items,q,"tag='z'",[](const Item&){return true;});
    // Two distinct text predicate columns deliberately fall back to raw rows.
    auto both=query(db,statement(q,"label='😀' AND tag='z'"));
    CHECK(both.rows==oracle(items,q,[](const Item&i){return i.label=="😀";}));
    CHECK(both.info.typed_rows_examined==0&&both.info.typed_cache_bytes==0);
    db.execute("BEGIN");db.execute("COMMIT");
    warm(db,items,q,"label='a'",[](const Item&i){return i.label=="a";});
  }
  {Database db(path,o);warm(db,items,q,"label='😀'",[](const Item&i){return i.label=="😀";});}
}
void text_budget(const std::filesystem::path& root) {
  const auto items=text_items();const std::vector<float> q(128,0);
  for(std::size_t budget:{1U,1024U,65536U}) {
    auto o=settings(budget);Database db(root/std::to_string(budget),o);text_seed(db,items);
    for(int round=0;round<7;++round) {
      auto r=query(db,statement(q,round%2?"label='a'":""));
      CHECK(r.rows==oracle(items,q,round%2?Filter([](const Item&i){return i.label=="a";}):Filter{}));
      CHECK(r.info.typed_cache_bytes+r.info.snapshot_bytes<=budget);
      if(round>=4&&budget==65536)CHECK(r.info.typed_rows_examined==items.size());
    }
  }
}
}
int main() {
  try {
    const char* configured=std::getenv("MVDB_TEST_DIR");
    const auto parent=std::filesystem::path(configured?configured:"/data/core-tests");
    std::filesystem::create_directories(parent);
    std::string pattern=(parent/"text-cache-XXXXXX").string();
    CHECK(::mkdtemp(pattern.data()));const std::filesystem::path root(pattern);
    for(bool indexed:{false,true})for(bool selective:{false,true})
      text_lifecycle(root/(std::to_string(indexed)+std::to_string(selective)),indexed,selective);
    std::filesystem::create_directories(root/"budget");text_budget(root/"budget");
    std::filesystem::remove_all(root);std::cout<<"text snapshot tests passed\n";return 0;
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
