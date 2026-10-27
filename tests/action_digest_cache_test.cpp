#include <eosio/chain/action_digest_cache.hpp>
#include <array>
#include <thread>
#include <exception>
#include <iostream>
#include <random>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <time.h>
using namespace eosio::chain;
thread_local int fail_after=-1;
void* operator new(std::size_t n){if(fail_after>=0 && fail_after--==0){fail_after=-1;throw std::bad_alloc();}if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void require(bool ok){if(!ok)throw std::runtime_error("action-digest-complete-input-oracle");}
uint64_t cpu_ns(){timespec t{};require(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)==0);return uint64_t(t.tv_sec)*1000000000+t.tv_nsec;}
digest_type original(const action& a,const std::vector<char>& out,bool flag){return flag?generate_action_digest(a,out):digest_type::hash(a);}
int main(){
 std::mt19937_64 rng(0x574e410abcULL);uint64_t checked=0;
 for(size_t n_auth:{0u,1u,2u,16u,17u,127u,128u})
 for(size_t n_data:{0u,1u,32u,33u,127u,128u,255u,256u,1024u,1025u,16383u,16384u})
 for(size_t n_out:{0u,1u,32u,127u,128u,1024u,1025u,4096u})
 for(bool flag:{false,true}) {
  action a;a.account=name{rng()};a.name=name{rng()};a.data.resize(n_data);
  for(auto& c:a.data)c=char(rng());
  for(size_t i=0;i<n_auth;++i)a.authorization.push_back({name{rng()},name{rng()}});
  std::vector<char> out(n_out);for(auto& c:out)c=char(rng());
  bounded_action_digest_cache cache;
  auto check=[&](const action& x,const std::vector<char>& y,bool mode){
   require(cache.digest(x,y,mode)==original(x,y,mode));
   require(cache.retained_payload_bytes()<=bounded_action_digest_cache::max_payload_bytes);++checked;
  };
  check(a,out,flag);check(a,out,flag);check(a,out,!flag);check(a,out,flag);
  for(unsigned change=0;change<8;++change){
   auto altered=a;auto output=out;
   if(change==0)altered.account=name{altered.account.to_uint64_t()^16};
   if(change==1)altered.name=name{altered.name.to_uint64_t()^16};
   if(change==2){if(altered.data.empty())altered.data.push_back(1);else altered.data[0]^=1;}
   if(change==3)altered.authorization.push_back({"other"_n,"active"_n});
   if(change==4 && !altered.authorization.empty())altered.authorization[0].actor=name{rng()};
   if(change==5 && !altered.authorization.empty())altered.authorization[0].permission=name{rng()};
   if(change==6){if(output.empty())output.push_back(1);else output[0]^=1;}
   if(change==7 && altered.authorization.size()>1)std::swap(altered.authorization.front(),altered.authorization.back());
   check(altered,output,flag);check(a,out,flag);
  }
 }
 action first;first.account="tokenone"_n;first.name="transfer"_n;first.authorization={{"alice"_n,"active"_n}};first.data.assign(33,0);
 std::vector<char> output(7,'x');auto expected=original(first,output,true);
 for(int position=0;position<3;++position){
  bounded_action_digest_cache cache;require(cache.digest(first,output,true)==expected);
  auto second=first;second.data[0]=1;auto second_out=output;second_out[0]='y';
  const auto second_expected=original(second,second_out,true);
  fail_after=position;digest_type actual;
  try{actual=cache.digest(second,second_out,true);}catch(...){fail_after=-1;throw;}
  fail_after=-1;require(actual==second_expected);
  require(cache.digest(second,second_out,true)==second_expected);
  require(cache.digest(first,output,true)==expected);
 }
 std::array<std::exception_ptr,4> failures{};
 std::array<std::thread,4> workers;
 for(size_t worker=0;worker<workers.size();++worker)workers[worker]=std::thread([&,worker]{
  try{
   bounded_action_digest_cache cache;auto value=first;std::vector<char> out(1,char(worker));
   for(unsigned i=0;i<1000;++i){
    value.data[0]=char(i);value.data[1]=char(i>>8);
    const auto expected=original(value,out,true);
    for(unsigned repeat=0;repeat<3;++repeat)require(cache.digest(value,out,true)==expected);
   }
  }catch(...){failures[worker]=std::current_exception();}
 });
 for(auto& thread:workers)thread.join();
 for(const auto& failure:failures)if(failure)std::rethrow_exception(failure);
 checked+=12000;
 if(!std::getenv("PUREEOS_ACTION_DIGEST_MICROBENCH")){
  std::cout<<"{\"status\":\"full-input-OOM-and-independent-thread-oracles-passed\",\"checks\":"<<checked<<",\"microbenchmark\":false}\n";
  return 0;
 }
 std::vector<action> plans(16384,first);
 for(size_t i=0;i<plans.size();++i){auto& a=plans[i];a.account=(i%2)?"tokenone"_n:"eosio.token"_n;std::memcpy(a.data.data(),&i,sizeof(i));}
 uint64_t check_sum=0;std::cout<<"{\"status\":\"full-input-and-allocation-failure-oracles-passed\",\"checks\":"<<checked<<",\"runs\":[";
 bool comma=false;
 for(bool caching:{false,true,true,false}){
  uint64_t checksum=0;const auto begin=cpu_ns();
  for(unsigned round=0;round<4;++round)for(const auto& a:plans){
   bounded_action_digest_cache cache;
   for(unsigned repeat=0;repeat<3;++repeat){auto d=caching?cache.digest(a,{},true):original(a,{},true);uint64_t word;std::memcpy(&word,d.data(),sizeof(word));checksum+=word;}
  }
  const auto elapsed=cpu_ns()-begin;
  if(!comma)check_sum=checksum;require(checksum==check_sum);
  if(comma)std::cout<<",";comma=true;
  std::cout<<"{\"cache\":"<<(caching?"true":"false")<<",\"cpu_ns\":"<<elapsed<<",\"checksum\":"<<checksum<<"}";
 }
 std::cout<<"]}\n";
}
