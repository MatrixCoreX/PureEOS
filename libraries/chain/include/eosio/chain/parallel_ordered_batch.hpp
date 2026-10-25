#pragma once

#include <map>

#include <eosio/chain/parallel_action_tree.hpp>
#include <eosio/chain/parallel_sync_call.hpp>
#include <eosio/chain/parallel_mixed_tree.hpp>
#include <eosio/chain/parallel_runtime_limits.hpp>
#include <fc/crypto/sha256.hpp>
#include <functional>
#include <type_traits>

namespace eosio::chain::experimental {

// This identity is process-local and owns no controller/database references.
// A fresh allocation distinguishes even batches at the same height after undo
// or a fork. Workers retain it; recycling a numeric epoch is unnecessary.
class ordered_attempt_batch {
public:
   struct request {
      fc::sha256 transaction_id;
      fc::sha256 execution_fingerprint;
   };
private:
   // Each prepared transaction owns an immutable descriptor. Discovery may
   // replace an unfinished slot without retagging a completed result or
   // changing any base/input observed by an outstanding worker ticket.
   struct preparation {
      std::shared_ptr<const frozen_primary_state> base;
      request transaction;
      size_t committed_prefix = 0;
   };
   struct identity {
      std::vector<std::shared_ptr<const preparation>> slots;
      std::stop_source stop;
   };
public:
   class ticket {
   public:
      std::shared_ptr<const frozen_primary_state> base() const { check(); return preparation_->base; }
      std::stop_token stop_token() const { check(); return identity_->stop.get_token(); }
      const fc::sha256& transaction_id() const { check(); return preparation_->transaction.transaction_id; }
      size_t index() const { return index_; }
   private:
      friend class ordered_attempt_batch;
      void check() const { if (!identity_ || !preparation_) throw primary_state_invalid_attempt(); }
      ticket(std::shared_ptr<identity> source, size_t index)
         : identity_(std::move(source)), preparation_(identity_->slots[index]), index_(index) {}
      std::shared_ptr<identity> identity_;
      std::shared_ptr<const preparation> preparation_;
      size_t index_;
   };

   // Both callbacks execute ONLY on the serial owner. The adopter must use the
   // original transaction authorization, undo, resource and receipt paths, not
   // raw Chainbase writes. The coordinator supplies ordering/dependency checks,
   // not those consensus operations. Any serial fallback conservatively disables
   // the remaining speculative batch BEFORE the callback can mutate anything.
   ordered_attempt_batch(std::shared_ptr<const frozen_primary_state>, std::span<const request> transactions);
   ~ordered_attempt_batch();
   ordered_attempt_batch(const ordered_attempt_batch&) = delete;
   ordered_attempt_batch& operator=(const ordered_attempt_batch&) = delete;
   ticket get_ticket(size_t index) const;
   struct retry_request { size_t index; request transaction; };
   // Owner-only, before ANY canonical callback, after joining the previous
   // wave. Atomically replace just the unfinished descriptors. Unselected
   // tickets/results remain bound to their original snapshots; replaced
   // tickets cannot be adopted. Aggregate retained payload stays <= 4 MiB.
   void prepare_retries(std::shared_ptr<const frozen_primary_state>, std::span<const retry_request>);
   enum class candidate_status { reusable, dependency_conflict, unavailable };
   // Owner-only, non-consuming dependency inspection. No writes or conflict
   // metrics are recorded. This is NOT a canonical adoption/authorization proof.
   template<typename Result>
   candidate_status inspect_next(const ticket& tag, const fc::sha256& expected_transaction,
                                 const std::optional<Result>& attempt,
                                 const std::function<void()>& checkpoint = {}) {
      check_next(tag, expected_transaction);
      try {
         in_callback_ = true;
         const auto status = classify_attempt(attempt, checkpoint);
         in_callback_ = false;
         return status;
      } catch (...) {
         in_callback_ = false;
         closed_ = true;
         disable_speculation();
         throw;
      }
   }
   struct refreshed_input {
      std::shared_ptr<const frozen_primary_state> base;
      request transaction;
   };
   // After all workers have joined, and after a nonempty committed prefix,
   // capture ONLY the current slot from the current owner state. The callback
   // must capture fresh code/context/reads, not relabel an old result. Guarded
   // against reentry; capture exceptions close the batch. Validation/allocation
   // failures publish nothing. Existing tail descriptors/dirty sets stay intact.
   // The caller must share the original absolute time/discovery budget; this
   // API grants no new execution budget and cannot reopen a serial barrier.
   ticket refresh_next(const ticket&, const fc::sha256& expected_transaction,
                       const std::function<refreshed_input()>& capture);
   size_t next_index() const;
   bool complete() const;
   void discard_remaining(); // a canonical adopter selected full serial fallback
   void close();

   template<typename Result, typename Adopt, typename Serial>
   decltype(auto) run_next(const ticket& tag, const fc::sha256& expected_transaction,
                          const std::optional<Result>& attempt, Adopt&& adopt, Serial&& serial,
                          const std::function<void()>& checkpoint = {}) {
      check_next(tag, expected_transaction); // stale/foreign/repeated/out-of-order is a caller bug
      try {
         // Checkpoints are caller code too. Cover preflight as well as commit,
         // so neither can recursively consume/close this in-progress slot.
         in_callback_ = true;
         const auto status = classify_attempt(attempt, checkpoint);
         if (status == candidate_status::dependency_conflict) record_conflict();
         bool use_private = status == candidate_status::reusable;
         if (use_private) {
            // Finish potentially allocating dependency bookkeeping BEFORE the
            // canonical callback. Even A->B->A or a later undo stays dirty.
            use_private = remember_writes(attempt->state, checkpoint);
         }
         if (!use_private) disable_speculation();
         // Reentrant calls from observers may not consume another transaction
         // while this one's original commit/receipt/signals are still running.
         auto finish = [this] { in_callback_ = false; ++next_; };
         using result_type = std::invoke_result_t<Adopt, const Result&>;
         static_assert(std::is_same_v<result_type, std::invoke_result_t<Serial>>);
         if constexpr (std::is_void_v<result_type>) {
            if (use_private) std::forward<Adopt>(adopt)(*attempt);
            else std::forward<Serial>(serial)();
            finish();
         } else {
            static_assert(!std::is_reference_v<result_type>, "commit results must own their lifetime");
            auto result = use_private ? std::forward<Adopt>(adopt)(*attempt) : std::forward<Serial>(serial)();
            finish();
            return result;
         }
      } catch (...) {
         in_callback_ = false;
         closed_ = true;
         disable_speculation();
         throw; // never replay a partially entered canonical callback
      }
   }

private:
   void check_owner() const;
   void check_next(const ticket&, const fc::sha256&) const;
   template<typename Result>
   candidate_status classify_attempt(const std::optional<Result>& attempt,
                                     const std::function<void()>& checkpoint) const {
      static_assert(std::is_same_v<Result, isolated_action_result> || std::is_same_v<Result, isolated_transaction_result> ||
                    std::is_same_v<Result, isolated_notification_result> || std::is_same_v<Result, isolated_action_tree_result> ||
                    std::is_same_v<Result, isolated_sync_result> || std::is_same_v<Result, isolated_sync_transaction_result> ||
                    std::is_same_v<Result, isolated_mixed_tree_result>);
      if (!attempt) return candidate_status::unavailable;
      constexpr bool has_cursor_transcript = !std::is_same_v<Result, isolated_action_result> &&
                                              !std::is_same_v<Result, isolated_transaction_result>;
      if constexpr (std::is_same_v<Result, isolated_notification_result> || std::is_same_v<Result, isolated_action_tree_result>)
         return classify(attempt->state, attempt->root_fingerprint, has_cursor_transcript, checkpoint);
      else return classify(attempt->state, attempt->execution_fingerprint, has_cursor_transcript, checkpoint);
   }
   candidate_status classify(const primary_overlay_result&, const fc::sha256&, bool secondary_allowed,
                             const std::function<void()>& checkpoint) const;
   static void check_snapshot_budget(const std::vector<std::shared_ptr<const preparation>>&);
   static void record_conflict();
   bool remember_writes(const primary_overlay_result&, const std::function<void()>& checkpoint);
   void disable_speculation();
   const std::thread::id owner_ = std::this_thread::get_id();
   std::shared_ptr<identity> identity_;
   // Existing-row updates invalidate selected keys; lifecycle changes
   // conservatively invalidate ALL readers of that table, even net-zero ABA.
   std::map<table_key, std::set<uint64_t>, table_key_less> dirty_;
   std::set<table_key, table_key_less> structural_dirty_;
   struct secondary_position {
      secondary_key key;
      uint64_t primary;
   };
   struct position_less {
      bool operator()(const secondary_position& a, const secondary_position& b) const {
         if (secondary_key_less(a.key, b.key)) return true;
         if (secondary_key_less(b.key, a.key)) return false;
         return a.primary < b.primary;
      }
   };
   struct secondary_changes {
      std::set<uint64_t> primary_keys;
      // Retain all intermediate positions, conservatively including ABA.
      // Ordered lookups avoid queries times writes scans on the owner thread.
      std::set<secondary_position, position_less> positions;
   };
   std::map<table_key, std::array<secondary_changes, 5>, table_key_less> secondary_dirty_;
   static constexpr size_t max_operations_per_result = 4096;
   static constexpr size_t max_batch_write_operations = parallel_runtime_limits::max_batch_transactions * max_operations_per_result;
   size_t write_operations_ = 0;
   size_t next_ = 0;
   size_t refreshes_ = 0;
   bool closed_ = false;
   bool in_callback_ = false;
};

} // namespace eosio::chain::experimental
