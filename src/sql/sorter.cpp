#include "sorter.hpp"
#include "../storage/binary.hpp"
#include <algorithm>
#include <bit>
#include <fstream>
#include <unistd.h>
namespace minivectordb::sql {
  namespace {
    std::size_t memory(const SortRecord&r) {
      std::size_t n=sizeof(r)+r.values.capacity()*sizeof(Cell);
      for(const auto&v:r.values)if(auto p=std::get_if<std::string>(&v))n+=p->capacity()+1;
      return n;
    }
    void write_record(std::ofstream&out,const SortRecord&r) {
      std::string data;
      binary::put(data,std::bit_cast<std::uint64_t>(r.key));
      binary::put(data,r.values.size(),4);
      for(const auto&v:r.values) {
        binary::put(data,v.index(),1);
        if(auto p=std::get_if<std::int64_t>(&v))binary::put(data,std::bit_cast<std::uint64_t>(*p));
        else if(auto p=std::get_if<std::string>(&v))binary::text(data,*p);
        else binary::put(data,std::bit_cast<std::uint64_t>(std::get<double>(v)));
      }
      std::string size;
      binary::put(size,data.size());
      out.write(size.data(),8);
      out.write(data.data(),static_cast<std::streamsize>(data.size()));
      if(!out)throw std::runtime_error("cannot write sort run");
    }
    bool read_record(std::ifstream&in,SortRecord&r) {
      char length[8];
      in.read(length,8);
      if(in.gcount()==0&&in.eof())return false;
      if(in.gcount()!=8)throw Error(ErrorCode::Corruption,"truncated sort run");
      binary::Reader header(std::string_view(length,8));
      auto n=header.get();
      const auto start=in.tellg();
      in.seekg(0,std::ios::end);
      auto end=in.tellg();
      in.seekg(start);
      if(start<0||end<start||n>static_cast<std::uint64_t>(end-start)||n>static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))throw Error(ErrorCode::Corruption,"invalid sort record length");
      std::string data(static_cast<std::size_t>(n),'\0');
      in.read(data.data(),static_cast<std::streamsize>(n));
      if(!in)throw std::runtime_error("cannot read sort run");
      binary::Reader reader(data);
      r.key=std::bit_cast<std::int64_t>(reader.get());
      auto count=reader.get(4);
      if(count>reader.remaining()/2)throw Error(ErrorCode::Corruption,"invalid sort key count");
      r.values.clear();
      for(std::uint64_t i=0;i<count;++i) {
        auto type=reader.get(1);
        if(type==0)r.values.emplace_back(std::bit_cast<std::int64_t>(reader.get()));
        else if(type==1)r.values.emplace_back(reader.text());
        else if(type==3)r.values.emplace_back(std::bit_cast<double>(reader.get()));
        else throw Error(ErrorCode::Corruption,"invalid sort key type");
      }
      reader.end();
      return true;
    }
  }
  struct Sorter::Impl {
    Options options;
    const std::vector<BoundOrder>& order;
    ExecutionInfo& info;
    std::vector<SortRecord> buffer;
    std::size_t used=0;
    std::uint64_t runs=0;
    std::filesystem::path directory;
    Impl(const Options&o,const std::vector<BoundOrder>&keys,ExecutionInfo&i):options(o),order(keys),info(i) {
    }
    ~Impl() {
      if(!directory.empty()) {
        std::error_code error;
        std::filesystem::remove_all(directory,error);
      }
    }
    bool less(const SortRecord&a,const SortRecord&b)const {
      for(std::size_t i=0;i<order.size();++i) {
        auto cmp=compare_cells(a.values.at(i),b.values.at(i));
        if(cmp)return order[i].descending?cmp>0:cmp<0;
      }
      return a.key<b.key;
    }
    std::filesystem::path path(unsigned pass,std::uint64_t index) {
      return directory/(std::to_string(pass)+"-"+std::to_string(index));
    }
    void ensure_directory() {
      if(!directory.empty())return;
      std::filesystem::create_directories(options.sort_directory);
      auto pattern=(options.sort_directory/"mvdb-sort-XXXXXX").string();
      if(!::mkdtemp(pattern.data()))throw std::runtime_error("cannot create sort directory");
      directory=pattern;
    }
    void flush() {
      if(buffer.empty())return;
      ensure_directory();
      std::sort(buffer.begin(),buffer.end(),[&](const auto&a,const auto&b) {
        return less(a,b);
      });
      std::ofstream out(path(0,runs),std::ios::binary|std::ios::trunc);
      if(!out)throw std::runtime_error("cannot create sort run");
      for(const auto&r:buffer)write_record(out,r);
      out.close();
      if(!out)throw std::runtime_error("cannot close sort run");
      ++runs;
      ++info.spill_runs;
      buffer.clear();
      buffer.shrink_to_fit();
      used=0;
    }
    void merge(unsigned pass,std::uint64_t first,std::uint64_t total) {
      auto leftpath=path(pass,first),rightpath=path(pass,first+1),outpath=path(pass+1,first/2);
      if(first+1==total) {
        std::filesystem::rename(leftpath,outpath);
        return;
      }
      std::ifstream left(leftpath,std::ios::binary),right(rightpath,std::ios::binary);
      std::ofstream out(outpath,std::ios::binary|std::ios::trunc);
      if(!left||!right||!out)throw std::runtime_error("cannot open sort merge");
      SortRecord a,b;
      bool has_a=read_record(left,a),has_b=read_record(right,b);
      while(has_a||has_b) {
        if(has_a&&(!has_b||less(a,b))) {
          write_record(out,a);
          has_a=read_record(left,a);
        }
        else {
          write_record(out,b);
          has_b=read_record(right,b);
        }
      }
      out.close();
      if(!out)throw std::runtime_error("cannot close merged run");
      left.close();
      right.close();
      std::filesystem::remove(leftpath);
      std::filesystem::remove(rightpath);
    }
  };
  Sorter::Sorter(const Options&o,const std::vector<BoundOrder>&order,ExecutionInfo&i):impl_(std::make_unique<Impl>(o,order,i)) {
  }
  Sorter::~Sorter()=default;
  void Sorter::add(SortRecord r) {
    auto&s=*impl_;
    auto size=memory(r);
    if(!s.buffer.empty()&&(size>s.options.memory_budget_bytes||s.used>s.options.memory_budget_bytes-size))s.flush();
    s.used+=size;
    s.buffer.push_back(std::move(r));
    if(s.used>=s.options.memory_budget_bytes)s.flush();
  }
  void Sorter::finish(std::uint64_t limit,const std::function<void(const SortRecord&)>&emit) {
    auto&s=*impl_;
    if(!s.runs) {
      std::sort(s.buffer.begin(),s.buffer.end(),[&](const auto&a,const auto&b) {
        return s.less(a,b);
      });
      for(const auto&r:s.buffer) {
        if(!limit--)break;
        emit(r);
      }
      return;
    }
    s.flush();
    unsigned pass=0;
    auto count=s.runs;
    while(count>1) {
      for(std::uint64_t i=0;i<count;i+=2)s.merge(pass,i,count);
      count=(count+1)/2;
      ++pass;
    }
    std::ifstream input(s.path(pass,0),std::ios::binary);
    if(!input)throw std::runtime_error("cannot open sorted results");
    SortRecord record;
    while(limit&&read_record(input,record)) {
      --limit;
      emit(record);
    }
  }
}
