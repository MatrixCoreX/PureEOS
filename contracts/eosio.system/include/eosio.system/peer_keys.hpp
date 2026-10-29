#pragma once
#include <eosio/contract.hpp>
#include <eosio/crypto.hpp>
#include <eosio/eosio.hpp>
#include <optional>
#include <variant>
#include <vector>
namespace eosiosystem {
struct [[eosio::table("peerkeys"), eosio::contract("eosio.system")]] peer_key {
   struct v0_data {
      std::optional<eosio::public_key> pubkey;
      EOSLIB_SERIALIZE(v0_data, (pubkey))
   };
   eosio::name account;
   std::variant<v0_data> data;
   uint64_t primary_key() const { return account.value; }
   std::optional<eosio::public_key> get_public_key() const { return std::get<0>(data).pubkey; }
};
using peer_keys_table = eosio::multi_index<"peerkeys"_n, peer_key>;

// A separate dispatcher class avoids the state-writing system_contract
// destructor during the node's read-only getpeerkeys call.
class [[eosio::contract("eosio.system")]] peer_keys : public eosio::contract {
 public:
   using eosio::contract::contract;
   struct peerkeys_t {
      eosio::name producer_name;
      std::optional<eosio::public_key> peer_key;
      EOSLIB_SERIALIZE(peerkeys_t, (producer_name)(peer_key))
   };
   using getpeerkeys_res_t = std::vector<peerkeys_t>;
   [[eosio::action]] void regpeerkey(const eosio::name& account, const eosio::public_key& key);
   [[eosio::action]] void delpeerkey(const eosio::name& account);
   [[eosio::action]] getpeerkeys_res_t getpeerkeys();
};
}
