#include "minivectordb/database.hpp"
#include "minivectordb/profile.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

using namespace minivectordb;
struct Item {std::int64_t id;std::string category;std::vector<float> vector;};
std::vector<Row> oracle(const std::vector<Item>& data,int q,bool filter) {
    std::vector<Row> result;
    for(const auto& row:data) {
        if(filter&&row.category!="alpha")continue;
        double total=0;
        const float query[]{float(q),0,1};
        for(std::size_t d=0;d<3;++d) {
            const double delta=double(query[d])-double(row.vector[d]);
            total+=delta*delta;
        }
        result.push_back({row.id,std::sqrt(total),row.category});
    }
    std::sort(result.begin(),result.end(),[](const Row& a,const Row& b) {
        const auto x=std::get<double>(a[1]),y=std::get<double>(b[1]);
        return x<y||(x==y&&std::get<std::int64_t>(a[0])<std::get<std::int64_t>(b[0]));
    });
    if(result.size()>5)result.resize(5);return result;
}
void run(const std::filesystem::path& directory,bool typed,bool indexed) {
    Options options;options.typed_scan_cache=typed;
    Database db(directory/(std::to_string(typed)+"-"+std::to_string(indexed)+".db"),options);
    db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,category VARCHAR(20),v VECTOR(3))");
    std::vector<Item> data;
    std::string insert="INSERT INTO t VALUES ";
    for(int i=59;i>=0;--i) {
        const auto category=i%3==0?"alpha":i%3==1?"beta":"gamma";
        Item row{std::int64_t(i-30)*100003,category,{float(i%7-3),float(i/7-4),float(i%5-2)}};
        if(i!=59)insert+=',';
        insert+='('+std::to_string(row.id)+",'"+category+"','["+
            std::to_string(row.vector[0])+','+std::to_string(row.vector[1])+','+
            std::to_string(row.vector[2])+"]')";
        data.push_back(std::move(row));
    }
    db.execute(insert);
    if(indexed)db.execute("CREATE VECTOR INDEX tv ON t(v)");
    for(int query_index=0;query_index<10;++query_index) {
        const bool filtered=query_index%2;
        const int q=query_index%4-2;
        const auto sql="SELECT id,L2_DISTANCE(v,'["+std::to_string(q)+",0,1]') AS d,category FROM t"+
            std::string(filtered?" WHERE category='alpha'":"")+" ORDER BY d LIMIT 5";
        std::vector<Row> actual;
        profile::set_enabled(true);profile::reset();
        const auto info=db.execute(sql,{{},[&](const Row& row){actual.push_back(row);}});
        const auto stats=profile::snapshot();profile::set_enabled(false);
        const auto expected=oracle(data,q,filtered);
        if(actual.size()!=expected.size())throw std::runtime_error("row count mismatch");
        for(std::size_t i=0;i<actual.size();++i) {
            if(actual[i][0]!=expected[i][0]||actual[i][2]!=expected[i][2]||
               std::bit_cast<std::uint64_t>(std::get<double>(actual[i][1]))!=
               std::bit_cast<std::uint64_t>(std::get<double>(expected[i][1])))
                throw std::runtime_error("independent oracle mismatch");
        }
        std::cout<<"{\"typed_enabled\":"<<(typed?"true":"false")<<",\"indexed\":"<<(indexed?"true":"false")
            <<",\"query\":"<<query_index+1<<",\"varchar_filter\":"<<(filtered?"true":"false")
            <<",\"rows_examined\":"<<info.rows_examined<<",\"typed_rows\":"<<info.typed_rows_examined
            <<",\"snapshot_rows\":"<<info.snapshot_rows_examined<<",\"index_entries\":"<<info.vector_index_entries_examined
            <<",\"row_validations\":"<<stats.row_validation.calls<<",\"typed_bytes\":"<<info.typed_cache_bytes
            <<",\"snapshot_bytes\":"<<info.snapshot_bytes<<",\"oracle_passed\":true}\n";
    }
}
int main() {
    try {
        const std::filesystem::path parent="/mnt/d/Codex/MiniVectorDB/data";
        std::filesystem::create_directories(parent);
        std::string path=(parent/"varchar-cache-probe-XXXXXX").string();
        if(!::mkdtemp(path.data()))throw std::runtime_error("mkdtemp failed");
        for(bool indexed:{false,true})for(bool typed:{false,true})run(path,typed,indexed);
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
