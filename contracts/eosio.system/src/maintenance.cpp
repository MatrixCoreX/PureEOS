#include <eosio.system/eosio.system.hpp>
#include <eosio.system/maintenance.hpp>
#include <eosio.system/upgrade_governance.hpp>
#include <algorithm>
#include <limits>
namespace eosiosystem {
namespace {
rotation_context_v1 maintenance_context() {
   const auto size=get_rotation_context_v1(nullptr,0);
   check(size>0,"maintenance requires native rotation context");
   std::vector<char> bytes(size);
   check(get_rotation_context_v1(bytes.data(),size)==size,"rotation context changed");
   auto ctx=eosio::unpack<rotation_context_v1>(bytes.data(),bytes.size());
   check(ctx.version==1,"unsupported native rotation context");
   check(ctx.producers.size()<=rotation::committee_limit&&ctx.finalizers.size()<=rotation::committee_limit,
         "maintenance requires the bounded PureEOS committee");
   return ctx;
}
bool has_duty(name owner,const rotation_context_v1& ctx,const rotation_info& state) {
   return std::any_of(ctx.producers.begin(),ctx.producers.end(),[&](const auto& p){return p.producer_name==owner;})||
      std::any_of(ctx.finalizers.begin(),ctx.finalizers.end(),[&](const auto& f){return f.description==owner.to_string();})||
      std::find(state.target.begin(),state.target.end(),owner)!=state.target.end()||
      std::find(state.active.begin(),state.active.end(),owner)!=state.active.end();
}
}
void system_contract::setmaint(const name& producer,bool maintenance) {
   require_auth(producer);
   check(get_upgrade_developer()!=0,"maintenance requires sealed rotation governance");
   rotation_singleton rotations(get_self(),get_self().value);
   check(rotations.exists(),"rotation is not initialized");
   auto rotation=rotations.get();check(rotation.version==2,"unsupported rotation version");
   check(rotation.target.size()<=rotation::committee_limit&&rotation.active.size()<=rotation::committee_limit,
         "invalid rotation membership");
   rotation_candidates candidates(get_self(),get_self().value);
   auto candidate=candidates.find(producer.value);
   check(candidate==candidates.end()||!candidate->banned,"permanently banned producer cannot use maintenance");
   auto registered=_producers.find(producer.value);
   check(registered!=_producers.end()&&registered->active(),"producer must have an active registration");
   bp_maintenance_table table(get_self(),get_self().value);
   auto existing=table.find(producer.value);
   const auto ctx=maintenance_context();
   check(ctx.block_num<std::numeric_limits<uint32_t>::max(),"maintenance checkpoint exhausted");
   if(maintenance) {
      check(existing==table.end()||existing->state==uint8_t(maintenance_status::normal),"maintenance already requested");
      const bool duty=has_duty(producer,ctx,rotation);
      // A native transition outside the known rotation target cannot be guessed.
      check(duty||!ctx.transitioning||rotation.stage!=0,"unknown policy transition; retry maintenance later");
      auto update=[&](auto& row){
         row.owner=producer;row.state=uint8_t(duty?maintenance_status::draining:maintenance_status::paused);
         row.requested_slot=ctx.slot;row.effective_slot=duty?0:ctx.slot;
         row.effective_block=duty?0:ctx.block_num+1;
      };
      if(existing==table.end())table.emplace(producer,update);
      else table.modify(existing,same_payer,update);
      // Reconsider only the next idle selection; never roll back an in-flight target.
      rotation.next_attempt_slot=std::min<uint64_t>(rotation.next_attempt_slot,ctx.slot);
      rotations.set(rotation,get_self());
   } else {
      check(existing!=table.end()&&existing->state!=uint8_t(maintenance_status::normal),"producer is not in maintenance");
      table.modify(existing,same_payer,[&](auto& row){row.state=uint8_t(maintenance_status::normal);row.resumed_slot=ctx.slot;});
      rotation.next_attempt_slot=std::min<uint64_t>(rotation.next_attempt_slot,ctx.slot);
      rotations.set(rotation,get_self());
   }
   // Deliberately preserve ranking, votes, resource weights, unpaid blocks,
   // last claim time, zero-window evidence and all permanent-ban records.
}
void system_contract::advance_maintenance() {
   bp_maintenance_table table(get_self(),get_self().value);
   auto pending=table.get_index<"bystate"_n>();
   auto it=pending.lower_bound(uint64_t(maintenance_status::draining));
   if(it==pending.end()||it->state!=uint8_t(maintenance_status::draining))return;
   const auto ctx=maintenance_context();
   if(ctx.transitioning)return;
   rotation_singleton rotations(get_self(),get_self().value);if(!rotations.exists())return;
   const auto rotation=rotations.get();
   check(ctx.block_num<std::numeric_limits<uint32_t>::max(),"maintenance checkpoint exhausted");
   // Only current proposer/finalizer/target members can enter draining.
   // Their union is bounded by 3*21; process at most 64 entries per block.
   for(uint32_t work=0;work<64&&it!=pending.end()&&it->state==uint8_t(maintenance_status::draining);++work) {
      auto current=it++;
      if(has_duty(current->owner,ctx,rotation))continue;
      pending.modify(current,same_payer,[&](auto& row){
         row.state=uint8_t(maintenance_status::paused);
         row.effective_slot=ctx.slot;row.effective_block=ctx.block_num+1;
      });
   }
}
}
