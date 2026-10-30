#pragma once
#include <eosio/eosio.hpp>
namespace eosiosystem {
enum class maintenance_status : uint8_t { normal = 0, draining = 1, paused = 2 };
struct [[eosio::table("bpmaint"), eosio::contract("eosio.system")]] bp_maintenance {
   eosio::name owner;
   uint8_t state = 0;
   uint32_t requested_slot = 0;
   uint32_t effective_slot = 0;
   uint32_t effective_block = 0;
   uint32_t resumed_slot = 0;
   uint64_t primary_key() const { return owner.value; }
   uint64_t by_state() const { return state; }
   EOSLIB_SERIALIZE(bp_maintenance,(owner)(state)(requested_slot)(effective_slot)(effective_block)(resumed_slot))
};
using bp_maintenance_table = eosio::multi_index<"bpmaint"_n,bp_maintenance,
   eosio::indexed_by<"bystate"_n,eosio::const_mem_fun<bp_maintenance,uint64_t,&bp_maintenance::by_state>>>;
inline bool maintenance_excludes(eosio::name code,eosio::name owner) {
   bp_maintenance_table table(code,code.value);auto it=table.find(owner.value);
   return it!=table.end()&&it->state!=uint8_t(maintenance_status::normal);
}
inline bool maintenance_paused(eosio::name code,eosio::name owner) {
   bp_maintenance_table table(code,code.value);auto it=table.find(owner.value);
   return it!=table.end()&&it->state==uint8_t(maintenance_status::paused);
}
}
