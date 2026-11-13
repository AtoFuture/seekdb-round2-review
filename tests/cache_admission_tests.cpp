#include "minivectordb/database.hpp"
#include "../src/sql/scan_cache.hpp"
#include "../src/sql/typed_scan_cache.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <string>
#include <system_error>
#include <unistd.h>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)

namespace {
thread_local bool track_arrays=false;
thread_local std::size_t array_allocations=0;
bool fail_read=false;
std::string failure_needle;
unsigned failed_reads=0;
}
// Track allocations, not final retained bytes: an allocate/fill/refuse cycle
// must not be mistaken for successful pre-admission simply because it frees.
void* operator new[](std::size_t size) {
    if(auto* p=std::malloc(size?size:1)) {
        if(track_arrays)++array_allocations;
        return p;
    }
    throw std::bad_alloc();
}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* buffer,size_t size,off_t offset) {
    const auto result=__real_pread(fd,buffer,size,offset);
    if(fail_read&&result>0) {
        const auto* first=static_cast<const char*>(buffer);
        if(std::search(first,first+result,failure_needle.begin(),failure_needle.end())!=first+result) {
            ++failed_reads;errno=EIO;return -1;
        }
    }
    return result;
}
namespace {
struct Item {std::int64_t id,g;float value;std::string label;};
struct Result {std::vector<Row> rows;ExecutionInfo info;std::size_t arrays=0;};
std::string vector_literal(float value) {
    std::string result="[";
    for(unsigned i=0;i<128;++i){if(i)result+=',';result+=std::to_string(value);}
    return result+"]";
}
std::vector<Item> fixture(unsigned count) {
    std::vector<Item> result;
    for(unsigned i=0;i<count;++i)
        result.push_back({std::int64_t(i)-8,std::int64_t(i%3),float(int(i%7)-3),
                          "row-"+std::to_string(i)+(i%2?"中":"é")});
    return result;
}
void seed(Database& db,const std::string& table,const std::vector<Item>& rows) {
    db.execute("CREATE TABLE "+table+"(id BIGINT PRIMARY KEY,g BIGINT,v VECTOR(128),label VARCHAR(1000))");
    for(const auto& row:rows)
        db.execute("INSERT INTO "+table+" VALUES("+std::to_string(row.id)+","+std::to_string(row.g)+",'"+
                   vector_literal(row.value)+"','"+row.label+"')");
}
std::string query_text(const std::string& table,const std::string& predicate={}) {
    return "SELECT id,L2_DISTANCE(v,'"+vector_literal(0)+"') AS d,g,label FROM "+table+
           (predicate.empty()?"":" WHERE "+predicate)+" ORDER BY d LIMIT 5";
}
Result query(Database& db,const std::string& table,const std::string& predicate={}) {
    const auto text=query_text(table,predicate);Result result;
    array_allocations=0;track_arrays=true;
    try {result.info=db.execute(text,{{},[&](const Row& row){result.rows.push_back(row);}});}
    catch(...) {track_arrays=false;throw;}
    track_arrays=false;result.arrays=array_allocations;return result;
}
// Full-sort oracle uses fixture values, never production codecs or TopK.
std::vector<Row> oracle(const std::vector<Item>& items) {
    std::vector<Row> rows;
    for(const auto& item:items)rows.push_back({item.id,std::sqrt(128.0*double(item.value)*double(item.value)),item.g,item.label});
    std::sort(rows.begin(),rows.end(),[](const Row& a,const Row& b) {
        const auto x=std::get<double>(a[1]),y=std::get<double>(b[1]);
        return x<y||(x==y&&std::get<std::int64_t>(a[0])<std::get<std::int64_t>(b[0]));
    });
    if(rows.size()>5)rows.resize(5);return rows;
}
Options settings(bool typed) {
    Options o;o.typed_scan_cache=typed;o.scan_snapshot_bytes=8192;
    o.index_cache_bytes=o.data_cache_bytes=o.packed_scan_cache_bytes=0;
    o.packed_values=false; // Each fault needle belongs to one row's overflow page.
    return o;
}
void oversize(Database& db,const std::string& table,const std::vector<Item>& rows) {
    // A rejecting filter must still measure ALL rows; otherwise the second
    // query allocates an arena and only discovers overflow while filling it.
    for(unsigned i=0;i<4;++i) {
        const auto r=query(db,table,"g=99");CHECK(r.rows.empty());CHECK(r.arrays==0);
        CHECK(r.info.rows_examined==rows.size());
        if(i)CHECK(r.info.snapshot_bytes==0&&r.info.typed_cache_bytes==0);
    }
    const auto r=query(db,table);CHECK(r.rows==oracle(rows));CHECK(r.arrays==0);
}
void fits(Database& db,const std::string& table,const std::vector<Item>& rows,bool typed) {
    for(unsigned phase=0;phase<3;++phase)CHECK(query(db,table).rows==oracle(rows));
    const auto r=query(db,table);
    CHECK((typed?r.info.typed_rows_examined:r.info.snapshot_rows_examined)==rows.size());
    CHECK(r.arrays==0);
}
void lifecycle(const std::filesystem::path& path,bool typed,bool indexed) {
    const auto large=fixture(32),small=fixture(8);
    Database db(path,settings(typed));seed(db,"t",large);seed(db,"u",small);
    if(indexed)db.execute("CREATE VECTOR INDEX tv ON t(v)");
    oversize(db,"t",large);fits(db,"u",small,typed);oversize(db,"t",large);
    // A transaction may change both sides of the admission boundary. Rollback
    // must discard the small-table measurement and its completed cache.
    db.execute("BEGIN");db.execute("DELETE FROM t WHERE id>=0");fits(db,"t",small,typed);
    db.execute("ROLLBACK");oversize(db,"t",large);
    db.execute("BEGIN");db.execute("DELETE FROM t WHERE id>=0");db.execute("COMMIT");fits(db,"t",small,typed);
    bool rejected=false;
    try {db.execute("INSERT INTO t VALUES(-8,0,'"+vector_literal(0)+"','duplicate')");}
    catch(const Error& e){CHECK(e.code()==ErrorCode::Constraint);rejected=true;}
    CHECK(rejected);CHECK(query(db,"t").info.typed_rows_examined==0);
    fits(db,"t",small,typed);
    if(!indexed){db.execute("CREATE VECTOR INDEX tv ON t(v)");fits(db,"t",small,typed);}
}
void first_scan_failure(const std::filesystem::path& path,bool typed,bool indexed) {
    const auto rows=fixture(32);Database db(path,settings(typed));seed(db,"t",rows);
    if(indexed)db.execute("CREATE VECTOR INDEX tv ON t(v)");
    failure_needle=rows.back().label;fail_read=true;failed_reads=0;
    for(unsigned attempt=0;attempt<2;++attempt) {
        bool rejected=false;
        try {query(db,"t","g=99");}
        catch(const std::system_error& error){CHECK(error.code().value()==EIO);rejected=true;}
        CHECK(rejected&&failed_reads==attempt+1);
    }
    fail_read=false;failure_needle.clear();oversize(db,"t",rows);
}
void text_fallback(const std::filesystem::path& path,bool indexed) {
    auto rows=fixture(6);rows.back().label.clear();
    for(unsigned i=0;i<975;++i)rows.back().label+="😀";
    Database db(path,settings(true));seed(db,"t",rows);
    if(indexed)db.execute("CREATE VECTOR INDEX tv ON t(v)");
    // One long UTF-8 value makes even measured equal-width slots too large,
    // while the sum of encoded rows fits raw snapshots. Admission must not
    // allocate typed slots or let that refusal suppress the raw alternative.
    for(unsigned phase=0;phase<2;++phase) {
        const auto r=query(db,"t","label>=''");CHECK(r.rows==oracle(rows));CHECK(r.arrays==0);
    }
    CHECK(query(db,"t","label>=''").rows==oracle(rows));
    const auto warm=query(db,"t");CHECK(warm.rows==oracle(rows));
    CHECK(warm.info.snapshot_rows_examined==rows.size()&&warm.info.typed_rows_examined==0);
    db.execute("UPDATE t SET g=0 WHERE id=999999");fits(db,"t",rows,true);
}
void measured_text(const std::filesystem::path& path) {
    auto rows=fixture(8);Database db(path,settings(true));seed(db,"t",rows);
    for(unsigned i=0;i<4;++i)CHECK(query(db,"t","label>=''").rows==oracle(rows));
    auto warm=query(db,"t","label>=''");
    CHECK(warm.info.typed_rows_examined==rows.size()&&warm.info.snapshot_rows_examined==0);
    std::size_t width=0;for(const auto&r:rows)width=std::max(width,r.label.size());
    CHECK(warm.info.typed_cache_bytes==16+rows.size()*(128*4+16+4+width));
    // Both expansion and rollback must remeasure the actual maximum; retained
    // arrays from the smaller width cannot be reused at a different stride.
    db.execute("BEGIN");rows[0].label=std::string(300,'x');
    db.execute("UPDATE t SET label='"+rows[0].label+"' WHERE id="+std::to_string(rows[0].id));
    for(unsigned i=0;i<4;++i)CHECK(query(db,"t","label>=''").rows==oracle(rows));
    warm=query(db,"t","label>=''");CHECK(warm.info.typed_rows_examined==rows.size());
    CHECK(warm.info.typed_cache_bytes==16+rows.size()*(128*4+16+4+300));
    db.execute("ROLLBACK");rows=fixture(8);
    for(unsigned i=0;i<4;++i)CHECK(query(db,"t","label>=''").rows==oracle(rows));
    warm=query(db,"t","label>=''");CHECK(warm.info.typed_cache_bytes==16+rows.size()*(128*4+16+4+width));
}
void actual_capacity() {
    const Schema schema{10,"growing",{{"id",ColumnType::Bigint,0},{"g",ColumnType::Bigint,0},{"v",ColumnType::Vector,128}},0};
    sql::TypedScanCache cache;
    const std::size_t budget=64*1024*1024;
    std::size_t previous_bytes=0;
    for(const std::size_t count:{1U,17U,30000U,8U,40000U}) {
        cache.invalidate();cache.start(schema,0,2,budget);
        for(std::size_t i=0;i<count;++i)cache.observe_row();
        cache.finish();cache.start(schema,0,2,budget);CHECK(cache.building());
        const auto needed=16+count*(128*4+16);
        CHECK(cache.bytes()==std::max(needed,previous_bytes));previous_bytes=cache.bytes();
        std::vector<float> values(128);
        for(std::size_t i=0;i<count;++i) {
            for(std::size_t d=0;d<128;++d)values[d]=float((i*7+d*13)%101)-50;
            cache.append(values,[&](std::size_t column){return std::int64_t(column?i%10:i);});
        }
        cache.finish(true,true);CHECK(cache.ready(10,0,2)&&cache.size()==count);
        std::vector<float> scratch;
        for(std::size_t row=0;row<count;++row) {
            const auto id=static_cast<std::size_t>(cache.key(row));CHECK(id<count);
            CHECK(cache.scalar(row,1)==std::int64_t(id%10));
            const auto actual=cache.read_vector(row,scratch);
            for(std::size_t d=0;d<128;++d)CHECK(actual[d]==float((id*7+d*13)%101)-50);
        }
    }
    // Completed over-budget measurements refuse BEFORE any allocation, even
    // when an arena from a smaller table exists and reuse was requested.
    cache.invalidate();cache.start(schema,0,2,budget);
    for(std::size_t i=0;i<budget/528+1;++i)cache.observe_row();
    cache.finish();array_allocations=0;track_arrays=true;
    cache.start(schema,0,2,budget);track_arrays=false;
    CHECK(cache.refused()&&cache.bytes()==0&&array_allocations==0);
}
void exact_boundaries() {
    // Raw admission uses actual encoded lengths, including both headers and
    // multi-byte text, rather than VARCHAR's declared maximum or character count.
    const Schema schema{7,"x",{{"id",ColumnType::Bigint,0},{"v",ColumnType::Vector,2},{"label",ColumnType::Varchar,1000}},0};
    const auto a=encode_row(schema,{std::int64_t(1),std::vector<float>{0,-0.f},std::string("é")});
    const auto b=encode_row(schema,{std::int64_t(2),std::vector<float>{1,2},std::string("中")});
    const auto required=32+a.size()+b.size();
    for(const auto budget:{required-1,required}) {
        sql::ScanSnapshot cache;cache.start(7,0,budget);
        cache.append(1,a);cache.append(2,b);cache.finish();CHECK(!cache.ready(7,0));
        array_allocations=0;track_arrays=true;cache.start(7,0,budget);track_arrays=false;
        CHECK(cache.building()==(budget==required));CHECK(array_allocations==(budget==required?1U:0U));
        if(budget==required) {
            cache.append(1,a);cache.append(2,b);cache.finish();CHECK(cache.ready(7,0));
            std::vector<std::string> seen;cache.scan([&](auto,std::string_view raw){seen.emplace_back(raw);});
            CHECK(seen==std::vector<std::string>({a,b}));
        } else for(unsigned i=0;i<3;++i){cache.start(7,0,budget);CHECK(!cache.building()&&cache.bytes()==0);}
        // Changing only the index identity must discard the previous count.
        cache.start(7,19,budget);cache.append(1,a);cache.finish();cache.start(7,19,budget);
        CHECK(cache.building());
    }
    // Incomplete measurements never prove rejection. Production exceptions
    // clear the cache; this also checks the cache API's conservative fallback.
    sql::ScanSnapshot partial;partial.start(7,0,32);partial.append(1,a);partial.start(7,0,32);
    CHECK(partial.building());
    Schema numeric{9,"n",{{"id",ColumnType::Bigint,0},{"g",ColumnType::Bigint,0},{"v",ColumnType::Vector,2}},0};
    for(std::size_t budget:{63U,64U}) {
        sql::TypedScanCache cache;cache.start(numeric,0,2,budget);
        cache.observe_row();cache.observe_row();cache.finish();
        array_allocations=0;track_arrays=true;cache.start(numeric,0,2,budget);track_arrays=false;
        CHECK(cache.building()==(budget==64));CHECK((array_allocations>0)==(budget==64));
        if(budget==63) {
            CHECK(cache.refused());cache.start(numeric,0,2,budget);CHECK(cache.refused());
            cache.invalidate();cache.start(numeric,0,2,budget);cache.observe_row();cache.finish();
            cache.start(numeric,0,2,budget);CHECK(cache.building());
        }
    }
    sql::TypedScanCache partial_typed;partial_typed.start(numeric,0,2,64);
    for(unsigned i=0;i<3;++i)partial_typed.observe_row();
    partial_typed.start(numeric,0,2,64);CHECK(partial_typed.building());
}
}
int main() {
    try {
        exact_boundaries();actual_capacity();const char* env=std::getenv("MVDB_TEST_DIR");CHECK(env);
        std::filesystem::create_directories(env);auto name=(std::filesystem::path(env)/"admission-XXXXXX").string();
        CHECK(mkdtemp(name.data()));const std::filesystem::path dir(name);
        for(bool typed:{false,true})for(bool indexed:{false,true}) {
            const auto suffix=std::to_string(typed)+std::to_string(indexed);
            lifecycle(dir/("life-"+suffix),typed,indexed);
            first_scan_failure(dir/("fault-"+suffix),typed,indexed);
        }
        for(bool indexed:{false,true})text_fallback(dir/("text-"+std::to_string(indexed)),indexed);
        measured_text(dir/"measured-text");
        std::filesystem::remove_all(dir);
        std::cout<<"PASS cache pre-admission: no rejected-arena allocation, full-scan oracle, encoded-byte boundaries, identity/transaction/failure invalidation\n";
    } catch(const std::exception& e){track_arrays=false;fail_read=false;std::cerr<<e.what()<<'\n';return 1;}
}
