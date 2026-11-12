#include "../src/sql/sql.hpp"
#include <bit>
#include <iostream>
#include <optional>
#include <random>
using namespace minivectordb;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));}while(false)
struct Outcome {std::optional<ErrorCode> error;std::string message;std::vector<float> values;};
Outcome bind(const std::string& text,std::uint32_t dimension,bool fast) {
  sql::Literal literal;literal.kind=sql::Literal::String;literal.text=text;
  Outcome out;
  try {out.values=std::get<std::vector<float>>(sql::bind_literal(literal,{"v",ColumnType::Vector,dimension},fast));}
  catch(const Error& e){out.error=e.code();out.message=e.what();}
  return out;
}
void same(const std::string& text,std::uint32_t dimension) {
  const auto a=bind(text,dimension,false),b=bind(text,dimension,true);
  CHECK(a.error==b.error);CHECK(a.message==b.message);CHECK(a.values.size()==b.values.size());
  for(std::size_t i=0;i<a.values.size();++i)
    CHECK(std::bit_cast<std::uint32_t>(a.values[i])==std::bit_cast<std::uint32_t>(b.values[i]));
}
void quoted_tokens() {
  std::mt19937 rng(712904);
  // Round-trip arbitrary bytes independently of VARCHAR UTF-8 validation.
  // Quoted identifiers fold ASCII only; string literals preserve every byte.
  for(char quote:std::string("'`"))for(int round=0;round<128;++round) {
    std::string raw,expected,input(1,quote);
    const auto size=round<64?std::size_t(round):std::size_t(4096+round*19);
    for(std::size_t i=0;i<size;++i)raw+=static_cast<char>(rng()%256);
    for(char c:raw) {
      input+=c;if(c==quote)input+=quote;
      expected+=(quote=='`'&&c>='A'&&c<='Z')?static_cast<char>(c+'a'-'A'):c;
    }
    input+=quote;
    const auto tokens=sql::lex(input);
    CHECK(tokens.size()==2);CHECK(tokens[0].text==expected);
    CHECK(tokens[0].kind==(quote=='`'?sql::TokenKind::Identifier:sql::TokenKind::String));
    input.pop_back();bool failed=false;
    try{(void)sql::lex(input);}catch(const Error& error){
      failed=error.code()==ErrorCode::Syntax&&std::string(error.what())=="unterminated quoted token";
    }
    CHECK(failed);
  }
}
int main() {
  try {
    quoted_tokens();
    const std::string cases[]{"[]","[0,-0,+0,0e-999,-1e-999]","[3.4028235e38,-3.4028235e38]",
      "[3.4028236e38]","[1e309]","[1e-324]","[.1,1.,+1E+3,-.0]","[16777217,16777219]",
      " [ 1 ,\t2\r\n, 3\f] ","[1/*keep*/,2]","[1,--comment\n2]","[1,2]/*tail*/",
      "[1e999,@]","[1e999,/*]","[nan]","[inf]","[0x1p0]","[1,,2]","[1,]","[,1]",
      "[+-1]","[- 1]","[1e+]","[.]","[1.2.3]","[1]x","[1];","[1\v,2]","[1,2",""};
    for(const auto& text:cases)for(auto dimension:{0u,1u,2u,3u,5u})same(text,dimension);
    std::mt19937 rng(1325);
    for(int round=0;round<3000;++round) {
      const auto count=1u+rng()%32;std::string text="[";
      for(unsigned i=0;i<count;++i) {
        if(i)text+=',';
        text+=(rng()%2?"+":"-")+std::to_string(rng())+"."+std::to_string(rng())+"e"+std::to_string(int(rng()%1000)-500);
      }
      text+=']';same(text,count);
      const std::string replacements="[],+-.eE/ *@0129\t\n";
      text[rng()%text.size()]=replacements[rng()%replacements.size()];same(text,count);
    }
    std::cout<<"PASS vector literal differential: 6150 cases, bitwise values and identical errors; 256 quoted-token round trips and 256 unterminated checks\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
