#pragma once
#include <eosio/chain/upgrade_governance.hpp>

namespace eosio::chain {

// Version2 keeps sealed authorities and binds one target::init payload plus
// the human review hash into the existing native review commitment.
constexpr uint32_t max_system_account_init_bytes = 4096;
constexpr uint32_t max_pending_system_account_proposals = 4;

// A separate, bounded proposal lane preserves any existing reviewed contract
// upgrade (including a vetoed pending proposal). All actions require the new
// protocol feature; merely installing a binary grants no creation capability.
struct syspropose {
   account_name target;
   account_name resource_payer;
   int64_t ram_payment = 0; // Exact 4-decimal AIC amount, not an unlimited allowance.
   uint32_t authority_version = 1; // Fixed sealed-owner/code-only-active policy.
   uint32_t code_bytes = 0;
   uint32_t abi_bytes = 0;
   digest_type code_hash, abi_hash, review_document_hash;
   static account_name get_account() { return "eosio"_n; }
   static action_name get_name() { return "syspropose"_n; }
};
struct sysveto {
   account_name voter;
   uint64_t proposal_id = 0;
   static account_name get_account() { return "eosio"_n; }
   static action_name get_name() { return "sysveto"_n; }
};
struct syscancel {
   uint64_t proposal_id = 0;
   static account_name get_account() { return "eosio"_n; }
   static action_name get_name() { return "syscancel"_n; }
};
struct sysexec {
   uint64_t proposal_id = 0;
   bytes code, abi;
   bytes init_data;
   digest_type initialization_review_hash{};
   static account_name get_account() { return "eosio"_n; }
   static action_name get_name() { return "sysexec"_n; }
};
// Native-only continuation. It runs after newaccount, so all deployment
// authorizing actors and permissions already exist when actions are queued.
struct sysstage {
   uint64_t proposal_id = 0;
   bytes code, abi;
   bytes init_data;
   digest_type initialization_review_hash{};
   static account_name get_account() { return "eosio"_n; }
   static action_name get_name() { return "sysstage"_n; }
};
struct sysfinish {
   uint64_t proposal_id = 0;
   static account_name get_account() { return "eosio"_n; }
   static action_name get_name() { return "sysfinish"_n; }
};

class system_account_proposal_object : public chainbase::object<system_account_proposal_object_type, system_account_proposal_object> {
   OBJECT_CTOR(system_account_proposal_object, (proposal_voters)(vetoes))
   id_type id;
   uint64_t proposal_id = 0;
   bool pending = false;
   uint8_t outcome = 0; // 0=pending/unset,1=executed,2=cancelled; no inferred outcome.
   account_name target, resource_payer;
   int64_t ram_payment = 0;
   uint32_t authority_version = 1, code_bytes = 0, abi_bytes = 0, review_seconds = 0;
   digest_type code_hash, abi_hash, review_document_hash;
   digest_type system_code_hash, token_code_hash;
   time_point proposed_at, execute_after;
   time_point completed_at;
   shared_vector<account_name> proposal_voters, vetoes;
};
using system_account_proposal_index = chainbase::shared_multi_index_container<system_account_proposal_object,
   indexed_by<ordered_unique<tag<by_id>, member<system_account_proposal_object,
      system_account_proposal_object::id_type, &system_account_proposal_object::id>>>>;

const system_account_proposal_object* find_system_account_proposal(const chainbase::database&);
const system_account_proposal_object* find_system_account_proposal(const chainbase::database&, uint64_t proposal_id);
upgrade_effective_tally get_system_account_effective_tally(const chainbase::database&, const system_account_proposal_object&);
bool consume_system_account_creation(apply_context&, const newaccount&);
void apply_eosio_syspropose(apply_context&);
void apply_eosio_sysveto(apply_context&);
void apply_eosio_syscancel(apply_context&);
void apply_eosio_sysexec(apply_context&);
void apply_eosio_sysstage(apply_context&);
void apply_eosio_sysfinish(apply_context&);
}

CHAINBASE_SET_INDEX_TYPE(eosio::chain::system_account_proposal_object, eosio::chain::system_account_proposal_index)
FC_REFLECT(eosio::chain::syspropose, (target)(resource_payer)(ram_payment)(authority_version)(code_bytes)(abi_bytes)(code_hash)(abi_hash)(review_document_hash))
FC_REFLECT(eosio::chain::sysveto, (voter)(proposal_id))
FC_REFLECT(eosio::chain::syscancel, (proposal_id))
FC_REFLECT(eosio::chain::sysexec, (proposal_id)(code)(abi)(init_data)(initialization_review_hash))
FC_REFLECT(eosio::chain::sysstage, (proposal_id)(code)(abi)(init_data)(initialization_review_hash))
FC_REFLECT(eosio::chain::sysfinish, (proposal_id))
FC_REFLECT(eosio::chain::system_account_proposal_object,
   (proposal_id)(pending)(outcome)(target)(resource_payer)(ram_payment)(authority_version)(code_bytes)(abi_bytes)(review_seconds)
   (code_hash)(abi_hash)(review_document_hash)(system_code_hash)(token_code_hash)
   (proposed_at)(execute_after)(completed_at)(proposal_voters)(vetoes))
