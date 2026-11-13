#include "minivectordb/database.hpp"
#include "minivectordb/profile.hpp"
#include "minivectordb/row_codec.hpp"
#include "../src/sql/scan_cache.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

using namespace minivectordb;
#define CHECK(x) do {if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));} while(false)
namespace {
struct Item {std::int64_t id,g;std::string category;std::vector<float> value;};
struct Result {std::vector<Row> rows;ExecutionInfo info;profile::Snapshot stats;};
std::vector<Item> fixture() {
    std::vector<Item> rows;
    for(int i=23;i>=0;--i)rows.push_back({std::int64_t(i-12)*100003,i%4,
        i%3==0?"alpha":i%3==1?"beta":"gamma",{float(i%7-3),float(i/7-1)}});
    return rows;
}
void seed(Database& db,const std::vector<Item>& rows,const std::string& table="t",bool reordered=false) {
    db.execute("CREATE TABLE "+table+(reordered?"(v VECTOR(2),g BIGINT,category VARCHAR(100),id BIGINT PRIMARY KEY)":
        "(id BIGINT PRIMARY KEY,category VARCHAR(100),g BIGINT,v VECTOR(2))"));
    std::string sql="INSERT INTO "+table+"(id,category,g,v) VALUES ";
    for(std::size_t n=0;n<rows.size();++n) {
        if(n)sql+=',';const auto& r=rows[n];
        sql+='('+std::to_string(r.id)+",'"+r.category+"',"+std::to_string(r.g)+",'["+
            std::to_string(r.value[0])+','+std::to_string(r.value[1])+"]')";
    }
    db.execute(sql);
}
std::string statement(int q,bool filtered,const std::string& table="t") {
    return "SELECT id,L2_DISTANCE(v,'["+std::to_string(q)+",1]') AS d,category,g FROM "+table+
        (filtered?" WHERE category='alpha'":"")+" ORDER BY d LIMIT 5";
}
std::vector<Row> oracle(const std::vector<Item>& rows,int q,bool filtered) {
    std::vector<Row> result;
    for(const auto& row:rows) {
        if(filtered&&row.category!="alpha")continue;
        const float query[]{float(q),1};double squared=0;
        for(std::size_t d=0;d<2;++d) {const double x=double(query[d])-double(row.value[d]);squared+=x*x;}
        result.push_back({row.id,std::sqrt(squared),row.category,row.g});
    }
    std::sort(result.begin(),result.end(),[](const Row& a,const Row& b) {
        const auto x=std::get<double>(a[1]),y=std::get<double>(b[1]);
        return x<y||(x==y&&std::get<std::int64_t>(a[0])<std::get<std::int64_t>(b[0]));
    });
    if(result.size()>5)result.resize(5);return result;
}
Result query(Database& db,const std::string& text) {
    Result r;const bool enabled=profile::enabled();profile::set_enabled(true);profile::reset();
    try {r.info=db.execute(text,{{},[&](const Row& row){r.rows.push_back(row);}});}
    catch(...) {profile::set_enabled(enabled);throw;}
    r.stats=profile::snapshot();profile::set_enabled(enabled);return r;
}
void verify(const Result& r,const std::vector<Item>& rows,int q,bool filter) {
    const auto expected=oracle(rows,q,filter);CHECK(r.rows.size()==expected.size());
    for(std::size_t i=0;i<expected.size();++i) {
        CHECK(r.rows[i][0]==expected[i][0]&&r.rows[i][2]==expected[i][2]&&r.rows[i][3]==expected[i][3]);
        CHECK(std::bit_cast<std::uint64_t>(std::get<double>(r.rows[i][1]))==
              std::bit_cast<std::uint64_t>(std::get<double>(expected[i][1])));
    }
}
void mixed_reads(Database& db,const std::vector<Item>& rows,bool filter_first,const std::string& table="t") {
    for(int step=0;step<10;++step) {
        const bool filtered=(step%2==0)==filter_first;const int q=step%5-2;
        const auto r=query(db,statement(q,filtered,table));verify(r,rows,q,filtered);
        if(step>=(filter_first?2:3)) {
            // Runtime RED on the previous selector: the two query shapes reset
            // each other's probation forever, so neither cache reaches ready.
            const auto examined=filtered?std::count_if(rows.begin(),rows.end(),[](const Item& row){return row.category=="alpha";}):rows.size();
            CHECK(r.info.typed_rows_examined==static_cast<std::uint64_t>(examined));
            CHECK(r.info.snapshot_rows_examined==0&&r.info.snapshot_bytes==0);
            CHECK(r.info.typed_cache_bytes>0&&r.info.typed_cache_bytes<=8U*1024U*1024U);
        }
    }
}
void alternating(const std::filesystem::path& path,bool indexed,bool selective,bool filter_first) {
    Options options;options.typed_scan_cache=true;options.selective_decode=selective;
    Database db(path,options);const auto rows=fixture();seed(db,rows);
    if(indexed)db.execute("CREATE VECTOR INDEX tv ON t(v)");
    mixed_reads(db,rows,filter_first);
}
void numeric_reads(Database& db,const std::vector<Item>& rows,const std::string& table="t") {
    Result r;
    for(int phase=0;phase<3;++phase) {r=query(db,statement(phase-1,false,table));verify(r,rows,phase-1,false);}
    CHECK(r.info.typed_rows_examined==rows.size());CHECK(r.info.snapshot_rows_examined==0);
    CHECK(r.info.typed_cache_bytes>0&&r.info.snapshot_bytes==0);
    auto filtered=statement(0,false,table);const auto pos=filtered.find(" ORDER BY");
    filtered.insert(pos," WHERE g>=-9223372036854775808");
    r=query(db,filtered);verify(r,rows,0,false);CHECK(r.info.typed_rows_examined==rows.size());
}
template<class F>void error(ErrorCode expected,F&& f) {
    bool failed=false;try {f();}catch(const Error& e){CHECK(e.code()==expected);failed=true;}CHECK(failed);
}
void lifecycle(const std::filesystem::path& path,bool indexed) {
    auto rows=fixture();Options options;options.typed_scan_cache=true;
    {
        Database db(path,options);seed(db,rows);
        if(indexed)db.execute("CREATE VECTOR INDEX tv ON t(v)");
        numeric_reads(db,rows);mixed_reads(db,rows,true);
        error(ErrorCode::Constraint,[&]{db.execute("INSERT INTO t VALUES("+std::to_string(rows[0].id)+",'alpha',0,'[0,0]')");});
        numeric_reads(db,rows);mixed_reads(db,rows,true);
        db.execute("BEGIN");numeric_reads(db,rows);mixed_reads(db,rows,true);
        error(ErrorCode::Schema,[&]{db.execute("UPDATE t SET missing=1");});
        CHECK(db.in_transaction());numeric_reads(db,rows);mixed_reads(db,rows,true);
        auto changed=rows;changed[0].category="alpha";changed[0].g=99;changed[0].value={0,0};
        db.execute("UPDATE t SET category='alpha',g=99,v='[0,0]' WHERE id="+std::to_string(rows[0].id));
        numeric_reads(db,changed);mixed_reads(db,changed,true);
        db.execute("ROLLBACK");numeric_reads(db,rows);mixed_reads(db,rows,true);
        error(ErrorCode::Transaction,[&]{db.execute("ROLLBACK");});numeric_reads(db,rows);mixed_reads(db,rows,true);
        db.execute("BEGIN");numeric_reads(db,rows);mixed_reads(db,rows,true);
        db.execute("COMMIT");numeric_reads(db,rows);mixed_reads(db,rows,true);
        if(!indexed) {db.execute("CREATE VECTOR INDEX tv ON t(v)");numeric_reads(db,rows);mixed_reads(db,rows,true);}
        error(ErrorCode::Schema,[&]{db.execute("CREATE VECTOR INDEX invalid ON t(category)");});
        numeric_reads(db,rows);mixed_reads(db,rows,true);
        auto other=rows;for(auto& row:other){row.value[0]+=10;row.g=66;}
        seed(db,other,"u",true);mixed_reads(db,rows,true);
        numeric_reads(db,other,"u");numeric_reads(db,rows);
        mixed_reads(db,other,true,"u");numeric_reads(db,rows);
        mixed_reads(db,rows,true);
        bool stopped=false;
        try {db.execute(statement(0,true),{{},[](const Row&){throw std::runtime_error("sink stopped");}});}
        catch(const std::runtime_error& e){stopped=std::string(e.what())=="sink stopped";}
        CHECK(stopped);mixed_reads(db,rows,false);
    }
    Database reopened(path,options);numeric_reads(reopened,rows);mixed_reads(reopened,rows,false);
}
void refused_and_tiny(const std::filesystem::path& directory) {
    auto rows=fixture();for(auto& row:rows)if(row.category!="alpha")row.category+=std::string(90,'x');
    for(std::size_t budget:{0U,1U,100U,1024U})for(bool indexed:{false,true}) {
        Options options;options.typed_scan_cache=true;options.scan_snapshot_bytes=budget;
        Database db(directory/(std::to_string(budget)+std::to_string(indexed)+".db"),options);seed(db,rows);
        if(indexed)db.execute("CREATE VECTOR INDEX tv ON t(v)");
        for(int step=0;step<8;++step) {
            const bool filtered=step%2==0;
            const auto r=query(db,statement(0,filtered));verify(r,rows,0,filtered);
            CHECK(r.info.typed_rows_examined==0&&r.info.snapshot_rows_examined==0);
            CHECK(r.info.typed_cache_bytes==0&&r.info.snapshot_bytes==0);
        }
        // Numeric payload fits 1024 bytes, but the observed oversized raw choice
        // must remain refused until version invalidation, not retry on each read.
        for(int step=0;step<3;++step) {
            const auto r=query(db,statement(0,false));verify(r,rows,0,false);
            CHECK(r.info.typed_rows_examined==0&&r.info.typed_cache_bytes==0&&r.info.snapshot_bytes==0);
        }
        if(budget==1024) {
            db.execute("UPDATE t SET g=0 WHERE id=9223372036854775807");
            numeric_reads(db,rows);
        }
    }
}
void raw_state_contract() {
    sql::ScanSnapshot cache;
    CHECK(!cache.observed(1,2));cache.start(1,2,64);
    CHECK(cache.observed(1,2)&&!cache.ready(1,2)); // probation is sticky
    cache.start(1,2,64);CHECK(cache.building());cache.append(7,std::string(100,'x'));cache.finish();
    CHECK(cache.observed(1,2)&&!cache.ready(1,2)&&cache.bytes()==0); // refused is sticky
    for(int i=0;i<4;++i) {cache.start(1,2,64);CHECK(!cache.building()&&cache.observed(1,2)&&cache.bytes()==0);}
    CHECK(!cache.observed(1,3)&&!cache.observed(2,2));
    cache.invalidate();CHECK(!cache.observed(1,2));
    cache.start(1,2,64);cache.start(1,2,64);cache.append(8,"valid");cache.finish();
    CHECK(cache.ready(1,2)&&cache.observed(1,2));
    cache.start(1,3,64);CHECK(cache.observed(1,3)&&!cache.observed(1,2)&&!cache.ready(1,3));
    cache.clear();CHECK(!cache.observed(1,3)&&cache.bytes()==0);
}
void malformed_row(const std::filesystem::path& path) {
    const auto rows=fixture();Options options;options.typed_scan_cache=true;
    {Database db(path,options);seed(db,rows);mixed_reads(db,rows,true);}
    const auto& item=rows.front();
    const Schema schema{1,"t",{{"id",ColumnType::Bigint,0},{"category",ColumnType::Varchar,100},
        {"g",ColumnType::Bigint,0},{"v",ColumnType::Vector,2}},0};
    auto bad=encode_row(schema,{item.id,item.category,item.g,item.value});
    // Version/lengths remain correct; this candidate is filtered out by alpha,
    // but loading either cache still has to reject its invalid UTF-8 payload.
    CHECK(item.category!="alpha");bad[17]=char(0x80);
    {Store store(path);store.begin();store.put_in(1,item.id,bad);store.commit();}
    Database db(path,options);
    for(int phase=0;phase<4;++phase)
        error(ErrorCode::Corruption,[&]{query(db,statement(0,phase%2==0));});
}
}
int main() {
    try {
        const char* root_env=std::getenv("MVDB_TEST_DIR");
        const auto parent=std::filesystem::path(root_env?root_env:"/data/core-tests");
        std::filesystem::create_directories(parent);
        std::string name=(parent/"mixed-filter-XXXXXX").string();CHECK(::mkdtemp(name.data()));
        const std::filesystem::path root(name);
        for(bool indexed:{false,true})for(bool selective:{false,true})for(bool first:{false,true})
            alternating(root/(std::to_string(indexed)+std::to_string(selective)+std::to_string(first)+".db"),indexed,selective,first);
        for(bool indexed:{false,true})lifecycle(root/("lifecycle"+std::to_string(indexed)),indexed);
        std::filesystem::create_directories(root/"budget");refused_and_tiny(root/"budget");
        raw_state_contract();malformed_row(root/"malformed");
        std::filesystem::remove_all(root);
        std::cout<<"mixed filter cache tests passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
