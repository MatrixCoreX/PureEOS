#pragma once

#include <eosio/chain/contract_types.hpp>
#include <eosio/chain/database_utils.hpp>
#include <eosio/chain/transaction.hpp>
#include "multi_index_includes.hpp"

namespace eosio::chain {

class apply_context;

// At least two thirds, not strictly more than two thirds. This is a veto
// threshold, and MUST NOT be reused as a Savanna finality threshold.
constexpr uint32_t upgrade_veto_threshold(uint32_t voters) {
   return (2 * voters + 2) / 3;
}
constexpr uint32_t max_upgrade_voters = 210;
// Legacy constant remains immutable for replay before the v2 protocol feature.
constexpr uint32_t upgrade_review_seconds = 14 * 24 * 60 * 60;
constexpr uint32_t upgrade_review_seconds_v2 = 7 * 24 * 60 * 60;

// These native actions are enabled only on the opt-in native genesis profile 2.
// govinit is a one-time, owner-authorized bootstrap operation, never an upgrade bypass.
struct govinit {
   account_name developer;
   // Wire format unchanged:14days before governance v2 activation,7days after.
   uint32_t review_seconds = upgrade_review_seconds;
   std::vector<account_name> protected_accounts;
   static account_name get_account() { return config::system_account_name; }
   static action_name get_name() { return "govinit"_n; }
};
struct devpropose {
   account_name target;
   digest_type old_code_hash;
   digest_type old_abi_hash;
   digest_type new_code_hash;
   digest_type new_abi_hash;
   digest_type review_document_hash;
   static account_name get_account() { return config::system_account_name; }
   static action_name get_name() { return "devpropose"_n; }
};
struct devveto {
   account_name voter;
   uint64_t proposal_id = 0;
   static account_name get_account() { return config::system_account_name; }
   static action_name get_name() { return "devveto"_n; }
};
struct devcancel {
   uint64_t proposal_id = 0;
   static account_name get_account() { return config::system_account_name; }
   static action_name get_name() { return "devcancel"_n; }
};
struct devexec {
   uint64_t proposal_id = 0;
   bytes code;
   bytes abi;
   static account_name get_account() { return config::system_account_name; }
   static action_name get_name() { return "devexec"_n; }
};

// Native consensus state, not a WASM-owned table that a privileged forwarding
// contract could rewrite. All fields participate in undo, replay and snapshots.
class upgrade_governance_object : public chainbase::object<upgrade_governance_object_type, upgrade_governance_object> {
   OBJECT_CTOR(upgrade_governance_object, (protected_accounts)(current_voters)(proposal_voters)(vetoes))
   id_type id;
   account_name developer;
   uint32_t review_seconds = 0;
   shared_vector<account_name> protected_accounts;
   shared_vector<account_name> current_voters;
   time_point voters_updated_at;
   uint64_t proposal_id = 0;
   bool pending = false;
   account_name target;
   digest_type old_code_hash;
   digest_type old_abi_hash;
   digest_type new_code_hash;
   digest_type new_abi_hash;
   digest_type review_document_hash;
   time_point proposed_at;
   time_point execute_after;
   shared_vector<account_name> proposal_voters;
   shared_vector<account_name> vetoes;
};
using upgrade_governance_index = chainbase::shared_multi_index_container<
   upgrade_governance_object, indexed_by<ordered_unique<tag<by_id>,
      member<upgrade_governance_object, upgrade_governance_object::id_type, &upgrade_governance_object::id>>>>;

const upgrade_governance_object* find_upgrade_governance(const chainbase::database& db);
// Read only the versioned, candidate-funded permanent-ban record. Missing
// bootstrap tables mean no ban; incompatible existing state fails closed.
bool is_upgrade_voter_permanently_banned(const chainbase::database&, account_name);
struct upgrade_effective_tally {
   std::vector<account_name> voters, vetoes, excluded_banned_voters;
};
upgrade_effective_tally get_upgrade_effective_tally(const chainbase::database&, const upgrade_governance_object&, bool exclude_banned);
bool is_protected_system_account(const chainbase::database& db, account_name account);
void check_system_permission_change(apply_context& context, account_name account);
void check_developer_account_authority(apply_context&,account_name,permission_name,const authority&);
void check_developer_account_permission_deletion(apply_context&,account_name,permission_name);
void check_system_code_change(apply_context& context, account_name account, bool abi, const bytes& payload);
void apply_eosio_govinit(apply_context&);
void apply_eosio_devpropose(apply_context&);
void apply_eosio_devveto(apply_context&);
void apply_eosio_devcancel(apply_context&);
void apply_eosio_devexec(apply_context&);

} // namespace eosio::chain

CHAINBASE_SET_INDEX_TYPE(eosio::chain::upgrade_governance_object, eosio::chain::upgrade_governance_index)
FC_REFLECT(eosio::chain::govinit, (developer)(review_seconds)(protected_accounts))
FC_REFLECT(eosio::chain::devpropose, (target)(old_code_hash)(old_abi_hash)(new_code_hash)(new_abi_hash)(review_document_hash))
FC_REFLECT(eosio::chain::devveto, (voter)(proposal_id))
FC_REFLECT(eosio::chain::devcancel, (proposal_id))
FC_REFLECT(eosio::chain::devexec, (proposal_id)(code)(abi))
FC_REFLECT(eosio::chain::upgrade_governance_object,
   (developer)(review_seconds)(protected_accounts)(current_voters)(voters_updated_at)(proposal_id)(pending)(target)
   (old_code_hash)(old_abi_hash)(new_code_hash)(new_abi_hash)(review_document_hash)
   (proposed_at)(execute_after)(proposal_voters)(vetoes))
