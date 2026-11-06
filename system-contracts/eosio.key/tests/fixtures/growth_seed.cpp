// TEST ONLY. Never ship this program as eosio.key or publish it as an upgrade.
// An owned, isolated pre-sealing test account uses it to create arithmetic
// boundary vectors without pretending to allocate billions of real records.
#include "eosio.key.hpp"

class [[eosio::contract("growthseed")]] growth_seed : public eosio::contract {
 public:
   using contract::contract;
   [[eosio::action]] void seed(uint8_t mode) {
      using namespace keywallet;
      require_auth(permission_level{get_self(), "owner"_n});
      check(mode >= 1 && mode <= 3, "test seed mode");
      key_contract::config_table config(get_self(), get_self().value);
      auto cfg = config.get();
      key_contract::growth_table growth(get_self(), get_self().value);
      key_contract::assets registered(get_self(), get_self().value);
      auto a = registered.require_find(1, "test fee asset");
      if (mode == 1) {
         cfg.wallet_count = uint32_t(-1) - 1;
         cfg.balance_count = uint32_t(-1) - 1;
         cfg.next_wallet = uint64_t(uint32_t(-1));
         if (growth.exists()) growth.remove();
         registered.modify(a, same_payer, [&](auto& row) {
            row.ram_sponsor = get_self();
            row.ram_purchased = row.ram_used = (uint64_t(1) << 40) - 1;
         });
      } else if (mode == 2) {
         cfg.wallet_count = cfg.balance_count = uint32_t(-1);
         cfg.next_wallet = uint64_t(-1) - 1;
         key_contract::growth_counters counts;
         counts.wallet_count = uint64_t(-1) - 2;
         counts.balance_count = uint64_t(-1) - 1;
         counts.asset_count = cfg.asset_count;
         growth.set(counts, get_self());
      } else {
         registered.modify(a, same_payer, [&](auto& row) {
            row.ram_purchased = row.ram_used = uint64_t(-1) - 128;
         });
      }
      config.set(cfg, get_self());
   }
};
