#pragma once
#include <eosio/eosio.hpp>
#include <eosio/singleton.hpp>
#include <eosio/asset.hpp>

namespace eosiosystem {
inline constexpr int64_t bp_bond_required = 100000000; // 10,000.0000 AIC
inline constexpr eosio::name bp_bond_escrow = "eosio.bond"_n;
inline constexpr eosio::name bp_bond_forfeit_pool = "eosio.bslash"_n;
inline constexpr uint32_t bp_bond_refund_delay_sec = 7u*24*60*60;
enum class bp_bond_status : uint8_t { bonded=0, exiting=1, cooling=2, refunded=3, slashed=4 };

struct [[eosio::table("bpbondcfg"), eosio::contract("eosio.system")]] bp_bond_config {
   uint8_t version = 1;
   bool enforced = true;
   uint32_t refund_delay_sec = bp_bond_refund_delay_sec;
   int64_t locked = 0;
   int64_t slash_pending = 0;
   int64_t slash_settled = 0;
   int64_t refunded = 0;
   EOSLIB_SERIALIZE(bp_bond_config,(version)(enforced)(refund_delay_sec)
                   (locked)(slash_pending)(slash_settled)(refunded))
};
using bp_bond_config_singleton = eosio::singleton<"bpbondcfg"_n,bp_bond_config>;

struct [[eosio::table("bpbonds"), eosio::contract("eosio.system")]] bp_bond {
   eosio::name owner;
   uint8_t state = uint8_t(bp_bond_status::bonded);
   int64_t balance = 0;
   int64_t forfeited = 0;
   int64_t settled = 0;
   uint32_t bonded_at = 0;
   uint32_t exit_block = 0;
   uint32_t refundable_at = 0;
   uint32_t ban_final_block = 0;
   uint32_t ban_record_block = 0;
   uint64_t primary_key() const { return owner.value; }
   uint64_t by_state() const { return state; }
   EOSLIB_SERIALIZE(bp_bond,(owner)(state)(balance)(forfeited)(settled)(bonded_at)
                   (exit_block)(refundable_at)(ban_final_block)(ban_record_block))
};
using bp_bonds = eosio::multi_index<"bpbonds"_n,bp_bond,
   eosio::indexed_by<"bystate"_n,eosio::const_mem_fun<bp_bond,uint64_t,&bp_bond::by_state>>>;

// Fresh-chain profile: custody is initialized before governance sealing.
// Every ordinary BP must fund its own bond before registration or admission.
inline bool bp_bond_enforced(eosio::name code) {
   bp_bond_config_singleton cfg(code,code.value);
   return cfg.exists() && cfg.get().enforced;
}
inline bool bp_bond_funded(eosio::name code,eosio::name producer) {
   bp_bonds table(code,code.value);const auto row=table.find(producer.value);
   return row!=table.end() && row->state==uint8_t(bp_bond_status::bonded) && row->balance==bp_bond_required;
}
inline bool bp_bond_excludes(eosio::name code,eosio::name producer) {
   return bp_bond_enforced(code) && !bp_bond_funded(code,producer);
}

// Called only by the deterministic system-contract blacklist path. No external
// actions or recipient notifications may interrupt onblock/rotation here.
inline void forfeit_bp_bond(eosio::name code,eosio::name producer,uint32_t evidence_block,uint32_t record_block) {
   bp_bonds bonds(code,code.value);auto row=bonds.find(producer.value);
   if(row==bonds.end() || row->balance==0)return;
   bp_bond_config_singleton configs(code,code.value);
   eosio::check(configs.exists(),"BP bond accounting is not initialized");
   auto cfg=configs.get();const auto amount=row->balance;
   eosio::check(amount==bp_bond_required && cfg.locked>=amount,"invalid BP bond liability");
   eosio::check(cfg.slash_pending<=eosio::asset::max_amount-amount,"BP slash pool overflow");
   cfg.locked-=amount;cfg.slash_pending+=amount;
   bonds.modify(row,eosio::same_payer,[&](auto& r){
      r.balance=0;r.forfeited=amount;r.settled=0;r.state=uint8_t(bp_bond_status::slashed);
      r.refundable_at=0;r.ban_final_block=evidence_block;r.ban_record_block=record_block;
   });
   configs.set(cfg,code);
}
}
