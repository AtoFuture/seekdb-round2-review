#include "minivectordb/value.hpp"
#include "../src/types/text_validation.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>

using namespace minivectordb;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__)); } while(false)
namespace {
constexpr auto unlimited=std::numeric_limits<std::size_t>::max();
std::size_t comparisons=0;
struct Outcome { bool valid; std::size_t count; std::string error; };

// Independent UTF-8 oracle: constrain encoded byte ranges directly, without
// assembling code points or using the production parser's state machine.
std::size_t oracle(std::string_view text,std::size_t limit) {
  std::size_t count=0,offset=0;
  while(offset<text.size()) {
    auto byte=[&](std::size_t n){return static_cast<unsigned char>(text[offset+n]);};
    const auto first=byte(0);
    std::size_t width=0;
    if(first<=0x7f)width=1;
    else if(first>=0xc2&&first<=0xdf)width=2;
    else if(first>=0xe0&&first<=0xef)width=3;
    else if(first>=0xf0&&first<=0xf4)width=4;
    else throw std::invalid_argument("oracle leading byte");
    if(width>text.size()-offset)throw std::invalid_argument("oracle truncated");
    for(std::size_t n=1;n<width;++n)
      if(byte(n)<0x80||byte(n)>0xbf)throw std::invalid_argument("oracle continuation");
    if((first==0xe0&&byte(1)<0xa0)||(first==0xed&&byte(1)>0x9f)||
       (first==0xf0&&byte(1)<0x90)||(first==0xf4&&byte(1)>0x8f))
      throw std::invalid_argument("oracle scalar range");
    if(count==limit)throw std::invalid_argument("oracle length");
    ++count;offset+=width;
  }
  return count;
}
template<class F> Outcome outcome(F&& validate) {
  try {return {true,validate(),{}};}
  catch(const std::invalid_argument& e) {return {false,0,e.what()};}
}
void check(std::string_view text,std::size_t limit) {
  const auto expected=outcome([&]{return oracle(text,limit);});
  detail::text_validation_mode=detail::TextValidationMode::scalar;
  const auto scalar=outcome([&]{return validate_text(text,limit);});
  detail::text_validation_mode=detail::TextValidationMode::ascii_blocks;
  const auto actual=outcome([&]{return validate_text(text,limit);});
  CHECK(scalar.valid==expected.valid);
  if(expected.valid)CHECK(scalar.count==expected.count);
  CHECK(actual.valid==expected.valid);
  if(expected.valid)CHECK(actual.count==expected.count);
  CHECK(actual.error==scalar.error);
  ++comparisons;
}
void boundaries() {
  const std::array<std::size_t,13> limits{0,1,2,6,7,8,9,15,16,17,unlimited-7,unlimited-1,unlimited};
  for(std::size_t length=0;length<=80;++length) {
    for(std::size_t alignment=0;alignment<16;++alignment) {
      std::string storage(alignment+length+16,'x');
      for(std::size_t n=0;n<length;++n)storage[alignment+n]=static_cast<char>((n*37)%128);
      const auto text=std::string_view(storage).substr(alignment,length);
      for(auto limit:limits)check(text,limit);
      check(text,length);check(text,length+1);
      if(length)check(text,length-1);
    }
  }
  const std::array<std::string,27> pieces{
    "\xc2\x80","\xdf\xbf","\xe0\xa0\x80","\xed\x9f\xbf",
    "\xee\x80\x80","\xef\xbf\xbf","\xf0\x90\x80\x80","\xf4\x8f\xbf\xbf",
    "\x80","\xbf","\xc0\x80","\xc1\xbf","\xe0\x9f\xbf","\xed\xa0\x80",
    "\xf0\x8f\xbf\xbf","\xf4\x90\x80\x80","\xf5\x80\x80\x80","\xff",
    "\xc2","\xe0\xa0","\xf0\x90\x80","\xc2\x7f","\xe1\x80\x41",
    "\xf1\x80\x80\xc0",std::string("\0\x7f",2),"\xef\xbb\xbf","\xef\xbf\xbe"};
  for(const auto& piece:pieces)for(std::size_t prefix=0;prefix<24;++prefix)
    for(std::size_t suffix=0;suffix<10;++suffix) {
      const auto text=std::string(prefix,'a')+piece+std::string(suffix,'z');
      for(auto limit:limits)check(text,limit);
      check(text,prefix);check(text,prefix+1);check(text,prefix+suffix+1);
    }
  for(unsigned first=0;first<256;++first)for(unsigned second=0;second<256;++second) {
    auto text=std::string(7,'a');text.push_back(static_cast<char>(first));text.push_back(static_cast<char>(second));
    check(text,unlimited);check(text,8);
  }
}
void random_inputs() {
  std::mt19937 random(20261110);
  const std::array<std::string,7> scalars{
    "a",std::string(1,'\0'),"\x7f","\xc2\xa2","\xe4\xb8\xad","\xf0\x9f\x98\x80","\xf4\x8f\xbf\xbf"};
  for(unsigned trial=0;trial<12000;++trial) {
    std::string text;
    const auto length=random()%129;
    for(unsigned n=0;n<length;++n)text.push_back(static_cast<char>(random()%(trial%2?128:256)));
    check(text,unlimited);check(text,random()%130);
    text.insert(0,32+random()%16,'a');
    check(text,unlimited);check(text,32+random()%16);
  }
  for(unsigned trial=0;trial<4000;++trial) {
    std::string text;
    const auto count=random()%65;
    for(unsigned n=0;n<count;++n)text+=scalars[random()%scalars.size()];
    check(text,count);check(text,count+1);
    if(count)check(text,count-1);
    if(!text.empty()) {text[random()%text.size()]=static_cast<char>(random()%256);check(text,unlimited);}
  }
}
void guard_pages() {
  const auto page_size=sysconf(_SC_PAGESIZE);CHECK(page_size>0);
  const auto page=static_cast<std::size_t>(page_size);
  auto* base=static_cast<char*>(mmap(nullptr,3*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
  CHECK(base!=MAP_FAILED);
  struct Mapping {char* base;std::size_t bytes;~Mapping(){munmap(base,bytes);}} mapping{base,3*page};
  auto* data=base+page;CHECK(mprotect(data,page,PROT_READ|PROT_WRITE)==0);
  for(std::size_t length=0;length<=80;++length)for(bool at_end:{false,true}) {
    auto* start=at_end?data+page-length:data;
    if(length)std::memset(start,'a',length);
    const std::string_view text(start,length);
    check(text,length);check(text,unlimited);if(length)check(text,length-1);
    for(const auto& ending:std::array<std::string,5>{"\xc2","\xe0\xa0","\xf0\x90\x80","\xc2\x80","\xf4\x8f\xbf\xbf"})
      if(length>=ending.size()) {
        std::memset(start,'a',length);std::memcpy(start+length-ending.size(),ending.data(),ending.size());
        check(text,unlimited);check(text,length-ending.size());
      }
  }
}
void thread_isolation() {
  CHECK(detail::text_validation_mode==detail::TextValidationMode::ascii_blocks);
  detail::text_validation_mode=detail::TextValidationMode::scalar;
  bool default_in_thread=false;
  std::thread worker([&]{default_in_thread=detail::text_validation_mode==detail::TextValidationMode::ascii_blocks;});
  worker.join();
  CHECK(default_in_thread);CHECK(detail::text_validation_mode==detail::TextValidationMode::scalar);
  detail::text_validation_mode=detail::TextValidationMode::ascii_blocks;
}
}
int main() {
  try {thread_isolation();boundaries();random_inputs();guard_pages();std::cout<<"PASS text validation "<<comparisons<<" comparisons per mode\n";}
  catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
