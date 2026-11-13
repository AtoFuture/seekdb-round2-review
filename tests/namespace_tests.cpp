#include "minivectordb/store.hpp"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <limits>
#include <unistd.h>
using namespace minivectordb;
#define CHECK(x) do {if(!(x)) throw std::runtime_error(#x);}while(false)
int main() {
  try {
    const auto* configured=std::getenv("MVDB_TEST_DIR");
    auto base=configured?std::filesystem::path(configured):std::filesystem::path("/data/core-tests");
    std::filesystem::create_directories(base);
    auto pattern=(base/"namespaces-XXXXXX").string();
    CHECK(::mkdtemp(pattern.data())!=nullptr);
    auto dir=std::filesystem::path(pattern);
    auto p=dir/"main.db";
    {
      Store s(p);
      s.begin();
      s.put_in(1,7,"a");
      s.put_in(2,7,"b");
      s.put(7,"default");
      s.put_in(1,INT64_MIN,"min");
      s.put_in(1,INT64_MAX,"max");
      s.commit();
      CHECK(s.get_in(1,7)=="a" && s.get_in(2,7)=="b" && s.get(7)=="default");
      s.begin();
      int n=0;
      s.scan_in(1,[&](auto k,auto) {
        ++n;
        s.erase_in(1,k);
        s.put_in(2,k,"copy");
      });
      CHECK(n==3);
      CHECK(!s.get_in(1,7));
      s.rollback();
      CHECK(s.get_in(1,7)=="a" && !s.get_in(2,INT64_MIN));
      s.begin();
      s.put_in(1,8,"lost");
      s.put_in(2,8,"lost");
    }
    {
      Store s(p);
      CHECK(!s.get_in(1,8)&&!s.get_in(2,8));
      CHECK(s.get_in(1,INT64_MIN)=="min"&&s.get_in(1,INT64_MAX)=="max");
    }
    auto old = dir / "format01.db";
    { std::string header(8192, '\0'); header.replace(0, 8, "MVDBRT01"); std::ofstream out(old, std::ios::binary); out.write(header.data(), static_cast<std::streamsize>(header.size())); }
    bool rejected_old = false;
    try { Store store(old); } catch (const std::runtime_error& error) { rejected_old = std::string(error.what()).find("format 01") != std::string::npos; }
    CHECK(rejected_old);
    std::filesystem::remove_all(dir);
    std::cout<<"PASS namespaces\n";
  }
  catch(const std::exception&e) {
    std::cerr<<e.what()<<'\n';
    return 1;
  }
}
