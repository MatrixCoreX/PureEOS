// SPDX-License-Identifier: LicenseRef-AIC-Noncommercial-3.0
#include <eosio/eosio.hpp>
#include <eosio/action.hpp>
#include <eosio/asset.hpp>
#include <eosio/crypto.hpp>
#include <eosio/crypto_ext.hpp>
#include <tuple>
#include <vector>
#include "key_codec.hpp"
#include "curve_check.hpp"

namespace signup {
using namespace eosio;
struct key_weight {public_key key;uint16_t weight;EOSLIB_SERIALIZE(key_weight,(key)(weight))};
struct permission_weight {permission_level permission;uint16_t weight;EOSLIB_SERIALIZE(permission_weight,(permission)(weight))};
struct wait_weight {uint32_t wait_sec;uint16_t weight;EOSLIB_SERIALIZE(wait_weight,(wait_sec)(weight))};
struct authority {
   uint32_t threshold=1;std::vector<key_weight> keys;std::vector<permission_weight> accounts;std::vector<wait_weight> waits;
   EOSLIB_SERIALIZE(authority,(threshold)(keys)(accounts)(waits))
};
struct connector {asset balance;double weight;EOSLIB_SERIALIZE(connector,(balance)(weight))};
struct ram_row {
   asset supply;connector base,quote;
   uint64_t primary_key()const{return supply.symbol.raw();}
   EOSLIB_SERIALIZE(ram_row,(supply)(base)(quote))
};
using ram_market=multi_index<"rammarket"_n,ram_row>;
const checksum256 token_code(std::array<uint8_t,32>{
   0xf8,0xc5,0xfb,0xfb,0x14,0xbb,0xab,0x7d,0xa4,0xf5,0xbd,0x3c,0xb0,0x4a,0x2a,0x89,
   0x67,0xc5,0xe0,0xd3,0x9f,0x57,0x94,0xeb,0x12,0x76,0x8a,0x13,0xe1,0x37,0x5d,0x0c});
const checksum256 system_code(std::array<uint8_t,32>{
   0xb5,0x91,0xca,0x66,0x1e,0x82,0x56,0xb7,0xd1,0xc7,0x37,0x0f,0xe1,0x1e,0x58,0x25,
   0x42,0x7c,0x98,0xb4,0xf8,0x4e,0xce,0xb3,0xcd,0xf7,0x35,0xc0,0x6e,0x5e,0x81,0x70});
constexpr int64_t stake_net=1000,stake_cpu=1000;
constexpr int64_t minimum_ram_bytes=4096;

class [[eosio::contract("eosio.signup")]] signup_contract:public contract {
public:
   using contract::contract;
   [[eosio::on_notify("*::transfer")]]
   void ontransfer(name from,name to,asset quantity,std::string memo) {
      if(from==get_self()||to!=get_self())return;
      check(get_self()=="eosio.signup"_n,"registration must use the reviewed eosio.signup account");
      check(get_first_receiver()=="eosio.token"_n,"signup accepts only eosio.token AIC");
      require_auth(from);
      check(get_code_hash("eosio.token"_n)==token_code&&get_code_hash("eosio"_n)==system_code,
            "registration token/system code requires review");
      check(quantity.is_valid()&&quantity.symbol==symbol("AIC",4)&&quantity.amount>stake_net+stake_cpu,
            "positive AIC sufficient for CPU, NET and RAM is required");
      signup_request request;const char* error=nullptr;
      const bool memo_valid=parse_memo(memo,request,error);
      check(memo_valid,error?error:"invalid registration memo");
      const name account{request.account};check(!is_account(account),"account name is already registered");
      const auto owner=decode(request.owner),active=request.active==request.owner?owner:decode(request.active);
      const asset ram{quantity.amount-stake_net-stake_cpu,quantity.symbol};
      ram_market market("eosio"_n,"eosio"_n.value);
      const auto& row=market.get(symbol("RAMCORE",4).raw(),"RAM market is unavailable");
      check(row.base.balance.symbol==symbol("RAM",0)&&row.quote.balance.symbol==quantity.symbol&&
            row.base.balance.amount>0&&row.quote.balance.amount>0&&row.base.weight==0.5&&row.quote.weight==0.5,
            "unsupported RAM market");
      const int64_t net=ram.amount-(ram.amount+199)/200;
      const __int128 estimated=__int128(net)*row.base.balance.amount/(__int128(row.quote.balance.amount)+net);
      check(net>0&&estimated>=minimum_ram_bytes,"registration amount buys less than the minimum RAM allocation");
      authority owner_auth,active_auth;owner_auth.keys.push_back({owner,1});active_auth.keys.push_back({active,1});
      const permission_level self{get_self(),"active"_n};
      action(self,"eosio"_n,"newaccount"_n,std::make_tuple(get_self(),account,owner_auth,active_auth)).send();
      action(self,"eosio"_n,"buyram"_n,std::make_tuple(get_self(),account,ram)).send();
      action(self,"eosio"_n,"delegatebw"_n,
             std::make_tuple(get_self(),account,asset(stake_net,quantity.symbol),asset(stake_cpu,quantity.symbol),true)).send();
   }
   [[eosio::on_notify("eosio::logbuyram")]]
   void onbuyram(name payer,name receiver,asset quantity,int64_t bytes,int64_t ram_bytes,asset fee) {
      if(payer!=get_self())return;
      check(get_first_receiver()=="eosio"_n&&get_code_hash("eosio"_n)==system_code,
            "unreviewed RAM purchase notification");
      check(receiver!=get_self()&&quantity.symbol==symbol("AIC",4)&&bytes>=minimum_ram_bytes&&ram_bytes>=bytes&&
            fee.symbol==quantity.symbol&&fee.amount==(quantity.amount+199)/200,
            "actual registration RAM purchase does not meet policy");
   }
private:
   static public_key decode(const std::string& text) {
      decoded_key key;const char* error=nullptr;
      auto digest=[](const uint8_t* p,size_t n){return ripemd160(reinterpret_cast<const char*>(p),n).extract_as_byte_array();};
      const bool key_valid=decode_public_key(text,key,digest,error);
      check(key_valid,error?error:"invalid public key");
      auto exponentiate=[](const field_element& base,const field_element& exp,const field_element& mod,field_element& out){
         return eosio::mod_exp(reinterpret_cast<const char*>(base.data()),32,reinterpret_cast<const char*>(exp.data()),32,
                               reinterpret_cast<const char*>(mod.data()),32,reinterpret_cast<char*>(out.data()),32)==0;
      };
      check(valid_point(key,exponentiate),"public key is not on the selected curve");
      ecc_public_key bytes{};std::copy(key.bytes.begin(),key.bytes.end(),bytes.begin());
      return key.kind==0?public_key(std::in_place_index<0>,bytes):public_key(std::in_place_index<1>,bytes);
   }
};
}
