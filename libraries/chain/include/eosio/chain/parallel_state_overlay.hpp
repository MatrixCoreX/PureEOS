#pragma once

#include <eosio/chain/transaction_state_view.hpp>
#include <eosio/chain/parallel_secondary_state.hpp>
#include <map>
#include <boost/container/flat_map.hpp>
#include <array>
#include <type_traits>
#include <initializer_list>
#include <optional>
#include <utility>
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <tuple>
#include <variant>

namespace eosio::chain::experimental {

// Invocation-local membership/catalog helpers. Only the first eight keys
// are scanned; larger inputs keep the original ordered-tree overflow behavior.
// No iterator/order API, canonical cache or cross-callback state is introduced.
template<typename Key,size_t Inline=8>
class small_seen_keys {
   static_assert(Inline>0 && Inline<=64);
   static_assert(std::is_same_v<Key,name> || std::is_same_v<Key,int64_t>);
   std::array<Key,Inline> inline_{};
   size_t used_=0;
   std::set<Key> overflow_;
public:
   small_seen_keys()=default;
   small_seen_keys(std::initializer_list<Key> keys) {for(const auto& key:keys)insert(key);}
   bool insert(const Key& key) {
      for(size_t i=0;i<used_;++i)if(inline_[i]==key)return false;
      if(used_<Inline) {inline_[used_++]=key;return true;}
      return overflow_.insert(key).second;
   }
   size_t size()const {return used_+overflow_.size();}
};

template<typename Value,size_t Inline=8>
class small_name_map {
   static_assert(Inline>0 && Inline<=64);
   std::array<std::optional<std::pair<name,Value>>,Inline> inline_{};
   size_t used_=0;
   std::map<name,Value> overflow_;
public:
   const Value* find(name key)const {
      for(size_t i=0;i<used_;++i)if(inline_[i]->first==key)return &inline_[i]->second;
      const auto it=overflow_.find(key);
      return it==overflow_.end()?nullptr:&it->second;
   }
   bool emplace(name key,Value value) {
      if(find(key))return false;
      if(used_<Inline) {
         inline_[used_].emplace(key,std::move(value));++used_;return true;
      }
      return overflow_.emplace(key,std::move(value)).second;
   }
   size_t size()const {return used_+overflow_.size();}
};

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

// Owner-side capture diagnostics only; never billable CPU or a dependency.
struct primary_snapshot_capture_stats {
   size_t tables_read = 0, primary_queries_read = 0, primary_rows_copied = 0;
   size_t secondary_queries_read = 0, accounts_read = 0;
};

class frozen_primary_capture_session;

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
struct secondary_capture_request { secondary_query query; };
using state_capture_request = std::variant<primary_capture_request, account_capture_request, secondary_capture_request>;
struct primary_state_missing : primary_state_unsupported {
   explicit primary_state_missing(table_key table) : request(primary_capture_request{table, std::nullopt}) {}
   explicit primary_state_missing(primary_query query) : request(primary_capture_request{query.table, query}) {
      auto& normalized = std::get<primary_capture_request>(request).query;
      if (normalized->kind == primary_query_kind::last) normalized->primary_key = 0;
   }
   explicit primary_state_missing(name account) : request(account_capture_request{account}) {}
   explicit primary_state_missing(secondary_query query)
      : request(secondary_capture_request{normalize_secondary_query(std::move(query))}) {}
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
   // Primary and all secondary families share this snapshot's table metadata,
   // row/query/value budgets and owner. No secondary host or mutation is enabled
   // by capturing values; unrequested sparse queries still require discovery.
   static std::shared_ptr<const frozen_primary_state> capture_queries_with_secondary(
      const chainbase_transaction_state_view&, std::span<const table_key>, std::span<const primary_query>,
      std::span<const secondary_query>, primary_snapshot_limits = {},
      const std::function<void()>& checkpoint = {}, std::span<const name> accounts = {});
   frozen_primary_state(const frozen_primary_state&) = delete;
   frozen_primary_state& operator=(const frozen_primary_state&) = delete;

   std::optional<account_read> read_account(name) const override;
   std::optional<table_read> read_table(const table_key&) const override;
   primary_metadata_result locate_primary(const primary_query&) const override;
   primary_prefix_result read_primary_prefix(const primary_query&, size_t max_bytes) const override;
   primary_read_result read_primary(const primary_query&, size_t max_value_bytes) const override;

   secondary_read_result read_secondary(const secondary_query&) const override;

   size_t table_count() const { return tables_.size(); }
   size_t row_count() const { return row_count_; }
   size_t value_bytes() const { return value_bytes_; }
   size_t account_count() const { return accounts_.size(); }

private:
   friend class frozen_primary_capture_session;
   friend class primary_overlay_result; // immediate immutable row-byte comparison only
   frozen_primary_state() = default;
   static std::shared_ptr<const frozen_primary_state> capture_queries_impl(
      const chainbase_transaction_state_view&, const chainbase_secondary_state_view*, std::span<const table_key>,
      std::span<const primary_query>, std::span<const secondary_query>, primary_snapshot_limits,
      const std::function<void()>&, std::span<const name>,
      const frozen_primary_state* same_prefix = nullptr, primary_snapshot_capture_stats* = nullptr);
   struct table_snapshot {
      std::optional<table_read> metadata;
      std::map<uint64_t, std::shared_ptr<const primary_row_read>> rows;
      bool complete = true;
      std::map<primary_query, std::optional<uint64_t>, primary_query_less> queries;
      std::map<std::pair<secondary_key_kind, uint64_t>, secondary_row_read> secondary_rows;
      std::map<secondary_query, std::optional<uint64_t>, secondary_query_less> secondary_queries;
   };
   const table_snapshot& get_table(const table_key&) const;
   std::pair<const table_snapshot*, const primary_row_read*> locate(const primary_query&) const;
   boost::container::flat_map<table_key, std::shared_ptr<const table_snapshot>, table_key_less> tables_;
   boost::container::flat_map<name, std::optional<account_read>> accounts_;
   size_t row_count_ = 0;
   size_t value_bytes_ = 0;
   size_t query_count_ = 0;
};

// One synchronous discovery invocation, on ONE unchanged canonical prefix.
// Capture and extension remain owner-only;workers may concurrently read an
// already published immutable version. Execution jobs join before canonical
// mutation;immutable-only reclamation drains before the next batch capture.
// Reclamation holds no DB capability and may overlap ordinary owner work.
// Canonical state MUST NOT mutate/undo during this capture-session scope. Never retain this across adoption, a
// prefix refresh, block abort, or another batch. Each returned version remains
// immutable: table maps use copy-on-write and row payloads have const ownership.
// This is not a cross-batch cache, MVCC, or permission for concurrent DB reads.
class frozen_primary_capture_session {
public:
   explicit frozen_primary_capture_session(const chainbase_transaction_state_view& source) : source_(source) {}
   frozen_primary_capture_session(const frozen_primary_capture_session&) = delete;
   frozen_primary_capture_session& operator=(const frozen_primary_capture_session&) = delete;
   std::shared_ptr<const frozen_primary_state> capture(std::span<const table_key>, std::span<const primary_query>,
      std::span<const secondary_query>, primary_snapshot_limits = {},
      const std::function<void()>& checkpoint = {}, std::span<const name> accounts = {});
   const primary_snapshot_capture_stats& stats() const { check_owner(); return stats_; }
private:
   void check_owner() const {
      if (std::this_thread::get_id() != owner_) throw state_view_wrong_thread();
   }
   const chainbase_transaction_state_view& source_;
   const std::thread::id owner_ = std::this_thread::get_id();
   std::shared_ptr<const frozen_primary_state> captured_;
   primary_snapshot_capture_stats stats_;
};

enum class primary_operation : uint8_t { update, store, remove };

struct primary_mutation {
   table_key table;
   uint64_t primary_key = 0;
   name payer;
   std::vector<char> value;
   primary_operation operation = primary_operation::update;
   int64_t object_id = 0;
   int64_t table_object_id = 0;
   bool operator==(const primary_mutation&) const = default;
};

struct secondary_mutation {
   table_key table;
   uint64_t primary_key = 0;
   name payer;
   secondary_key key = uint64_t{0};
   primary_operation operation = primary_operation::update; // same store/update/remove operations
   int64_t object_id = 0;
   int64_t table_object_id = 0;
   bool operator==(const secondary_mutation&) const = default;
};
// One chronology across all families: no side journal may lose interleaving,
// allocate another temporary table or escape existing operation/byte budgets.
using state_mutation = std::variant<primary_mutation, secondary_mutation>;
inline const primary_mutation& require_primary_mutation(const state_mutation& mutation) {
   if (const auto* primary = std::get_if<primary_mutation>(&mutation)) return *primary;
   throw primary_state_unsupported();
}

class primary_state_overlay;

// Not a transaction receipt or an authorization to mutate canonical state.
// Later adoption must validate the batch, dependencies and protocol context,
// settle resources/receipts, and replay writes within a transaction undo.
class primary_overlay_result {
public:
   const auto& base() const { return base_; }
   const auto& read_tables() const { return reads_; }
   const auto& read_queries() const { return queries_; }
   const auto& secondary_read_queries() const { return secondary_queries_; }
   const auto& read_accounts() const { return accounts_; }
   const auto& writes() const { return writes_; }
   const primary_mutation& primary_write(size_t index) const {
      return require_primary_mutation(writes_.at(index));
   }
   // Temporary fail-closed boundary for existing primary-only executors and
   // adopters; remove it per entry point only with typed replay/oracle coverage.
   bool primary_only() const { return primary_only_; }
   bool matches_base_reads(const transaction_state_view& live, const std::function<void()>& checkpoint) const;
private:
   friend class primary_state_overlay;
   primary_overlay_result(std::shared_ptr<const frozen_primary_state> base,
                          std::set<table_key, table_key_less> reads,
                          std::set<primary_query, primary_query_less> queries, std::vector<state_mutation> writes,
                          std::set<name> accounts, std::set<secondary_query, secondary_query_less> secondary_queries,
                          bool primary_only)
      : base_(std::move(base)), reads_(std::move(reads)), queries_(std::move(queries)), writes_(std::move(writes)),
        accounts_(std::move(accounts)), secondary_queries_(std::move(secondary_queries)), primary_only_(primary_only) {}
   std::shared_ptr<const frozen_primary_state> base_;
   std::set<table_key, table_key_less> reads_;
   std::set<primary_query, primary_query_less> queries_;
   std::vector<state_mutation> writes_;
   std::set<name> accounts_;
   std::set<secondary_query, secondary_query_less> secondary_queries_;
   bool primary_only_ = true;
};

// One thread owns each attempt. All mutations are retained in order. Created
// identities are attempt-local negative IDs, NEVER canonical object IDs. Only
// the owner's original hosts allocate canonical IDs and settle RAM/auth/undo.
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
   secondary_read_result read_secondary(const secondary_query&) const override;
   void update_existing(const table_key&, uint64_t primary_key, name payer, std::span<const char> value);
   void store(const table_key&, uint64_t primary_key, name payer, std::span<const char> value);
   void remove(const table_key&, uint64_t primary_key);
   void update_secondary(const table_key&, uint64_t primary_key, name payer, const secondary_key&);
   void store_secondary(const table_key&, uint64_t primary_key, name payer, const secondary_key&);
   void remove_secondary(const table_key&, uint64_t primary_key, const secondary_key& index_type);
   size_t write_count() const;
   primary_overlay_result seal();

private:
   friend class primary_overlay_cursor;
   friend class secondary_overlay_cursor;
   void check_owner_and_valid() const;
   void depend_on(const table_key&) const;
   void depend_on_query(primary_query) const;
   void depend_on_secondary_query(secondary_query) const;
   const primary_mutation* latest_write(const table_key&, uint64_t primary_key) const;
   const secondary_mutation* latest_secondary_write(const table_key&, secondary_key_kind, uint64_t primary_key) const;
   void check_write_budget(size_t) const;
   void append(primary_mutation);
   void append(secondary_mutation);
   int64_t temporary_id();
   std::shared_ptr<const frozen_primary_state> base_;
   primary_overlay_limits limits_;
   const std::thread::id owner_;
   mutable bool valid_ = true;
   mutable std::set<table_key, table_key_less> reads_;
   mutable std::set<primary_query, primary_query_less> queries_;
   mutable std::set<secondary_query, secondary_query_less> secondary_queries_;
   // end() and next(end) can avoid row queries, but still require a typed
   // secondary event proof before any legacy primary-only adoption is safe.
   mutable bool secondary_accessed_ = false;
   mutable std::set<name> accounts_;
   std::vector<state_mutation> writes_;
   std::map<table_key, std::map<uint64_t, size_t>, table_key_less> latest_writes_;
   std::map<table_key, std::map<std::pair<secondary_key_kind, uint64_t>, size_t>, table_key_less> latest_secondary_writes_;
   std::map<table_key, std::optional<table_read>, table_key_less> changed_tables_;
   size_t written_bytes_ = 0;
   int64_t next_temporary_id_ = -1;
};

// Action-local handles retain object identity, not just a logical key. Removed
// rows cannot be revived by recreate. Retired-table end handles force fallback;
// do not invent canonical behavior for the original cache's stale pointers.
class primary_overlay_cursor {
public:
   primary_overlay_cursor(primary_state_overlay& state, name receiver,
                          size_t max_iterators = 4096, size_t max_end_iterators = 64)
      : state_(state), receiver_(receiver), max_iterators_(max_iterators), max_ends_(max_end_iterators) {}
   primary_overlay_cursor(const primary_overlay_cursor&) = delete;
   primary_overlay_cursor& operator=(const primary_overlay_cursor&) = delete;

   // A canonical apply_context keeps one iterator cache across its notification
   // group. Switching its receiver does NOT authorize writes to cached foreign
   // rows: store/update/remove still compare their table code with receiver_.
   void set_receiver_for_notification(name receiver);

   int find(const table_key&, uint64_t primary_key);
   int lower_bound(const table_key&, uint64_t primary_key);
   int upper_bound(const table_key&, uint64_t primary_key);
   int end(const table_key&);
   int next(int iterator, uint64_t& primary_key);
   int previous(int iterator, uint64_t& primary_key);
   int get(int iterator, std::span<char> output);
   void update(int iterator, name payer, std::span<const char> value);
   int store(const table_key&, uint64_t primary_key, name payer, std::span<const char> value);
   void remove(int iterator);

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

// One cursor cache per index family, with the same lifetime as a canonical
// generic_index. Notification receivers may share it; roots/inline/sync frames
// must construct independent caches. These are owned handles, not WASM pointers.
class secondary_overlay_cursor {
public:
   secondary_overlay_cursor(primary_state_overlay&, name receiver, secondary_key_kind,
                            size_t max_iterators = 4096, size_t max_end_iterators = 64);
   secondary_overlay_cursor(const secondary_overlay_cursor&) = delete;
   secondary_overlay_cursor& operator=(const secondary_overlay_cursor&) = delete;
   void set_receiver_for_notification(name);
   int find_primary(const table_key&, secondary_key& output, uint64_t primary);
   int find_secondary(const table_key&, const secondary_key&, uint64_t& primary);
   int lower_bound(const table_key&, secondary_key&, uint64_t& primary);
   int upper_bound(const table_key&, secondary_key&, uint64_t& primary);
   int end(const table_key&);
   int next(int iterator, uint64_t& primary);
   int previous(int iterator, uint64_t& primary);
   int store(const table_key&, uint64_t primary, name payer, const secondary_key&);
   void update(int iterator, name payer, const secondary_key&);
   void remove(int iterator);
private:
   template<class F> decltype(auto) checked(F&& fn) {
      state_.check_owner_and_valid();
      try { return std::forward<F>(fn)(); }
      catch (...) { state_.valid_ = false; throw; }
   }
   void require_kind(const secondary_key&) const;
   struct row_handle { table_key table; secondary_row_read row; };
   row_handle get_handle(int iterator) const;
   int cache_table(const table_read&);
   int cache_row(const table_key&, const secondary_row_read&);
   int cache_result(const table_key&, const secondary_read_result&);
   int bound(const table_key&, secondary_key&, uint64_t&, secondary_query_kind);
   primary_state_overlay& state_;
   name receiver_;
   secondary_key_kind kind_;
   secondary_key zero_ = uint64_t{0};
   size_t max_iterators_, max_ends_;
   std::vector<row_handle> rows_;
   std::vector<table_read> tables_;
   std::map<int64_t, int> row_handles_;
   std::map<int64_t, int> table_handles_;
};

} // namespace eosio::chain::experimental
