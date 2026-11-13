#define main original_mutation_prefix_main
#include "mutation_prefix_tests.cpp"
#undef main
#include "../src/storage/packed_pointer.hpp"
#include <fstream>
void pointer_codec() {
    std::mt19937_64 random(0x73141);
    constexpr auto value_tag=UINT64_C(1)<<63,node_tag=UINT64_C(1)<<62,mask=node_tag-1;
    for(unsigned i=0;i<100000;++i)for(const auto tag:{UINT64_C(0),value_tag,node_tag}) {
        const auto reference=(random()&mask)|tag;
        std::string bytes="prefix";detail::append_packed_pointer(bytes,reference);
        std::size_t offset=6;CHECK(detail::read_packed_pointer(bytes,offset)==reference);CHECK(offset==bytes.size());
        std::string canonical;detail::append_packed_pointer(canonical,reference);CHECK(bytes.substr(6)==canonical);
        for(std::size_t size=6;size<bytes.size();++size) {
            offset=6;bool rejected=false;
            try{(void)detail::read_packed_pointer(std::string_view(bytes).substr(0,size),offset);}catch(const std::exception&){rejected=true;}
            CHECK(rejected);
        }
    }
    for(unsigned bit=0;bit<62;++bit)for(const auto delta:{UINT64_C(0),UINT64_C(1)}) {
        const auto reference=(UINT64_C(1)<<bit)-delta;
        std::string bytes;detail::append_packed_pointer(bytes,reference);std::size_t at=0;
        CHECK(detail::read_packed_pointer(bytes,at)==reference&&at==bytes.size());
    }
    std::string golden;detail::append_packed_pointer(golden,value_tag|(2ULL<<9)|3);
    CHECK(golden==std::string("\x8d\x20",2));
    for(const std::string bytes:{std::string("\x80",1),std::string("\x80\0",2),std::string("\x83\0",2),
        std::string(10,char(255)),std::string(10,char(128)),std::string("\3",1)}) {
        std::size_t at=0;bool rejected=false;try{(void)detail::read_packed_pointer(bytes,at);}catch(const std::exception&){rejected=true;}CHECK(rejected);
    }
}
int main(){try{
    pointer_codec();const char* env=std::getenv("MVDB_TEST_DIR");CHECK(env);
    fs::create_directories(env);auto path=(fs::path(env)/"node-pointer-XXXXXX").string();CHECK(mkdtemp(path.data()));
    const auto file=fs::path(path)/"format06";trace(file,true,true,true,true,true);
    std::ifstream in(file,std::ios::binary);std::string magic(8,'\0');in.read(magic.data(),8);CHECK(magic=="MVDBRT06");
    // Enabling the option on an existing format05 file does not migrate it.
    const auto legacy=fs::path(path)/"format05";
    {Store s(legacy,0,true,0,0,CommitSyncPolicy::ProcessCrash,true,true,true);s.begin();s.put(1,"old");s.commit();}
    {Store s(legacy,0,true,0,0,CommitSyncPolicy::ProcessCrash,true,true,true,false,true);s.begin();s.put(2,"new");s.commit();CHECK(s.get(1)=="old"&&s.get(2)=="new");}
    std::ifstream old(legacy,std::ios::binary);old.read(magic.data(),8);CHECK(magic=="MVDBRT05");
    fs::remove_all(path);std::cout<<"PASS 300000 tagged-pointer round trips, boundaries/malformed encodings, format06 model/recovery and format05 preservation\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
