// TEST ONLY: incompatible/forged notification tokens, never a production token.
#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <eosio/singleton.hpp>
#include <eosio/crypto.hpp>
using namespace eosio;
class [[eosio::contract("keybadtokenx")]] bad_token : public contract {
 public:
 using contract::contract;
 struct [[eosio::table]] stats {asset supply,max_supply;name issuer;uint64_t primary_key()const{return supply.symbol.code().raw();}};
 struct [[eosio::table]] row {asset balance;uint64_t primary_key()const{return balance.symbol.code().raw();}};
 struct [[eosio::table("mode")]] mode {uint8_t value=0;};
 using stat_table=multi_index<"stat"_n,stats>;
 using accounts=multi_index<"accounts"_n,row>;
 using modes=singleton<"mode"_n,mode>;
 struct withdrawal {
    public_key from;name token_contract,to;asset quantity;std::string memo;
    uint64_t nonce;uint32_t expires;name relayer,reward_to;asset max_fee;
    EOSLIB_SERIALIZE(withdrawal,(from)(token_contract)(to)(quantity)(memo)(nonce)(expires)(relayer)(reward_to)(max_fee))
 };
 struct [[eosio::table("reentry")]] callback {withdrawal request;signature sig;};
 using callbacks=singleton<"reentry"_n,callback>;
 [[eosio::action]]void arm(withdrawal request,signature sig){require_auth(get_self());callbacks(get_self(),get_self().value).set(callback{request,sig},get_self());}
 [[eosio::action]]void setmode(uint8_t value){require_auth(get_self());modes(get_self(),get_self().value).set(mode{value},get_self());}
 [[eosio::action]]void create(name owner,asset quantity){
    require_auth(get_self());check(quantity.amount>0,"positive test issue");
    stat_table s(get_self(),quantity.symbol.code().raw());
    s.emplace(get_self(),[&](auto&r){r.supply=quantity;r.max_supply=quantity;r.issuer=get_self();});
    add(owner,quantity);
 }
 void add(name owner,asset q){accounts t(get_self(),owner.value);auto it=t.find(q.symbol.code().raw());
    if(it==t.end())t.emplace(get_self(),[&](auto&r){r.balance=q;});else t.modify(it,same_payer,[&](auto&r){r.balance+=q;});}
 [[eosio::action]]void transfer(name from,name to,asset quantity,std::string memo){
    auto v=modes(get_self(),get_self().value).get_or_default().value;
    if(v!=3)require_auth(from); // Mode3 deliberately forges a victim's from field.
    check(from!=to&&is_account(to)&&quantity.amount>0,"invalid transfer");
    accounts t(get_self(),from.value);auto it=t.require_find(quantity.symbol.code().raw(),"balance missing");
    check(it->balance.symbol==quantity.symbol&&it->balance.amount>=quantity.amount,"insufficient test token");
    t.modify(it,same_payer,[&](auto&r){r.balance-=quantity;});
    auto received=quantity;if(v==1)received.amount-=1;add(to,received);
    if(!(v==2&&from=="eosio.key"_n))require_recipient(from);
    require_recipient(to);
    if(from=="eosio.key"_n&&v==4)
       action(permission_level{"eosio.key"_n,"active"_n},"eosio.token"_n,"transfer"_n,
              std::make_tuple("eosio.key"_n,get_self(),asset(10000,symbol("AIC",4)),std::string("TEST unauthorized cross-token callback"))).send();
    if(from=="eosio.key"_n&&v==5){
       auto cb=callbacks(get_self(),get_self().value).get();
       action(permission_level{get_self(),"active"_n},"eosio.key"_n,"selfout"_n,
              std::make_tuple(get_self(),cb.request,cb.sig)).send();
    }
 }
};
