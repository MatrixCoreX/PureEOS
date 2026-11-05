#include <eosio/chain/upgrade_governance.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/protocol_feature_manager.hpp>
#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/authorization_manager.hpp>
#include <eosio/chain/permission_link_object.hpp>
#include <eosio/chain/permission_object.hpp>
#include <eosio/chain/transaction_context.hpp>

#include <algorithm>
#include <limits>
#include <set>

#if defined(PUREEOS_TEST_GOVERNANCE_30MIN) && !defined(PUREEOS_TEST_BUILD)
#error "Thirty-minute governance is restricted to the separate TEST build"
#endif

namespace eosio::chain {
namespace {
constexpr auto upgrade_permission = "upgrade"_n;
constexpr auto veto_permission = "veto"_n;
#ifdef PUREEOS_TEST_GOVERNANCE_30MIN
constexpr uint32_t active_review_seconds_v2 = 30 * 60;
#else
constexpr uint32_t active_review_seconds_v2 = upgrade_review_seconds_v2;
#endif
bool governance_v2(const apply_context& ctx) {
   return ctx.control.is_builtin_activated(builtin_protocol_feature_t::pureeos_seven_day_governance);
}

bool permanently_disabled(account_name account) {
   return account == "eosio.prods"_n || account == "eosio.wrap"_n ||
          account == "eosio.msig"_n || account == "eosio.null"_n;
}
const upgrade_governance_object& enabled(apply_context& ctx) {
   EOS_ASSERT(!ctx.is_read_only(), action_validate_exception, "upgrade governance requires a writable transaction");
   EOS_ASSERT(ctx.get_current_action_trace().creator_action_ordinal.value == 0, action_validate_exception,
              "governance actions require top-level transaction authorization, not privileged forwarding");
   auto* state = find_upgrade_governance(ctx.db);
   EOS_ASSERT(state && state->developer != account_name{}, action_validate_exception,
              "developer upgrade governance is not initialized");
   return *state;
}
void require_developer(apply_context& ctx, const upgrade_governance_object& state) {
   ctx.require_authorization(state.developer, upgrade_permission);
}
digest_type abi_hash(apply_context& ctx, account_name target) {
   const auto& abi = ctx.db.get<account_object, by_name>(target).abi;
   return ctx.trx_context.hash_with_checktime<fc::sha256>(abi.data(), abi.size());
}
void check_pending(const upgrade_governance_object& state, uint64_t id) {
   EOS_ASSERT(state.pending && state.proposal_id == id, action_validate_exception,
              "upgrade proposal is not pending or id does not match");
}
void link_permission(apply_context& ctx, account_name account, action_name action, permission_name permission) {
   auto* existing = ctx.db.find<permission_link_object, by_action_name>(boost::make_tuple(account, "eosio"_n, action));
   if (existing) {
      ctx.db.modify(*existing, [&](auto& p) { p.required_permission = permission; });
   } else {
      ctx.db.create<permission_link_object>([&](auto& p) {
         p.account = account;
         p.code = "eosio"_n;
         p.message_type = action;
         p.required_permission = permission;
      });
      ctx.add_ram_usage(account, config::billable_size_v<permission_link_object>);
   }
}
void validate_developer_delegates(const authority& auth) {
   EOS_ASSERT(auth.threshold>0 && auth.keys.empty() && auth.waits.empty() &&
              auth.accounts.size()>=auth.threshold,action_validate_exception,
              "eosio.dev roles require nonempty account-based M-of-N authorities without keys or waits");
   std::set<account_name> members;
   for(const auto& member:auth.accounts) {
      EOS_ASSERT(member.weight==1 && member.permission.actor!="eosio"_n &&
                 !member.permission.actor.to_string().starts_with("eosio.") &&
                 member.permission.permission!=config::eosio_code_name &&
                 members.insert(member.permission.actor).second,action_validate_exception,
                 "eosio.dev members must be distinct ordinary accounts with unit weight and signing permissions");
   }
}
bool named_developer(const apply_context& ctx,account_name account) {
   const auto* state=find_upgrade_governance(ctx.db);
   return account=="eosio.dev"_n && state && state->developer==account &&
          ctx.control.is_builtin_activated(builtin_protocol_feature_t::pureeos_system_account_governance);
}
} // namespace

const upgrade_governance_object* find_upgrade_governance(const chainbase::database& db) {
   const auto& index = db.get_index<upgrade_governance_index, by_id>();
   return index.empty() ? nullptr : &*index.begin();
}

bool is_upgrade_voter_permanently_banned(const chainbase::database& db, account_name account) {
   const auto* table = db.find<table_id_object, by_code_scope_table>(boost::make_tuple("eosio"_n,"eosio"_n,"rotcands"_n));
   if (!table) return false;
   const auto* row = db.find<key_value_object, by_scope_primary>(boost::make_tuple(table->id,account.to_uint64_t()));
   if (!row) return false;
   const auto* rotation_table = db.find<table_id_object, by_code_scope_table>(boost::make_tuple("eosio"_n,"eosio"_n,"rotation"_n));
   EOS_ASSERT(rotation_table, action_validate_exception, "permanent-ban table requires rotation v2 state");
   const auto* rotation = db.find<key_value_object, by_scope_primary>(boost::make_tuple(rotation_table->id,"rotation"_n.to_uint64_t()));
   EOS_ASSERT(rotation && rotation->value.size()>=4, action_validate_exception, "missing rotation v2 state");
   const auto* version = reinterpret_cast<const unsigned char*>(rotation->value.data());
   EOS_ASSERT(version[0]==2 && version[1]==0 && version[2]==0 && version[3]==0 && row->value.size()==114,
              action_validate_exception, "unsupported permanent-ban record schema");
   const auto* data = reinterpret_cast<const unsigned char*>(row->value.data());
   uint64_t owner=0;for(unsigned i=0;i<8;++i) owner|=uint64_t(data[i])<<(8*i);
   EOS_ASSERT(owner==account.to_uint64_t() && data[73]<=1, action_validate_exception, "malformed permanent-ban record");
   return data[73]!=0;
}
upgrade_effective_tally get_upgrade_effective_tally(const chainbase::database& db,
      const upgrade_governance_object& state, bool exclude_banned) {
   upgrade_effective_tally result;
   for (auto voter : state.proposal_voters) {
      if (exclude_banned && is_upgrade_voter_permanently_banned(db,voter)) result.excluded_banned_voters.push_back(voter);
      else result.voters.push_back(voter);
   }
   for (auto voter : state.vetoes)
      if (std::binary_search(result.voters.begin(),result.voters.end(),voter)) result.vetoes.push_back(voter);
   return result;
}

bool is_protected_system_account(const chainbase::database& db, account_name account) {
   auto* state = find_upgrade_governance(db);
   if (!state || state->developer == account_name{}) return false;
   return permanently_disabled(account) ||
          std::binary_search(state->protected_accounts.begin(), state->protected_accounts.end(), account);
}

void check_system_permission_change(apply_context& ctx, account_name account) {
   EOS_ASSERT(!is_protected_system_account(ctx.db, account), action_validate_exception,
              "protected system permissions cannot be changed after govinit");
}

void check_developer_account_authority(apply_context& ctx,account_name account,permission_name permission,const authority& proposed) {
   if(!named_developer(ctx,account) || (permission!=config::owner_name && permission!=config::active_name && permission!=upgrade_permission))return;
   validate_developer_delegates(proposed);
   const auto& manager=ctx.control.get_authorization_manager();
   auto threshold=[&](permission_name name){return name==permission?proposed.threshold:manager.get_permission({account,name}).auth.threshold;};
   EOS_ASSERT(threshold(config::owner_name)>=threshold(upgrade_permission) &&
              threshold(config::active_name)>=threshold(upgrade_permission),action_validate_exception,
              "eosio.dev owner and active thresholds cannot be weaker than upgrade");
}
void check_developer_account_permission_deletion(apply_context& ctx,account_name account,permission_name permission) {
   EOS_ASSERT(!named_developer(ctx,account) || permission!=upgrade_permission,action_validate_exception,
              "eosio.dev must retain its account-based upgrade role");
}

void check_system_code_change(apply_context& ctx, account_name account, bool abi, const bytes& payload) {
   if (!is_protected_system_account(ctx.db, account)) return;
   EOS_ASSERT(!permanently_disabled(account), action_validate_exception,
              "legacy multisig, wrap and producer-governance accounts are disabled");
   auto& permit = ctx.trx_context.system_upgrade;
   EOS_ASSERT(permit && permit->target == account && ctx.get_sender() == "eosio"_n &&
              ctx.get_current_action_trace().creator_action_ordinal.value == permit->parent_action_ordinal,
              action_validate_exception, "system upgrade requires a reviewed native devexec capability");
   auto& used = abi ? permit->abi_used : permit->code_used;
   EOS_ASSERT(!used && ctx.trx_context.hash_with_checktime<fc::sha256>(payload.data(), payload.size()) == (abi ? permit->abi_hash : permit->code_hash),
              action_validate_exception, "system upgrade payload does not match the single-use capability");
   used = true;
}

void apply_eosio_govinit(apply_context& ctx) {
   if (!find_upgrade_governance(ctx.db)) return; // Preserve legacy unknown-action behavior.
   EOS_ASSERT(ctx.get_current_action_trace().creator_action_ordinal.value == 0, action_validate_exception,
              "govinit must be a top-level action");
   EOS_ASSERT(!ctx.is_read_only(), action_validate_exception, "govinit requires a writable transaction");
   auto* state = find_upgrade_governance(ctx.db);
   EOS_ASSERT(state && state->developer == account_name{}, action_validate_exception,
              "govinit requires a fresh native genesis profile 2 and can run only once");
   ctx.require_authorization("eosio"_n, config::owner_name);
   const auto args = ctx.get_action().data_as<govinit>();
   const auto review_seconds = governance_v2(ctx) ? active_review_seconds_v2 : upgrade_review_seconds;
   EOS_ASSERT(args.review_seconds == review_seconds,
              action_validate_exception, "review_seconds must match the activated governance protocol");
   EOS_ASSERT(args.developer != account_name{} && args.developer != "eosio"_n && !permanently_disabled(args.developer),
              action_validate_exception, "developer must be a separate ordinary account");
   const bool named_role=args.developer=="eosio.dev"_n;
   EOS_ASSERT(!args.developer.to_string().starts_with("eosio.") ||
              (named_role && ctx.control.is_builtin_activated(builtin_protocol_feature_t::pureeos_system_account_governance)),
              action_validate_exception,"only activated eosio.dev may be the namespaced developer identity");
   const auto& schedule = ctx.control.head_active_producers().producers;
   EOS_ASSERT(schedule.size() == 1 && schedule.front().producer_name == "eosio"_n,
              action_validate_exception, "govinit must complete during single-node bootstrap");

   auto& auth = ctx.control.get_mutable_authorization_manager();
   auth.get_permission({args.developer, upgrade_permission}); // Must be deliberately provisioned before sealing.
   if(named_role) {
      const auto& owner=auth.get_permission({args.developer,config::owner_name});
      const auto& active=auth.get_permission({args.developer,config::active_name});
      const auto& upgrade=auth.get_permission({args.developer,upgrade_permission});
      for(const auto* role:{&owner,&active,&upgrade})validate_developer_delegates(role->auth.to_authority());
      EOS_ASSERT(owner.auth.threshold>=upgrade.auth.threshold && active.auth.threshold>=upgrade.auth.threshold && upgrade.parent==active.id,
                 action_validate_exception,"eosio.dev requires non-weaker management thresholds and active-parented upgrade");
      const auto& metadata=ctx.db.get<account_metadata_object,by_name>(args.developer);
      EOS_ASSERT(!metadata.is_privileged() && metadata.code_hash==digest_type{},action_validate_exception,
                 "eosio.dev must be a nonprivileged signing identity without contract code at genesis sealing");
   }
   const auto genesis_auth = auth.get_permission({"eosio"_n, config::active_name}).auth.to_authority();
   EOS_ASSERT(genesis_auth.threshold == 1 && genesis_auth.keys.size() == 1 && genesis_auth.keys.front().weight == 1 &&
              genesis_auth.accounts.empty() && genesis_auth.waits.empty(), action_validate_exception,
              "bootstrap eosio authority must still be its single genesis key");

   std::set<account_name> targets(args.protected_accounts.begin(), args.protected_accounts.end());
   targets.insert("eosio"_n);
   targets.insert("eosio.token"_n);
   // An omitted system account must never leave a bootstrap-key or BP-controlled back door.
   for (const auto& account : ctx.db.get_index<account_index, by_id>()) {
      ctx.trx_context.checktime();
      if (account.name.to_string().starts_with("eosio.") && !(named_role && account.name==args.developer)) targets.insert(account.name);
   }
   EOS_ASSERT(targets.size() <= 64 && !targets.contains(args.developer), action_validate_exception,
              "invalid protected target list or developer is itself a protected system account");
   for (auto target : targets) ctx.db.get<account_object, by_name>(target);

   // No privileged intermediary may survive sealing. Only the reviewed eosio code retains native system intrinsics.
   for (const auto& account : ctx.db.get_index<account_metadata_index, by_id>()) {
      ctx.trx_context.checktime();
      if (account.name != "eosio"_n && account.is_privileged())
         ctx.db.modify(account, [](auto& a) { a.set_privileged(false); });
   }
   for (auto target : targets) {
      const auto& permissions = ctx.db.get_index<permission_index, by_owner>();
      auto it = permissions.lower_bound(boost::make_tuple(target));
      for (; it != permissions.end() && it->owner == target; ++it) {
         ctx.trx_context.checktime();
         authority replacement(1, {}, {}); // owner and all custom authorities become unsatisfiable.
         if (it->name == config::active_name && !permanently_disabled(target)) {
            replacement.accounts.push_back({{"eosio"_n, config::eosio_code_name}, 1});
            if (target != "eosio"_n) replacement.accounts.push_back({{target, config::eosio_code_name}, 1});
            std::sort(replacement.accounts.begin(), replacement.accounts.end());
         }
         const auto old_size = it->auth.get_billable_size();
         ctx.db.modify(*it, [&](auto& p) { p.auth = replacement; p.last_updated = ctx.control.pending_block_time(); });
         ctx.add_ram_usage(target, int64_t(it->auth.get_billable_size()) - int64_t(old_size));
      }
   }
   // eosio is also the genesis producer: retain only its narrowly linked veto permission, never its management key.
   EOS_ASSERT(!auth.find_permission({"eosio"_n, veto_permission}), action_validate_exception, "bootstrap veto permission already exists");
   const auto& veto = auth.create_permission("eosio"_n, veto_permission,
      auth.get_permission({"eosio"_n, config::active_name}).id, genesis_auth, false, ctx.control.pending_block_time());
   ctx.add_ram_usage("eosio"_n, config::billable_size_v<permission_object> + veto.auth.get_billable_size());
   link_permission(ctx, "eosio"_n, "devveto"_n, veto_permission);
   for (auto action : {"refreshgov"_n, "devpropose"_n, "devcancel"_n, "devexec"_n})
      link_permission(ctx, args.developer, action, upgrade_permission);
   if(named_role)for(auto action:{"syspropose"_n,"syscancel"_n,"sysexec"_n})
      link_permission(ctx,args.developer,action,upgrade_permission);

   ctx.db.modify(*state, [&](auto& s) {
      s.developer = args.developer;
      s.review_seconds = review_seconds;
      s.protected_accounts = std::vector<account_name>(targets.begin(), targets.end());
      s.current_voters = {"eosio"_n};
      s.voters_updated_at = ctx.control.pending_block_time();
   });
   // Reserve the bounded native singleton's maximum logical storage once.
   // Subsequent proposals/votes cannot grow unbounded or bill unsuspecting nodes.
   // Protocol billing uses fixed byte counts, never a compiler-dependent sizeof(object).
   ctx.add_ram_usage(args.developer, 1024 + 8 * (64 + 3 * max_upgrade_voters));
}

void apply_eosio_devpropose(apply_context& ctx) {
   if (!find_upgrade_governance(ctx.db)) return;
   const auto& state = enabled(ctx);
   require_developer(ctx, state);
   const auto args = ctx.get_action().data_as<devpropose>();
   EOS_ASSERT(!state.pending, action_validate_exception, "cancel or execute the previous upgrade first");
   EOS_ASSERT(is_protected_system_account(ctx.db, args.target) && !permanently_disabled(args.target),
              action_validate_exception, "target is not an upgradeable protected system account");
   EOS_ASSERT(!state.current_voters.empty() && state.current_voters.size() <= max_upgrade_voters,
              action_validate_exception, "no valid veto electorate");
   const auto& trace = ctx.get_current_action_trace();
   EOS_ASSERT(ctx.trx_context.upgrade_voters_action_ordinal && trace.creator_action_ordinal.value == 0 &&
              trace.action_ordinal.value == *ctx.trx_context.upgrade_voters_action_ordinal + 1,
              action_validate_exception, "devpropose must immediately follow refreshgov in the same transaction");
   ctx.trx_context.upgrade_voters_action_ordinal.reset();
   EOS_ASSERT((ctx.db.get<account_metadata_object, by_name>(args.target).code_hash == args.old_code_hash &&
              abi_hash(ctx, args.target) == args.old_abi_hash), action_validate_exception, "upgrade base hashes do not match");
   EOS_ASSERT(args.review_document_hash != digest_type{}, action_validate_exception, "review document hash is required");
   EOS_ASSERT(args.new_code_hash != digest_type{}, action_validate_exception, "protected system code cannot be removed");
   EOS_ASSERT(args.old_code_hash != args.new_code_hash || args.old_abi_hash != args.new_abi_hash,
              action_validate_exception, "upgrade changes neither code nor ABI");
   EOS_ASSERT(state.proposal_id < std::numeric_limits<uint64_t>::max(), action_validate_exception, "proposal id exhausted");
   ctx.db.modify(state, [&](auto& s) {
      ++s.proposal_id;
      s.pending = true;
      s.target = args.target;
      s.old_code_hash = args.old_code_hash;
      s.old_abi_hash = args.old_abi_hash;
      s.new_code_hash = args.new_code_hash;
      s.new_abi_hash = args.new_abi_hash;
      s.review_document_hash = args.review_document_hash;
      s.proposed_at = ctx.control.pending_block_time();
      s.review_seconds = governance_v2(ctx) ? active_review_seconds_v2 : upgrade_review_seconds;
      s.execute_after = s.proposed_at + fc::seconds(s.review_seconds);
      s.proposal_voters = s.current_voters;
      s.vetoes.clear();
   });
}

void apply_eosio_devveto(apply_context& ctx) {
   if (!find_upgrade_governance(ctx.db)) return;
   const auto& state = enabled(ctx);
   const auto args = ctx.get_action().data_as<devveto>();
   check_pending(state, args.proposal_id);
   if (args.voter == "eosio"_n) ctx.require_authorization(args.voter, veto_permission);
   else ctx.require_authorization(args.voter);
   EOS_ASSERT(ctx.control.pending_block_time() < state.execute_after, action_validate_exception, "veto period has ended");
   EOS_ASSERT(std::binary_search(state.proposal_voters.begin(), state.proposal_voters.end(), args.voter),
              action_validate_exception, "voter is not in this proposal's frozen electorate");
   EOS_ASSERT(!governance_v2(ctx) || !is_upgrade_voter_permanently_banned(ctx.db,args.voter),
              action_validate_exception, "permanently blacklisted BP cannot veto an upgrade");
   EOS_ASSERT(std::find(state.vetoes.begin(), state.vetoes.end(), args.voter) == state.vetoes.end(),
              action_validate_exception, "voter has already vetoed this proposal");
   ctx.db.modify(state, [&](auto& s) { s.vetoes.emplace_back(args.voter); });
}

void apply_eosio_devcancel(apply_context& ctx) {
   if (!find_upgrade_governance(ctx.db)) return;
   const auto& state = enabled(ctx);
   require_developer(ctx, state);
   check_pending(state, ctx.get_action().data_as<devcancel>().proposal_id);
   ctx.db.modify(state, [](auto& s) { s.pending = false; });
}

void apply_eosio_devexec(apply_context& ctx) {
   if (!find_upgrade_governance(ctx.db)) return;
   const auto& state = enabled(ctx);
   require_developer(ctx, state);
   const auto args = ctx.get_action().data_as<devexec>();
   check_pending(state, args.proposal_id);
   EOS_ASSERT(ctx.control.pending_block_time() >= state.execute_after, action_validate_exception, "upgrade review period has not ended");
   const auto tally = get_upgrade_effective_tally(ctx.db,state,governance_v2(ctx));
   EOS_ASSERT(!tally.voters.empty(), action_validate_exception, "no effective veto electorate; upgrade execution blocked");
   EOS_ASSERT(tally.vetoes.size() < upgrade_veto_threshold(tally.voters.size()),
              action_validate_exception, "upgrade vetoed by at least two thirds of the effective frozen electorate");
   EOS_ASSERT((ctx.db.get<account_metadata_object, by_name>(state.target).code_hash == state.old_code_hash &&
              abi_hash(ctx, state.target) == state.old_abi_hash), action_validate_exception, "upgrade base hashes no longer match");
   EOS_ASSERT(!args.code.empty() && ctx.trx_context.hash_with_checktime<fc::sha256>(args.code.data(), args.code.size()) == state.new_code_hash &&
              ctx.trx_context.hash_with_checktime<fc::sha256>(args.abi.data(), args.abi.size()) == state.new_abi_hash,
              action_validate_exception, "execution payload differs from the published upgrade hashes");
   EOS_ASSERT(!ctx.trx_context.system_upgrade, action_validate_exception, "only one upgrade execution per transaction");
   ctx.trx_context.system_upgrade = transaction_context::system_upgrade_execution{
      state.target, state.new_code_hash, state.new_abi_hash, ctx.get_current_action_trace().action_ordinal.value};
   const auto target = state.target;
   const bool change_code = state.old_code_hash != state.new_code_hash;
   const bool change_abi = state.old_abi_hash != state.new_abi_hash;
   // Any failure in either native child action rolls back the entire upgrade and its pending state.
   ctx.db.modify(state, [](auto& s) { s.pending = false; });
   if (change_code) ctx.execute_inline(action({permission_level{target, config::active_name}}, setcode{target, 0, 0, args.code}));
   if (change_abi) ctx.execute_inline(action({permission_level{target, config::active_name}}, setabi{target, args.abi}));
}
} // namespace eosio::chain
