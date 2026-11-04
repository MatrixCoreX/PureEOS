// SPDX-License-Identifier: LicenseRef-AIC-Noncommercial-3.0
#pragma once
#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <eosio/crypto.hpp>
#include <eosio/singleton.hpp>
#include <eosio/binary_extension.hpp>
#include <string>
#include "system_view.hpp"

namespace keywallet {
using namespace eosio;

struct fee_policy {
   int64_t minimum_fee;
   int64_t unit_fee;
   int64_t new_row_fee; // Legacy wire/state field; market-priced WASM ignores it.
   uint32_t maximum_multiplier_bps;
   uint32_t epoch_seconds;
   uint32_t max_wallets;
   uint32_t max_balances;
   uint32_t max_assets;
   uint32_t max_native_payers;
   EOSLIB_SERIALIZE(fee_policy, (minimum_fee)(unit_fee)(new_row_fee)(maximum_multiplier_bps)(epoch_seconds)(max_wallets)(max_balances)(max_assets)(max_native_payers))
};
struct transfer_request {
   public_key from, to;
   name token_contract;
   asset quantity;
   std::string memo;
   uint64_t auth_epoch;
   checksum256 request_id;
   name relayer, reward_to;
   asset max_fee;
   binary_extension<uint16_t> relay_fee_bps;
   // Preserve canonical legacy bytes: this CDT serializes an empty binary
   // extension as its default value, so signed requests handle omission here.
   template<typename Stream> friend Stream& operator<<(Stream& ds, const transfer_request& r) {
      ds  << r.from << r.to << r.token_contract << r.quantity << r.memo << r.auth_epoch << r.request_id << r.relayer << r.reward_to << r.max_fee;
      if (r.relay_fee_bps.has_value()) ds << r.relay_fee_bps.value();
      return ds;
   }
   template<typename Stream> friend Stream& operator>>(Stream& ds, transfer_request& r) {
      return ds  >> r.from >> r.to >> r.token_contract >> r.quantity >> r.memo >> r.auth_epoch >> r.request_id >> r.relayer >> r.reward_to >> r.max_fee >> r.relay_fee_bps;
   }
};
struct withdrawal_request {
   public_key from;
   name token_contract, to;
   asset quantity;
   std::string memo;
   uint64_t auth_epoch;
   checksum256 request_id;
   name relayer, reward_to;
   asset max_fee;
   binary_extension<uint16_t> relay_fee_bps;
   // Preserve canonical legacy bytes: this CDT serializes an empty binary
   // extension as its default value, so signed requests handle omission here.
   template<typename Stream> friend Stream& operator<<(Stream& ds, const withdrawal_request& r) {
      ds  << r.from << r.token_contract << r.to << r.quantity << r.memo << r.auth_epoch << r.request_id << r.relayer << r.reward_to << r.max_fee;
      if (r.relay_fee_bps.has_value()) ds << r.relay_fee_bps.value();
      return ds;
   }
   template<typename Stream> friend Stream& operator>>(Stream& ds, withdrawal_request& r) {
      return ds  >> r.from >> r.token_contract >> r.to >> r.quantity >> r.memo >> r.auth_epoch >> r.request_id >> r.relayer >> r.reward_to >> r.max_fee >> r.relay_fee_bps;
   }
};
struct cancellation_request {
   public_key from;
   uint64_t auth_epoch, next_epoch;
   checksum256 request_id;
   name relayer, reward_to;
   asset max_fee;
   binary_extension<uint16_t> relay_fee_bps;
   // Preserve canonical legacy bytes: this CDT serializes an empty binary
   // extension as its default value, so signed requests handle omission here.
   template<typename Stream> friend Stream& operator<<(Stream& ds, const cancellation_request& r) {
      ds  << r.from << r.auth_epoch << r.next_epoch << r.request_id << r.relayer << r.reward_to << r.max_fee;
      if (r.relay_fee_bps.has_value()) ds << r.relay_fee_bps.value();
      return ds;
   }
   template<typename Stream> friend Stream& operator>>(Stream& ds, cancellation_request& r) {
      return ds  >> r.from >> r.auth_epoch >> r.next_epoch >> r.request_id >> r.relayer >> r.reward_to >> r.max_fee >> r.relay_fee_bps;
   }
};

class [[eosio::contract("eosio.key")]] key_contract : public contract {
 public:
   using contract::contract;

   [[eosio::action]] void init(checksum256 chain_id, checksum256 deployment,
                              name fee_contract, symbol fee_symbol,
                              checksum256 system_hash, fee_policy policy);
   [[eosio::action]] void regtoken(name payer, name token_contract, symbol token_symbol, uint32_t min_bytes);
   [[eosio::on_notify("eosio::logbuyram")]]
   void onbuyram(name payer, name receiver, asset quantity, int64_t bytes, int64_t ram_bytes, asset fee);
   [[eosio::action]] void regrelay(name bp, name permission, std::string endpoint);
   [[eosio::action]] void setrelay(name bp, bool enabled);
   [[eosio::action]] void setrate(name bp, uint16_t fee_bps);
   [[eosio::action]] void prove(public_key owner, checksum256 chain_id, checksum256 deployment, uint32_t reference_block_num);
   [[eosio::action]] void ksend(transfer_request request);
   [[eosio::action]] void withdraw(withdrawal_request request);
   [[eosio::action]] void selfout(name payer, withdrawal_request request);
   [[eosio::action]] void cancel(cancellation_request request);
   [[eosio::action]] void selfcancel(name payer, cancellation_request request);
   // Pay full earnings, fee-free; recipient token RAM must be pre-opened.
   [[eosio::action]] void claim(name bp, name to, asset quantity, asset max_fee);
   [[eosio::action]] void refundfee(name owner, name to, asset quantity, asset max_fee);
   [[eosio::action]] void finishout(uint64_t asset_id, uint64_t ticket);
   [[eosio::action]] void receipt(name kind, checksum256 request_id, checksum256 transaction_id, name native_from,
                                 checksum256 from_key, checksum256 to_key,
                                 name token_contract, asset quantity, asset fee,
                                 name relayer, uint64_t auth_epoch);
   [[eosio::on_notify("*::transfer")]]
   void ontransfer(name from, name to, asset quantity, std::string memo);

   struct [[eosio::table("config")]] configuration {
      uint32_t version = 2;
      checksum256 chain_id, deployment, system_hash;
      name fee_contract;
      symbol fee_symbol;
      fee_policy policy;
      uint64_t next_wallet = 1, next_asset = 2;
      uint32_t wallet_count = 0, balance_count = 0, asset_count = 1, native_payer_count = 0;
      checksum256 token_hash;
      EOSLIB_SERIALIZE(configuration, (version)(chain_id)(deployment)(system_hash)(fee_contract)(fee_symbol)(policy)(next_wallet)(next_asset)(wallet_count)(balance_count)(asset_count)(native_payer_count)(token_hash))
   };
   using config_table = singleton<"config"_n, configuration>;
   struct [[eosio::table]] wallet {
      uint64_t id;
      public_key key;
      checksum256 key_hash;
      uint64_t auth_epoch = 0;
      uint64_t primary_key() const { return id; }
      checksum256 by_key() const { return key_hash; }
      EOSLIB_SERIALIZE(wallet, (id)(key)(key_hash)(auth_epoch))
   };
   using wallets = multi_index<"wallets"_n, wallet,
      indexed_by<"bykey"_n, const_mem_fun<wallet, checksum256, &wallet::by_key>>>;
   struct [[eosio::table]] balance {
      uint64_t asset_id;
      int64_t amount = 0;
      uint64_t primary_key() const { return asset_id; }
      EOSLIB_SERIALIZE(balance, (asset_id)(amount))
   };
   using balances = multi_index<"balances"_n, balance>;
   struct [[eosio::table]] registered_asset {
      uint64_t id;
      name token_contract;
      symbol token_symbol;
      checksum256 identity;
      int64_t reserve = 0;
      bool busy = false, notified = false;
      uint64_t out_ticket = 0;
      name out_to;
      int64_t out_amount = 0, receiver_before = 0;
      name ram_sponsor;
      uint64_t ram_purchased = 0, ram_used = 0;
      checksum256 funding_tx;
      uint64_t primary_key() const { return id; }
      checksum256 by_identity() const { return identity; }
      EOSLIB_SERIALIZE(registered_asset, (id)(token_contract)(token_symbol)(identity)(reserve)(busy)(notified)(out_ticket)(out_to)(out_amount)(receiver_before)(ram_sponsor)(ram_purchased)(ram_used)(funding_tx))
   };
   using assets = multi_index<"assets"_n, registered_asset,
      indexed_by<"byidentity"_n, const_mem_fun<registered_asset, checksum256, &registered_asset::by_identity>>>;
   struct [[eosio::table]] native_credit {
      name owner;
      int64_t amount = 0;
      uint64_t primary_key() const { return owner.value; }
      EOSLIB_SERIALIZE(native_credit, (owner)(amount))
   };
   using native_credits = multi_index<"nativefees"_n, native_credit>;
   struct [[eosio::table]] fee_bucket {
      name owner;
      int64_t network = 0, relay_earned = 0;
      uint64_t primary_key() const { return owner.value; }
      EOSLIB_SERIALIZE(fee_bucket, (owner)(network)(relay_earned))
   };
   using fee_buckets = multi_index<"feebuckets"_n, fee_bucket>;
   struct [[eosio::table]] relay {
      name bp, permission;
      std::string endpoint;
      bool enabled;
      uint64_t primary_key() const { return bp.value; }
      EOSLIB_SERIALIZE(relay, (bp)(permission)(endpoint)(enabled))
   };
   using relays = multi_index<"relays"_n, relay>;
   struct [[eosio::table]] relay_rate {
      name bp;
      uint16_t fee_bps;
      uint64_t primary_key() const { return bp.value; }
      EOSLIB_SERIALIZE(relay_rate, (bp)(fee_bps))
   };
   using relay_rates = multi_index<"relayrates"_n, relay_rate>;
   struct [[eosio::table("pricing")]] pricing {
      uint64_t epoch = 0;
      uint32_t multiplier_bps = 10000, cpu_occupancy_bps = 0, net_occupancy_bps = 0;
      EOSLIB_SERIALIZE(pricing, (epoch)(multiplier_bps)(cpu_occupancy_bps)(net_occupancy_bps))
   };
   using pricing_table = singleton<"pricing"_n, pricing>;

 private:
   struct charge { int64_t base, premium, total; };
   configuration config() const;
   void check_system(const configuration& cfg) const;
   void check_token(name token, symbol sym) const;
   void check_bp(name bp, const configuration& cfg) const;
   void check_relay(name bp, name reward_to, const configuration& cfg) const;
   static checksum256 hash_key(const public_key& key);
   static checksum256 hash_asset(name token, symbol sym);
   static void validate_key(const public_key& key);
   static public_key decode_key(const std::string& text);
   static int64_t checked(__int128 value);
   static int64_t raw_balance(name token, name owner, symbol sym);
   uint64_t find_wallet(const public_key& key) const;
   uint64_t ensure_wallet(const public_key& key, uint64_t asset_id);
   uint64_t find_asset(name token, symbol sym) const;
   uint64_t ensure_asset(name token, symbol sym);
   void consume_ram(uint64_t asset_id, uint64_t bytes);
   void check_asset(uint64_t id) const;
   int64_t balance_of(uint64_t wallet_id, uint64_t asset_id) const;
   void credit(uint64_t wallet_id, uint64_t asset_id, int64_t amount);
   void debit(uint64_t wallet_id, uint64_t asset_id, int64_t amount);
   void epoch(uint64_t wallet_id, uint64_t expected, uint64_t next);
   uint32_t ram_for(const public_key& key, uint64_t asset_id) const;
   int64_t ram_cost(const configuration& cfg, uint32_t bytes) const;
   uint32_t multiplier(const configuration& cfg, bool exit_path);
   uint16_t relay_fee(name bp, const binary_extension<uint16_t>& signed_bps) const;
   charge quote(const configuration& cfg, uint32_t units, uint32_t bytes, uint16_t relay_bps, bool exit_path = false);
   void credit_fee_bucket(name owner, const charge& fee);
   void charge_native(name owner, const charge& fee, const asset& maximum);
   void charge_key(uint64_t wallet_id, name bp, const charge& fee, const asset& maximum);
   void check_max(const configuration& cfg, const charge& fee, const asset& maximum) const;
   void send_out(uint64_t asset_id, name to, int64_t quantity, const std::string& memo);
   void record(name kind, checksum256 request_id, name native_from, checksum256 from,
               checksum256 to, name token, asset quantity, int64_t fee, name relay, uint64_t n);
   void perform_withdraw(name payer, const withdrawal_request& request, bool direct);
   void perform_cancel(name payer, const cancellation_request& request, bool direct);
   static std::vector<char> raw_transaction();
   void verify_envelope(name operation, const public_key& owner, uint64_t auth_epoch,
                        checksum256 request_id, const std::vector<char>& payload, const configuration& cfg) const;
};
}
