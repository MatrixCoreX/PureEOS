#pragma once

#include <map>

#include <eosio/chain/parallel_action_tree.hpp>
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
   struct identity {
      std::shared_ptr<const frozen_primary_state> base;
      std::vector<request> transactions;
      std::stop_source stop;
   };
public:
   class ticket {
   public:
      std::shared_ptr<const frozen_primary_state> base() const { check(); return identity_->base; }
      std::stop_token stop_token() const { check(); return identity_->stop.get_token(); }
      const fc::sha256& transaction_id() const { check(); return identity_->transactions[index_].transaction_id; }
      size_t index() const { return index_; }
   private:
      friend class ordered_attempt_batch;
      void check() const { if (!identity_) throw primary_state_invalid_attempt(); }
      ticket(std::shared_ptr<identity> source, size_t index) : identity_(std::move(source)), index_(index) {}
      std::shared_ptr<identity> identity_;
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
   size_t next_index() const;
   bool complete() const;
   void discard_remaining(); // a canonical adopter selected full serial fallback
   void close();

   template<typename Result, typename Adopt, typename Serial>
   decltype(auto) run_next(const ticket& tag, const fc::sha256& expected_transaction,
                          const std::optional<Result>& attempt, Adopt&& adopt, Serial&& serial) {
      static_assert(std::is_same_v<Result, isolated_action_result> || std::is_same_v<Result, isolated_transaction_result> ||
                    std::is_same_v<Result, isolated_notification_result> || std::is_same_v<Result, isolated_action_tree_result>);
      check_next(tag, expected_transaction); // stale/foreign/repeated/out-of-order is a caller bug
      try {
         const bool use_private = attempt && [&] {
            if constexpr (std::is_same_v<Result, isolated_notification_result> || std::is_same_v<Result, isolated_action_tree_result>)
               return can_use(attempt->state, attempt->root_fingerprint);
            else return can_use(attempt->state, attempt->execution_fingerprint);
         }();
         if (use_private) {
            // Finish potentially allocating dependency bookkeeping BEFORE the
            // canonical callback. Even A->B->A or a later undo stays dirty.
            for (const auto& write : attempt->state.writes()) {
               dirty_[write.table].insert(write.primary_key);
               if (write.operation != primary_operation::update) structural_dirty_.insert(write.table);
            }
         } else {
            disable_speculation();
         }
         // Reentrant calls from observers may not consume another transaction
         // while this one's original commit/receipt/signals are still running.
         in_callback_ = true;
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
   bool can_use(const primary_overlay_result&, const fc::sha256&) const;
   void disable_speculation();
   const std::thread::id owner_ = std::this_thread::get_id();
   std::shared_ptr<identity> identity_;
   // Existing-row updates invalidate selected keys; lifecycle changes
   // conservatively invalidate ALL readers of that table, even net-zero ABA.
   std::map<table_key, std::set<uint64_t>, table_key_less> dirty_;
   std::set<table_key, table_key_less> structural_dirty_;
   size_t next_ = 0;
   bool closed_ = false;
   bool in_callback_ = false;
};

} // namespace eosio::chain::experimental
