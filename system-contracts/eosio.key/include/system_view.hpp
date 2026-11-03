// SPDX-License-Identifier: LicenseRef-AIC-Noncommercial-3.0
#pragma once
#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <eosio/crypto.hpp>
#include <eosio/singleton.hpp>

namespace keywallet {
using namespace eosio;
#ifndef KEY_SYSTEM_ACCOUNT
#define KEY_SYSTEM_ACCOUNT "eosio"
#endif
inline constexpr name system_account{KEY_SYSTEM_ACCOUNT};

// Read-only prefixes of qualified PureEOS b230c7e table layouts. Never write
// these projections: native rows have additional serialized trailing fields.
struct producer_view {
   name owner;
   int64_t total_votes;
   public_key producer_key;
   bool is_active;
   uint64_t primary_key() const { return owner.value; }
   EOSLIB_SERIALIZE(producer_view, (owner)(total_votes)(producer_key)(is_active))
};
using producers_view = multi_index<"producersv2"_n, producer_view>;
struct candidate_view {
   name owner;
   uint128_t rank, previous_rank;
   uint64_t frozen_epoch, seen_epoch, ticket, penalty_epoch;
   uint8_t zero_windows;
   bool banned;
   uint64_t primary_key() const { return owner.value; }
   EOSLIB_SERIALIZE(candidate_view, (owner)(rank)(previous_rank)(frozen_epoch)(seen_epoch)(ticket)(penalty_epoch)(zero_windows)(banned))
};
using candidates_view = multi_index<"rotcands"_n, candidate_view>;
struct maintenance_view {
   name owner;
   uint8_t state;
   uint64_t primary_key() const { return owner.value; }
   EOSLIB_SERIALIZE(maintenance_view, (owner)(state))
};
using maintenance_views = multi_index<"bpmaint"_n, maintenance_view>;
struct bond_view {
   name owner;
   uint8_t state;
   int64_t balance;
   uint64_t primary_key() const { return owner.value; }
   EOSLIB_SERIALIZE(bond_view, (owner)(state)(balance))
};
using bonds_view = multi_index<"bpbonds"_n, bond_view>;

struct market_resource {
   uint8_t version;
   int64_t weight, weight_ratio, assumed_stake_weight;
   int64_t initial_weight_ratio, target_weight_ratio;
   time_point_sec initial_timestamp, target_timestamp;
   double exponent;
   uint32_t decay_secs;
   asset min_price, max_price;
   int64_t utilization, adjusted_utilization;
   time_point_sec utilization_timestamp;
   EOSLIB_SERIALIZE(market_resource, (version)(weight)(weight_ratio)(assumed_stake_weight)(initial_weight_ratio)(target_weight_ratio)(initial_timestamp)(target_timestamp)(exponent)(decay_secs)(min_price)(max_price)(utilization)(adjusted_utilization)(utilization_timestamp))
};
struct market_view {
   uint8_t version;
   market_resource net, cpu;
   uint32_t powerup_days;
   asset min_powerup_fee;
   EOSLIB_SERIALIZE(market_view, (version)(net)(cpu)(powerup_days)(min_powerup_fee))
};
using market_singleton = singleton<"powup.state"_n, market_view>;

struct token_stat {
   asset supply, max_supply;
   name issuer;
   uint64_t primary_key() const { return supply.symbol.code().raw(); }
   EOSLIB_SERIALIZE(token_stat, (supply)(max_supply)(issuer))
};
using token_stats = multi_index<"stat"_n, token_stat>;
struct token_balance {
   asset balance;
   uint64_t primary_key() const { return balance.symbol.code().raw(); }
   EOSLIB_SERIALIZE(token_balance, (balance))
};
using token_balances = multi_index<"accounts"_n, token_balance>;
}
