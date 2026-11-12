#include "minivectordb/database.hpp"
#include <algorithm>
#include "minivectordb/profile.hpp"
#include "../src/storage/page_checksum.hpp"
#include "../src/types/text_validation.hpp"
#include "../src/storage/row_cpu.hpp"
#include <ctime>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <unordered_map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>
#include <unistd.h>

using namespace minivectordb;
using Clock = std::chrono::steady_clock;
std::size_t dimensions = 128;
std::size_t groups = 10;
bool churn = false;
std::vector<std::int64_t> keys;
std::unordered_map<std::int64_t,std::size_t> key_positions;
constexpr std::size_t batch_size = 100;
constexpr std::uint32_t seed = 20261109;
struct Record { std::vector<float> vector; std::string text; std::int64_t group; };
struct Query { std::vector<float> vector; std::string sql; bool filtered; std::int64_t group; };
struct Hit { std::int64_t id; double distance; std::string text; };
std::size_t number(const char* arg) {
    std::string_view value(arg);
    std::size_t n = 0;
    auto [end, error] = std::from_chars(value.data(), value.data()+value.size(), n);
    if (error != std::errc{} || end != value.data()+value.size()) throw std::invalid_argument("invalid integer");
    return n;
}
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double,std::milli>(Clock::now()-start).count();
}
std::uint64_t resident_bytes() {
    std::ifstream statm("/proc/self/statm");
    std::uint64_t total=0,resident=0;
    statm>>total>>resident;
    const auto page_size=sysconf(_SC_PAGESIZE);
    return page_size>0?resident*static_cast<std::uint64_t>(page_size):0;
}
std::vector<float> random_vector(std::mt19937& rng) {
    std::vector<float> result(dimensions);
    for (auto& v : result) v = static_cast<float>(static_cast<int>(rng()%4097)-2048)/128.0f;
    return result;
}
std::string literal(const std::vector<float>& values) {
    std::ostringstream out;
    out << std::setprecision(9) << '[';
    for (std::size_t i=0;i<values.size();++i) {if(i)out<<',';out<<values[i];}
    out << ']';
    return out.str();
}
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::vector<Hit> truth(const std::vector<Record>& records, const Query& q) {
    std::vector<std::pair<double,std::int64_t>> ranks;
    for(std::size_t i=0;i<records.size();++i) {
        const auto& row=records[i];
        if(churn && i >= records.size()*3/4)continue;
        if(q.filtered&&row.group!=q.group)continue;
        double sum=0;
        for(std::size_t d=0;d<dimensions;++d) {double delta=double(row.vector[d])-q.vector[d];sum+=delta*delta;}
        ranks.emplace_back(std::sqrt(sum),keys[i]);
    }
    std::sort(ranks.begin(),ranks.end());
    if(ranks.size()>10)ranks.resize(10);
    std::vector<Hit> result;
    for(auto [distance,id]:ranks) result.push_back({id,distance,records[key_positions.at(id)].text});
    return result;
}
std::uint64_t hash_bytes(std::uint64_t hash, const void* raw, std::size_t size) {
    const auto* p=static_cast<const unsigned char*>(raw);
    for(std::size_t i=0;i<size;++i){hash^=p[i];hash*=1099511628211ULL;}
    return hash;
}
int main(int argc,char** argv) {
    try {
        require(argc==6,"usage: buffer-bench database-path rows queries selective batch");
        const std::filesystem::path path(argv[1]);
        const auto count=number(argv[2]),query_count=number(argv[3]);
        const auto selective=number(argv[4]),batch=number(argv[5]);
        require(count>=20&&count<=1000000&&query_count>=2&&query_count<=10000&&query_count%2==0,"invalid workload size");
        require(selective<=1&&batch<=1,"flags must be 0 or 1");
        require(!std::filesystem::exists(path),"refusing to overwrite existing database");
        std::filesystem::create_directories(path.parent_path());
        dimensions=std::getenv("MVDB_DIMENSIONS")?number(std::getenv("MVDB_DIMENSIONS")):128;
        groups=std::getenv("MVDB_GROUPS")?number(std::getenv("MVDB_GROUPS")):10;
        const auto payload_bytes=std::getenv("MVDB_PAYLOAD")?number(std::getenv("MVDB_PAYLOAD")):256;
        const auto extra_columns=std::getenv("MVDB_EXTRA_COLUMNS")?number(std::getenv("MVDB_EXTRA_COLUMNS")):0;
        const bool unicode_body=std::getenv("MVDB_UNICODE_BODY")!=nullptr;
        const bool text_scalar=std::getenv("MVDB_TEXT_SCALAR")!=nullptr;
        detail::text_validation_mode=text_scalar?detail::TextValidationMode::scalar:detail::TextValidationMode::ascii_blocks;
        const bool vector_scalar=std::getenv("MVDB_VECTOR_SCALAR")!=nullptr;
        const bool row_heap=std::getenv("MVDB_ROW_HEAP")!=nullptr;
        detail::vector_validation_mode=vector_scalar?detail::VectorValidationMode::scalar:detail::VectorValidationMode::batched;
        detail::row_metadata_heap=row_heap;
        const bool ordered=std::getenv("MVDB_ORDERED")!=nullptr;
        const bool index_present=std::getenv("MVDB_NO_INDEX")==nullptr;
        churn=std::getenv("MVDB_CHURN")!=nullptr;
        require(dimensions>=1&&dimensions<=8192&&groups>=1&&groups<=100&&payload_bytes<=600000,"invalid dataset");
        require(extra_columns<=64,"too many extra columns");
        require(count*(dimensions*4+payload_bytes+128+extra_columns*8)<=128*1024*1024,"harness dataset exceeds 128MiB bound");
        std::mt19937 rng(seed);
        std::vector<Record> records;
        records.reserve(count);
        for(std::size_t i=0;i<count;++i)
            records.push_back({random_vector(rng),"record-"+std::to_string(i)+"-"+std::string(payload_bytes,'a'+static_cast<char>(i%26)),static_cast<std::int64_t>(i%groups)});
        const bool sparse=std::getenv("MVDB_SPARSE_KEYS")!=nullptr;
        const auto invalidate_every=std::getenv("MVDB_INVALIDATE_EVERY")?number(std::getenv("MVDB_INVALIDATE_EVERY")):0;
        require(!(sparse&&churn),"separate sparse and churn cases");
        for(std::size_t i=0;i<count;++i) {
            std::uint64_t key=i;
            if(sparse) { key+=0x9e3779b97f4a7c15ULL;key=(key^(key>>30))*0xbf58476d1ce4e5b9ULL;key=(key^(key>>27))*0x94d049bb133111ebULL;key^=key>>31; }
            keys.push_back(std::bit_cast<std::int64_t>(key));key_positions.emplace(keys.back(),i);
        }
        if(unicode_body)for(auto& record:records) {
            record.text.clear();
            // Mixed two/three/four-byte code points, with ASCII at block boundaries.
            while(record.text.size()+10<=payload_bytes)record.text+="a\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80";
            record.text.append(payload_bytes-record.text.size(),'x');
        }
        std::vector<std::size_t> recovery_order(count);std::iota(recovery_order.begin(),recovery_order.end(),0);
        std::sort(recovery_order.begin(),recovery_order.end(),[&](auto a,auto b){return keys[a]<keys[b];});
        std::vector<std::size_t> insertion_order(count);
        std::iota(insertion_order.begin(),insertion_order.end(),0);
        if(!ordered)std::shuffle(insertion_order.begin(),insertion_order.end(),rng);
        std::vector<Query> queries;
        for(std::size_t i=0;i<query_count;++i) {
            Query q{random_vector(rng),{},i%2==1,static_cast<std::int64_t>((i/2)%groups)};
            q.sql="SELECT id,L2_DISTANCE(v,"+literal(q.vector)+") AS d,body FROM t";
            if(q.filtered)q.sql+=" WHERE g="+std::to_string(q.group);
            q.sql+=" ORDER BY d LIMIT 10";
            queries.push_back(std::move(q));
        }
        Options options{path.parent_path()/"tmp",8*1024*1024};
        options.selective_decode=selective!=0;
        options.batch_writes=batch!=0;
        options.index_cache_bytes=1024*1024;
        options.packed_values=std::getenv("MVDB_PACKED")!=nullptr;
        options.packed_scan_cache_bytes=std::getenv("MVDB_SCAN_CACHE_BYTES")?number(std::getenv("MVDB_SCAN_CACHE_BYTES")):262144;
        options.paired_index_scan=std::getenv("MVDB_PAIRED")!=nullptr;
        options.scan_snapshot_bytes=std::getenv("MVDB_SNAPSHOT_BYTES")?number(std::getenv("MVDB_SNAPSHOT_BYTES")):0;
        options.typed_scan_cache=std::getenv("MVDB_TYPED_CACHE") && number(std::getenv("MVDB_TYPED_CACHE"))!=0;
        const auto table_snapshot=std::getenv("MVDB_TABLE_SNAPSHOT")?number(std::getenv("MVDB_TABLE_SNAPSHOT")):1;
        require(table_snapshot<=1,"MVDB_TABLE_SNAPSHOT must be 0 or 1");
        options.table_scan_snapshot=table_snapshot!=0;
        const auto reuse_snapshot=std::getenv("MVDB_REUSE_SNAPSHOT")?number(std::getenv("MVDB_REUSE_SNAPSHOT")):1;
        require(reuse_snapshot<=1,"MVDB_REUSE_SNAPSHOT must be 0 or 1");
        options.reuse_scan_snapshot_buffer=reuse_snapshot!=0;
        const auto vector_scratch=std::getenv("MVDB_VECTOR_SCRATCH")?number(std::getenv("MVDB_VECTOR_SCRATCH")):1;
        require(vector_scratch<=1,"MVDB_VECTOR_SCRATCH must be 0 or 1");
        options.reuse_vector_scratch=vector_scratch!=0;
        options.data_cache_bytes=std::getenv("MVDB_DATA_CACHE_BYTES")?number(std::getenv("MVDB_DATA_CACHE_BYTES")):0;
        const bool bytewise=std::getenv("MVDB_CRC_BYTEWISE")!=nullptr;
        detail::page_checksum_mode=bytewise?detail::PageChecksumMode::bytewise:detail::PageChecksumMode::slicing_by_8;
        CacheStats write_cache{},query_cache{},write_data_cache{},query_data_cache{};
        double first_query_ms=0,second_query_ms=0,index_build_ms=0;
        std::uintmax_t after_index_bytes=0;
        std::vector<Hit> first_results;
        double write_ms=0,all_ms=0,filtered_ms=0,write_cpu_ms=0,query_cpu_ms=0,churn_ms=0,churn_cpu_ms=0;
        const bool profiling=std::getenv("MVDB_PROFILE")!=nullptr;
        profile::Snapshot write_profile{},query_profile{};
        profile::set_enabled(profiling);
        std::uint64_t snapshot_rows=0,typed_rows=0; std::size_t snapshot_capacity=0,typed_capacity=0;
        std::uint64_t warm_resident=0,invalidated_resident=0;
        long query_minor_faults=0;
        std::uint64_t vector_cells=0,filtered_vectors=0,unfiltered_vectors=0,cells=0,examined=0,distances=0;
        std::vector<std::vector<Hit>> results(query_count);
        std::uintmax_t empty_bytes=0,database_bytes=0,after_insert_bytes=0;
        {
            Database db(path,options);
            std::string schema_sql="CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR("+std::to_string(dimensions)+"),g BIGINT,body VARCHAR(700000)";
            for(std::size_t n=0;n<extra_columns;++n)schema_sql+=",extra"+std::to_string(n)+" BIGINT";
            db.execute(schema_sql+")");
            empty_bytes=std::filesystem::file_size(path);
            profile::reset();
            db.reset_cache_stats(); db.reset_data_cache_stats();
            for(std::size_t begin=0;begin<count;begin+=batch_size) {
                std::string sql="INSERT INTO t VALUES";
                const auto end=std::min(begin+batch_size,count);
                for(std::size_t j=begin;j<end;++j) {
                    if(j!=begin)sql+=',';
                    const auto id=insertion_order[j];
                    const auto& row=records[id];
                    sql+="("+std::to_string(keys[id])+","+literal(row.vector)+","+std::to_string(row.group)+",'"+row.text+"'";
                    for(std::size_t n=0;n<extra_columns;++n)sql+=","+std::to_string(n+id);
                    sql+=")";
                }
                const auto cpu_start=std::clock();
                const auto start=Clock::now();
                const auto info=db.execute(sql);
                write_ms+=elapsed(start);
                write_cpu_ms+=1000.0*(std::clock()-cpu_start)/CLOCKS_PER_SEC;
                require(info.affected_rows==end-begin,"write count mismatch");
            }
            write_profile=profile::snapshot();
            write_cache=db.cache_stats(); write_data_cache=db.data_cache_stats();
            after_insert_bytes=std::filesystem::file_size(path);
            if(index_present) {
                const auto index_start=Clock::now();
                db.execute("CREATE VECTOR INDEX tv ON t(v)");
                index_build_ms=elapsed(index_start);
            }
            after_index_bytes=std::filesystem::file_size(path);
            if(churn) {
                const auto start=Clock::now(); const auto cpu=std::clock();
                db.execute("UPDATE t SET g=999 WHERE id>="+std::to_string(count/2)+" AND id<"+std::to_string(count*3/4));
                db.execute("DELETE FROM t WHERE id>="+std::to_string(count*3/4));
                churn_ms=elapsed(start); churn_cpu_ms=1000.0*(std::clock()-cpu)/CLOCKS_PER_SEC;
                for(std::size_t i=count/2;i<count*3/4;++i)records[i].group=999;
            }
            database_bytes=std::filesystem::file_size(path);
            std::cerr<<"write complete "<<write_ms<<" ms; query warm-up\n";
            for(std::size_t i=0;i<2;++i) {
                const auto start=Clock::now();
                db.execute(queries[i].sql,{{},[&](const Row& row){
                    if(i==0)first_results.push_back({std::get<std::int64_t>(row[0]),std::get<double>(row[1]),std::get<std::string>(row[2])});
                }});
                if(i==0)first_query_ms=elapsed(start);
                else second_query_ms=elapsed(start);
            }
            profile::reset();
            db.reset_cache_stats(); db.reset_data_cache_stats();
            warm_resident=resident_bytes();
            rusage before_queries{};
            require(getrusage(RUSAGE_SELF,&before_queries)==0,"getrusage failed");
            for(std::size_t i=0;i<queries.size();++i) {
                if(invalidate_every && i%invalidate_every==0) {
                    const auto start=Clock::now(); const auto cpu=std::clock();
                    db.execute("UPDATE t SET g="+std::to_string(records[0].group)+" WHERE id="+std::to_string(keys[0]));
                    churn_ms+=elapsed(start);churn_cpu_ms+=1000.0*(std::clock()-cpu)/CLOCKS_PER_SEC;
                    if(i==0)invalidated_resident=resident_bytes();
                }
                auto& output=results[i];
                const auto cpu_start=std::clock();
                const auto start=Clock::now();
                auto info=db.execute(queries[i].sql,{{},[&](const Row& row) {
                    output.push_back({std::get<std::int64_t>(row[0]),std::get<double>(row[1]),std::get<std::string>(row[2])});
                }});
                const auto ms=elapsed(start);
                query_cpu_ms+=1000.0*(std::clock()-cpu_start)/CLOCKS_PER_SEC;
                (queries[i].filtered?filtered_ms:all_ms)+=ms;
                snapshot_rows+=info.snapshot_rows_examined; snapshot_capacity=std::max(snapshot_capacity,info.snapshot_bytes); typed_rows+=info.typed_rows_examined; typed_capacity=std::max(typed_capacity,info.typed_cache_bytes);
                vector_cells+=info.vector_values_materialized;
                (queries[i].filtered?filtered_vectors:unfiltered_vectors)+=info.vector_values_materialized;
                cells+=info.cells_materialized;
                examined+=info.rows_examined;
                distances+=info.distances_computed;
            }
            rusage after_queries{};
            require(getrusage(RUSAGE_SELF,&after_queries)==0,"getrusage failed");
            query_minor_faults=after_queries.ru_minflt-before_queries.ru_minflt;
            database_bytes=std::filesystem::file_size(path);
            query_profile=profile::snapshot();
            query_cache=db.cache_stats(); query_data_cache=db.data_cache_stats();
            profile::set_enabled(false);
        }
        // Capture diagnostics before the independent checker does additional IO/work.
        rusage usage{};
        require(getrusage(RUSAGE_SELF,&usage)==0,"getrusage failed");
        std::uint64_t local_peak=0;
        if(std::getenv("MVDB_BENCH_CGROUP")) {
            std::ifstream peak("/sys/fs/cgroup/memory.peak");
            peak>>local_peak;
        }
        const auto first_expected=truth(records,queries[0]);
        require(first_results.size()==first_expected.size(),"first query size mismatch");
        for(std::size_t i=0;i<first_expected.size();++i)
            require(first_results[i].id==first_expected[i].id && first_results[i].text==first_expected[i].text &&
                first_results[i].distance==first_expected[i].distance,"first query exact result mismatch");
        std::uint64_t digest=1469598103934665603ULL;
        for(std::size_t i=0;i<queries.size();++i) {
            const auto expected=truth(records,queries[i]);
            const auto& actual=results[i];
            require(actual.size()==expected.size(),"query count differs from exact truth");
            for(std::size_t j=0;j<expected.size();++j) {
                require(actual[j].id==expected[j].id&&actual[j].text==expected[j].text,"query row/order differs from exact truth");
                require(std::abs(actual[j].distance-expected[j].distance)<=1e-12*std::max(1.0,expected[j].distance),"distance differs from exact truth");
                digest=hash_bytes(digest,&actual[j].id,sizeof(actual[j].id));
                digest=hash_bytes(digest,&actual[j].distance,sizeof(actual[j].distance));
                digest=hash_bytes(digest,actual[j].text.data(),actual[j].text.size());
            }
        }
        std::size_t recovered=0;
        {
            Database reopened(path,options);
            reopened.execute("SELECT * FROM t",{{},[&](const Row& row) {
                require(recovered<recovery_order.size(),"too many recovered rows");
                const auto id=std::get<std::int64_t>(row[0]);
                require(key_positions.contains(id),"invalid recovered primary key");
                require(id==keys[recovery_order[recovered]],"recovered key set/order differs");
                const auto& original=records[key_positions.at(id)];
                require(row.size()==4+extra_columns,"recovered column count differs");
                for(std::size_t n=0;n<extra_columns;++n)
                    require(std::get<std::int64_t>(row[4+n])==static_cast<std::int64_t>(n+key_positions.at(id)),"recovered extra column differs");
                require(std::get<std::vector<float>>(row[1])==original.vector&&
                        std::get<std::int64_t>(row[2])==original.group&&
                        std::get<std::string>(row[3])==original.text,"recovered row differs");
                ++recovered;
            }});
        }
        require(recovered==(churn?count*3/4:count),"recovered row count differs");
        auto emit_profile=[](const profile::Snapshot& s) {
            std::cout<<"{";
            bool first=true;
            auto metric=[&](const char* name,const auto& m) {
                if(!first)std::cout<<",";
                first=false;
                std::cout<<"\""<<name<<"\":{\"calls\":"<<m.calls<<",\"bytes\":"<<m.bytes<<",\"nanoseconds\":"<<m.nanoseconds<<"}";
            };
            metric("pread",s.pread);
            metric("pwrite",s.pwrite);
            metric("fsync",s.fsync);
            metric("page_checksum",s.page_checksum);
            metric("row_validation",s.row_validation);
            metric("row_materialization",s.row_materialization);
            metric("row_decode",s.row_decode);
            metric("vector_distance",s.vector_distance);
            metric("page_read_branch",s.page_read_branch);
            metric("page_read_overflow",s.page_read_overflow);
            metric("page_read_packed",s.page_read_packed);
            metric("packed_cache_hit",s.packed_cache_hit);
            metric("table_scan",s.table_scan);
            metric("scan_consume",s.scan_consume);
            metric("predicate_evaluation",s.predicate_evaluation);
            metric("vector_scratch_copy",s.vector_scratch_copy);
            std::cout<<"}";
        };
        auto emit_cache=[](const CacheStats& c) {
            std::cout<<"{\"hits\":"<<c.hits<<",\"misses\":"<<c.misses<<",\"evictions\":"<<c.evictions
                     <<",\"capacity_bytes\":"<<c.capacity_bytes<<",\"resident_pages\":"<<c.resident_pages<<"}";
        };
        std::cout<<"{\"packed_values\":"<<(options.packed_values?"true":"false")
                 <<",\"index_present\":"<<(index_present?"true":"false")
                 <<",\"vector_scalar\":"<<(vector_scalar?"true":"false")<<",\"row_heap\":"<<(row_heap?"true":"false")
                 <<",\"extra_columns\":"<<extra_columns
                 <<",\"text_scalar\":"<<(text_scalar?"true":"false")<<",\"unicode_body\":"<<(unicode_body?"true":"false")
                 <<",\"paired_index_scan\":"<<(options.paired_index_scan?"true":"false")<<",\"sparse_keys\":"<<(sparse?"true":"false")<<",\"index_build_ms\":"<<index_build_ms
                 <<",\"invalidate_every\":"<<invalidate_every
                 <<",\"table_scan_snapshot\":"<<(options.table_scan_snapshot?"true":"false")
                 <<",\"reuse_scan_snapshot_buffer\":"<<(options.reuse_scan_snapshot_buffer?"true":"false")
                 <<",\"reuse_vector_scratch\":"<<(options.reuse_vector_scratch?"true":"false")
                 <<",\"warm_resident_bytes\":"<<warm_resident<<",\"invalidated_resident_bytes\":"<<invalidated_resident
                 <<",\"query_and_mutation_minor_faults\":"<<query_minor_faults
                 <<",\"typed_enabled\":"<<(options.typed_scan_cache?"true":"false")<<",\"typed_rows\":"<<typed_rows<<",\"typed_capacity\":"<<typed_capacity<<",\"snapshot_budget\":"<<options.scan_snapshot_bytes<<",\"snapshot_rows\":"<<snapshot_rows<<",\"snapshot_capacity\":"<<snapshot_capacity
                 <<",\"scan_cache_budget\":"<<options.packed_scan_cache_bytes
                 <<",\"payload_bytes\":"<<payload_bytes<<",\"groups\":"<<groups
                 <<",\"churn\":"<<(churn?"true":"false")<<",\"churn_ms\":"<<churn_ms<<",\"churn_cpu_ms\":"<<churn_cpu_ms
                 <<",\"cache_budget\":"<<options.index_cache_bytes<<",\"crc_bytewise\":"<<(bytewise?"true":"false")
                 <<",\"write_cache\":";emit_cache(write_cache);std::cout<<",\"query_cache\":";emit_cache(query_cache);
        std::cout<<",\"write_data_cache\":";emit_cache(write_data_cache);
        std::cout<<",\"query_data_cache\":";emit_cache(query_data_cache);
        std::cout<<",\"data_cache_budget\":"<<options.data_cache_bytes<<",\"first_query_validated\":true";
        std::cout<<std::setprecision(12)<<",\"first_query_ms\":"<<first_query_ms;
        std::cout<<",\"second_query_ms\":"<<second_query_ms;
        std::cout<<std::setprecision(12)<<",\"profile_enabled\":"<<(profiling?"true":"false")
                 <<",\"write_cpu_ms\":"<<write_cpu_ms<<",\"query_cpu_ms\":"<<query_cpu_ms<<",\"write_profile\":";
        emit_profile(write_profile);std::cout<<",\"query_profile\":";emit_profile(query_profile);
        std::cout<<std::setprecision(12)
            <<",\"rows\":"<<count<<",\"dimensions\":"<<dimensions<<",\"queries\":"<<query_count
            <<",\"seed\":"<<seed<<",\"sql_batch_size\":"<<batch_size<<",\"shuffled_inserts\":"<<(ordered?"false":"true")
            <<",\"selective_decode\":"<<selective<<",\"batch_writes\":"<<batch
            <<",\"write_ms\":"<<write_ms<<",\"query_unfiltered_ms\":"<<all_ms
            <<",\"query_filtered_ms\":"<<filtered_ms<<",\"query_total_ms\":"<<all_ms+filtered_ms
            <<",\"database_bytes\":"<<database_bytes<<",\"insert_file_growth_bytes\":"<<after_insert_bytes-empty_bytes<<",\"index_file_growth_bytes\":"<<after_index_bytes-after_insert_bytes<<",\"churn_file_growth_bytes\":"<<database_bytes-after_index_bytes
            <<",\"rows_examined\":"<<examined<<",\"distances_computed\":"<<distances
            <<",\"vector_values_filtered\":"<<filtered_vectors<<",\"vector_values_unfiltered\":"<<unfiltered_vectors
            <<",\"vector_values_materialized\":"<<vector_cells<<",\"cells_materialized\":"<<cells
            <<",\"process_maxrss_kib\":"<<usage.ru_maxrss<<",\"local_cgroup_peak_bytes\":"<<local_peak
            <<",\"validated_queries\":"<<query_count<<",\"recovered_rows\":"<<recovered
            <<",\"result_digest\":\""<<std::hex<<digest<<"\"}\n";
    } catch(const std::exception& error) {std::cerr<<"FAIL benchmark: "<<error.what()<<'\n';return 1;}
}
