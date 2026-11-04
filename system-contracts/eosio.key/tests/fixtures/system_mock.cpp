// TEST ONLY. Never deploy as eosio.system or publish this as production WASM.
// Alternate system account is selected only in the clearly marked fixture build.
#include "system_view.hpp"
using namespace eosio;
class [[eosio::contract("keysysmockup")]] mock_system : public contract {
 public:
 using contract::contract;
 struct [[eosio::table]] producer : keywallet::producer_view {
    uint64_t trailing=0x12345678;
    EOSLIB_SERIALIZE_DERIVED(producer,keywallet::producer_view,(trailing))
 };
 struct [[eosio::table]] candidate : keywallet::candidate_view {
    std::array<uint64_t,3> evidence{};
    EOSLIB_SERIALIZE_DERIVED(candidate,keywallet::candidate_view,(evidence))
 };
 struct [[eosio::table]] bond : keywallet::bond_view {
    uint64_t trailing=0x87654321;
    EOSLIB_SERIALIZE_DERIVED(bond,keywallet::bond_view,(trailing))
 };
 struct [[eosio::table]] maintenance : keywallet::maintenance_view {
    uint64_t trailing=7;
    EOSLIB_SERIALIZE_DERIVED(maintenance,keywallet::maintenance_view,(trailing))
 };
 using producers=multi_index<"producersv2"_n,producer>;
 using candidates=multi_index<"rotcands"_n,candidate>;
 using bonds=multi_index<"bpbonds"_n,bond>;
 using maint=multi_index<"bpmaint"_n,maintenance>;
 [[eosio::action]]void setram(asset supply,asset ram,asset aic,double ram_weight,double aic_weight){
    require_auth(get_self());
    keywallet::ram_market table(get_self(),get_self().value);
    auto it=table.find(supply.symbol.raw());
    auto update=[&](auto& r){r.supply=supply;r.base.balance=ram;r.quote.balance=aic;r.base.weight=ram_weight;r.quote.weight=aic_weight;};
    if(it==table.end())table.emplace(get_self(),update);else table.modify(it,same_payer,update);
 }
 [[eosio::action]]void dropram(){
    require_auth(get_self());keywallet::ram_market table(get_self(),get_self().value);
    auto it=table.begin();while(it!=table.end())it=table.erase(it);
 }
 [[eosio::action]]void setbp(name owner,public_key key,bool active,bool banned,uint8_t state,int64_t balance,uint8_t maintenance_state){
    require_auth(get_self());
    producers p(get_self(),get_self().value);auto pi=p.find(owner.value);
    auto pu=[&](auto& r){r.owner=owner;r.total_votes=17;r.producer_key=key;r.is_active=active;};
    if(pi==p.end())p.emplace(get_self(),pu);else p.modify(pi,same_payer,pu);
    candidates c(get_self(),get_self().value);auto ci=c.find(owner.value);
    auto cu=[&](auto& r){r.owner=owner;r.banned=banned;r.rank=19;r.previous_rank=31;};
    if(ci==c.end())c.emplace(get_self(),cu);else c.modify(ci,same_payer,cu);
    bonds b(get_self(),get_self().value);auto bi=b.find(owner.value);
    auto bu=[&](auto& r){r.owner=owner;r.state=state;r.balance=balance;};
    if(bi==b.end())b.emplace(get_self(),bu);else b.modify(bi,same_payer,bu);
    maint m(get_self(),get_self().value);auto mi=m.find(owner.value);
    auto mu=[&](auto& r){r.owner=owner;r.state=maintenance_state;};
    if(mi==m.end())m.emplace(get_self(),mu);else m.modify(mi,same_payer,mu);
 }
 [[eosio::action]]void setmarket(int64_t cpu,int64_t net){
    require_auth(get_self());check(cpu>=0&&cpu<=10000&&net>=0&&net<=10000,"invalid test occupancy");
    keywallet::market_view v{};v.version=1;v.powerup_days=30;v.min_powerup_fee=asset(1,symbol("AIC",4));
    for(auto* r:{&v.cpu,&v.net}){r->version=0;r->weight=10000;r->min_price=asset(1,symbol("AIC",4));r->max_price=asset(100000,symbol("AIC",4));}
    v.cpu.utilization=v.cpu.adjusted_utilization=cpu;v.net.utilization=v.net.adjusted_utilization=net;
    keywallet::market_singleton(get_self(),0).set(v,get_self());
 }
};
