#include <eosio/chain/system_account_proposal.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/permission_object.hpp>
#include <eosio/chain/authorization_manager.hpp>
#include <eosio/chain/protocol_feature_manager.hpp>
#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/transaction_context.hpp>
#include <eosio/chain/asset.hpp>
#include <algorithm>
#include <limits>

namespace eosio::chain {
namespace {
constexpr uint32_t creation_authority_version = 1;
constexpr uint32_t max_creation_code_bytes = 16 * 1024 * 1024;
constexpr uint32_t max_creation_abi_bytes = 1024 * 1024;
constexpr int64_t max_creation_ram_payment = 10000 * 10000ll; // Reviewed bounded bootstrap, never a treasury permission.

bool activated(const apply_context& ctx) {
   return ctx.control.is_builtin_activated(builtin_protocol_feature_t::pureeos_system_account_governance);
}

const upgrade_governance_object& enabled(apply_context& ctx) {
   EOS_ASSERT(ctx.control.is_builtin_activated(builtin_protocol_feature_t::pureeos_system_account_governance),
              action_validate_exception,"system account proposal feature is not activated");
   EOS_ASSERT(!ctx.is_read_only() && ctx.get_current_action_trace().creator_action_ordinal.value == 0,
              action_validate_exception,"system account governance requires top-level writable authorization");
   const auto* gov=find_upgrade_governance(ctx.db);
   EOS_ASSERT(gov && gov->developer != account_name{},action_validate_exception,"sealed developer governance is required");
   return *gov;
}
void require_developer(apply_context& ctx,const upgrade_governance_object& gov) {
   ctx.require_authorization(gov.developer,"upgrade"_n);
}
void pending(const system_account_proposal_object* state,uint64_t id) {
   EOS_ASSERT(state && state->pending && state->proposal_id==id,action_validate_exception,
              "system account proposal is not pending or id does not match");
}
authority creation_owner() {
   authority a(1,{},{});a.accounts.push_back({{"eosio"_n,config::eosio_code_name},1});return a;
}
authority creation_active(account_name target) {
   auto a=creation_owner();a.accounts.push_back({{target,config::eosio_code_name},1});std::sort(a.accounts.begin(),a.accounts.end());return a;
}
bool valid_target(account_name target) {
   const auto text=target.to_string();
   return text.size()>6 && text.size()<=12 && text.starts_with("eosio.") &&
          target!="eosio.prods"_n && target!="eosio.msig"_n && target!="eosio.wrap"_n && target!="eosio.null"_n;
}
uint32_t review_seconds(const apply_context& ctx) {
   if(!ctx.control.is_builtin_activated(builtin_protocol_feature_t::pureeos_seven_day_governance))return upgrade_review_seconds;
#if defined(PUREEOS_TEST_BUILD) && defined(PUREEOS_TEST_GOVERNANCE_30MIN)
   return 1800;
#else
   return upgrade_review_seconds_v2;
#endif
}
void direct_inline(apply_context& ctx,action_name name,bytes data,std::vector<permission_level> permissions) {
   action act;act.account="eosio"_n;act.name=name;act.authorization=std::move(permissions);act.data=std::move(data);
   ctx.execute_inline(std::move(act));
}
}

const system_account_proposal_object* find_system_account_proposal(const chainbase::database& db) {
   const auto& index=db.get_index<system_account_proposal_index,by_id>();return index.empty()?nullptr:&*index.begin();
}
upgrade_effective_tally get_system_account_effective_tally(const chainbase::database& db,const system_account_proposal_object& state) {
   upgrade_effective_tally tally;
   for(auto voter:state.proposal_voters) {
      if(is_upgrade_voter_permanently_banned(db,voter))tally.excluded_banned_voters.push_back(voter);
      else tally.voters.push_back(voter);
   }
   for(auto voter:state.vetoes)if(std::binary_search(tally.voters.begin(),tally.voters.end(),voter))tally.vetoes.push_back(voter);
   return tally;
}

void apply_eosio_syspropose(apply_context& ctx) {
   // Preserve the pre-feature unknown-action behavior during historical replay.
   if(!activated(ctx))return;
   const auto& gov=enabled(ctx);require_developer(ctx,gov);
   const auto args=ctx.get_action().data_as<syspropose>();
   const auto ordinal=ctx.get_current_action_trace().action_ordinal.value;
   EOS_ASSERT(ctx.trx_context.upgrade_voters_action_ordinal && ordinal==*ctx.trx_context.upgrade_voters_action_ordinal+1,
              action_validate_exception,"syspropose must immediately follow refreshgov in the same transaction");
   ctx.trx_context.upgrade_voters_action_ordinal.reset();
   const auto* state=find_system_account_proposal(ctx.db);
   EOS_ASSERT(!state || !state->pending,action_validate_exception,"cancel or execute the previous system account proposal first");
   EOS_ASSERT((valid_target(args.target) && !ctx.db.find<account_object,by_name>(args.target)),
              action_validate_exception,"target must be a new permitted eosio.* account");
   EOS_ASSERT(gov.protected_accounts.size()<64 && !is_protected_system_account(ctx.db,args.target),
              action_validate_exception,"protected account capacity exhausted or target already protected");
   EOS_ASSERT(args.resource_payer!=account_name{} && !is_protected_system_account(ctx.db,args.resource_payer),
              action_validate_exception,"resource payer must be an existing non-system account");
   ctx.db.get<account_object,by_name>(args.resource_payer);
   EOS_ASSERT(args.ram_payment>0 && args.ram_payment<=max_creation_ram_payment,
              action_validate_exception,"invalid bounded system account RAM payment");
   EOS_ASSERT(args.authority_version==creation_authority_version && args.code_bytes>0 && args.code_bytes<=max_creation_code_bytes &&
              args.abi_bytes>0 && args.abi_bytes<=max_creation_abi_bytes && args.code_hash!=digest_type{} &&
              args.abi_hash!=digest_type{} && args.review_document_hash!=digest_type{},
              action_validate_exception,"invalid creation authority or reviewed payload bounds");
   EOS_ASSERT(!gov.current_voters.empty() && gov.current_voters.size()<=max_upgrade_voters,
              action_validate_exception,"no valid frozen system account electorate");
   const auto system_hash=ctx.db.get<account_metadata_object,by_name>("eosio"_n).code_hash;
   const auto token_hash=ctx.db.get<account_metadata_object,by_name>("eosio.token"_n).code_hash;
   EOS_ASSERT(system_hash!=digest_type{} && token_hash!=digest_type{},action_validate_exception,
              "reviewed system and fee-token code must exist");
   if(!state) {
      state=&ctx.db.create<system_account_proposal_object>([](auto&){});
      // Fixed logical reservation, independent of compiler object layout.
      ctx.add_ram_usage(gov.developer,1024+16*max_upgrade_voters);
   }
   EOS_ASSERT(state->proposal_id<std::numeric_limits<uint64_t>::max(),action_validate_exception,"creation proposal id exhausted");
   ctx.db.modify(*state,[&](auto& s){
      ++s.proposal_id;s.pending=true;s.outcome=0;s.completed_at=time_point{};
      s.target=args.target;s.resource_payer=args.resource_payer;s.ram_payment=args.ram_payment;
      s.authority_version=args.authority_version;s.code_bytes=args.code_bytes;s.abi_bytes=args.abi_bytes;
      s.code_hash=args.code_hash;s.abi_hash=args.abi_hash;s.review_document_hash=args.review_document_hash;
      s.system_code_hash=system_hash;s.token_code_hash=token_hash;
      s.review_seconds=review_seconds(ctx);s.proposed_at=ctx.control.pending_block_time();s.execute_after=s.proposed_at+fc::seconds(s.review_seconds);
      s.proposal_voters=gov.current_voters;s.vetoes.clear();
   });
}

void apply_eosio_sysveto(apply_context& ctx) {
   if(!activated(ctx))return;
   enabled(ctx);const auto args=ctx.get_action().data_as<sysveto>();const auto* state=find_system_account_proposal(ctx.db);pending(state,args.proposal_id);
   if(args.voter=="eosio"_n)ctx.require_authorization(args.voter,"veto"_n);else ctx.require_authorization(args.voter);
   EOS_ASSERT(ctx.control.pending_block_time()<state->execute_after,action_validate_exception,"system account veto period has ended");
   EOS_ASSERT(std::binary_search(state->proposal_voters.begin(),state->proposal_voters.end(),args.voter) &&
              !is_upgrade_voter_permanently_banned(ctx.db,args.voter),action_validate_exception,"voter is not an effective frozen BP");
   EOS_ASSERT(std::find(state->vetoes.begin(),state->vetoes.end(),args.voter)==state->vetoes.end(),
              action_validate_exception,"BP already vetoed this system account proposal");
   ctx.db.modify(*state,[&](auto& s){s.vetoes.emplace_back(args.voter);});
}
void apply_eosio_syscancel(apply_context& ctx) {
   if(!activated(ctx))return;
   const auto& gov=enabled(ctx);require_developer(ctx,gov);const auto* state=find_system_account_proposal(ctx.db);
   pending(state,ctx.get_action().data_as<syscancel>().proposal_id);
   ctx.db.modify(*state,[&](auto& s){s.pending=false;s.outcome=2;s.completed_at=ctx.control.pending_block_time();});
}

void apply_eosio_sysexec(apply_context& ctx) {
   if(!activated(ctx))return;
   const auto& gov=enabled(ctx);require_developer(ctx,gov);const auto args=ctx.get_action().data_as<sysexec>();
   const auto* state=find_system_account_proposal(ctx.db);pending(state,args.proposal_id);
   // The sponsor explicitly signs execution; a developer proposal cannot debit
   // an arbitrary account or spend unbounded system treasury funds.
   ctx.require_authorization(state->resource_payer,config::active_name);
   EOS_ASSERT(ctx.control.pending_block_time()>=state->execute_after,action_validate_exception,"system account review period has not ended");
   const auto tally=get_system_account_effective_tally(ctx.db,*state);
   EOS_ASSERT(!tally.voters.empty() && tally.vetoes.size()<upgrade_veto_threshold(tally.voters.size()),
              action_validate_exception,"system account creation vetoed or effective electorate empty");
   EOS_ASSERT((valid_target(state->target) && !ctx.db.find<account_object,by_name>(state->target) && gov.protected_accounts.size()<64),
              action_validate_exception,"system account target or capacity changed");
   EOS_ASSERT((ctx.db.get<account_metadata_object,by_name>("eosio"_n).code_hash==state->system_code_hash &&
              ctx.db.get<account_metadata_object,by_name>("eosio.token"_n).code_hash==state->token_code_hash),
              action_validate_exception,"system or fee-token code changed since creation review");
   EOS_ASSERT(args.code.size()==state->code_bytes && args.abi.size()==state->abi_bytes &&
              ctx.trx_context.hash_with_checktime<fc::sha256>(args.code.data(),args.code.size())==state->code_hash &&
              ctx.trx_context.hash_with_checktime<fc::sha256>(args.abi.data(),args.abi.size())==state->abi_hash,
              action_validate_exception,"creation payload differs from reviewed code/ABI");
   EOS_ASSERT(!ctx.trx_context.system_upgrade,action_validate_exception,"only one reviewed system capability per transaction");
   const auto target=state->target;
   std::vector<account_name> protected_accounts(gov.protected_accounts.begin(),gov.protected_accounts.end());
   protected_accounts.insert(std::lower_bound(protected_accounts.begin(),protected_accounts.end(),target),target);
   ctx.db.modify(gov,[&](auto& s){s.protected_accounts=protected_accounts;});
   auto& permit=ctx.trx_context.system_upgrade.emplace();permit.target=target;permit.code_hash=state->code_hash;permit.abi_hash=state->abi_hash;
   permit.parent_action_ordinal=ctx.get_current_action_trace().action_ordinal.value;
   permit.account_creation_authorized=true;permit.creation_proposal_id=state->proposal_id;
   ctx.execute_inline(action({permission_level{"eosio"_n,config::active_name}},newaccount{"eosio"_n,target,creation_owner(),creation_active(target)}));
   const asset payment{state->ram_payment,symbol::from_string("4,AIC")};
   action fund;fund.account="eosio.token"_n;fund.name="transfer"_n;fund.authorization={{state->resource_payer,config::active_name}};
   fund.data=fc::raw::pack(std::make_tuple(state->resource_payer,target,payment,std::string("reviewed system account RAM funding")));
   ctx.execute_inline(std::move(fund));
   direct_inline(ctx,"buyram"_n,fc::raw::pack(std::make_tuple(target,target,payment)),{{target,config::active_name}});
   ctx.execute_inline(action({permission_level{target,config::active_name}},setcode{target,0,0,args.code}));
   ctx.execute_inline(action({permission_level{target,config::active_name}},setabi{target,args.abi}));
   ctx.execute_inline(action({permission_level{"eosio"_n,config::active_name}},sysfinish{state->proposal_id}));
}

bool consume_system_account_creation(apply_context& ctx,const newaccount& args) {
   auto& permit=ctx.trx_context.system_upgrade;
   if(!permit || !permit->account_creation_authorized)return false;
   EOS_ASSERT(ctx.control.is_builtin_activated(builtin_protocol_feature_t::pureeos_system_account_governance) &&
              !permit->account_created && ctx.get_sender()=="eosio"_n && args.creator=="eosio"_n && args.name==permit->target &&
              ctx.get_current_action_trace().creator_action_ordinal.value==permit->parent_action_ordinal &&
              fc::raw::pack(args.owner)==fc::raw::pack(creation_owner()) && fc::raw::pack(args.active)==fc::raw::pack(creation_active(args.name)),
              action_validate_exception,"system account creation does not match its single-use reviewed capability");
   const auto* state=find_system_account_proposal(ctx.db);pending(state,permit->creation_proposal_id);
   EOS_ASSERT(state->target==args.name,action_validate_exception,"system account creation target mismatch");
   permit->account_created=true;return true;
}

void apply_eosio_sysfinish(apply_context& ctx) {
   if(!activated(ctx))return;
   auto& permit=ctx.trx_context.system_upgrade;const auto args=ctx.get_action().data_as<sysfinish>();
   EOS_ASSERT(permit && permit->account_creation_authorized && permit->account_created && permit->code_used && permit->abi_used &&
              permit->creation_proposal_id==args.proposal_id && ctx.get_sender()=="eosio"_n &&
              ctx.get_current_action_trace().creator_action_ordinal.value==permit->parent_action_ordinal,
              action_validate_exception,"system account finalization requires its exact reviewed native parent");
   ctx.require_authorization("eosio"_n,config::active_name);
   const auto* state=find_system_account_proposal(ctx.db);pending(state,args.proposal_id);
   const auto& metadata=ctx.db.get<account_metadata_object,by_name>(state->target);
   const auto& account=ctx.db.get<account_object,by_name>(state->target);
   EOS_ASSERT(!metadata.is_privileged() && metadata.code_hash==state->code_hash &&
              ctx.trx_context.hash_with_checktime<fc::sha256>(account.abi.data(),account.abi.size())==state->abi_hash,
              action_validate_exception,"created account is privileged or code/ABI changed");
   auto& manager=ctx.control.get_mutable_authorization_manager();
   const auto& owner=manager.get_permission({state->target,config::owner_name});
   const auto& active=manager.get_permission({state->target,config::active_name});
   EOS_ASSERT(fc::raw::pack(owner.auth.to_authority())==fc::raw::pack(creation_owner()) &&
              fc::raw::pack(active.auth.to_authority())==fc::raw::pack(creation_active(state->target)),
              action_validate_exception,"creation authority changed before final sealing");
   const auto old=owner.auth.get_billable_size();
   ctx.db.modify(owner,[&](auto& p){p.auth=authority(1,{},{});p.last_updated=ctx.control.pending_block_time();});
   ctx.add_ram_usage(state->target,int64_t(owner.auth.get_billable_size())-int64_t(old));
   ctx.db.modify(*state,[&](auto& s){s.pending=false;s.outcome=1;s.completed_at=ctx.control.pending_block_time();});
   permit->account_creation_authorized=false;
}
}
