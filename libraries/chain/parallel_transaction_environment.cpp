#include <eosio/chain/parallel_transaction_environment.hpp>
#include <eosio/chain/parallel_state_overlay.hpp>
#include <eosio/chain/transaction.hpp>
#include <fc/io/raw.hpp>
#include <algorithm>

namespace eosio::chain::experimental {
namespace {
fc::sha256 environment_hash(const fc::sha256& id, uint64_t pending, uint64_t published, uint32_t number) {
   fc::sha256::encoder out;
   constexpr char domain[] = "pureeos/isolated-transaction-environment/v1";
   out.write(domain, sizeof(domain));
   fc::raw::pack(out, id); fc::raw::pack(out, pending);
   fc::raw::pack(out, published); fc::raw::pack(out, number);
   return out.result();
}
} // namespace

isolated_transaction_environment::isolated_transaction_environment(std::vector<char> bytes,
      std::array<std::vector<action_range>, 2> actions, const transaction& trx,
      uint64_t pending, uint64_t published, uint32_t number)
   : bytes_(std::move(bytes)), actions_(std::move(actions)),
     id_(fc::sha256::hash(bytes_.data(), bytes_.size())),
     fingerprint_(environment_hash(id_, pending, published, number)),
     pending_time_us_(pending), published_time_us_(published), pending_block_num_(number),
     expiration_(trx.expiration.sec_since_epoch()), ref_block_prefix_(trx.ref_block_prefix), ref_block_num_(trx.ref_block_num) {}

std::shared_ptr<const isolated_transaction_environment> isolated_transaction_environment::capture(
      const transaction& trx, uint64_t pending, uint64_t published, uint32_t number,
      const std::function<void()>& checkpoint, size_t wire_budget) {
   const auto check = [&] { if (checkpoint) checkpoint(); };
   check();
   if (trx.context_free_actions.size() > max_actions ||
       trx.actions.size() > max_actions - trx.context_free_actions.size() ||
       trx.transaction_extensions.size() > max_extensions) throw primary_state_budget_exceeded();
   const std::array lists{&trx.context_free_actions, &trx.actions};
   size_t counted = 0;
   // Bound sizes BEFORE pack_size/pack or constructing owned directories. The
   // controller keeps its stricter transaction eligibility (16 roots, no top
   // CF/deferred/extensions). This helper does not expand canonical admission.
   for (const auto* list : lists) for (const auto& act : *list) {
      check();
      if (act.authorization.size() > max_authorizations || act.data.size() > max_action_bytes)
         throw primary_state_budget_exceeded();
      const size_t size = fc::raw::pack_size(act);
      if (size > max_wire_bytes - counted) throw primary_state_budget_exceeded();
      counted += size;
   }
   for (const auto& extension : trx.transaction_extensions) {
      check();
      if (extension.second.size() > max_wire_bytes - counted) throw primary_state_budget_exceeded();
      counted += extension.second.size();
   }
   check();
   const size_t wire_size = fc::raw::pack_size(trx);
   if (wire_size > std::min(max_wire_bytes, wire_budget)) throw primary_state_budget_exceeded();
   std::array<std::vector<action_range>, 2> ranges;
   // Follow the CURRENT original FC_REFLECT_DERIVED(transaction) order. Do
   // not serialize a different transaction format or use compressed bytes.
   // Oracle tests MUST compare every selected slice with original pack(action)
   // at varuint boundaries and compare the WHOLE wire with original pack(trx).
   size_t offset = fc::raw::pack_size(static_cast<const transaction_header&>(trx));
   const auto advance = [&](size_t size) {
      if (offset > wire_size || size > wire_size - offset) throw primary_state_invalid_attempt();
      offset += size;
   };
   for (size_t type = 0; type < lists.size(); ++type) {
      check();
      const auto& list = *lists[type];
      advance(fc::raw::pack_size(fc::unsigned_int(static_cast<uint32_t>(list.size()))));
      ranges[type].reserve(list.size());
      for (const auto& act : list) {
         check();
         const size_t size = fc::raw::pack_size(act);
         const auto begin = offset; advance(size); ranges[type].push_back({begin, size});
      }
   }
   advance(fc::raw::pack_size(trx.transaction_extensions));
   if (offset != wire_size) throw primary_state_invalid_attempt();
   check();
   auto bytes = fc::raw::pack(trx);
   check();
   if (bytes.size() != wire_size) throw primary_state_invalid_attempt();
   auto result = std::shared_ptr<const isolated_transaction_environment>(
      new isolated_transaction_environment(std::move(bytes), std::move(ranges), trx, pending, published, number));
   check();
   return result;
}

size_t isolated_transaction_environment::action_count(uint32_t type) const {
   if (type > 1) throw primary_state_unsupported();
   return actions_[type].size();
}

int32_t isolated_transaction_environment::read_transaction(std::span<char> output) const {
   if (output.empty()) return transaction_size();
   const auto count = std::min(output.size(), bytes_.size());
   std::copy_n(bytes_.begin(), count, output.begin());
   return static_cast<int32_t>(count); // actual copied bytes for nonempty output
}

int32_t isolated_transaction_environment::get_action(uint32_t type, uint32_t index, std::span<char> output) const {
   if (type > 1) throw primary_state_unsupported();
   if (index >= actions_[type].size()) return -1;
   const auto range = actions_[type][index];
   if (output.size() >= range.size) std::copy_n(bytes_.begin() + range.offset, range.size, output.begin());
   return static_cast<int32_t>(range.size); // insufficient output is left entirely unchanged
}

bool isolated_transaction_environment::matches(const fc::sha256& id, uint64_t pending,
      uint64_t published, uint32_t number) const {
   return id_ == id && pending_time_us_ == pending && published_time_us_ == published && pending_block_num_ == number;
}
} // namespace eosio::chain::experimental
