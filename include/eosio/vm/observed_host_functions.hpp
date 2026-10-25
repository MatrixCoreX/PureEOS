#pragma once

#include <eosio/vm/host_function.hpp>
#include <array>
#include <span>
#include <stdexcept>
#include <string_view>

namespace eosio::vm {

struct native_work_argument {
   value_type type;
   uint64_t bits;
   bool operator==(const native_work_argument&) const = default;
};
struct native_work_call {
   static constexpr size_t max_captured_arguments=64;
   std::string_view module, function;
   uint32_t registry_index; // PROCESS LOCAL, never a consensus identifier/price
   std::span<const value_type> signature_params, signature_returns;
   std::span<const native_work_argument> arguments;
   bool arguments_complete;
};

// Opt-in observation/cost-hook adapter, not active billing. It delegates to the
// ORIGINAL registered function/type converter. No address is put in a shared
// module. Callbacks see borrowed ABI values before conversion/delegation, never
// native pointers derived from WASM memory. Full native/authorization/commit
// coverage and a calibrated policy remain separate requirements.
template<typename Registry>
class observed_host_functions : public Registry {
   struct descriptor {std::string_view module,function; bool present=false;};
   static const auto& descriptors() {
      // Complete registration before execution/worker startup. No later registry
      // mutation is supported by this adapter, including replacing a name.
      static const auto result=[] {
         const auto& mappings=Registry::mappings::get();
         std::vector<descriptor> result(mappings.functions.size());
         for(const auto& [name,index]:mappings.named_mapping) {
            if(index>=result.size()) throw std::logic_error("invalid observed host registry");
            result[index]={name.first,name.second,true};
         }
         return result;
      }();
      return result;
   }
public:
   static constexpr size_t max_captured_arguments=native_work_call::max_captured_arguments;
   using callback_type=void(*)(void*,const native_work_call&);
   class binding;
private:
   static inline thread_local binding* current_=nullptr;
public:
   class binding {
      friend class observed_host_functions;
      binding* previous_;
      void* data_;
      callback_type callback_;
   public:
      binding(void* data,callback_type callback) : previous_(current_),data_(data),callback_(callback) {
         if(!callback) throw std::invalid_argument("missing native work observer");
         (void)descriptors(); // publish immutable descriptors before execution
         current_=this;
      }
      binding(const binding&)=delete;
      binding& operator=(const binding&)=delete;
      binding(binding&&)=delete;
      binding& operator=(binding&&)=delete;
      ~binding() {current_=previous_;}
   };
   [[nodiscard]] static binding observe(void* data,callback_type callback) {return binding(data,callback);}

   void operator()(typename Registry::host_type_t* host,typename Registry::execution_interface_t ei,uint32_t index) {
      if(!current_) throw std::logic_error("native work observer is unbound");
      const auto& maps=Registry::mappings::get(); const auto& names=descriptors();
      if(maps.functions.size()!=names.size() || index>=names.size() || !names[index].present || index>=maps.host_functions.size())
         throw std::logic_error("observed host registry changed after freezing");
      const auto& signature=maps.host_functions[index];
      std::array<native_work_argument,max_captured_arguments> values;
      const bool complete=signature.params.size()<=values.size();
      if(complete) {
         if(!ei.os || ei.os->size()<signature.params.size()) throw wasm_interpreter_exception{"native work argument stack underflow"};
         for(size_t i=0;i<signature.params.size();++i) {
            const auto& value=ei.operand_from_back(signature.params.size()-1-i);
            uint64_t bits;
            switch(signature.params[i]) {
               case i32: if(!value.template is_a<i32_const_t>()) throw wasm_interpreter_exception{"invalid observed i32"}; bits=value.to_ui32(); break;
               case i64: if(!value.template is_a<i64_const_t>()) throw wasm_interpreter_exception{"invalid observed i64"}; bits=value.to_ui64(); break;
               case f32: if(!value.template is_a<f32_const_t>()) throw wasm_interpreter_exception{"invalid observed f32"}; bits=value.to_fui32(); break;
               case f64: if(!value.template is_a<f64_const_t>()) throw wasm_interpreter_exception{"invalid observed f64"}; bits=value.to_fui64(); break;
               default: throw wasm_interpreter_exception{"unsupported observed native ABI type"};
            }
            values[i]={signature.params[i],bits};
         }
      }
      // Oversized signatures still produce ONE opaque call event. A future
      // policy must treat !arguments_complete as unknown, not price partial data.
      const auto& name=names[index];
      const native_work_call event{name.module,name.function,index,signature.params,signature.ret,
         std::span<const native_work_argument>{values.data(),complete?signature.params.size():0},complete};
      current_->callback_(current_->data_,event);
      Registry::operator()(host,std::move(ei),index);
   }
};

} // namespace eosio::vm
