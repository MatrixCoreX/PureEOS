#pragma once

#include <eosio/chain/native_work_pricing.hpp>
#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace eosio::chain {

// A small, explicit ABI catalog, not a complete native cost policy. There are
// no default fees or hardware inputs. Only names in this catalog are emitted;
// authorization, database, variable-complexity crypto and commit stay unpriced.
enum class native_pure_work_operation : uint8_t {
   memory_copy, memory_move, memory_compare, memory_set,
   sha1, sha256, sha512, ripemd160,
   assert_sha1, assert_sha256, assert_sha512, assert_ripemd160,
   count
};
inline constexpr size_t native_pure_work_rule_count=
   static_cast<size_t>(native_pure_work_operation::count);

struct native_pure_work_descriptor {
   native_pure_work_operation operation;
   std::string_view function;
   uint32_t length_argument;
   bool returns_i32;
};

inline constexpr std::array<native_pure_work_descriptor,native_pure_work_rule_count>
native_pure_work_descriptors{{
   {native_pure_work_operation::memory_copy,"memcpy",2,true},
   {native_pure_work_operation::memory_move,"memmove",2,true},
   {native_pure_work_operation::memory_compare,"memcmp",2,true},
   {native_pure_work_operation::memory_set,"memset",2,true},
   {native_pure_work_operation::sha1,"sha1",1,false},
   {native_pure_work_operation::sha256,"sha256",1,false},
   {native_pure_work_operation::sha512,"sha512",1,false},
   {native_pure_work_operation::ripemd160,"ripemd160",1,false},
   {native_pure_work_operation::assert_sha1,"assert_sha1",1,false},
   {native_pure_work_operation::assert_sha256,"assert_sha256",1,false},
   {native_pure_work_operation::assert_sha512,"assert_sha512",1,false},
   {native_pure_work_operation::assert_ripemd160,"assert_ripemd160",1,false}
}};

struct native_pure_work_rate {
   uint64_t base_units=0;
   uint64_t input_bytes_per_chunk=0;
   uint64_t units_per_input_chunk=0;
};

struct native_pure_work_policy {
   uint32_t version=0;
   // Unset is invalid. Explicit zero permits only zero-length inputs, which
   // still pay base_units. A common chain policy must supply the actual bound.
   std::optional<uint64_t> maximum_input_bytes;
   std::array<native_pure_work_rate,native_pure_work_rule_count> rates{};
};

// Price = base + ceil(requested_input_bytes / chunk) * chunk_units.
// These are policy work units, NOT exact hash compression rounds. In particular
// base/chunk rates must conservatively cover padding/finalization, digest
// writes/comparisons, pointer validation and the operation's reads AND writes.
// memcmp is priced at its full requested length even when it exits early.
// Calibration and full native/authorization/commit coverage remain separate.
[[nodiscard]] inline std::vector<native_work_rule>
make_native_pure_work_rules(const native_pure_work_policy& policy) {
   if (!policy.version || !policy.maximum_input_bytes || *policy.maximum_input_bytes>UINT32_MAX)
      throw std::invalid_argument("native pure work policy needs a version and explicit WASM32 input bound");
   const uint64_t maximum=*policy.maximum_input_bytes;
   // Validate the entire policy before publishing any rule. Every allowed
   // input, including the maximum, must have a representable positive price.
   for (const auto& rate:policy.rates) {
      if (!rate.base_units || !rate.input_bytes_per_chunk || !rate.units_per_input_chunk)
         throw std::invalid_argument("native pure work policy contains an unconfigured rate");
      const auto chunks=maximum/rate.input_bytes_per_chunk+(maximum%rate.input_bytes_per_chunk!=0);
      if (chunks>(UINT64_MAX-rate.base_units)/rate.units_per_input_chunk)
         throw std::invalid_argument("native pure work maximum input price overflows");
   }
   std::vector<native_work_rule> rules;
   rules.reserve(native_pure_work_rule_count);
   for (const auto& descriptor:native_pure_work_descriptors) {
      const auto& rate=policy.rates[static_cast<size_t>(descriptor.operation)];
      // All 12 ABIs are three i32 operands. Pointer/span conversion and return
      // pointers are WASM32, regardless of the native process's pointer width.
      // Only memory operations return i32; all eight hash operations are void.
      rules.push_back(native_work_rule{
         "env",std::string(descriptor.function),
         {eosio::vm::i32,eosio::vm::i32,eosio::vm::i32},
         descriptor.returns_i32?std::vector<eosio::vm::value_type>{eosio::vm::i32}
                               :std::vector<eosio::vm::value_type>{},
         rate.base_units,0,
         {{descriptor.length_argument,rate.input_bytes_per_chunk,rate.units_per_input_chunk,0,maximum}}
      });
   }
   return rules;
}

[[nodiscard]] inline native_work_schedule
make_native_pure_work_schedule(const native_pure_work_policy& policy) {
   return native_work_schedule(policy.version,make_native_pure_work_rules(policy));
}

} // namespace eosio::chain
