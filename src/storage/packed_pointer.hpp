#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <stdexcept>
namespace minivectordb::detail {
// Canonical unsigned LEB128. The format's two address-tag bits are moved to
// the low end, so small physical addresses do not require nine-byte varints.
inline void append_packed_pointer(std::string& out,std::uint64_t reference) {
    constexpr auto value_tag=UINT64_C(1)<<63,node_tag=UINT64_C(1)<<62;
    if((reference&(value_tag|node_tag))==(value_tag|node_tag))
        throw std::runtime_error("invalid node address tags");
    const auto kind=reference&value_tag?1ULL:reference&node_tag?2ULL:0ULL;
    auto encoded=((reference&~(value_tag|node_tag))<<2)|kind;
    do {auto byte=static_cast<unsigned char>(encoded&127);encoded>>=7;
        out.push_back(static_cast<char>(byte|(encoded?128:0)));
    } while(encoded);
}
inline std::uint64_t read_packed_pointer(std::string_view input,std::size_t& offset) {
    auto bad=[](){throw std::runtime_error("corrupt MiniVectorDB node address");};
    std::uint64_t encoded=0;
    for(unsigned byte=0;byte<10;++byte) {
        if(offset==input.size())bad();
        const auto value=static_cast<unsigned char>(input[offset++]);
        if(byte==9 && value>1)bad();
        encoded|=std::uint64_t(value&127)<<(7*byte);
        if(!(value&128)) {
            if(byte && value==0)bad();
            const auto kind=encoded&3;if(kind==3)bad();
            return (encoded>>2)|(kind==1?(UINT64_C(1)<<63):kind==2?(UINT64_C(1)<<62):0);
        }
    }
    bad();return 0;
}
}
