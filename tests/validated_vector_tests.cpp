#include "minivectordb/exact.hpp"
#include "../src/vector/validated.hpp"
#include "minivectordb/profile.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

using namespace minivectordb;
#define CHECK(x) do {if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));} while(false)

namespace {
struct Item {std::int64_t key;std::vector<float> value;};
using Kernel=std::vector<Neighbor>(*)(std::span<const float>,std::size_t,const VectorSource&,const KeyPredicate&);
constexpr Kernel kernels[]{exact_top_k,detail::exact_top_k_validated_candidates,
                          detail::exact_top_k_batched_validated_candidates,
                          detail::exact_top_k_simd_validated_candidates,
                          detail::exact_top_k_wide_validated_candidates};
std::uint64_t cases=0;
void same(const std::vector<Neighbor>& actual,const std::vector<Neighbor>& expected) {
    CHECK(actual.size()==expected.size());
    for(std::size_t i=0;i<actual.size();++i) {
        CHECK(actual[i].key==expected[i].key);
        CHECK(std::bit_cast<std::uint64_t>(actual[i].distance)==std::bit_cast<std::uint64_t>(expected[i].distance));
    }
}
// Separate full sort and arithmetic oracle, with the required scalar double
// accumulation and sqrt-before-ordering; never calls either production kernel.
std::vector<Neighbor> oracle(const std::vector<Item>& data,const std::vector<float>& query,
                             std::size_t k,const KeyPredicate& predicate) {
    std::vector<Neighbor> out;
    for(const auto& row:data) {
        if(predicate&&!predicate(row.key))continue;
        CHECK(row.value.size()==query.size());double squared=0;
        for(std::size_t i=0;i<query.size();++i) {
            const double difference=static_cast<double>(query[i])-static_cast<double>(row.value[i]);
            squared+=difference*difference;
        }
        out.push_back({row.key,std::sqrt(squared)});
    }
    std::sort(out.begin(),out.end(),[](const Neighbor& a,const Neighbor& b) {
        return a.distance<b.distance||(a.distance==b.distance&&a.key<b.key);
    });
    if(out.size()>k)out.resize(k);return out;
}
void compare(const std::vector<Item>& data,const std::vector<float>& query,std::size_t k,
             const KeyPredicate& predicate={}) {
    const auto expected=oracle(data,query,k,predicate);
    const VectorSource source=[&](const VectorVisitor& visit) {for(const auto& row:data)visit(row.key,row.value);};
    const auto reference=exact_top_k(query,k,source,predicate);
    const auto validated=detail::exact_top_k_validated_candidates(query,k,source,predicate);
    const auto batched=detail::exact_top_k_batched_validated_candidates(query,k,source,predicate);
    const auto simd=detail::exact_top_k_simd_validated_candidates(query,k,source,predicate);
    const auto wide=detail::exact_top_k_wide_validated_candidates(query,k,source,predicate);
    const detail::BlockVectorSource blocks=[&](const detail::BlockVectorVisitor& visit) {
        std::vector<float> block(16*query.size());
        std::vector<std::int64_t> keys;
        for(std::size_t start=0;start<data.size();start+=16) {
            keys.clear();std::fill(block.begin(),block.end(),0);std::uint16_t mask=0;
            for(std::size_t lane=0;lane<16&&start+lane<data.size();++lane) {
                const auto& item=data[start+lane];keys.push_back(item.key);
                if(!predicate||predicate(item.key))mask|=std::uint16_t(1U<<lane);
                for(std::size_t d=0;d<query.size();++d)block[d*16+lane]=item.value[d];
            }
            visit(keys,block,mask);
        }
    };
    const auto blocked=detail::exact_top_k_transposed_validated_candidates(query,k,blocks);
    const auto narrow_blocked=detail::exact_top_k_transposed_validated_candidates(query,k,blocks,false);
    const auto screened=detail::exact_top_k_transposed_validated_candidates(query,k,blocks,true,true);
    same(screened,expected);
    same(detail::exact_top_k_transposed_validated_candidates(query,k,blocks,false,true),expected);
    same(reference,expected);same(validated,reference);same(batched,reference);same(simd,reference);same(blocked,reference);same(narrow_blocked,reference);same(wide,reference);++cases;
}
float arbitrary_finite(std::mt19937& random) {
    auto bits=static_cast<std::uint32_t>(random());
    // Preserve a broad exponent/sign/mantissa distribution, including subnormals.
    if((bits&0x7f800000u)==0x7f800000u)bits^=0x00800000u;
    return std::bit_cast<float>(bits);
}
void random_matrix() {
    std::mt19937 random(0x735a29u);
    for(std::size_t dimension:{0U,1U,2U,3U,7U,8U,15U,16U,31U,32U,64U,127U,128U,129U,768U,1024U,1025U}) {
        for(unsigned round=0;round<5;++round) {
            const auto next=[&]() {
                if(round==4) {
                    // Cluster exponents so many terms influence rounding, rather
                    // than letting one extreme component dominate every sum.
                    const auto bits=(static_cast<std::uint32_t>(random())&0x807fffffu)|
                        ((100u+static_cast<std::uint32_t>(random()%55))<<23);
                    return std::bit_cast<float>(bits);
                }
                return round%2?arbitrary_finite(random):float(int(random()%4097)-2048)/128;
            };
            std::vector<float> query(dimension);
            for(auto& value:query)value=next();
            std::vector<Item> data;
            for(int row=0;row<43;++row) {
                std::vector<float> vector(dimension);
                for(auto& value:vector)value=next();
                data.push_back({std::int64_t(row-21)*100000003,std::move(vector)});
            }
            std::shuffle(data.begin(),data.end(),random);
            for(std::size_t k:{0U,1U,3U,10U,43U,60U}) {
                compare(data,query,k);
                compare(data,query,k,[](std::int64_t key) {return key%3==0;});
            }
        }
    }
    compare({},std::vector<float>{0,1},10);
}
void extremes_and_rounding() {
    const float max=std::numeric_limits<float>::max(),tiny=std::numeric_limits<float>::denorm_min();
    const std::vector<Item> data{
        {std::numeric_limits<std::int64_t>::max(),{max,-max}},
        {std::numeric_limits<std::int64_t>::min(),{-max,max}},
        {-8,{1,std::ldexp(1.0f,-26)}},{5,{1,0}},{-3,{1,std::ldexp(1.0f,-25)}},
        {0,{tiny,-tiny}},{-1,{-0.0f,0.0f}},{1,{0.0f,-0.0f}},
        {-10,{-1,0}},{10,{1,0}},
        {31,{std::nextafter(max,0.0f),max}},{-31,{-max,std::nextafter(-max,0.0f)}}};
    for(const std::vector<float> query:std::vector<std::vector<float>>{{0,0},{max,max},{-max,-max},{tiny,-tiny},{1,0}})
        for(std::size_t k:{0U,1U,2U,3U,10U,20U})compare(data,query,k);
    const auto tied=oracle({{-8,{1,std::ldexp(1.0f,-26)}},{5,{1,0}}},{0,0},2,{});
    CHECK(tied[0].key==-8&&tied[0].distance==tied[1].distance);
}
template<class F>void invalid(F&& f) {
    bool rejected=false;try {f();}catch(const std::invalid_argument&){rejected=true;}CHECK(rejected);
}
void validation_boundaries() {
    const float nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
    const std::vector<float> finite{0,1},short_value{0};
    for(const auto kernel:kernels) {
        for(float bad:{nan,inf,-inf})for(std::size_t k:{0U,1U}) {
            bool called=false;
            const VectorSource source=[&](const VectorVisitor&){called=true;};
            invalid([&]{kernel(std::vector<float>{bad,1},k,source,{});});CHECK(!called);
        }
        const VectorSource mismatched=[&](const VectorVisitor& visit){visit(1,short_value);};
        invalid([&]{kernel(finite,1,mismatched,{});});
        // As with the public API, a predicate-rejected row is never a candidate.
        CHECK(kernel(finite,1,mismatched,[](std::int64_t){return false;}).empty());
        bool called=false;
        CHECK(kernel(finite,0,[&](const VectorVisitor&){called=true;},{}).empty());CHECK(!called);
        bool propagated=false;
        try {kernel(finite,1,[](const VectorVisitor&){throw std::runtime_error("source failed");},{});}
        catch(const std::runtime_error& error){propagated=std::string(error.what())=="source failed";}
        CHECK(propagated);
    }
    // The private entrance's finite-candidate premise does NOT weaken public APIs.
    for(float bad:{nan,inf,-inf}) {
        const std::vector<float> value{0,bad};
        invalid([&]{l2_distance(finite,value);});invalid([&]{l2_distance(value,finite);});
        invalid([&]{exact_top_k(finite,1,[&](const VectorVisitor& visit){visit(1,value);});});
    }
    invalid([&]{l2_distance(finite,short_value);});
}
void batch_lifetimes_and_tails() {
    for(const auto kernel:{detail::exact_top_k_batched_validated_candidates,
                           detail::exact_top_k_simd_validated_candidates,
                           detail::exact_top_k_wide_validated_candidates})
    for(std::size_t count=0;count<=65;++count) {
        std::vector<Item> data;
        const std::vector<float> query(128,0.125f);
        for(std::size_t row=0;row<count;++row) {
            std::vector<float> value(query.size());
            for(std::size_t d=0;d<value.size();++d)value[d]=float(int((row*7+d)%23)-11)/8;
            data.push_back({std::int64_t(count-row)-33,std::move(value)});
        }
        std::vector<float> reused(query.size());
        const VectorSource source=[&](const VectorVisitor& visit) {
            for(const auto& item:data) {
                reused=item.value;visit(item.key,reused);
                std::fill(reused.begin(),reused.end(),std::numeric_limits<float>::quiet_NaN());
            }
        };
        profile::set_enabled(true);profile::reset();
        const auto result=kernel(query,7,source,{});
        const auto measured=profile::snapshot();profile::set_enabled(false);
        same(result,oracle(data,query,7,{}));
        CHECK(measured.vector_distance.calls==count);++cases;
    }
    const std::vector<float> query(128,0),wrong(127,0),value(128,1);
    invalid([&]{detail::exact_top_k_batched_validated_candidates(query,3,[&](const VectorVisitor& visit){visit(1,wrong);});});
    bool propagated=false;
    try {
        detail::exact_top_k_batched_validated_candidates(query,3,[&](const VectorVisitor& visit) {
            for(int i=0;i<5;++i)visit(i,value);
            throw std::runtime_error("partial batch source failed");
        });
    } catch(const std::runtime_error& error){propagated=std::string(error.what())=="partial batch source failed";}
    CHECK(propagated);
}
}
int main() {
    try {random_matrix();extremes_and_rounding();validation_boundaries();batch_lifetimes_and_tails();
        std::cout<<"validated vector kernel: "<<cases<<" bitwise cases and validation boundaries passed\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
