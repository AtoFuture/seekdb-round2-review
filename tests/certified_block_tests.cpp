#define main original_typed_fixture_main
#include "typed_scan_tests.cpp"
#undef main
#include "../src/vector/validated.hpp"
int main(){try{
  const char* env=std::getenv("MVDB_TEST_DIR");CHECK(env);std::filesystem::create_directories(env);
  auto temp=(std::filesystem::path(env)/"certified-XXXXXX").string();CHECK(mkdtemp(temp.data()));
  const auto items=range_fixture();const std::vector<float> q(128,0.25f);std::uint64_t blocks=0;
  for(bool enabled:{false,true})for(bool wide:{false,true})for(bool indexed:{false,true}) {
    auto o=settings(262144);o.clustered_range_scan=true;o.certified_block_scan=enabled;
    o.wide_simd_vector_kernel=wide;o.bounded_fp32_screen=true;
    Database db(std::filesystem::path(temp)/(std::to_string(enabled)+std::to_string(wide)+std::to_string(indexed)),o);
    range_seed(db,items);if(indexed)db.execute("CREATE VECTOR INDEX vi ON t(v)");
    const std::vector<std::pair<std::string,Filter>> cases{
      {"",[](const Item&){return true;}},
      {"g=0",[](const Item&i){return i.g==0;}},
      {"g<0",[](const Item&i){return i.g<0;}},
      {"g>=0",[](const Item&i){return i.g>=0;}},
      {"g>=-1 AND g<2",[](const Item&i){return i.g>=-1&&i.g<2;}},
      {"g=-9223372036854775808",[](const Item&i){return i.g==INT64_MIN;}},
      {"g=9223372036854775807",[](const Item&i){return i.g==INT64_MAX;}},
      {"g=0 AND g=1",[](const Item&){return false;}},
      {"g>=0 AND h=1",[](const Item&i){return i.g>=0&&i.h==1;}},
      {"label=''",[](const Item&i){return i.label.empty();}},
      {"label='"+std::string("a\0b",3)+"'",[](const Item&i){return i.label==std::string("a\0b",3);}},
      {"label>='a' AND label<'😀'",[](const Item&i){return i.label>="a"&&i.label<"😀";}},
      {"label>'中' AND label<'a'",[](const Item&){return false;}},
      {"label>='a' AND h=1",[](const Item&i){return i.label>="a"&&i.h==1;}},
      {"g>=0 AND h=1",[](const Item&i){return i.g>=0&&i.h==1;}}
    };
    for(const auto& [where,filter]:cases)for(auto k:{10u,200u}) {
      ExecutionInfo info;
      for(int phase=0;phase<3;++phase) {
        std::vector<Row> rows;profile::set_enabled(false);
        info=db.execute(statement(q,where,k),{{},[&](const Row&r){rows.push_back(r);}});
        CHECK(rows==oracle(items,q,filter,k));
      }
      blocks+=info.certified_blocks_examined;
      if(!enabled)CHECK(info.certified_blocks_examined==0);
      if(where.find("h=1")!=std::string::npos)CHECK(info.certified_blocks_examined==0);
    }
    db.execute("BEGIN");db.execute("DELETE FROM t");
    std::vector<Row> empty;db.execute(statement(q),{{},[&](const Row&r){empty.push_back(r);}});CHECK(empty.empty());
    db.execute("ROLLBACK");std::vector<Row> restored;
    db.execute(statement(q),{{},[&](const Row&r){restored.push_back(r);}});CHECK(restored==oracle(items,q));
  }
  if(detail::has_avx2())CHECK(blocks>0);
  std::filesystem::remove_all(temp);std::cout<<"PASS independent full-sort certified-block checks; blocks="<<blocks<<'\n';return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
