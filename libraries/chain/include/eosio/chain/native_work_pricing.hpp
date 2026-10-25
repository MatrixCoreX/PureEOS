#pragma once

#include <eosio/chain/execution_work_meter.hpp>
#include <eosio/vm/observed_host_functions.hpp>
#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

namespace eosio::chain {

// Prototype policy mechanism, NOT a production cost table or activation. The
// eventual version/rates must come from common chain policy, never local core
// count, clocks, registry order, database row addresses or benchmark results.
struct native_work_size_price {
   uint32_t argument = 0;
   uint64_t bytes_per_chunk = 0;
   uint64_t units_per_chunk = 0;
   uint64_t serial_units_per_chunk = 0; // subset, not an extra fee
   uint64_t maximum_argument = 0; // must be explicitly bounded by the policy
};
struct native_work_rule {
   std::string module, function;
   std::vector<eosio::vm::value_type> parameters, returns;
   uint64_t base_units = 0, serial_base_units = 0;
   std::vector<native_work_size_price> sizes;
};

class native_work_schedule {
   uint32_t version_;
   std::vector<native_work_rule> rules_;
   static bool scalar(eosio::vm::value_type type) {
      return type==eosio::vm::i32 || type==eosio::vm::i64 || type==eosio::vm::f32 || type==eosio::vm::f64;
   }
   static auto name(const native_work_rule& rule) {
      return std::pair<std::string_view,std::string_view>{rule.module,rule.function};
   }
   static uint64_t multiply(uint64_t a,uint64_t b) {
      if(b && a>UINT64_MAX/b) throw std::overflow_error("native work cost overflow");
      return a*b;
   }
public:
   static constexpr size_t max_rules=1024, max_size_terms=16; // component bounds, not chain rules
   native_work_schedule(uint32_t version,std::vector<native_work_rule> rules)
      : version_(version),rules_(std::move(rules)) {
      if(!version_ || rules_.empty() || rules_.size()>max_rules)
         throw std::invalid_argument("invalid native work schedule");
      std::sort(rules_.begin(),rules_.end(),[](const auto& a,const auto& b) {return name(a)<name(b);});
      for(size_t i=0;i<rules_.size();++i) {
         const auto& r=rules_[i];
         if(r.module.empty() || r.function.empty() || r.module.size()>256 || r.function.size()>256 ||
            (i && name(rules_[i-1])==name(r)) || !r.base_units || r.serial_base_units>r.base_units ||
            r.parameters.size()>eosio::vm::native_work_call::max_captured_arguments ||
            r.returns.size()>1 || r.sizes.size()>max_size_terms ||
            !std::all_of(r.parameters.begin(),r.parameters.end(),scalar) || !std::all_of(r.returns.begin(),r.returns.end(),scalar))
            throw std::invalid_argument("invalid native work rule");
         for(const auto& s:r.sizes) {
            if(s.argument>=r.parameters.size() ||
               (r.parameters[s.argument]!=eosio::vm::i32 && r.parameters[s.argument]!=eosio::vm::i64) ||
               !s.bytes_per_chunk || !s.units_per_chunk || s.serial_units_per_chunk>s.units_per_chunk ||
               (r.parameters[s.argument]==eosio::vm::i32 && s.maximum_argument>UINT32_MAX))
               throw std::invalid_argument("invalid native work size price");
         }
      }
   }
   native_work_schedule(const native_work_schedule&)=default;
   native_work_schedule& operator=(const native_work_schedule&)=delete;
   uint32_t version() const noexcept {return version_;}

   // No access to WASM memory, no side effect, no allocation on the success
   // path. Payload work is bounded from raw ABI lengths before delegation.
   // This does NOT cover data-dependent native internals (e.g. auth traversal),
   // outputs, code loading or canonical commit. Those need additional hooks.
   execution_work_usage quote(const eosio::vm::native_work_call& call) const {
      const auto key=std::pair{call.module,call.function};
      auto it=std::lower_bound(rules_.begin(),rules_.end(),key,[](const auto& rule,const auto& key) {return name(rule)<key;});
      if(it==rules_.end() || name(*it)!=key || !call.arguments_complete ||
         !std::equal(call.signature_params.begin(),call.signature_params.end(),it->parameters.begin(),it->parameters.end()) ||
         !std::equal(call.signature_returns.begin(),call.signature_returns.end(),it->returns.begin(),it->returns.end()) ||
         call.arguments.size()!=it->parameters.size())
         throw std::invalid_argument("native call has no complete matching work policy");
      for(size_t i=0;i<call.arguments.size();++i) {
         const auto& a=call.arguments[i];
         if(a.type!=it->parameters[i] || ((a.type==eosio::vm::i32 || a.type==eosio::vm::f32) && a.bits>UINT32_MAX))
            throw std::invalid_argument("invalid native work ABI argument");
      }
      execution_work_usage cost{.host=it->base_units,.serial=it->serial_base_units};
      for(const auto& s:it->sizes) {
         const auto bytes=call.arguments[s.argument].bits;
         if(bytes>s.maximum_argument) throw std::invalid_argument("native work size exceeds policy bound");
         const auto chunks=bytes/s.bytes_per_chunk+(bytes%s.bytes_per_chunk!=0);
         cost.host=execution_work_usage::add(cost.host,multiply(chunks,s.units_per_chunk));
         cost.serial=execution_work_usage::add(cost.serial,multiply(chunks,s.serial_units_per_chunk));
      }
      cost.validate();
      return cost;
   }
};

// Pass as the observer's per-execution callback data. No schedule or budget is
// retained by compiled code. Each attempted call is charged before delegation;
// unknown/opaque/mismatched policies fail closed and do not call native code.
struct native_work_charge {
   execution_work_meter& meter;
   const native_work_schedule& schedule;
   static void before_call(void* opaque,const eosio::vm::native_work_call& call) {
      auto& self=*static_cast<native_work_charge*>(opaque);
      try {self.meter.consume(self.schedule.quote(call));}
      catch(...) {self.meter.invalidate(); throw;}
   }
};

} // namespace eosio::chain
