#pragma once

#include <eosio/chain/transaction_state_view.hpp>
#include <map>
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <tuple>
#include <variant>

namespace eosio::chain::experimental {

struct table_key_less {
   bool operator()(const table_key& a, const table_key& b) const {
      return std::tie(a.code, a.scope, a.table) < std::tie(b.code, b.scope, b.table);
   }
};

struct primary_query_less {
   bool operator()(const primary_query& a, const primary_query& b) const {
      return std::tie(a.table.code, a.table.scope, a.table.table, a.kind, a.primary_key) <
             std::tie(b.table.code, b.table.scope, b.table.table, b.kind, b.primary_key);
   }
};

struct primary_snapshot_limits {
   size_t max_tables = 64;
   size_t max_rows = 4096;
   size_t max_value_bytes = 4 * 1024 * 1024;
   size_t max_queries = 4096;
   size_t max_accounts = 256;
};

struct primary_overlay_limits {
   size_t max_read_tables = 64;
   size_t max_write_operations = 4096;
   size_t max_write_value_bytes = 4 * 1024 * 1024;
   size_t max_read_queries = 4096;
   size_t max_read_accounts = 256;
};

struct primary_state_unsupported : std::runtime_error {
   primary_state_unsupported() : std::runtime_error("primary state operation requires serial fallback") {}
};
struct primary_capture_request {
   table_key table;
   std::optional<primary_query> query;
};
struct account_capture_request { name account; };
using state_capture_request = std::variant<primary_capture_request, account_capture_request>;
struct primary_state_missing : primary_state_unsupported {
   explicit primary_state_missing(table_key table) : request(primary_capture_request{table, std::nullopt}) {}
   explicit primary_state_missing(primary_query query) : request(primary_capture_request{query.table, query}) {
      auto& normalized = std::get<primary_capture_request>(request).query;
      if (normalized->kind == primary_query_kind::last) normalized->primary_key = 0;
   }
   explicit primary_state_missing(name account) : request(account_capture_request{account}) {}
   state_capture_request request;
};
struct primary_state_budget_exceeded : std::runtime_error {
   primary_state_budget_exceeded() : std::runtime_error("primary state local budget exceeded") {}
};
struct primary_state_invalid_attempt : std::logic_error {
   primary_state_invalid_attempt() : std::logic_error("primary attempt is failed or already sealed") {}
};

// Bounded, explicitly selected tables copied by the serial owner, then exposed
// as immutable owned values. No Chainbase references survive capture. The
// controller's batch lifetime is managed separately; this owns only copied data.
// Unknown tables mean unsupported, NEVER "table does not exist".
class frozen_primary_state final : public transaction_state_view {
public:
   static std::shared_ptr<const frozen_primary_state> capture(
      const chainbase_transaction_state_view&, std::span<const table_key>, primary_snapshot_limits = {});
   // Owner-only sparse capture. Unknown queries raise owned data requests;
   // they must NEVER be interpreted as absent rows or empty ranges.
   static std::shared_ptr<const frozen_primary_state> capture_queries(
      const chainbase_transaction_state_view&, std::span<const table_key>, std::span<const primary_query>,
      primary_snapshot_limits = {}, const std::function<void()>& checkpoint = {}, std::span<const name> accounts = {});
   frozen_primary_state(const frozen_primary_state&) = delete;
   frozen_primary_state& operator=(const frozen_primary_state&) = delete;

   std::optional<account_read> read_account(name) const override;
   std::optional<table_read> read_table(const table_key&) const override;
   primary_metadata_result locate_primary(const primary_query&) const override;
   primary_prefix_result read_primary_prefix(const primary_query&, size_t max_bytes) const override;
   primary_read_result read_primary(const primary_query&, size_t max_value_bytes) const override;

   size_t table_count() const { return tables_.size(); }
   size_t row_count() const { return row_count_; }
   size_t value_bytes() const { return value_bytes_; }
   size_t account_count() const { return accounts_.size(); }

private:
   frozen_primary_state() = default;
   struct table_snapshot {
      std::optional<table_read> metadata;
      std::map<uint64_t, primary_row_read> rows;
      bool complete = true;
      std::map<primary_query, std::optional<uint64_t>, primary_query_less> queries;
   };
   const table_snapshot& get_table(const table_key&) const;
   std::pair<const table_snapshot*, const primary_row_read*> locate(const primary_query&) const;
   std::map<table_key, table_snapshot, table_key_less> tables_;
   std::map<name, std::optional<account_read>> accounts_;
   size_t row_count_ = 0;
   size_t value_bytes_ = 0;
};

struct primary_update {
   table_key table;
   uint64_t primary_key = 0;
   name payer;
   std::vector<char> value;
};

class primary_state_overlay;

// Not a transaction receipt or an authorization to mutate canonical state.
// Later adoption must validate the batch, dependencies and protocol context,
// settle resources/receipts, and replay writes within a transaction undo.
class primary_overlay_result {
public:
   const auto& base() const { return base_; }
   const auto& read_tables() const { return reads_; }
   const auto& read_queries() const { return queries_; }
   const auto& read_accounts() const { return accounts_; }
   const auto& writes() const { return writes_; }
   bool matches_base_reads(const transaction_state_view& live, const std::function<void()>& checkpoint) const;
private:
   friend class primary_state_overlay;
   primary_overlay_result(std::shared_ptr<const frozen_primary_state> base,
                          std::set<table_key, table_key_less> reads,
                          std::set<primary_query, primary_query_less> queries, std::vector<primary_update> writes,
                          std::set<name> accounts)
      : base_(std::move(base)), reads_(std::move(reads)), queries_(std::move(queries)), writes_(std::move(writes)),
        accounts_(std::move(accounts)) {}
   std::shared_ptr<const frozen_primary_state> base_;
   std::set<table_key, table_key_less> reads_;
   std::set<primary_query, primary_query_less> queries_;
   std::vector<primary_update> writes_;
   std::set<name> accounts_;
};

// One thread owns each attempt. First capability: existing-row updates only,
// unchanged payer and payload size. No create/delete, RAM accounting or IDs are
// simulated. Unsupported operations poison the WHOLE attempt for fallback.
class primary_state_overlay final : public transaction_state_view {
public:
   explicit primary_state_overlay(std::shared_ptr<const frozen_primary_state>, primary_overlay_limits = {});
   primary_state_overlay(const primary_state_overlay&) = delete;
   primary_state_overlay& operator=(const primary_state_overlay&) = delete;
   std::optional<account_read> read_account(name) const override;
   std::optional<table_read> read_table(const table_key&) const override;
   primary_metadata_result locate_primary(const primary_query&) const override;
   primary_prefix_result read_primary_prefix(const primary_query&, size_t max_bytes) const override;
   primary_read_result read_primary(const primary_query&, size_t max_value_bytes) const override;
   void update_existing(const table_key&, uint64_t primary_key, name payer, std::span<const char> value);
   primary_overlay_result seal();

private:
   friend class primary_overlay_cursor;
   void check_owner_and_valid() const;
   void depend_on(const table_key&) const;
   void depend_on_query(primary_query) const;
   const std::vector<char>* written_value(const table_key&, uint64_t primary_key) const;
   std::shared_ptr<const frozen_primary_state> base_;
   primary_overlay_limits limits_;
   const std::thread::id owner_;
   mutable bool valid_ = true;
   mutable std::set<table_key, table_key_less> reads_;
   mutable std::set<primary_query, primary_query_less> queries_;
   mutable std::set<name> accounts_;
   std::vector<primary_update> writes_;
   std::map<table_key, std::map<uint64_t, size_t>, table_key_less> latest_writes_;
   size_t written_bytes_ = 0;
};

// Action-local iterator handles for the restricted existing-row capability.
// There is deliberately no create/delete/sync-call API. Handles may not cross
// actions, attempts or batches; adding lifecycle-changing operations requires
// explicit invalidation, not a logical-key lookup that revives deleted rows.
class primary_overlay_cursor {
public:
   primary_overlay_cursor(primary_state_overlay& state, name receiver,
                          size_t max_iterators = 4096, size_t max_end_iterators = 64)
      : state_(state), receiver_(receiver), max_iterators_(max_iterators), max_ends_(max_end_iterators) {}
   primary_overlay_cursor(const primary_overlay_cursor&) = delete;
   primary_overlay_cursor& operator=(const primary_overlay_cursor&) = delete;

   int find(const table_key&, uint64_t primary_key);
   int lower_bound(const table_key&, uint64_t primary_key);
   int upper_bound(const table_key&, uint64_t primary_key);
   int end(const table_key&);
   int next(int iterator, uint64_t& primary_key);
   int previous(int iterator, uint64_t& primary_key);
   int get(int iterator, std::span<char> output);
   void update(int iterator, name payer, std::span<const char> value);

private:
   template<typename F> decltype(auto) checked(F&& fn) {
      state_.check_owner_and_valid(); // wrong-thread calls never mutate even the failure flag
      try { return std::forward<F>(fn)(); }
      catch (...) { state_.valid_ = false; throw; }
   }
   struct row_handle { table_key table; primary_row_metadata row; };
   const row_handle& get_handle(int iterator) const;
   int cache_table(const table_read&);
   int cache_row(const table_key&, const primary_row_metadata&);
   int seek(const primary_query&);
   primary_state_overlay& state_;
   name receiver_;
   size_t max_iterators_, max_ends_;
   std::vector<row_handle> rows_;
   std::vector<table_read> tables_;
   std::map<int64_t, int> row_handles_;
   std::map<int64_t, int> table_handles_;
};

} // namespace eosio::chain::experimental
