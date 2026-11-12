#include "minivectordb/database.hpp"
#include "../src/catalog/catalog.hpp"
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <unistd.h>

using namespace minivectordb;
#define CHECK(x) do {if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
std::string vector_text(int n) {
    std::string out="[";
    for(int d=0;d<128;++d){if(d)out+=',';out+=std::to_string((n+d)%13-6);}
    return out+"]";
}
std::vector<Row> query(Database& db,std::string filter="",SearchMode mode=SearchMode::Exact) {
    std::vector<Row> rows;
    db.execute("SELECT id,L2_DISTANCE(v,'"+vector_text(3)+"') AS d,g,body FROM t "+filter+" ORDER BY d LIMIT 9",
        {{},[&](const Row& row){rows.push_back(row);}},mode);
    return rows;
}
void reject(Database& db) {
    bool bad=false;try{query(db,"WHERE g=999");}catch(const Error& e){bad=e.code()==ErrorCode::Corruption;}CHECK(bad);
}
void exercise(const std::filesystem::path& dir,bool paired) {
    const auto old_path=dir/"old",ref_path=dir/"reference";
    Options old_options,new_options;
    old_options.referenced_vector_index=false;
    old_options.paired_index_scan=new_options.paired_index_scan=paired;
    std::vector<Row> expected;
    {
        Database old(old_path,old_options),ref(ref_path,new_options);
        auto both=[&](const std::string& sql){old.execute(sql);ref.execute(sql);};
        both("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(128),g BIGINT,body VARCHAR(128))");
        std::string insert="INSERT INTO t VALUES ";
        for(int n=0;n<67;++n) {
            if(n)insert+=',';
            insert+='('+std::to_string(n-33)+",'"+vector_text(n)+"',"+std::to_string(n%5)+",'row"+std::to_string(n)+"')";
        }
        both(insert);
        both("CREATE VECTOR INDEX vi ON t(v)");
        auto compare=[&]() {
            for(auto mode:{SearchMode::Exact,SearchMode::Ann})for(auto filter:{"","WHERE g=2","WHERE g>=3","WHERE g=999"})
                CHECK(query(old,filter,mode)==query(ref,filter,mode));
        };
        compare();
        both("BEGIN");
        both("UPDATE t SET v='"+vector_text(7)+"',body='updated' WHERE g=2");
        both("DELETE FROM t WHERE g=3");
        both("INSERT INTO t VALUES(999,'"+vector_text(2)+"',2,'new')");
        compare();both("ROLLBACK");compare();
        both("BEGIN");both("UPDATE t SET v='"+vector_text(9)+"' WHERE g=1");
        both("DELETE FROM t WHERE id=-33");both("COMMIT");compare();
        expected=query(ref);
    }
    // Opening with opposite creation preferences must not reinterpret old bytes.
    {Database old(old_path,new_options),ref(ref_path,old_options);CHECK(query(old)==expected);CHECK(query(ref)==expected);}
    std::string entry;
    {
        Store old(old_path),ref(ref_path);
        const auto oi=vector_indexes(old,1),ri=vector_indexes(ref,1);
        CHECK(oi.size()==1&&ri.size()==1);CHECK(!oi[0].referenced&&ri[0].referenced);
        CHECK(old.get_in(2,0)->size()==128*4);
        entry=*ref.get_in(2,0);CHECK(entry.size()==28);
    }
    // Correct-size reference corruption must not be hidden by a rejecting filter.
    for(std::size_t position:{0U,4U,12U,16U,20U}) {
        auto bad=entry;bad[position]^=1;
        {Store ref(ref_path);ref.begin();ref.put_in(2,0,bad);ref.commit();}
        {Database ref(ref_path,new_options);reject(ref);}
        {Store ref(ref_path);ref.begin();ref.put_in(2,0,entry);ref.commit();}
    }
    for(auto bad:{entry.substr(0,27),entry+"x"}) {
        {Store ref(ref_path);ref.begin();ref.put_in(2,0,bad);ref.commit();}
        {Database ref(ref_path,new_options);reject(ref);}
        {Store ref(ref_path);ref.begin();ref.put_in(2,0,entry);ref.commit();}
    }
    // A missing base row is a dangling reference, even on the unpaired fallback.
    std::string base;
    {Store ref(ref_path);base=*ref.get_in(1,0);ref.begin();ref.erase_in(1,0);ref.commit();}
    {Database ref(ref_path,new_options);reject(ref);}
    {Store ref(ref_path);ref.begin();ref.put_in(1,0,base);ref.commit();}
    {Database ref(ref_path,new_options);CHECK(query(ref)==expected);}
}
int main() {
    const char* root=std::getenv("MVDB_TEST_DIR");
    const auto dir=std::filesystem::path(root?root:"/data")/("reference-index-"+std::to_string(getpid()));
    try {
        for(bool paired:{false,true}) {
            const auto child=dir/std::to_string(paired);std::filesystem::create_directories(child);exercise(child,paired);
        }
        std::filesystem::remove_all(dir);std::cout<<"reference and legacy vector indexes agree; rollback, restart and corruption checks passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
