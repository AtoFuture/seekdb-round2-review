#include "minivectordb/row_codec.hpp"
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace minivectordb;
using Clock=std::chrono::steady_clock;
struct Measurement {
    int round, selectivity;
    bool lazy;
    double ms, checksum;
    std::uint64_t vectors;
};
int main() {
    try {
        constexpr std::size_t rows=10000,repetitions=20,dimensions=128;
        const Schema schema{1,"t",{{"id",ColumnType::Bigint,0},{"v",ColumnType::Vector,128},
                                  {"g",ColumnType::Bigint,0},{"body",ColumnType::Varchar,1024}},0};
        std::vector<std::string> encoded;
        for(std::size_t i=0;i<rows;++i) {
            std::vector<float> vector(dimensions);
            for(std::size_t d=0;d<dimensions;++d)vector[d]=static_cast<float>((i*17+d*13)%1024)/64.0f;
            encoded.push_back(encode_row(schema,Row{static_cast<std::int64_t>(i),std::move(vector),
                                      static_cast<std::int64_t>(i%100),std::string(256,'x')}));
        }
        std::vector<Measurement> results;
        auto run=[&](bool lazy,int selectivity) {
            ExecutionInfo info;
            double checksum=0;
            const auto start=Clock::now();
            for(std::size_t pass=0;pass<repetitions;++pass)for(const auto& bytes:encoded) {
                auto consume=[&](const auto& row) {
                    checksum+=double(std::get<std::int64_t>(row[0]))*1e-9;
                    if(std::get<std::int64_t>(row[2])>=selectivity)return;
                    for(float component:std::get<std::vector<float>>(row[1]))checksum+=double(component)*component;
                };
                if(lazy) {LazyRowView row(schema,bytes,info);consume(row);}
                else {auto row=decode_row(schema,bytes);++info.vector_values_materialized;consume(row);}
            }
            const auto ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
            return Measurement{0,selectivity,lazy,ms,checksum,info.vector_values_materialized};
        };
        // Untimed warmup is separate and uses the same validated binary format.
        run(false,10);run(true,10);
        for(int round=0;round<3;++round)for(int ratio:{0,1,10,50,100}) {
            auto first=run(round%2==1,ratio),second=run(round%2!=1,ratio);
            if(first.checksum!=second.checksum)throw std::runtime_error("eager/lazy result mismatch");
            first.round=second.round=round+1;
            results.push_back(first);results.push_back(second);
        }
        std::cout<<std::setprecision(12)<<"{\"scope\":\"in-memory row validation/materialization and filtered squared-distance only; no SQL/store/IO/TopK\","
                 <<"\"rows\":"<<rows<<",\"dimensions\":"<<dimensions<<",\"repetitions\":"<<repetitions<<",\"samples\":[";
        for(std::size_t i=0;i<results.size();++i) {
            if(i)std::cout<<',';
            const auto& r=results[i];
            std::cout<<"{\"round\":"<<r.round<<",\"match_percent\":"<<r.selectivity<<",\"lazy\":"<<(r.lazy?"true":"false")
                     <<",\"ms\":"<<r.ms<<",\"checksum\":"<<r.checksum<<",\"vector_values_materialized\":"<<r.vectors<<'}';
        }
        std::cout<<"]}\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}