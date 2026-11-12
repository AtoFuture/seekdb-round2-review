#include "minivectordb/database.hpp"
#include <algorithm>
#include "minivectordb/profile.hpp"
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
#include <random>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>

using namespace minivectordb;
using Clock = std::chrono::steady_clock;
constexpr std::size_t dimensions = 128;
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
        if(q.filtered&&row.group!=q.group)continue;
        double sum=0;
        for(std::size_t d=0;d<dimensions;++d) {double delta=double(row.vector[d])-q.vector[d];sum+=delta*delta;}
        ranks.emplace_back(sum,static_cast<std::int64_t>(i));
    }
    std::sort(ranks.begin(),ranks.end());
    if(ranks.size()>10)ranks.resize(10);
    std::vector<Hit> result;
    for(auto [square,id]:ranks) result.push_back({id,std::sqrt(square),records[static_cast<std::size_t>(id)].text});
    return result;
}
std::uint64_t hash_bytes(std::uint64_t hash, const void* raw, std::size_t size) {
    const auto* p=static_cast<const unsigned char*>(raw);
    for(std::size_t i=0;i<size;++i){hash^=p[i];hash*=1099511628211ULL;}
    return hash;
}
int main(int argc,char** argv) {
    try {
        require(argc==6,"usage: optimization-bench database-path rows queries selective batch");
        const std::filesystem::path path(argv[1]);
        const auto count=number(argv[2]),query_count=number(argv[3]);
        const auto selective=number(argv[4]),batch=number(argv[5]);
        require(count>=20&&count<=1000000&&query_count>=2&&query_count<=10000&&query_count%2==0,"invalid workload size");
        require(selective<=1&&batch<=1,"flags must be 0 or 1");
        require(!std::filesystem::exists(path),"refusing to overwrite existing database");
        std::filesystem::create_directories(path.parent_path());
        std::mt19937 rng(seed);
        std::vector<Record> records;
        records.reserve(count);
        for(std::size_t i=0;i<count;++i)
            records.push_back({random_vector(rng),"record-"+std::to_string(i)+"-"+std::string(256,'a'+static_cast<char>(i%26)),static_cast<std::int64_t>(i%10)});
        std::vector<std::size_t> insertion_order(count);
        std::iota(insertion_order.begin(),insertion_order.end(),0);
        std::shuffle(insertion_order.begin(),insertion_order.end(),rng);
        std::vector<Query> queries;
        for(std::size_t i=0;i<query_count;++i) {
            Query q{random_vector(rng),{},i%2==1,static_cast<std::int64_t>((i/2)%10)};
            q.sql="SELECT id,L2_DISTANCE(v,"+literal(q.vector)+") AS d,body FROM t";
            if(q.filtered)q.sql+=" WHERE g="+std::to_string(q.group);
            q.sql+=" ORDER BY d LIMIT 10";
            queries.push_back(std::move(q));
        }
        Options options{path.parent_path()/"tmp",8*1024*1024};
        options.selective_decode=selective!=0;
        options.batch_writes=batch!=0;
        double write_ms=0,all_ms=0,filtered_ms=0,write_cpu_ms=0,query_cpu_ms=0;
        const bool profiling=std::getenv("MVDB_PROFILE")!=nullptr;
        profile::Snapshot write_profile{},query_profile{};
        profile::set_enabled(profiling);
        std::uint64_t vector_cells=0,filtered_vectors=0,unfiltered_vectors=0,cells=0,examined=0,distances=0;
        std::vector<std::vector<Hit>> results(query_count);
        std::uintmax_t empty_bytes=0,database_bytes=0;
        {
            Database db(path,options);
            db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(128),g BIGINT,body VARCHAR(1024))");
            empty_bytes=std::filesystem::file_size(path);
            profile::reset();
            for(std::size_t begin=0;begin<count;begin+=batch_size) {
                std::string sql="INSERT INTO t VALUES";
                const auto end=std::min(begin+batch_size,count);
                for(std::size_t j=begin;j<end;++j) {
                    if(j!=begin)sql+=',';
                    const auto id=insertion_order[j];
                    const auto& row=records[id];
                    sql+="("+std::to_string(id)+","+literal(row.vector)+","+std::to_string(row.group)+",'"+row.text+"')";
                }
                const auto cpu_start=std::clock();
                const auto start=Clock::now();
                const auto info=db.execute(sql);
                write_ms+=elapsed(start);
                write_cpu_ms+=1000.0*(std::clock()-cpu_start)/CLOCKS_PER_SEC;
                require(info.affected_rows==end-begin,"write count mismatch");
            }
            write_profile=profile::snapshot();
            database_bytes=std::filesystem::file_size(path);
            std::cerr<<"write complete "<<write_ms<<" ms; query warm-up\n";
            for(std::size_t i=0;i<2;++i)db.execute(queries[i].sql);
            profile::reset();
            for(std::size_t i=0;i<queries.size();++i) {
                auto& output=results[i];
                const auto cpu_start=std::clock();
                const auto start=Clock::now();
                auto info=db.execute(queries[i].sql,{{},[&](const Row& row) {
                    output.push_back({std::get<std::int64_t>(row[0]),std::get<double>(row[1]),std::get<std::string>(row[2])});
                }});
                const auto ms=elapsed(start);
                query_cpu_ms+=1000.0*(std::clock()-cpu_start)/CLOCKS_PER_SEC;
                (queries[i].filtered?filtered_ms:all_ms)+=ms;
                vector_cells+=info.vector_values_materialized;
                (queries[i].filtered?filtered_vectors:unfiltered_vectors)+=info.vector_values_materialized;
                cells+=info.cells_materialized;
                examined+=info.rows_examined;
                distances+=info.distances_computed;
            }
            query_profile=profile::snapshot();
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
                const auto id=std::get<std::int64_t>(row[0]);
                require(id>=0&&static_cast<std::size_t>(id)<count,"invalid recovered primary key");
                const auto& original=records[static_cast<std::size_t>(id)];
                require(std::get<std::vector<float>>(row[1])==original.vector&&
                        std::get<std::int64_t>(row[2])==original.group&&
                        std::get<std::string>(row[3])==original.text,"recovered row differs");
                ++recovered;
            }});
        }
        require(recovered==count,"recovered row count differs");
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
            std::cout<<"}";
        };
        std::cout<<std::setprecision(12)<<"{\"profile_enabled\":"<<(profiling?"true":"false")
                 <<",\"write_cpu_ms\":"<<write_cpu_ms<<",\"query_cpu_ms\":"<<query_cpu_ms<<",\"write_profile\":";
        emit_profile(write_profile);std::cout<<",\"query_profile\":";emit_profile(query_profile);
        std::cout<<std::setprecision(12)
            <<",\"rows\":"<<count<<",\"dimensions\":"<<dimensions<<",\"queries\":"<<query_count
            <<",\"seed\":"<<seed<<",\"sql_batch_size\":"<<batch_size<<",\"shuffled_inserts\":true"
            <<",\"selective_decode\":"<<selective<<",\"batch_writes\":"<<batch
            <<",\"write_ms\":"<<write_ms<<",\"query_unfiltered_ms\":"<<all_ms
            <<",\"query_filtered_ms\":"<<filtered_ms<<",\"query_total_ms\":"<<all_ms+filtered_ms
            <<",\"database_bytes\":"<<database_bytes<<",\"insert_file_growth_bytes\":"<<database_bytes-empty_bytes
            <<",\"rows_examined\":"<<examined<<",\"distances_computed\":"<<distances
            <<",\"vector_values_filtered\":"<<filtered_vectors<<",\"vector_values_unfiltered\":"<<unfiltered_vectors
            <<",\"vector_values_materialized\":"<<vector_cells<<",\"cells_materialized\":"<<cells
            <<",\"process_maxrss_kib\":"<<usage.ru_maxrss<<",\"local_cgroup_peak_bytes\":"<<local_peak
            <<",\"validated_queries\":"<<query_count<<",\"recovered_rows\":"<<recovered
            <<",\"result_digest\":\""<<std::hex<<digest<<"\"}\n";
    } catch(const std::exception& error) {std::cerr<<"FAIL benchmark: "<<error.what()<<'\n';return 1;}
}