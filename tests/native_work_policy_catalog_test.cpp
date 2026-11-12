#include <eosio/chain/native_work_policy_catalog.hpp>
#include <eosio/vm/backend.hpp>
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>

#ifdef AIC_CATALOG_SOURCE_ABI_WITNESS
#include AIC_CATALOG_SOURCE_ABI_WITNESS
#endif

// Standalone real WASM/native bridge, not an instantiated Node controller.
// The fixture delegates memory to libc and digest operations to OpenSSL; it
// does not pretend to be the canonical interface's context/permission checks.
namespace {
using namespace eosio::chain;
using namespace eosio::vm;
using bytes=std::vector<uint8_t>;
void require(bool v,const char* message) {if(!v) throw std::runtime_error(message);}
template<typename E,typename F> void rejects(F&& f) {
   try {f();}catch(const E&) {return;}
   throw std::runtime_error("expected pure-native policy rejection");
}
native_pure_work_policy policy(uint64_t maximum=4096) {
   native_pure_work_policy p;p.version=17;p.maximum_input_bytes=maximum;
   for(size_t i=0;i<p.rates.size();++i) p.rates[i]={13+i,32+i,3+i}; // TEST rates only
   return p;
}
const native_pure_work_descriptor& descriptor(std::string_view name) {
   for(const auto& d:native_pure_work_descriptors) if(d.function==name) return d;
   throw std::runtime_error("test descriptor not found");
}
struct event {
   std::array<value_type,3> params{i32,i32,i32};
   std::array<value_type,1> result{i32};
   std::array<native_work_argument,3> args{{{i32,UINT32_MAX},{i32,UINT32_MAX},{i32,UINT32_MAX}}};
   native_work_call call(const native_pure_work_descriptor& d,uint64_t length,uint32_t index=999) {
      args[d.length_argument].bits=length;
      return {"env",d.function,index,params,d.returns_i32?std::span<const value_type>(result):std::span<const value_type>{},args,true};
   }
};
uint64_t oracle(const native_pure_work_policy& p,const native_pure_work_descriptor& d,uint64_t length) {
   const auto& r=p.rates[static_cast<size_t>(d.operation)];
   const auto chunks=(static_cast<unsigned __int128>(length)+r.input_bytes_per_chunk-1)/r.input_bytes_per_chunk;
   const auto value=r.base_units+chunks*r.units_per_input_chunk;
   require(value<=UINT64_MAX,"test oracle overflow");return uint64_t(value);
}
const EVP_MD* digest_type(std::string_view name) {
   if(name.starts_with("assert_")) name.remove_prefix(7);
   if(name=="sha1") return EVP_sha1();
   if(name=="sha256") return EVP_sha256();
   if(name=="sha512") return EVP_sha512();
   if(name=="ripemd160") return EVP_ripemd160();
   throw std::runtime_error("test digest not found");
}
bytes digest(std::string_view name,std::span<const uint8_t> data) {
   bytes result(EVP_MAX_MD_SIZE);unsigned size=0;
   require(EVP_Digest(data.data(),data.size(),result.data(),&size,digest_type(name),nullptr)==1,"OpenSSL test digest failed");
   result.resize(size);return result;
}
struct native_host {
   std::span<uint8_t> memory;
   unsigned calls=0;
   std::span<uint8_t> region(uint32_t offset,uint32_t size) {
      if(uint64_t(offset)+size>memory.size()) throw std::runtime_error("test native pointer out of range");
      return memory.subspan(offset,size);
   }
   uint32_t memcpy(uint32_t dst,uint32_t src,uint32_t n) {
      ++calls;auto d=region(dst,n),s=region(src,n);region(dst,1);
      require((dst>src?uint64_t(dst)-src:uint64_t(src)-dst)>=n,"test memcpy overlap");
      std::memcpy(d.data(),s.data(),n);return dst;
   }
   uint32_t memmove(uint32_t dst,uint32_t src,uint32_t n) {
      ++calls;auto d=region(dst,n),s=region(src,n);region(dst,1);std::memmove(d.data(),s.data(),n);return dst;
   }
   int32_t memcmp(uint32_t a,uint32_t b,uint32_t n) {
      ++calls;auto x=region(a,n),y=region(b,n);auto r=std::memcmp(x.data(),y.data(),n);return (r>0)-(r<0);
   }
   uint32_t memset(uint32_t dst,int32_t value,uint32_t n) {
      ++calls;auto d=region(dst,n);region(dst,1);std::memset(d.data(),value,n);return dst;
   }
   void hash(std::string_view name,uint32_t input,uint32_t n,uint32_t out) {
      ++calls;auto value=digest(name,region(input,n));auto output=region(out,value.size());
      std::copy(value.begin(),value.end(),output.begin());
   }
   void check_hash(std::string_view name,uint32_t input,uint32_t n,uint32_t out) {
      ++calls;auto value=digest(name,region(input,n));auto expected=region(out,value.size());
      require(std::equal(value.begin(),value.end(),expected.begin()),"test native digest mismatch");
   }
#define DIGEST_METHOD(name) \
   void name(uint32_t p,uint32_t n,uint32_t out) {hash(#name,p,n,out);} \
   void assert_##name(uint32_t p,uint32_t n,uint32_t out) {check_hash(#name,p,n,out);}
   DIGEST_METHOD(sha1)
   DIGEST_METHOD(sha256)
   DIGEST_METHOD(sha512)
   DIGEST_METHOD(ripemd160)
#undef DIGEST_METHOD
   void unpriced(uint32_t,uint32_t,uint32_t) {++calls;}
};
using raw_registry=registered_host_functions<native_host>;
using registry=observed_host_functions<raw_registry>;
void register_functions() {
   static std::once_flag once;std::call_once(once,[] {
#define REGISTER(name) raw_registry::add<&native_host::name>("env",#name)
      REGISTER(memcpy);REGISTER(memmove);REGISTER(memcmp);REGISTER(memset);
      REGISTER(sha1);REGISTER(sha256);REGISTER(sha512);REGISTER(ripemd160);
      REGISTER(assert_sha1);REGISTER(assert_sha256);REGISTER(assert_sha512);REGISTER(assert_ripemd160);
      REGISTER(unpriced);
#undef REGISTER
   });
}
void uleb(bytes& out,uint64_t v) {do {auto b=uint8_t(v&127);v>>=7;out.push_back(b|(v?128:0));}while(v);}
void signed_i32(bytes& out,int32_t value) {
   out.push_back(0x41);
   while(true) {
      uint8_t b=value&127;value>>=7;
      const bool done=(value==0 && !(b&64)) || (value==-1 && (b&64));
      out.push_back(b|(done?0:128));if(done) return;
   }
}
void section(bytes& out,uint8_t id,const bytes& data) {out.push_back(id);uleb(out,data.size());out.insert(out.end(),data.begin(),data.end());}
bytes module(std::string_view name,bool returns,std::array<uint32_t,3> args,unsigned calls) {
   bytes out{0,97,115,109,1,0,0,0};
   bytes types{2,0x60,3,0x7f,0x7f,0x7f,uint8_t(returns?1:0)};if(returns) types.push_back(0x7f);
   types.insert(types.end(),{0x60,0,uint8_t(returns?1:0)});if(returns) types.push_back(0x7f);section(out,1,types);
   bytes imports{1,3,'e','n','v'};uleb(imports,name.size());imports.insert(imports.end(),name.begin(),name.end());imports.insert(imports.end(),{0,0});section(out,2,imports);
   section(out,3,{1,1});section(out,5,{1,1,1,1});section(out,7,{1,3,'r','u','n',0,1});
   bytes body{0};for(unsigned i=0;i<calls;++i) {
      for(auto a:args) signed_i32(body,int32_t(a));body.insert(body.end(),{0x10,0});
      if(returns && i+1<calls) body.push_back(0x1a);
   }
   body.push_back(0x0b);bytes code{1};uleb(code,body.size());code.insert(code.end(),body.begin(),body.end());section(out,10,code);return out;
}
template<typename Impl> struct fixture {
   native_host host;
   wasm_allocator allocator;
   backend<registry,Impl> vm;
   std::string name;
   uint32_t length;
   fixture(std::string_view function,uint32_t n=129,unsigned calls=1)
      :vm((register_functions(),module(function,function.starts_with("mem"),arguments(function,n),calls)),&allocator),name(function),length(n) {
      host.memory={reinterpret_cast<uint8_t*>(vm.get_context().linear_memory()),65536};
      for(size_t i=0;i<256;++i) host.memory[32+i]=uint8_t(31+i*7);
      std::copy_n(host.memory.begin()+32,256,host.memory.begin()+512);
      if(function.starts_with("assert_")) {auto h=digest(function,host.region(32,n));std::copy(h.begin(),h.end(),host.memory.begin()+1024);}
   }
   static std::array<uint32_t,3> arguments(std::string_view name,uint32_t n) {
      if(name=="memcmp") return {32,512,n};
      if(name=="memset") return {1024,125,n};
      if(name=="memcpy" || name=="memmove") return {1024,32,n};
      return {32,n,1024};
   }
   auto run(execution_work_meter& meter,const native_work_schedule& schedule) {
      native_work_charge charge{meter,schedule};auto binding=registry::observe(&charge,&native_work_charge::before_call);
      return vm.call_with_return(host,"env","run");
   }
};
template<typename F> void both(F&& f) {
   f.template operator()<interpreter>();
#if defined(__x86_64__)
   f.template operator()<jit>();
#endif
}
uint64_t random(uint64_t& s) {s^=s<<13;s^=s>>7;s^=s<<17;return s;}
}

int main() {
   const std::vector<std::pair<const char*,std::function<void()>>> cases{
      {"catalog_has_exact_names_and_flat_native_ABIs",[] {
         auto p=policy();auto rules=make_native_pure_work_rules(p);require(rules.size()==12,"catalog size changed");register_functions();
         const auto& map=raw_registry::mappings::get();
         for(size_t i=0;i<rules.size();++i) {
            const auto& r=rules[i];const auto& d=native_pure_work_descriptors[i];const auto index=map.named_mapping.at({r.module,r.function});
            const auto& actual=map.host_functions.at(index);
            require(r.function==d.function && actual.params==r.parameters && actual.ret==r.returns,"catalog differs from native registered ABI");
            require(r.sizes.size()==1 && r.sizes[0].argument==d.length_argument && !r.serial_base_units && !r.sizes[0].serial_units_per_chunk,
               "catalog length argument or pure/serial classification changed");
         }
#ifdef AIC_CATALOG_SOURCE_ABI_WITNESS
         verify_catalog_source_abi(rules);
#endif
      }},
      {"all_lengths_and_process_local_indices_match_wide_integer_oracle",[] {
         auto p=policy(UINT32_MAX);auto schedule=make_native_pure_work_schedule(p);require(schedule.version()==17,"lost policy version");
         for(const auto& d:native_pure_work_descriptors) {
            const auto chunk=p.rates[size_t(d.operation)].input_bytes_per_chunk;
            for(uint64_t n:{uint64_t(0),uint64_t(1),chunk-1,chunk,chunk+1,uint64_t(UINT32_MAX)}) {
               event e;for(uint32_t index:{0u,1u,UINT32_MAX}) {
                  auto cost=schedule.quote(e.call(d,n,index));require(cost==execution_work_usage{.host=oracle(p,d,n)},"wrong input operand, rounding, price or serial subset");
               }
            }
         }
      }},
      {"unset_rates_version_or_common_bound_cannot_create_a_free_policy",[] {
         rejects<std::invalid_argument>([] {make_native_pure_work_schedule({});});
         auto p=policy();p.version=0;rejects<std::invalid_argument>([&] {make_native_pure_work_rules(p);});
         p=policy();p.maximum_input_bytes.reset();rejects<std::invalid_argument>([&] {make_native_pure_work_rules(p);});
         p=policy();p.maximum_input_bytes=uint64_t(UINT32_MAX)+1;rejects<std::invalid_argument>([&] {make_native_pure_work_rules(p);});
         for(size_t i=0;i<12;++i) for(unsigned field=0;field<3;++field) {
            p=policy();if(field==0) p.rates[i].base_units=0;else if(field==1) p.rates[i].input_bytes_per_chunk=0;else p.rates[i].units_per_input_chunk=0;
            rejects<std::invalid_argument>([&] {make_native_pure_work_rules(p);});
         }
      }},
      {"explicit_zero_bound_still_charges_each_empty_input_attempt",[] {
         auto p=policy(0);auto schedule=make_native_pure_work_schedule(p);
         for(const auto& d:native_pure_work_descriptors) {
            event e;require(schedule.quote(e.call(d,0)).host==p.rates[size_t(d.operation)].base_units,"empty input is free");
            rejects<std::invalid_argument>([&] {schedule.quote(e.call(d,1));});
         }
      }},
      {"maximum_allowed_prices_are_checked_at_policy_construction",[] {
         uint64_t seed=0x712ce523;
         for(unsigned i=0;i<1000;++i) {
            auto p=policy(std::array<uint64_t,4>{0,1,32,UINT32_MAX}[i%4]);auto& r=p.rates[i%12];
            r.base_units=1+random(seed)%1000000;r.input_bytes_per_chunk=std::array<uint64_t,4>{1,3,64,UINT64_MAX}[(i/4)%4];r.units_per_input_chunk=random(seed)|1;
            const auto n=*p.maximum_input_bytes;const auto cost=static_cast<unsigned __int128>(r.base_units)+
               ((static_cast<unsigned __int128>(n)+r.input_bytes_per_chunk-1)/r.input_bytes_per_chunk)*r.units_per_input_chunk;
            if(cost>UINT64_MAX) rejects<std::invalid_argument>([&] {make_native_pure_work_rules(p);});
            else {auto s=make_native_pure_work_schedule(p);event e;require(s.quote(e.call(native_pure_work_descriptors[i%12],n)).host==uint64_t(cost),"maximum quote differs from wide oracle");}
         }
      }},
      {"unsupported_or_incomplete_metadata_stays_unpriced_and_fatal",[] {
         auto p=policy();auto schedule=make_native_pure_work_schedule(p);event e;auto c=e.call(descriptor("sha256"),7);
         for(unsigned bad=0;bad<5;++bad) {
            auto wrong=c;if(bad==0) wrong.function="unpriced";if(bad==1) wrong.module="eosio_injection";
            if(bad==2) wrong.arguments_complete=false;if(bad==3) wrong.signature_returns=e.result;
            if(bad==4) wrong.arguments=wrong.arguments.first(2);
            execution_work_meter meter(1000);native_work_charge charge{meter,schedule};
            rejects<std::invalid_argument>([&] {native_work_charge::before_call(&charge,wrong);});
            require(meter.exhausted() && !meter.usage().total(),"unpriced metadata did not fail closed atomically");
         }
      }},
      {"real_WASM_calls_preserve_all_twelve_native_results_and_prices",[] {both([]<typename Impl> {
         auto p=policy();auto schedule=make_native_pure_work_schedule(p);
         for(const auto& d:native_pure_work_descriptors) {
            fixture<Impl> f(d.function);const auto cost=oracle(p,d,129);execution_work_meter meter(cost);auto value=f.run(meter,schedule);
            require(f.host.calls==1 && !meter.exhausted() && meter.remaining()==0 && meter.usage()==execution_work_usage{.host=cost},"native bridge lost or duplicated a price");
            if(d.returns_i32) {
               require(value.has_value() && value->to_i32()==(d.function=="memcmp"?0:1024),"memory native return value changed");
               if(d.function=="memset") require(std::all_of(f.host.memory.begin()+1024,f.host.memory.begin()+1153,[](uint8_t b){return b==125;}),"memset data changed");
               else if(d.function!="memcmp") require(std::equal(f.host.memory.begin()+32,f.host.memory.begin()+161,f.host.memory.begin()+1024),"copied memory data changed");
            } else {
               require(!value.has_value(),"void native produced a return value");auto expected=digest(d.function,f.host.region(32,129));
               require(std::equal(expected.begin(),expected.end(),f.host.memory.begin()+1024),"digest output/assert bytes changed");
            }
         }
      });}},
      {"insufficient_budget_stops_before_native_effect_and_same_VM_recovers",[] {both([]<typename Impl> {
         auto p=policy();auto schedule=make_native_pure_work_schedule(p);
         for(const auto& d:native_pure_work_descriptors) {
            fixture<Impl> f(d.function);const auto before=bytes(f.host.memory.begin(),f.host.memory.end());const auto cost=oracle(p,d,129);
            execution_work_meter low(cost-1);rejects<execution_work_exhausted>([&] {f.run(low,schedule);});
            require(!f.host.calls && low.exhausted() && !low.usage().total() && std::equal(before.begin(),before.end(),f.host.memory.begin()),"overbudget native call caused effects or partial fee");
            execution_work_meter exact(cost);f.run(exact,schedule);require(f.host.calls==1 && exact.remaining()==0,"failed price poisoned VM or observer binding");
         }
      });}},
      {"repeated_native_attempts_are_charged_without_deduplication",[] {both([]<typename Impl> {
         auto p=policy();auto schedule=make_native_pure_work_schedule(p);fixture<Impl> f("memset",64,7);
         const auto cost=oracle(p,descriptor("memset"),64);execution_work_meter meter(7*cost);f.run(meter,schedule);
         require(f.host.calls==7 && meter.usage().host==7*cost && !meter.remaining(),"same native arguments were deduplicated");
      });}},
      {"native_assert_failure_keeps_the_attempt_fee_and_original_effect_rules",[] {both([]<typename Impl> {
         auto p=policy();auto schedule=make_native_pure_work_schedule(p);fixture<Impl> f("assert_sha256",3);f.host.memory[1024]^=1;
         const auto expected=bytes(f.host.memory.begin()+1024,f.host.memory.begin()+1056);const auto cost=oracle(p,descriptor("assert_sha256"),3);execution_work_meter meter(cost);
         rejects<std::runtime_error>([&] {f.run(meter,schedule);});
         require(f.host.calls==1 && meter.usage().host==cost && !meter.exhausted() && std::equal(expected.begin(),expected.end(),f.host.memory.begin()+1024),"native failure lost fee or wrote digest output");
      });}},
      {"unpriced_real_VM_import_never_delegates_to_native_code",[] {both([]<typename Impl> {
         auto schedule=make_native_pure_work_schedule(policy());fixture<Impl> f("unpriced",3);execution_work_meter meter(1000);
         rejects<std::invalid_argument>([&] {f.run(meter,schedule);});require(!f.host.calls && meter.exhausted() && !meter.usage().total(),"unpriced native import executed for free");
      });}},
      {"immutable_catalog_schedule_is_shared_with_distinct_thread_budgets",[] {
         auto p=policy();const auto schedule=make_native_pure_work_schedule(p);std::exception_ptr errors[2];
         auto work=[&](unsigned id) {try {
            const auto& d=native_pure_work_descriptors[id];const uint64_t n=31+id,cost=oracle(p,d,n);execution_work_meter meter(1000*cost);native_work_charge charge{meter,schedule};event e;
            for(unsigned i=0;i<1000;++i) native_work_charge::before_call(&charge,e.call(d,n,i));
            require(!meter.remaining() && !meter.exhausted() && meter.usage().host==1000*cost,"shared catalog mixed thread budgets");
         }catch(...) {errors[id]=std::current_exception();}};
         std::thread a(work,0),b(work,1);a.join();b.join();for(auto e:errors) if(e) std::rethrow_exception(e);
      }}
   };
   unsigned failed=0;
   for(const auto& [name,test]:cases) {
      try {test();std::cerr<<name<<": passed\n";}
      catch(const std::exception& e) {++failed;std::cerr<<name<<": FAILED: "<<e.what()<<'\n';}
   }
   std::cout<<"{\"status\":\""<<(failed?"failed":"passed")<<"\",\"cases\":"<<cases.size()<<",\"passed\":"<<cases.size()-failed<<",\"failed\":"<<failed<<"}\n";
   return failed?1:0;
}
