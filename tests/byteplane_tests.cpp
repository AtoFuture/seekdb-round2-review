#define main original_codec_main
#include "packed_codec_tests.cpp"
#undef main
void planes() {
  std::mt19937 gen(20261112);std::size_t packed=0,shuffled=0;
  for(std::size_t n=0;n<4100;++n)for(unsigned mode=0;mode<5;++mode) {
    std::string source(n,'\0');
    for(std::size_t i=0;i<n;++i)source[i]=char(mode==0?gen():mode==1?i%4:mode==2?(i%4<2?0:gen()):mode==3?i/130:gen()%3);
    std::string compressed,ordinary;
    const bool old=detail::pack_bytes(source,ordinary);
    if(detail::pack_bytes(source,compressed,true)) {
      ++packed;CHECK(compressed.size()+8<=n);
      CHECK(!old || compressed.size()<=ordinary.size());
      CHECK(detail::unpack_bytes(compressed,4064,true)==source);
      const bool transform=(static_cast<unsigned char>(compressed[3])&128)!=0;
      if(transform) {
        ++shuffled;bool rejected=false;
        try{(void)detail::unpack_bytes(compressed);}catch(...){rejected=true;}CHECK(rejected);
      }
      // All byte-boundary truncations on short frames; sampled boundaries on
      // long frames, including header, last run, and advertised-size failures.
      for(std::size_t cut=0;cut<compressed.size();cut+=compressed.size()<130?1:std::max(std::size_t(1),compressed.size()/17)) {
        bool rejected=false;try{(void)detail::unpack_bytes(std::string_view(compressed).substr(0,cut),4064,true);}catch(...){rejected=true;}CHECK(rejected);
      }
      auto extra=compressed;extra.append("\x80x",2);bool rejected=false;
      try{(void)detail::unpack_bytes(extra,4064,true);}catch(...){rejected=true;}CHECK(rejected);
    }else CHECK(!old);
  }
  CHECK(shuffled>1000);std::cout<<"byte-plane cases=20500 packed="<<packed<<" shuffled="<<shuffled<<'\n';
}
void storage07(const fs::path& p) {
  std::map<std::int64_t,std::string> model;
  auto value=[](int seed){std::string s;for(int i=0;i<257;++i){s.push_back(0);s.push_back(0);s.push_back(char((i+seed)%251));s.push_back(char(seed));}return s;};
  auto verify=[&](Store& db){std::map<std::int64_t,std::string> got;db.scan_in(3,[&](auto k,auto v){got[k]=v;});CHECK(got==model);for(const auto& [k,v]:model)CHECK(db.get_in(3,k)==v);};
  {
    Store db(p,65536,true,32768,65536,CommitSyncPolicy::ProcessCrash,true,true,true,false,true,true);
    db.begin();std::vector<Store::Mutation> rows;
    for(int i=0;i<700;++i){model[i]=value(i);rows.push_back({i,model[i]});if(rows.size()==100){db.apply_batch_in(3,rows);rows.clear();}}
    if(!rows.empty())db.apply_batch_in(3,rows);db.commit();verify(db);
    db.begin();auto save=db.savepoint();auto before=model;
    rows.clear();for(int i=0;i<300;++i){rows.push_back({i,std::nullopt});model.erase(i);if(rows.size()==100){db.apply_batch_in(3,rows);rows.clear();}}verify(db);
    db.restore(save);model=before;verify(db);db.rollback();verify(db);
  }
  {Store db(p);verify(db);db.begin();db.put_in(3,800,value(800));model[800]=value(800);db.commit();}
  {Store db(p);verify(db);}
  std::ifstream file(p,std::ios::binary);std::string magic(8,'\0');file.read(magic.data(),8);CHECK(magic=="MVDBRT07");
  // Explicit format06 remains format06 when reopened with new defaults.
  auto old=p.string()+"-old";
  {Store db(old,0,true,0,0,CommitSyncPolicy::ProcessCrash,true,true,true,false,true);db.begin();db.put(1,value(1));db.commit();}
  {Store db(old,0,true,0,0,CommitSyncPolicy::ProcessCrash,true,true,true,false,true,true);db.begin();db.put(2,value(2));db.commit();CHECK(db.get(1)==value(1)&&db.get(2)==value(2));}
  std::ifstream legacy(old,std::ios::binary);legacy.read(magic.data(),8);CHECK(magic=="MVDBRT06");
}
int main(){try{planes();const char* env=std::getenv("MVDB_TEST_DIR");CHECK(env);fs::create_directories(env);auto path=(fs::path(env)/"planes-XXXXXX").string();CHECK(mkdtemp(path.data()));storage07(fs::path(path)/"db");sql_trace(fs::path(path)/"sql");fs::remove_all(path);std::cout<<"PASS generic lossless planes, malformed frames, format07 recovery/rollback and format06 preservation\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
