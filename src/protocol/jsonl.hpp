#pragma once
#include "minivectordb/database.hpp"
#include "minivectordb/value.hpp"
#include <array>
#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <unistd.h>

namespace minivectordb::jsonl {
// Preserve raw validated JSON identifiers, avoiding any conversion through double.
// Raw lexemes borrow the immutable input frame until the response is finished.
struct Value { char kind = 0; std::string text; std::string_view raw; };
inline std::size_t plain_string_prefix(std::string_view input) noexcept {
    constexpr std::uint64_t ones=0x0101010101010101ULL,high=0x8080808080808080ULL;
    const auto zero=[](std::uint64_t x){return (x-ones)&~x&high;};
    std::size_t size=0;
    while(input.size()-size>=8) {
        std::uint64_t word;std::memcpy(&word,input.data()+size,8);
        // The subtraction tests can stop early on a neighbouring byte after
        // a borrow, but never skip a quote, backslash or raw control byte.
        if(zero(word^(ones*'"')) || zero(word^(ones*'\\')) ||
           ((word-ones*32)&~word&high))break;
        size+=8;
    }
    while(size<input.size()) {
        const auto c=static_cast<unsigned char>(input[size]);
        if(c<32 || c=='"' || c=='\\')break;
        ++size;
    }
    return size;
}
class Parser {
    std::string_view input;
    std::size_t pos = 0;
    [[noreturn]] void bad() const { throw std::invalid_argument("invalid JSON request"); }
    void space() { while (pos < input.size() && (input[pos]==' ' || input[pos]=='\t' || input[pos]=='\r' || input[pos]=='\n')) ++pos; }
    bool take(char c) { space(); if(pos<input.size() && input[pos]==c) { ++pos; return true; } return false; }
    unsigned hex() {
        unsigned n=0;
        for(int i=0;i<4;++i) {
            if(pos==input.size()) bad();
            const char c=input[pos++]; unsigned v;
            if(c>='0'&&c<='9') v=unsigned(c-'0');
            else if(c>='a'&&c<='f') v=unsigned(c-'a'+10);
            else if(c>='A'&&c<='F') v=unsigned(c-'A'+10);
            else bad();
            n=n*16+v;
        }
        return n;
    }
    void utf8(std::string& s,unsigned n) {
        if(n<0x80) s+=char(n);
        else if(n<0x800) {s+=char(0xc0|(n>>6));s+=char(0x80|(n&63));}
        else if(n<0x10000) {s+=char(0xe0|(n>>12));s+=char(0x80|((n>>6)&63));s+=char(0x80|(n&63));}
        else {s+=char(0xf0|(n>>18));s+=char(0x80|((n>>12)&63));s+=char(0x80|((n>>6)&63));s+=char(0x80|(n&63));}
    }
    std::string string() {
        if(!take('"')) bad();
        std::string s;
        while(pos<input.size()) {
            const auto count=plain_string_prefix(input.substr(pos));
            s.append(input.substr(pos,count));pos+=count;
            if(pos==input.size())bad();
            unsigned char c=static_cast<unsigned char>(input[pos++]);
            if(c=='"') { validate_text(s, s.size()); return s; }
            if(c<32) bad();
            if(c!='\\') { s+=char(c); continue; }
            if(pos==input.size()) bad();
            switch(input[pos++]) {
            case '"': s+='"';break; case '\\':s+='\\';break;case '/':s+='/';break;
            case 'b':s+='\b';break;case 'f':s+='\f';break;case 'n':s+='\n';break;case 'r':s+='\r';break;case 't':s+='\t';break;
            case 'u': {
                unsigned n=hex();
                if(n>=0xd800 && n<=0xdbff) {
                    if(pos+2>input.size() || input[pos]!='\\' || input[pos+1]!='u') bad();
                    pos+=2; unsigned low=hex();
                    if(low<0xdc00 || low>0xdfff) bad();
                    n=0x10000+((n-0xd800)<<10)+(low-0xdc00);
                } else if(n>=0xdc00 && n<=0xdfff) bad();
                utf8(s,n); break;
            }
            default: bad();
            }
        }
        bad();
    }
    Value value(unsigned depth) {
        if(depth>64) bad();
        space(); const auto start=pos;
        if(pos==input.size()) bad();
        Value v; v.kind=input[pos];
        if(v.kind=='"') v.text=string();
        else if(v.kind=='{') object(depth+1);
        else if(v.kind=='[') {
            ++pos;
            if(!take(']')) { do { value(depth+1); } while(take(',')); if(!take(']')) bad(); }
        } else if(v.kind=='t'||v.kind=='f'||v.kind=='n') {
            const std::string_view literal=v.kind=='t'?"true":v.kind=='f'?"false":"null";
            if(input.substr(pos,literal.size())!=literal) bad();
            pos+=literal.size();
        } else {
            if(input[pos]=='-') ++pos;
            if(pos==input.size()) bad();
            auto digit=[&] { return pos<input.size() && input[pos]>='0' && input[pos]<='9'; };
            if(input[pos]=='0') ++pos;
            else { if(!digit()) bad(); while(digit()) ++pos; }
            if(pos<input.size() && input[pos]=='.') { ++pos;if(!digit()) bad();while(digit()) ++pos; }
            if(pos<input.size() && (input[pos]=='e'||input[pos]=='E')) {
                ++pos;if(pos<input.size() && (input[pos]=='+'||input[pos]=='-')) ++pos;
                if(!digit()) bad();
                while(digit()) ++pos;
            }
            v.kind='0';
        }
        v.raw=input.substr(start,pos-start);
        // Integer identifiers are arbitrary precision lexemes. JSON floating IDs
        // must remain finite for the official Python driver (C locale is unchanged).
        if(v.kind=='0' && v.raw.find_first_of(".eE")!=std::string::npos &&
           !std::isfinite(std::strtod(std::string(v.raw).c_str(),nullptr))) bad();
        return v;
    }
    std::map<std::string,Value> object(unsigned depth) {
        if(!take('{')) bad();
        std::map<std::string,Value> result;
        if(!take('}')) {
            do {
                auto key=string(); if(!take(':')) bad();
                auto v=value(depth);
                if(!result.emplace(std::move(key),std::move(v)).second) bad();
            } while(take(','));
            if(!take('}')) bad();
        }
        return result;
    }
public:
    explicit Parser(std::string_view s):input(s) {}
    auto parse() { auto result=object(0);space();if(pos!=input.size()) bad();return result; }
};
inline std::string quote(std::string_view s) {
    std::string out;out.reserve(s.size()+2);out+='"';
    constexpr char hex[]="0123456789abcdef";
    while(!s.empty()) {
        const auto count=plain_string_prefix(s);out.append(s.substr(0,count));s.remove_prefix(count);
        if(s.empty())break;
        const auto c=static_cast<unsigned char>(s.front());s.remove_prefix(1);
        if(c=='"'||c=='\\') {out+='\\';out+=char(c);}
        else if(c<32) {out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}
        else out+=char(c);
    }
    out+='"';return out;
}
// Finalize before emitting: SELECT errors must not expose a partial success.
// Small responses avoid disk; larger responses use an anonymous file under /data.
class Response {
    std::string buffer;
    std::FILE* file=nullptr;
    std::filesystem::path directory;
public:
    explicit Response(std::filesystem::path dir):directory(std::move(dir)) {buffer.reserve(65536);}
    Response(const Response&)=delete;
    ~Response() {if(file) std::fclose(file);}
    // Reuse only bounded response storage. Close/unlink spools and discard all
    // previous payload bytes before accepting the next request.
    void reset() noexcept {if(file){std::fclose(file);file=nullptr;}buffer.clear();}
    void append(std::string_view s) {
        if(!file && s.size()<=65536-buffer.size()) {buffer.append(s);return;}
        if(!file) {
            std::string path=(directory/".jsonl-XXXXXX").string();
            int fd=::mkstemp(path.data());
            if(fd<0) throw std::runtime_error("cannot create JSON response spool");
            if(::unlink(path.c_str())!=0) {::close(fd);throw std::runtime_error("cannot unlink JSON response spool");}
            file=::fdopen(fd,"w+b");
            if(!file) {::close(fd);throw std::runtime_error("cannot open JSON response spool");}
            if(std::fwrite(buffer.data(),1,buffer.size(),file)!=buffer.size()) throw std::runtime_error("response spool write failed");
            buffer.clear();
        }
        if(std::fwrite(s.data(),1,s.size(),file)!=s.size()) throw std::runtime_error("response spool write failed");
    }
    void prepare() {if(file && (std::fflush(file)!=0 || std::fseek(file,0,SEEK_SET)!=0)) throw std::runtime_error("response spool finalize failed");}
    void emit(std::string_view id) {
        // Echo the validated identifier only after the complete payload is ready.
        // An arbitrarily long ID must not make a small mutation acknowledgement
        // spill after the mutation/COMMIT has already succeeded.
        std::cout<<"{\"id\":"<<id;
        if(file) {
            std::array<char,65536> chunk{};
            while(auto n=std::fread(chunk.data(),1,chunk.size(),file)) std::cout.write(chunk.data(),static_cast<std::streamsize>(n));
            if(std::ferror(file)) throw std::runtime_error("response spool read failed");
        } else std::cout<<buffer;
        std::cout<<'\n'<<std::flush;
    }
};
inline void cell(Response& out,const Cell& c) {
    std::visit([&](const auto& v) {
        using T=std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T,std::string>) out.append(quote(v));
        else if constexpr(std::is_same_v<T,std::int64_t>) out.append(std::to_string(v));
        else {
            auto number=[&](auto x) {
                if(!std::isfinite(x)) throw std::runtime_error("non-finite query result");
                std::array<char,64> buf{};
                auto [end,ec]=std::to_chars(buf.data(),buf.data()+buf.size(),x,std::chars_format::general);
                if(ec!=std::errc{}) throw std::runtime_error("number formatting failed");
                out.append(std::string_view(buf.data(),static_cast<std::size_t>(end-buf.data())));
            };
            if constexpr(std::is_same_v<T,double>) number(v);
            else {out.append("["); bool first=true; for(auto x:v) {if(!first) out.append(",");first=false;number(x);}out.append("]");}
        }
    },c);
}
inline const char* code(ErrorCode c) {
    switch(c) {
    case ErrorCode::Syntax:return "SYNTAX";case ErrorCode::Schema:return "SCHEMA";
    case ErrorCode::Type:return "TYPE";case ErrorCode::Constraint:return "CONSTRAINT";
    case ErrorCode::Transaction:return "TRANSACTION";case ErrorCode::Corruption:return "CORRUPTION";
    } return "EXECUTION";
}
class LineReader {
    std::array<char,65536> bytes;
    std::size_t begin=0,end=0;
    bool eof=false;
public:
    bool next(std::string& line,bool& oversized,std::size_t limit) {
        line.clear();oversized=false;
        for(;;) {
            if(begin==end) {
                if(eof)return !line.empty()||oversized;
                ssize_t count;
                do {count=::read(STDIN_FILENO,bytes.data(),bytes.size());} while(count<0&&errno==EINTR);
                if(count<0)throw std::runtime_error("JSON input read failed");
                if(count==0){eof=true;continue;}
                begin=0;end=static_cast<std::size_t>(count);
            }
            const auto* newline=static_cast<const char*>(std::memchr(bytes.data()+begin,'\n',end-begin));
            const auto count=newline?static_cast<std::size_t>(newline-(bytes.data()+begin)):end-begin;
            const auto keep=std::min(limit-line.size(),count);
            line.append(bytes.data()+begin,keep);oversized|=keep!=count;
            begin+=count;
            if(newline){++begin;return true;}
        }
    }
};
inline int serve(std::unique_ptr<Database>& db,const std::filesystem::path& directory) {
    std::cout<<"{\"type\":\"ready\"}\n"<<std::flush;
    constexpr std::size_t limit=16*1024*1024;
    LineReader input;
    Response response(directory);
    for(;;) {
        std::string line;bool oversized=false;
        if(!input.next(line,oversized,limit))break;
        std::string id="null";
        bool shutdown=false;
        response.reset();
        try {
            if(oversized) throw std::invalid_argument("JSON request exceeds 16 MiB");
            auto request=Parser(line).parse();
            auto ident=request.find("id");
            if(ident==request.end()) throw std::invalid_argument("missing id");
            id=ident->second.raw;
            auto field=[&](const char* name) -> std::string_view {
                auto it=request.find(name);
                if(it==request.end() || it->second.kind!='"') throw std::invalid_argument(std::string("missing or invalid ")+name);
                return it->second.text;
            };
            auto op=field("op");
            response.append(",\"ok\":true");
            if(op=="shutdown") {
                if(db->in_transaction()) db->execute("ROLLBACK;");
                shutdown=true;
            } else if(op=="execute") {
                auto mode=request.contains("search_mode")?field("search_mode"):"exact";
                if(mode!="exact" && mode!="ann") throw std::invalid_argument("invalid search_mode");
                bool result=false,first=true;
                QuerySink sink;
                sink.columns=[&](const std::vector<std::string>& names) {
                    result=true;response.append(",\"columns\":[");
                    bool comma=false;for(const auto& name:names) {if(comma) response.append(",");comma=true;response.append(quote(name));}
                    response.append("],\"rows\":[");
                };
                sink.row=[&](const Row& row) {
                    if(!first) response.append(",");
                    first=false;response.append("[");
                    bool comma=false;for(const auto& v:row) {if(comma) response.append(",");comma=true;cell(response,v);}response.append("]");
                };
                auto info=db->execute(field("sql"),sink,mode=="ann"?SearchMode::Ann:SearchMode::Exact);
                if(result) response.append("]");
                else {response.append(",\"affected_rows\":");response.append(std::to_string(info.affected_rows));}
            } else throw std::invalid_argument("unsupported op");
            response.append("}");response.prepare();
            if(shutdown) db.reset();
        } catch(const std::exception& e) {
            const auto* sql_error=dynamic_cast<const Error*>(&e);
            std::cout<<"{\"id\":"<<id<<",\"ok\":false,\"error\":{\"code\":"<<quote(sql_error?code(sql_error->code()):"EXECUTION")<<",\"message\":"<<quote(e.what())<<"}}\n"<<std::flush;
            continue;
        }
        // A transport failure after emission begins is fatal, never a second response.
        response.emit(id);
        if(shutdown) return 0;
    }
    if(db->in_transaction()) db->execute("ROLLBACK;");
    db.reset();return 0;
}
}
