// Isolated experiment, not linked into production.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
using Bytes=std::vector<unsigned char>;
using State=std::map<uint64_t,std::string>;
struct Op {uint64_t key; std::string value; bool erase=false;};
struct Stats {uint64_t bytes=0,writes=0,syncs=0;};
struct Fault {int64_t write_budget=-1; int sync_call=0; int pause_fd=-1;};
static void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
template<class F> static void rejects(F f,const char* m){bool bad=false;try{f();}catch(const std::exception&){bad=true;}check(bad,m);}
static Bytes readfile(const std::string&p){std::ifstream f(p,std::ios::binary);return Bytes(std::istreambuf_iterator<char>(f),{});}
static void savefile(const std::string&p,const Bytes&b){std::ofstream f(p,std::ios::binary|std::ios::trunc);f.write((const char*)b.data(),b.size());check(bool(f),"fixture write");}
static constexpr uint32_t MAX_PAYLOAD=1024*1024;
static uint32_t crc(const unsigned char* p,size_t n){
 static const auto table=[] {std::array<uint32_t,256> t{};for(uint32_t i=0;i<256;++i){uint32_t c=i;for(int k=0;k<8;++k)c=(c>>1)^(0xedb88320u&uint32_t(-int(c&1)));t[i]=c;}return t;}();
 uint32_t c=~0u;for(size_t i=0;i<n;++i)c=table[(c^p[i])&255]^(c>>8);return ~c;
}
static uint64_t get(const Bytes&b,size_t p,size_t n){uint64_t v=0;for(size_t i=0;i<n;++i)v|=uint64_t(b.at(p+i))<<(8*i);return v;}
static void put(Bytes&b,uint64_t v,size_t n){for(size_t i=0;i<n;++i)b.push_back((v>>(8*i))&255);}
static void apply_ops(State&s,const std::vector<Op>&ops){for(const auto&o:ops){if(o.erase)s.erase(o.key);else s[o.key]=o.value;}}
class Log {
 int fd=-1; bool two,poison=false;uint64_t sequence=0;off_t end=0;
 void sync(){
  ++stats.syncs;
  if(fault.sync_call>0&&--fault.sync_call==0)throw std::runtime_error("injected fsync EIO before syscall");
  check(fsync(fd)==0,"fsync failed");
 }
 void writeall(const unsigned char*p,size_t n){
  while(n){
   if(fault.write_budget==0)throw std::runtime_error("injected write EIO");
   size_t amount=fault.write_budget<0?n:std::min(n,size_t(fault.write_budget));
   ++stats.writes;ssize_t done=pwrite(fd,p,amount,end);
   if(done<0&&errno==EINTR)continue;
   check(done>0,"write failed");end+=done;p+=done;n-=done;stats.bytes+=done;
   if(fault.write_budget>=0)fault.write_budget-=done;
  }
  if(fault.pause_fd>=0){char c='R';check(write(fault.pause_fd,&c,1)==1,"pause notification");for(;;)pause();}
 }
 Bytes readat(off_t pos,size_t n){
  Bytes b(n);size_t done=0;while(done<n){ssize_t r=pread(fd,b.data()+done,n-done,pos+done);if(r<0&&errno==EINTR)continue;check(r>0,"read failed");done+=r;}return b;
 }
 void replay(){
  struct stat st{};check(fstat(fd,&st)==0&&st.st_size>=0,"stat failed");
  while(st.st_size-end>=32){
   Bytes h=readat(end,32);
   check(std::memcmp(h.data(),"MVLOG001",8)==0,"header magic/version");
   check(crc(h.data(),28)==get(h,28,4),"header checksum");
   auto seq=get(h,8,8),len=get(h,16,4),count=get(h,20,4);
   check(get(h,24,4)==0&&len<=MAX_PAYLOAD&&count<=len/13,"header limits");
   check(seq==sequence+1&&seq!=0,"sequence");
   uint64_t size=32+len+24;
   if(uint64_t(st.st_size-end)<size)break;
   Bytes b=readat(end,size);size_t footer=32+len;
   check(std::memcmp(b.data()+footer,"COMMIT01",8)==0,"footer magic");
   check(get(b,footer+8,8)==seq,"footer sequence");
   check(crc(b.data()+footer,20)==get(b,footer+20,4),"footer checksum");
   check(crc(b.data(),footer)==get(b,footer+16,4),"frame checksum");
   std::vector<Op> ops;size_t pos=32;
   for(uint64_t i=0;i<count;++i){
    check(footer-pos>=13,"operation header");uint64_t key=get(b,pos,8);unsigned tag=b[pos+8];size_t n=get(b,pos+9,4);pos+=13;
    check(tag<=1&&n<=footer-pos&&(tag==0||n==0),"operation limits");
    ops.push_back({key,std::string((char*)b.data()+pos,n),tag==1});pos+=n;
   }
   check(pos==footer,"trailing payload");apply_ops(state,ops);sequence=seq;end+=size;
  }
  if(end!=st.st_size){check(ftruncate(fd,end)==0,"tail truncate");check(fsync(fd)==0,"recovery sync");}
 }
public:
 State state;Stats stats;Fault fault; double replay_seconds=0;
 Log(const std::string&p,bool twice):two(twice){
  fd=open(p.c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600);check(fd>=0,"open failed");
  try{
   check(flock(fd,LOCK_EX|LOCK_NB)==0,"writer already open");
   auto recovery_start=std::chrono::steady_clock::now();
   replay();
   replay_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-recovery_start).count();
   // File creation and directory entry persistence occur before timed commits.
   check(fsync(fd)==0,"open file sync");
   auto parent=std::filesystem::path(p).parent_path();if(parent.empty())parent=".";
   int d=open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);check(d>=0,"directory open");
   int result=fsync(d);close(d);check(result==0,"directory sync");
  }catch(...){close(fd);fd=-1;throw;}
 }
 ~Log(){if(fd>=0)close(fd);}
 Log(const Log&)=delete;Log&operator=(const Log&)=delete;
 void append(const std::vector<Op>&ops){
  check(!poison,"writer poisoned; reopen required");check(sequence!=UINT64_MAX,"sequence overflow");
  size_t len=0;
  for(const auto&o:ops){check(!o.erase||o.value.empty(),"delete value");check(o.value.size()<=MAX_PAYLOAD-13&&len<=MAX_PAYLOAD-13-o.value.size(),"transaction too large");len+=13+o.value.size();}
  Bytes b;b.reserve(32+len+24);for(char c:std::string("MVLOG001"))b.push_back(c);
  put(b,sequence+1,8);put(b,len,4);put(b,ops.size(),4);put(b,0,4);put(b,crc(b.data(),28),4);
  for(const auto&o:ops){put(b,o.key,8);put(b,o.erase,1);put(b,o.value.size(),4);b.insert(b.end(),o.value.begin(),o.value.end());}
  size_t footer=b.size();uint32_t checksum=crc(b.data(),b.size());
  for(char c:std::string("COMMIT01"))b.push_back(c);
  put(b,sequence+1,8);put(b,checksum,4);put(b,crc(b.data()+footer,20),4);
  try{
   if(two){writeall(b.data(),footer);sync();writeall(b.data()+footer,24);sync();}
   else{writeall(b.data(),b.size());sync();}
   apply_ops(state,ops);++sequence;
  }catch(...){poison=true;throw;}
 }
};

static void selftest(const std::string&dir){
 std::filesystem::create_directories(dir);
 for(bool two:{false,true}){
  auto p=dir+(two?"/two.log":"/one.log"); std::filesystem::remove(p);Bytes first,whole;
  {Log l(p,two);l.append({{1,"alpha"},{2,"beta"}});first=readfile(p);
   l.append({{1,"new"},{2,"",true},{3,""},{1,"last"}});
   check(l.state==State({{1,"last"},{3,""}}),"mutation order");
   check(l.stats.syncs==(two?4:2),"sync count");whole=readfile(p);
   rejects([&]{Log x(p,two);},"writer lock");}
  {Log l(p,two);check(l.state==State({{1,"last"},{3,""}}),"reopen exact");}
  for(size_t n=first.size();n<whole.size();++n){
   auto q=dir+"/truncate.log";savefile(q,Bytes(whole.begin(),whole.begin()+n));Log l(q,two);
   check(l.state==State({{1,"alpha"},{2,"beta"}}),"truncate recovery");
   l.append({{4,"recovered"}});}
  for(size_t i=0;i<whole.size();++i){auto q=dir+"/corrupt.log";auto b=whole;b[i]^=1;savefile(q,b);rejects([&]{Log l(q,two);},"corruption rejection");}
  // CRC-valid hostile lengths and sequences must fail before any large allocation.
  for(int field:{8,16,20,24}){
   auto q=dir+"/hostile.log";auto b=whole;
   for(int j=0;j<(field==8?8:4);++j)b[field+j]=255;
   uint32_t c=crc(b.data(),28);for(int j=0;j<4;++j)b[28+j]=(c>>(j*8))&255;
   savefile(q,b);rejects([&]{Log l(q,two);},"CRC-valid header bounds");}
  for(int mode=0;mode<5;++mode){
   auto b=first;size_t footer=b.size()-24;
   auto patch=[&](size_t at,uint64_t value,size_t n){for(size_t j=0;j<n;++j)b[at+j]=(value>>(8*j))&255;};
   if(mode==0)patch(40,2,1); // unknown operation
   if(mode==1)patch(40,1,1); // delete with a value
   if(mode==2)patch(41,MAX_PAYLOAD,4); // value crosses frame
   if(mode==3)patch(20,0,4); // unconsumed body
   if(mode==4)patch(footer+8,9,8); // wrong commit sequence
   patch(28,crc(b.data(),28),4);patch(footer+16,crc(b.data(),footer),4);
   patch(footer+20,crc(b.data()+footer,20),4);
   auto q=dir+"/semantic.log";savefile(q,b);rejects([&]{Log l(q,two);},"CRC-valid semantic corruption");}
  {Log l(p,two);rejects([&]{l.append({{9,std::string(MAX_PAYLOAD,'x')}});},"oversize append");
   l.append({{9,"valid after validation failure"}});l.append({{10,std::string(MAX_PAYLOAD-13,'z')}});}
  {Log l(p,two);check(l.state.at(10)==std::string(MAX_PAYLOAD-13,'z'),"maximum frame");}
  for(int budget:{0,10,40,70}){
   auto q=dir+"/failure.log";savefile(q,first);
   {Log l(q,two);l.fault.write_budget=budget;rejects([&]{l.append({{5,std::string(100,'x')}});},"write failure");
    rejects([&]{l.append({{6,"retry"}});},"poison write");}
   {Log l(q,two);check(l.state==State({{1,"alpha"},{2,"beta"}}),"failed write recovery");l.append({{7,"reopen retry"}});}
   {Log l(q,two);check(l.state.at(7)=="reopen retry","retry durable");}}
  for(int sync=1;sync<=(two?2:1);++sync){
   auto q=dir+"/syncfail.log";savefile(q,first);
   {Log l(q,two);l.fault.sync_call=sync;rejects([&]{l.append({{5,"uncertain"}});},"sync failure");
    check(!l.state.count(5),"failed sync no ACK");rejects([&]{l.append({{6,"retry"}});},"poison sync");}
   {Log l(q,two);check(l.state.count(5)==size_t(!two||sync==2),"uncertain frame recovery");}}
  for(int stage:{0,1,2}){
   bool ack=stage==2;
   auto q=dir+"/kill.log";savefile(q,first);int pipes[2];check(pipe(pipes)==0,"pipe");pid_t child=fork();check(child>=0,"fork");
   if(child==0){close(pipes[0]);try{Log l(q,two);if(stage==1)l.fault.pause_fd=pipes[1];if(stage>0)l.append({{8,"ack"}});char c='R';if(write(pipes[1],&c,1)!=1)_exit(3);for(;;)pause();}catch(...){_exit(2);}}
   close(pipes[1]);char c=0;check(read(pipes[0],&c,1)==1,"child ready");close(pipes[0]);check(kill(child,SIGKILL)==0,"kill");int status=0;check(waitpid(child,&status,0)==child&&WIFSIGNALED(status),"wait");
   Log l(q,two);check(l.state.count(8)==size_t(ack||(stage==1&&!two)),"kill recovery");}
 }
 std::cout<<"{\"selftest\":\"pass\",\"modes\":2,\"truncation\":\"every byte\",\"corruption\":\"every byte\",\"faults\":\"write and fsync\",\"process_kill\":true}\n";
}
static void benchmark(const std::string&dir,int transactions,int rounds,int rows,int payload){
 check(transactions>0&&transactions<=10000&&rounds>0&&rounds<=20,"benchmark count limits");
 check(rows>0&&rows<=1000&&payload>=0&&uint64_t(rows)*(13+uint64_t(payload))<=MAX_PAYLOAD,"benchmark frame limits");
 check(uint64_t(transactions)*rows*(uint64_t(payload)+64)<=128*1024*1024,"benchmark preparation limit");
 std::filesystem::create_directories(dir);
 std::vector<std::vector<Op>> workload;State expected;
 for(int t=0;t<transactions;++t){
  std::vector<Op> ops;
  for(int r=0;r<rows;++r){
   uint64_t key=uint64_t(t)*rows+r;std::string value(payload,' ');
   for(int j=0;j<payload;++j)value[j]=char('!'+((key*17+uint64_t(j)*13)%90));
   expected.emplace(key,value);ops.push_back({key,std::move(value)});
  }workload.push_back(std::move(ops));
 }
 using Clock=std::chrono::steady_clock;
 for(int round=0;round<rounds;++round)for(int slot=0;slot<2;++slot){
  bool two=((round+slot)%2)==0;std::string mode=two?"two_sync":"one_sync";
  auto path=dir+"/"+mode+"-"+std::to_string(round)+".log";
  check(!std::filesystem::exists(path),"benchmark requires fresh output files");
  Stats stats;double seconds;
  {Log l(path,two);auto start=Clock::now();for(const auto&ops:workload)l.append(ops);
   seconds=std::chrono::duration<double>(Clock::now()-start).count();
   check(l.state==expected,"benchmark live exact state");stats=l.stats;}
  auto reopen=Clock::now();Log recovered(path,two);
  double reopen_seconds=std::chrono::duration<double>(Clock::now()-reopen).count();
  check(recovered.state==expected,"benchmark replay exact state");
  uint64_t bytes=std::filesystem::file_size(path);
  check(stats.bytes==bytes&&stats.syncs==uint64_t(transactions)*(two?2:1),"benchmark counters");
  std::cout<<"{\"mode\":\""<<mode<<"\",\"round\":"<<round<<",\"transactions\":"<<transactions
   <<",\"rows_per_transaction\":"<<rows<<",\"payload_bytes\":"<<payload<<",\"file_bytes\":"<<bytes
   <<",\"write_bytes\":"<<stats.bytes<<",\"write_calls\":"<<stats.writes<<",\"fsync_calls\":"<<stats.syncs
   <<",\"write_seconds\":"<<seconds<<",\"replay_seconds\":"<<recovered.replay_seconds
   <<",\"reopen_seconds\":"<<reopen_seconds<<",\"exact_live_and_replay\":true}\n";
 }
}
int main(int argc,char**argv){try{
 check(argc>=3,"usage: append_log --selftest DIR | --bench DIR [transactions=20] [rounds=3] [rows=100] [payload=800]");
 if(std::string(argv[1])=="--selftest"){check(argc==3,"selftest arguments");selftest(argv[2]);}
 else if(std::string(argv[1])=="--bench"){check(argc<=7,"benchmark arguments");benchmark(argv[2],argc>3?std::stoi(argv[3]):20,argc>4?std::stoi(argv[4]):3,argc>5?std::stoi(argv[5]):100,argc>6?std::stoi(argv[6]):800);}
 else throw std::runtime_error("unknown command");
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
