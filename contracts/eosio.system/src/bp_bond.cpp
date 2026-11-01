#include <eosio.system/eosio.system.hpp>
#include <eosio.system/bp_bond.hpp>
#include <eosio.system/rotation_state.hpp>
#include <eosio.system/rotation.hpp>
#include <eosio.token/eosio.token.hpp>
#include <eosio.system/upgrade_governance.hpp>
#include <algorithm>
#include <limits>

namespace eosiosystem {
namespace {
rotation_context_v1 bond_context() {
   const auto size=get_rotation_context_v1(nullptr,0);
   check(size>0,"BP bond requires native rotation context");std::vector<char> data(size);
   check(get_rotation_context_v1(data.data(),size)==size,"rotation context changed");
   auto ctx=eosio::unpack<rotation_context_v1>(data.data(),data.size());
   check(ctx.version==1 && ctx.producers.size()<=rotation::committee_limit &&
         ctx.finalizers.size()<=rotation::committee_limit,"unsupported BP bond committee");
   return ctx;
}
bool has_bond_duty(name owner,const rotation_context_v1& ctx,const rotation_info& state) {
   return std::any_of(ctx.producers.begin(),ctx.producers.end(),[&](const auto& p){return p.producer_name==owner;}) ||
      std::any_of(ctx.finalizers.begin(),ctx.finalizers.end(),[&](const auto& f){return f.description==owner.to_string();}) ||
      std::find(state.active.begin(),state.active.end(),owner)!=state.active.end() ||
      std::find(state.target.begin(),state.target.end(),owner)!=state.target.end();
}
bool unsettled_bond_window(name owner,const rotation_info& state) {
   return std::any_of(state.pending_windows.begin(),state.pending_windows.end(),[&](const auto& w){return w.owner==owner;});
}
bool bond_banned(name code,name owner) {
   rotation_candidates candidates(code,code.value);auto c=candidates.find(owner.value);
   return c!=candidates.end() && c->banned;
}
void check_bond_symbol(const symbol& core) {
   check(core==symbol(symbol_code("AIC"),4),"BP bond requires the 4-decimal AIC core token");
}
}

void system_contract::initbpbond() {
   require_auth(get_self());
   check(get_upgrade_developer()==0,"fresh-chain BP bonds must be initialized before governance sealing");
   check_bond_symbol(core_symbol());
   bp_bond_config_singleton configs(get_self(),get_self().value);
   check(!configs.exists(),"BP bond is already initialized");
   authority auth;auth.threshold=1;auth.accounts.push_back({{get_self(),"eosio.code"_n},1});
   for(const auto account:{bp_bond_escrow,bp_bond_forfeit_pool}){
      check(!is_account(account),"BP bond custody account already exists; refusing an unverified authority");
      eosio::action(permission_level{get_self(),active_permission},get_self(),"newaccount"_n,
         std::make_tuple(get_self(),account,auth,auth)).send();
      // Fund fixed custody metadata/RAM with real system tokens. These small
      // setup purchases are separate from BP deposits and issue no currency.
      eosio::action(permission_level{get_self(),active_permission},get_self(),"buyrambytes"_n,
         std::make_tuple(get_self(),account,uint32_t(8192))).send();
   }
   configs.set(bp_bond_config{},get_self());
}

void system_contract::bondbp(const name& producer) {
   require_auth(producer);check_bond_symbol(core_symbol());
   bp_bond_config_singleton configs(get_self(),get_self().value);
   check(configs.exists(),"BP bond is not initialized");
   check(producer!=bp_bond_escrow && producer!=bp_bond_forfeit_pool && producer!=get_self(),"system accounts cannot post a BP bond");
   check(!bond_banned(get_self(),producer),"permanently banned producer cannot post a BP bond");
   bp_bonds bonds(get_self(),get_self().value);auto row=bonds.find(producer.value);
   check(row==bonds.end() || row->state==uint8_t(bp_bond_status::refunded),"BP bond is already locked or forfeited");
   auto cfg=configs.get();check(cfg.locked<=asset::max_amount-bp_bond_required,"BP bond pool overflow");
   const auto stamp=eosio::current_time_point().sec_since_epoch();
   if(row==bonds.end())bonds.emplace(producer,[&](auto& r){r.owner=producer;r.balance=bp_bond_required;r.bonded_at=stamp;});
   else bonds.modify(row,same_payer,[&](auto& r){r=bp_bond{};r.owner=producer;r.balance=bp_bond_required;r.bonded_at=stamp;});
   cfg.locked+=bp_bond_required;configs.set(cfg,get_self());
   eosio::token::transfer_action transfer{token_account,{{producer,active_permission}}};
   transfer.send(producer,bp_bond_escrow,asset(bp_bond_required,core_symbol()),"BP bond: 10000 AIC");
   refresh_rotation_candidate(producer);
}

void system_contract::exitbond(const name& producer) {
   require_auth(producer);bp_bonds bonds(get_self(),get_self().value);
   auto row=bonds.require_find(producer.value,"BP bond not found");
   check(row->state==uint8_t(bp_bond_status::bonded) && row->balance==bp_bond_required,"BP bond is not active");
   check(!bond_banned(get_self(),producer),"permanently banned producer cannot exit a BP bond");
   bonds.modify(row,same_payer,[&](auto& r){r.state=uint8_t(bp_bond_status::exiting);});
   auto registered=_producers.find(producer.value);
   if(registered!=_producers.end())_producers.modify(registered,same_payer,[](auto& p){p.deactivate();});
   refresh_rotation_candidate(producer);
   rotation_singleton rotations(get_self(),get_self().value);
   if(rotations.exists()){
      auto state=rotations.get();state.next_attempt_slot=std::min<uint64_t>(state.next_attempt_slot,bond_context().slot);
      rotations.set(state,get_self());
   }
}

void system_contract::advance_bond_exits() {
   bp_bond_config_singleton configs(get_self(),get_self().value);if(!configs.exists())return;
   bp_bonds bonds(get_self(),get_self().value);auto pending=bonds.get_index<"bystate"_n>();
   auto it=pending.lower_bound(uint64_t(bp_bond_status::exiting));
   if(it==pending.end() || it->state!=uint8_t(bp_bond_status::exiting))return;
   const auto ctx=bond_context();if(ctx.transitioning)return;
   rotation_singleton rotations(get_self(),get_self().value);if(!rotations.exists())return;
   const auto state=rotations.get();const auto cfg=configs.get();
   check(ctx.block_num<std::numeric_limits<uint32_t>::max(),"BP bond exit checkpoint exhausted");
   const auto now=eosio::current_time_point().sec_since_epoch();
   check(now<=std::numeric_limits<uint32_t>::max()-cfg.refund_delay_sec,"BP bond refund time exhausted");
   // Bound work including rows that are not yet safe to release. No history
   // append or token transfer is performed in this per-block path.
   for(uint32_t work=0;work<64 && it!=pending.end() && it->state==uint8_t(bp_bond_status::exiting);++work){
      auto current=it++;
      if(bond_banned(get_self(),current->owner))continue;
      if(has_bond_duty(current->owner,ctx,state) || unsettled_bond_window(current->owner,state))continue;
      pending.modify(current,same_payer,[&](auto& r){r.state=uint8_t(bp_bond_status::cooling);r.exit_block=ctx.block_num+1;
         r.refundable_at=cfg.refund_delay_sec?now+cfg.refund_delay_sec:0;});
   }
}

void system_contract::refundbond(const name& producer) {
   require_auth(producer);bp_bond_config_singleton configs(get_self(),get_self().value);
   check(configs.exists(),"BP bond is not initialized");auto cfg=configs.get();
   check(cfg.refund_delay_sec==bp_bond_refund_delay_sec,"unsupported BP bond refund policy");
   bp_bonds bonds(get_self(),get_self().value);auto row=bonds.require_find(producer.value,"BP bond not found");
   check(row->state==uint8_t(bp_bond_status::cooling) && row->balance==bp_bond_required,"BP bond has not completed duty handoff");
   check(!bond_banned(get_self(),producer),"permanently banned producer cannot receive a BP bond refund");
   check(row->refundable_at && eosio::current_time_point().sec_since_epoch()>=row->refundable_at,"BP bond refund cooldown has not elapsed");
   const auto ctx=bond_context();rotation_singleton rotations(get_self(),get_self().value);
   check(rotations.exists() && !ctx.transitioning && row->exit_block>0 && ctx.final_block_num>=row->exit_block,"BP bond exit is not irreversible");
   const auto state=rotations.get();check(!has_bond_duty(producer,ctx,state) && !unsettled_bond_window(producer,state),"BP still has unresolved duties");
   check(cfg.locked>=bp_bond_required && cfg.refunded<=asset::max_amount-bp_bond_required,"invalid BP refund accounting");
   cfg.locked-=bp_bond_required;cfg.refunded+=bp_bond_required;
   bonds.modify(row,same_payer,[](auto& r){r.balance=0;r.state=uint8_t(bp_bond_status::refunded);});configs.set(cfg,get_self());
   eosio::token::transfer_action transfer{token_account,{{bp_bond_escrow,active_permission}}};
   transfer.send(bp_bond_escrow,producer,asset(bp_bond_required,core_symbol()),"BP bond refund");
}

void system_contract::settlebond(const name& producer) {
   // Anyone may submit settlement; recipient and amount are fixed by policy.
   bp_bond_config_singleton configs(get_self(),get_self().value);check(configs.exists(),"BP bond is not initialized");auto cfg=configs.get();
   bp_bonds bonds(get_self(),get_self().value);auto row=bonds.require_find(producer.value,"BP bond not found");
   check(row->state==uint8_t(bp_bond_status::slashed) && row->forfeited>row->settled,"no unsettled BP forfeiture");
   check(bond_banned(get_self(),producer),"BP forfeiture requires a permanent blacklist record");
   const auto ctx=bond_context();check(row->ban_record_block>0 && ctx.final_block_num>=row->ban_record_block,"BP blacklist decision is not irreversible");
   const auto amount=row->forfeited-row->settled;check(cfg.slash_pending>=amount && cfg.slash_settled<=asset::max_amount-amount,"invalid BP forfeiture accounting");
   cfg.slash_pending-=amount;cfg.slash_settled+=amount;
   bonds.modify(row,same_payer,[&](auto& r){r.settled+=amount;});configs.set(cfg,get_self());
   // Transfer INTO the dedicated forfeiture pool only. This reviewed profile
   // contains no action that transfers tokens out of eosio.bslash or burns them.
   eosio::token::transfer_action transfer{token_account,{{bp_bond_escrow,active_permission}}};
   transfer.send(bp_bond_escrow,bp_bond_forfeit_pool,asset(amount,core_symbol()),"Permanent BP blacklist: locked forfeiture pool");
}
}
