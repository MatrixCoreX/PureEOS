#include <eosio/chain/parallel_state_overlay.hpp>
#include <eosio/chain/exceptions.hpp>
#include <algorithm>
#include <array>
#include <limits>

namespace eosio::chain::experimental {
namespace {
primary_row_metadata metadata(const primary_row_read& row) {
   return {row.object_id, row.table_object_id, row.primary_key, row.payer, row.value.size()};
}
primary_row_metadata metadata(const primary_mutation& row) {
   return {row.object_id, row.table_object_id, row.primary_key, row.payer, row.value.size()};
}
secondary_row_read metadata(const secondary_mutation& row) {
   return {row.object_id, row.table_object_id, row.primary_key, row.payer, row.key};
}
bool secondary_row_less(const secondary_row_read& a, const secondary_row_read& b) {
   if (secondary_key_less(a.key, b.key)) return true;
   if (secondary_key_less(b.key, a.key)) return false;
   return a.primary_key < b.primary_key;
}
bool selected_by(const secondary_query& query, const secondary_row_read& row) {
   using kind = secondary_query_kind;
   switch (query.kind) {
      case kind::find_primary: return row.primary_key == query.primary_key;
      case kind::first:
      case kind::last: return true;
      case kind::find_secondary: return secondary_key_equivalent(row.key, query.key);
      case kind::lower_bound: return !secondary_key_less(row.key, query.key);
      case kind::upper_bound: return secondary_key_less(query.key, row.key);
      case kind::next:
         return secondary_key_less(query.key, row.key) ||
            (secondary_key_equivalent(row.key, query.key) && row.primary_key > query.primary_key);
      case kind::previous:
         return secondary_key_less(row.key, query.key) ||
            (secondary_key_equivalent(row.key, query.key) && row.primary_key < query.primary_key);
   }
   throw secondary_state_invalid_query();
}
void require_query(primary_query_kind kind) {
   switch (kind) {
      case primary_query_kind::find:
      case primary_query_kind::lower_bound:
      case primary_query_kind::upper_bound:
      case primary_query_kind::last:
      case primary_query_kind::previous: return;
   }
   throw state_view_unsupported_query();
}
} // namespace

std::shared_ptr<const frozen_primary_state> frozen_primary_state::capture(
      const chainbase_transaction_state_view& source, std::span<const table_key> keys, primary_snapshot_limits limits) {
   if (keys.size() > limits.max_tables) throw primary_state_budget_exceeded();
   auto result = std::shared_ptr<frozen_primary_state>(new frozen_primary_state);
   for (const auto& key : keys) {
      if (result->tables_.contains(key)) continue;
      auto owned_table = std::make_shared<table_snapshot>();
      result->tables_.emplace(key, owned_table);
      auto& table = *owned_table;
      table.metadata = source.read_table(key);
      primary_query query{key, primary_query_kind::lower_bound, 0};
      while (true) {
         const auto found = source.locate_primary(query);
         if (found.table != table.metadata) throw primary_state_invalid_attempt();
         if (!found.row) break;
         if (result->row_count_ == limits.max_rows ||
             found.row->value_size > limits.max_value_bytes - result->value_bytes_)
            throw primary_state_budget_exceeded();
         auto read = source.read_primary({key, primary_query_kind::find, found.row->primary_key},
                                         limits.max_value_bytes - result->value_bytes_);
         if (!read.row || read.table != found.table || metadata(*read.row) != *found.row)
            throw primary_state_invalid_attempt();
         result->value_bytes_ += read.row->value.size();
         ++result->row_count_;
         table.rows.emplace(read.row->primary_key, std::make_shared<const primary_row_read>(std::move(*read.row)));
         // Strict upper bound; no overflow at UINT64_MAX.
         query = {key, primary_query_kind::upper_bound, found.row->primary_key};
      }
   }
   return result;
}

const frozen_primary_state::table_snapshot& frozen_primary_state::get_table(const table_key& key) const {
   const auto it = tables_.find(key);
   if (it == tables_.end()) throw primary_state_missing(key);
   return *it->second;
}

std::shared_ptr<const frozen_primary_state> frozen_primary_state::capture_queries(
      const chainbase_transaction_state_view& source, std::span<const table_key> keys,
      std::span<const primary_query> queries, primary_snapshot_limits limits, const std::function<void()>& checkpoint,
      std::span<const name> accounts) {
   return capture_queries_impl(source, nullptr, keys, queries, {}, limits, checkpoint, accounts);
}

std::shared_ptr<const frozen_primary_state> frozen_primary_state::capture_queries_with_secondary(
      const chainbase_transaction_state_view& source, std::span<const table_key> keys,
      std::span<const primary_query> queries, std::span<const secondary_query> secondary_queries,
      primary_snapshot_limits limits, const std::function<void()>& checkpoint, std::span<const name> accounts) {
   const chainbase_secondary_state_view secondary(source); // same DB and original owner
   return capture_queries_impl(source, &secondary, keys, queries, secondary_queries, limits, checkpoint, accounts);
}

std::shared_ptr<const frozen_primary_state> frozen_primary_state::capture_queries_impl(
      const chainbase_transaction_state_view& source, const chainbase_secondary_state_view* secondary,
      std::span<const table_key> keys, std::span<const primary_query> queries,
      std::span<const secondary_query> secondary_queries, primary_snapshot_limits limits,
      const std::function<void()>& checkpoint, std::span<const name> accounts,
      const frozen_primary_state* same_prefix, primary_snapshot_capture_stats* stats) {
   if (keys.size() > limits.max_tables || queries.size() > limits.max_queries ||
       secondary_queries.size() > limits.max_queries - queries.size() || accounts.size() > limits.max_accounts)
      throw primary_state_budget_exceeded();
   auto result = std::shared_ptr<frozen_primary_state>(new frozen_primary_state);
   if (same_prefix) {
      // Sharing is safe ONLY within the caller's joined, unchanged-prefix
      // discovery scope. Copy maps/const pointers, never mutate an old version.
      if (same_prefix->tables_.size() > limits.max_tables || same_prefix->row_count_ > limits.max_rows ||
          same_prefix->value_bytes_ > limits.max_value_bytes || same_prefix->query_count_ > limits.max_queries ||
          same_prefix->accounts_.size() > limits.max_accounts) throw primary_state_budget_exceeded();
      if (checkpoint) checkpoint();
      result->tables_ = same_prefix->tables_;
      result->accounts_ = same_prefix->accounts_;
      result->row_count_ = same_prefix->row_count_;
      result->value_bytes_ = same_prefix->value_bytes_;
      result->query_count_ = same_prefix->query_count_;
   }
   for (const auto account : accounts) {
      if (checkpoint) checkpoint();
      if (!result->accounts_.contains(account)) {
         if (result->accounts_.size() == limits.max_accounts) throw primary_state_budget_exceeded();
         result->accounts_.emplace(account, source.read_account(account));
         if (stats) ++stats->accounts_read;
      }
   }
   std::map<table_key, std::shared_ptr<table_snapshot>, table_key_less> changed;
   auto capture_table = [&](const table_key& key) -> const table_snapshot& {
      if (checkpoint) checkpoint();
      if (auto it = result->tables_.find(key); it != result->tables_.end()) return *it->second;
      if (result->tables_.size() == limits.max_tables) throw primary_state_budget_exceeded();
      auto table = std::make_shared<table_snapshot>();
      table->complete = false;
      table->metadata = source.read_table(key);
      if (stats) ++stats->tables_read;
      result->tables_.emplace(key, table);
      changed.emplace(key, table);
      return *table;
   };
   auto writable_table = [&](const table_key& key) -> table_snapshot& {
      if (const auto found = changed.find(key); found != changed.end()) return *found->second;
      auto copy = std::make_shared<table_snapshot>(*result->tables_.at(key));
      result->tables_.at(key) = copy;
      changed.emplace(key, copy);
      return *copy;
   };
   for (const auto& key : keys) capture_table(key);
   for (auto query : queries) {
      require_query(query.kind);
      if (query.kind == primary_query_kind::last) query.primary_key = 0;
      const auto& captured = capture_table(query.table);
      if (captured.queries.contains(query)) continue;
      if (result->query_count_ == limits.max_queries) throw primary_state_budget_exceeded();
      auto& table = writable_table(query.table);
      // Selection, metadata validation, duplicate handling, budget checks and
      // owned payload copy share one fresh native lookup with no callback gap.
      auto read=source.read_primary_for_capture(query,table.metadata,table.rows,
         result->row_count_<limits.max_rows,limits.max_value_bytes-result->value_bytes_);
      if (stats) ++stats->primary_queries_read;
      const auto& found=read.metadata;
      if(read.changed || found.table!=table.metadata)throw primary_state_invalid_attempt();
      if(read.budget_exceeded)throw primary_state_budget_exceeded();
      std::optional<uint64_t> selected;
      if(found.row) {
         selected=found.row->primary_key;
         if(auto existing=table.rows.find(*selected);existing!=table.rows.end()) {
            if(metadata(*existing->second)!=*found.row || read.owned_row)throw primary_state_invalid_attempt();
         } else {
            if(result->row_count_==limits.max_rows ||
               found.row->value_size>limits.max_value_bytes-result->value_bytes_)
               throw primary_state_budget_exceeded();
            if(!read.owned_row || metadata(*read.owned_row)!=*found.row)throw primary_state_invalid_attempt();
            result->value_bytes_+=read.owned_row->value.size();
            ++result->row_count_;
            table.rows.emplace(*selected,std::make_shared<const primary_row_read>(std::move(*read.owned_row)));
            if(stats)++stats->primary_rows_copied;
         }
      }
      table.queries.emplace(query, selected);
      ++result->query_count_;
   }
   if (!secondary_queries.empty() && !secondary) throw primary_state_invalid_attempt();
   for (auto query : secondary_queries) {
      query = normalize_secondary_query(std::move(query));
      const auto& captured = capture_table(query.table);
      if (captured.secondary_queries.contains(query)) continue;
      if (result->query_count_ == limits.max_queries) throw primary_state_budget_exceeded();
      auto& table = writable_table(query.table);
      const auto found = secondary->read_secondary(query);
      if (stats) ++stats->secondary_queries_read;
      if (found.table != table.metadata) throw primary_state_invalid_attempt();
      std::optional<uint64_t> selected;
      if (found.row) {
         selected = found.row->primary_key;
         const auto identity = std::pair{secondary_kind(query.key), *selected};
         if (auto existing = table.secondary_rows.find(identity); existing != table.secondary_rows.end()) {
            if (existing->second != *found.row) throw primary_state_invalid_attempt();
         } else {
            const auto bytes = secondary_key_bytes(found.row->key);
            if (result->row_count_ == limits.max_rows || bytes > limits.max_value_bytes - result->value_bytes_)
               throw primary_state_budget_exceeded();
            table.secondary_rows.emplace(identity, *found.row);
            result->value_bytes_ += bytes;
            ++result->row_count_;
         }
      }
      table.secondary_queries.emplace(std::move(query), selected);
      ++result->query_count_;
   }
   return result;
}

std::shared_ptr<const frozen_primary_state> frozen_primary_capture_session::capture(
      std::span<const table_key> keys, std::span<const primary_query> queries,
      std::span<const secondary_query> secondary_queries, primary_snapshot_limits limits,
      const std::function<void()>& checkpoint, std::span<const name> accounts) {
   check_owner();
   const chainbase_secondary_state_view secondary(source_);
   auto next = frozen_primary_state::capture_queries_impl(source_, &secondary, keys, queries,
      secondary_queries, limits, checkpoint, accounts, captured_.get(), &stats_);
   // Do not publish a partially extended snapshot on exceptions or cancellation.
   captured_ = next;
   return next;
}

secondary_read_result frozen_primary_state::read_secondary(const secondary_query& input) const {
   const auto query = normalize_secondary_query(input); // validate before even missing-table lookup
   const auto captured = tables_.find(query.table);
   if (captured == tables_.end()) throw primary_state_missing(query);
   const auto& table = *captured->second;
   secondary_read_result result{table.metadata, std::nullopt};
   if (!table.metadata) return result;
   const auto answer = table.secondary_queries.find(query);
   if (answer == table.secondary_queries.end()) {
      // An owned positive row proves THIS typed primary lookup, not any
      // absence, duplicate-key selection or neighboring range. Never read DB.
      if (query.kind == secondary_query_kind::find_primary) {
         const auto row = table.secondary_rows.find({secondary_kind(query.key), query.primary_key});
         if (row != table.secondary_rows.end()) {
            result.row = row->second;
            return result;
         }
      }
      throw primary_state_missing(query);
   }
   if (answer->second) result.row = table.secondary_rows.at({secondary_kind(query.key), *answer->second});
   return result;
}

std::pair<const frozen_primary_state::table_snapshot*, const primary_row_read*>
frozen_primary_state::locate(const primary_query& query) const {
   require_query(query.kind);
   const auto captured = tables_.find(query.table);
   if (captured == tables_.end()) throw primary_state_missing(query);
   const auto& table = *captured->second;
   if (!table.complete) {
      // An absent TABLE proves all its primary queries absent. Otherwise the
      // exact query must have been captured; sparse rows cannot prove ranges.
      if (!table.metadata) return {&table, nullptr};
      auto normalized = query;
      if (normalized.kind == primary_query_kind::last) normalized.primary_key = 0;
      const auto answer = table.queries.find(normalized);
      if (answer == table.queries.end()) {
         // A range-selected owned row already proves its exact positive
         // primary lookup (e.g. db_get after lower_bound). It proves neither
         // unobserved absences nor adjacency; those still request discovery.
         if (normalized.kind == primary_query_kind::find) {
            const auto row = table.rows.find(normalized.primary_key);
            if (row != table.rows.end()) return {&table, row->second.get()};
         }
         throw primary_state_missing(normalized);
      }
      return {&table, answer->second ? table.rows.at(*answer->second).get() : nullptr};
   }
   const auto& rows = table.rows;
   auto it = rows.end();
   switch (query.kind) {
      case primary_query_kind::find: it = rows.find(query.primary_key); break;
      case primary_query_kind::lower_bound: it = rows.lower_bound(query.primary_key); break;
      case primary_query_kind::upper_bound: it = rows.upper_bound(query.primary_key); break;
      case primary_query_kind::last: if (!rows.empty()) it = std::prev(rows.end()); break;
      case primary_query_kind::previous:
         it = rows.lower_bound(query.primary_key);
         it = it == rows.begin() ? rows.end() : std::prev(it);
         break;
   }
   return {&table, it == rows.end() ? nullptr : it->second.get()};
}

std::optional<table_read> frozen_primary_state::read_table(const table_key& key) const {
   return get_table(key).metadata;
}

std::optional<account_read> frozen_primary_state::read_account(name account) const {
   const auto found = accounts_.find(account);
   if (found == accounts_.end()) throw primary_state_missing(account);
   return found->second;
}

primary_metadata_result frozen_primary_state::locate_primary(const primary_query& query) const {
   const auto [table, row] = locate(query);
   primary_metadata_result result{table->metadata, std::nullopt};
   if (row) result.row = metadata(*row);
   return result;
}

primary_prefix_result frozen_primary_state::read_primary_prefix(const primary_query& query, size_t max_bytes) const {
   const auto [table, row] = locate(query);
   primary_prefix_result result{{table->metadata, std::nullopt}, {}};
   if (row) {
      result.metadata.row = metadata(*row);
      result.value.assign(row->value.begin(), row->value.begin() + std::min(max_bytes, row->value.size()));
   }
   return result;
}

primary_read_result frozen_primary_state::read_primary(const primary_query& query, size_t max_value_bytes) const {
   const auto [table, row] = locate(query);
   primary_read_result result{table->metadata, std::nullopt};
   if (row) {
      if (row->value.size() > max_value_bytes) throw state_view_read_limit_exceeded();
      result.row = *row;
   }
   return result;
}

primary_state_overlay::primary_state_overlay(std::shared_ptr<const frozen_primary_state> base,
                                             primary_overlay_limits limits)
   : base_(std::move(base)), limits_(limits), owner_(std::this_thread::get_id()) {
   if (!base_) throw primary_state_invalid_attempt();
}

void primary_state_overlay::check_owner_and_valid() const {
   if (std::this_thread::get_id() != owner_) throw state_view_wrong_thread();
   if (!valid_) throw primary_state_invalid_attempt();
}

void primary_state_overlay::depend_on(const table_key& key) const {
   // Table existence/identity/payer/count remain dependencies even when the
   // transaction only asks for an end iterator. Values are tracked by queries.
   if (!reads_.contains(key)) {
      if (reads_.size() == limits_.max_read_tables) throw primary_state_budget_exceeded();
      reads_.insert(key);
   }
}

void primary_state_overlay::depend_on_query(primary_query query) const {
   depend_on(query.table);
   if (query.kind == primary_query_kind::last) query.primary_key = 0; // ignored by the API
   if (!queries_.contains(query)) {
      if (queries_.size() >= limits_.max_read_queries - secondary_queries_.size())
         throw primary_state_budget_exceeded();
      queries_.insert(query);
   }
}

void primary_state_overlay::depend_on_secondary_query(secondary_query query) const {
   query = normalize_secondary_query(std::move(query));
   depend_on(query.table);
   if (!secondary_queries_.contains(query)) {
      if (secondary_queries_.size() >= limits_.max_read_queries - queries_.size())
         throw primary_state_budget_exceeded();
      secondary_queries_.insert(std::move(query));
   }
}

const primary_mutation* primary_state_overlay::latest_write(const table_key& table, uint64_t key) const {
   const auto t = latest_writes_.find(table);
   if (t == latest_writes_.end()) return nullptr;
   const auto row = t->second.find(key);
   return row == t->second.end() ? nullptr : &std::get<primary_mutation>(writes_[row->second]);
}

const secondary_mutation* primary_state_overlay::latest_secondary_write(
      const table_key& table, secondary_key_kind kind, uint64_t key) const {
   const auto t = latest_secondary_writes_.find(table);
   if (t == latest_secondary_writes_.end()) return nullptr;
   const auto row = t->second.find({kind, key});
   return row == t->second.end() ? nullptr : &std::get<secondary_mutation>(writes_[row->second]);
}

std::optional<table_read> primary_state_overlay::read_table(const table_key& key) const {
   check_owner_and_valid();
   try {
      depend_on(key);
      const auto original = base_->read_table(key);
      if (original && original->object_id < 0) throw primary_state_unsupported();
      const auto changed = changed_tables_.find(key);
      return changed == changed_tables_.end() ? original : changed->second;
   } catch (...) { valid_ = false; throw; }
}

std::optional<account_read> primary_state_overlay::read_account(name account) const {
   check_owner_and_valid();
   try {
      if (!accounts_.contains(account)) {
         if (accounts_.size() == limits_.max_read_accounts) throw primary_state_budget_exceeded();
         accounts_.insert(account);
      }
      return base_->read_account(account);
   } catch (...) { valid_ = false; throw; }
}

primary_metadata_result primary_state_overlay::locate_primary(const primary_query& query) const {
   check_owner_and_valid();
   try {
      require_query(query.kind);
      auto observe = [&](const primary_query& q) {
         depend_on_query(q);
         auto result = base_->locate_primary(q);
         if (result.row && result.row->object_id < 0) throw primary_state_unsupported();
         return result;
      };
      auto found = observe(query);
      const auto table = read_table(query.table);
      if (!table) return {std::nullopt, std::nullopt};
      const bool descending = query.kind == primary_query_kind::last || query.kind == primary_query_kind::previous;
      // A recreated table contains no rows from the retired generation.
      if (!found.table || found.table->object_id != table->object_id) found.row.reset();
      while (found.row) {
         const auto* write = latest_write(query.table, found.row->primary_key);
         if (!write || write->operation != primary_operation::remove) break;
         if (query.kind == primary_query_kind::find) { found.row.reset(); break; }
         // Sparse captures do NOT prove the next surviving row. Observe every
         // skipped neighbor; an unknown query restarts discovery on the owner.
         found = observe({query.table, descending ? primary_query_kind::previous : primary_query_kind::upper_bound,
                          found.row->primary_key});
      }
      found.table = table;
      if (found.row) if (const auto* write = latest_write(query.table, found.row->primary_key)) found.row = metadata(*write);
      const auto changed = latest_writes_.find(query.table);
      if (changed == latest_writes_.end()) return found;
      const auto& rows = changed->second;
      auto it = rows.end();
      switch (query.kind) {
         case primary_query_kind::find: it = rows.find(query.primary_key); break;
         case primary_query_kind::lower_bound: it = rows.lower_bound(query.primary_key); break;
         case primary_query_kind::upper_bound: it = rows.upper_bound(query.primary_key); break;
         case primary_query_kind::last: if (!rows.empty()) it = std::prev(rows.end()); break;
         case primary_query_kind::previous:
            it = rows.lower_bound(query.primary_key);
            it = it == rows.begin() ? rows.end() : std::prev(it);
            break;
      }
      while (it != rows.end()) {
         const auto& write = std::get<primary_mutation>(writes_[it->second]);
         if (write.operation != primary_operation::remove && write.table_object_id == table->object_id) {
            if (!found.row || (descending ? it->first > found.row->primary_key : it->first < found.row->primary_key))
               found.row = metadata(write);
            break;
         }
         if (query.kind == primary_query_kind::find) break;
         if (descending) it = it == rows.begin() ? rows.end() : std::prev(it);
         else ++it;
      }
      return found;
   } catch (...) { valid_ = false; throw; }
}

primary_prefix_result primary_state_overlay::read_primary_prefix(const primary_query& query, size_t max_bytes) const {
   check_owner_and_valid();
   try {
      auto found = locate_primary(query);
      if (!found.row) return {found, {}};
      if (const auto* write = latest_write(query.table, found.row->primary_key)) {
         primary_prefix_result result{std::move(found), {}};
         result.value.assign(write->value.begin(), write->value.begin() + std::min(max_bytes, write->value.size()));
         return result;
      }
      const auto selected = base_->locate_primary(query);
      const auto source = selected.row && selected.row->object_id == found.row->object_id ? query :
         primary_query{query.table, primary_query_kind::find, found.row->primary_key};
      depend_on_query(source);
      auto original = base_->read_primary_prefix(source, max_bytes);
      return {found, std::move(original.value)};
   } catch (...) { valid_ = false; throw; }
}

primary_read_result primary_state_overlay::read_primary(const primary_query& query, size_t max_value_bytes) const {
   check_owner_and_valid();
   try {
      auto found = locate_primary(query);
      if (!found.row) return {found.table, std::nullopt};
      if (found.row->value_size > max_value_bytes) throw state_view_read_limit_exceeded();
      if (const auto* write = latest_write(query.table, found.row->primary_key)) {
         const auto& row = *found.row;
         return {found.table, primary_row_read{row.object_id, row.table_object_id, row.primary_key, row.payer, write->value}};
      }
      const auto selected = base_->locate_primary(query);
      const auto source = selected.row && selected.row->object_id == found.row->object_id ? query :
         primary_query{query.table, primary_query_kind::find, found.row->primary_key};
      depend_on_query(source);
      auto original = base_->read_primary(source, max_value_bytes);
      original.table = found.table;
      return original;
   } catch (...) { valid_ = false; throw; }
}

void primary_state_overlay::update_existing(const table_key& table, uint64_t key, name payer, std::span<const char> value) {
   check_owner_and_valid();
   try {
      const auto found = locate_primary({table, primary_query_kind::find, key});
      if (!found.row) throw primary_state_unsupported();
      check_write_budget(value.size());
      // Resolve same_payer against the latest PRIVATE row, not the base. Keep
      // all intermediate writes: collapsing A->B->A loses original RAM/auth
      // effects and can turn a canonically failing action into a success.
      append(primary_mutation{table, key, payer == name{} ? found.row->payer : payer, std::vector<char>(value.begin(), value.end()),
              primary_operation::update, found.row->object_id, found.row->table_object_id});
   } catch (...) { valid_ = false; throw; }
}

void primary_state_overlay::check_write_budget(size_t bytes) const {
   if (writes_.size() >= limits_.max_write_operations || bytes > limits_.max_write_value_bytes - written_bytes_)
      throw primary_state_budget_exceeded();
}

void primary_state_overlay::append(primary_mutation write) {
   check_write_budget(write.value.size());
   written_bytes_ += write.value.size();
   writes_.push_back(std::move(write));
   const auto& saved = std::get<primary_mutation>(writes_.back());
   latest_writes_[saved.table][saved.primary_key] = writes_.size() - 1;
}

void primary_state_overlay::append(secondary_mutation write) {
   const auto bytes = secondary_key_bytes(write.key);
   check_write_budget(bytes);
   written_bytes_ += bytes;
   writes_.push_back(std::move(write));
   const auto& saved = std::get<secondary_mutation>(writes_.back());
   latest_secondary_writes_[saved.table][{secondary_kind(saved.key), saved.primary_key}] = writes_.size() - 1;
}

int64_t primary_state_overlay::temporary_id() {
   if (next_temporary_id_ == std::numeric_limits<int64_t>::min()) throw primary_state_budget_exceeded();
   return next_temporary_id_--;
}

void primary_state_overlay::store(const table_key& key, uint64_t primary, name payer, std::span<const char> value) {
   check_owner_and_valid();
   try {
      check_write_budget(value.size());
      const auto found = locate_primary({key, primary_query_kind::find, primary});
      if (found.row || payer == name{}) throw primary_state_unsupported();
      auto table = found.table.value_or(table_read{key, 0, payer, 0});
      if (!found.table) table.object_id = temporary_id();
      if (table.count == std::numeric_limits<uint32_t>::max()) throw primary_state_unsupported();
      ++table.count;
      changed_tables_[key] = table;
      append(primary_mutation{key, primary, payer, std::vector<char>(value.begin(), value.end()), primary_operation::store,
              temporary_id(), table.object_id});
   } catch (...) { valid_ = false; throw; }
}

void primary_state_overlay::remove(const table_key& key, uint64_t primary) {
   check_owner_and_valid();
   try {
      check_write_budget(0);
      const auto found = locate_primary({key, primary_query_kind::find, primary});
      if (!found.row || !found.table || !found.table->count) throw primary_state_unsupported();
      auto table = *found.table;
      --table.count;
      changed_tables_[key] = table.count ? std::optional{table} : std::nullopt;
      append(primary_mutation{key, primary, found.row->payer, {}, primary_operation::remove,
              found.row->object_id, found.row->table_object_id});
   } catch (...) { valid_ = false; throw; }
}

secondary_read_result primary_state_overlay::read_secondary(const secondary_query& input) const {
   check_owner_and_valid();
   try {
      const auto query = normalize_secondary_query(input); // validate NaN even on an absent table
      secondary_accessed_ = true;
      const auto kind = secondary_kind(query.key);
      const auto observe = [&](const secondary_query& q) {
         depend_on_secondary_query(q);
         auto found = base_->read_secondary(q);
         if (found.row && (found.row->object_id < 0 || found.row->table_object_id < 0))
            throw primary_state_unsupported();
         return found;
      };
      auto found = observe(query);
      const auto table = read_table(query.table);
      if (!table) return {};
      if (!found.table || found.table->object_id != table->object_id) found.row.reset();
      const bool descending = query.kind == secondary_query_kind::last || query.kind == secondary_query_kind::previous;
      // Any privately changed base row is reconsidered in the merge below:
      // its secondary key may have moved to either side of the query bound.
      while (found.row && latest_secondary_write(query.table, kind, found.row->primary_key)) {
         if (query.kind == secondary_query_kind::find_primary) { found.row.reset(); break; }
         const auto anchor = *found.row;
         found = observe({query.table, descending ? secondary_query_kind::previous : secondary_query_kind::next,
                          anchor.key, anchor.primary_key});
         // find-secondary is equality, not an unconstrained next-row query.
         if (found.row && !selected_by(query, *found.row)) { found.row.reset(); break; }
      }
      found.table = table;
      const auto changed = latest_secondary_writes_.find(query.table);
      if (changed == latest_secondary_writes_.end()) return found;
      for (const auto& [identity, index] : changed->second) {
         if (identity.first != kind) continue;
         const auto& write = std::get<secondary_mutation>(writes_[index]);
         if (write.operation == primary_operation::remove || write.table_object_id != table->object_id) continue;
         const auto candidate = metadata(write);
         if (!selected_by(query, candidate)) continue;
         if (!found.row || (descending ? secondary_row_less(*found.row, candidate) : secondary_row_less(candidate, *found.row)))
            found.row = candidate;
      }
      return found;
   } catch (...) { valid_ = false; throw; }
}

void primary_state_overlay::update_secondary(const table_key& table, uint64_t primary, name payer,
                                            const secondary_key& key) {
   check_owner_and_valid();
   try {
      if (secondary_key_is_nan(key)) throw secondary_state_invalid_query();
      const auto found = read_secondary({table, secondary_query_kind::find_primary, key, primary});
      if (!found.row) throw primary_state_unsupported();
      append(secondary_mutation{table, primary, payer == name{} ? found.row->payer : payer, key,
         primary_operation::update, found.row->object_id, found.row->table_object_id});
   } catch (...) { valid_ = false; throw; }
}

void primary_state_overlay::store_secondary(const table_key& table_key, uint64_t primary, name payer,
                                           const secondary_key& key) {
   check_owner_and_valid();
   try {
      if (secondary_key_is_nan(key)) throw secondary_state_invalid_query();
      check_write_budget(secondary_key_bytes(key));
      const auto found = read_secondary({table_key, secondary_query_kind::find_primary, key, primary});
      if (found.row || payer == name{}) throw primary_state_unsupported();
      auto table = found.table.value_or(table_read{table_key, 0, payer, 0});
      if (!found.table) table.object_id = temporary_id();
      if (table.count == std::numeric_limits<uint32_t>::max()) throw primary_state_unsupported();
      ++table.count;
      changed_tables_[table_key] = table;
      append(secondary_mutation{table_key, primary, payer, key, primary_operation::store, temporary_id(), table.object_id});
   } catch (...) { valid_ = false; throw; }
}

void primary_state_overlay::remove_secondary(const table_key& table_key, uint64_t primary,
                                            const secondary_key& index_type) {
   check_owner_and_valid();
   try {
      const auto found = read_secondary({table_key, secondary_query_kind::find_primary, index_type, primary});
      if (!found.row || !found.table || !found.table->count) throw primary_state_unsupported();
      check_write_budget(secondary_key_bytes(found.row->key));
      auto table = *found.table;
      --table.count;
      changed_tables_[table_key] = table.count ? std::optional{table} : std::nullopt;
      append(secondary_mutation{table_key, primary, found.row->payer, found.row->key, primary_operation::remove,
         found.row->object_id, found.row->table_object_id});
   } catch (...) { valid_ = false; throw; }
}

size_t primary_state_overlay::write_count() const {
   check_owner_and_valid();
   return writes_.size();
}

primary_overlay_result primary_state_overlay::seal() {
   check_owner_and_valid();
   valid_ = false;
   const bool primary_only = !secondary_accessed_ && secondary_queries_.empty() && latest_secondary_writes_.empty();
   return {base_, std::move(reads_), std::move(queries_), std::move(writes_), std::move(accounts_),
      std::move(secondary_queries_), primary_only};
}

bool primary_overlay_result::matches_base_reads(const transaction_state_view& live,
                                               const std::function<void()>& checkpoint) const {
   // Small read sets need no per-ID heap nodes; larger sets retain logarithmic
   // tree storage after a bounded eight-element scan. Lifetime is this call.
   small_seen_keys<int64_t> checked_values;
   const auto first_value=[&](int64_t id) {return checked_values.insert(id);};
   const auto* native=dynamic_cast<const chainbase_transaction_state_view*>(&live);
   for (const auto account : accounts_) {
      checkpoint();
      if (live.read_account(account) != base_->read_account(account)) return false;
   }
   for (const auto& table : reads_) {
      checkpoint();
      if (live.read_table(table) != base_->read_table(table)) return false;
   }
   for (const auto& query : queries_) {
      checkpoint();
      const auto expected = base_->locate_primary(query);
      // Only the final native Chainbase adapter has the reviewed combined
      // metadata/full-prefix semantics. Preserve the original selection and
      // payload call protocol for every other state-view adapter.
      if (!native) {
         if (live.locate_primary(query) != expected) return false;
         // Distinct range/point queries can select the same large row. Selection
         // is checked for each query, but compare its payload once per object ID
         // while the serial owner holds this unchanged canonical state.
         if (expected.row && first_value(expected.row->object_id) &&
             live.read_primary(query, expected.row->value_size) !=
                             base_->read_primary(query, expected.row->value_size)) return false;
         continue;
      }
      // The frozen base owns these bytes for the complete sealed-result lifetime.
      // Compare all bytes plus fresh table/row metadata without copying either
      // payload. No canonical reference survives matches_primary or a callback.
      if (expected.row && first_value(expected.row->object_id)) {
         const auto& value=base_->get_table(query.table).rows.at(expected.row->primary_key)->value;
         if (!native->matches_primary(query,expected,value)) return false;
      } else if (live.locate_primary(query) != expected) return false;
   }
   for (const auto& query : secondary_queries_) {
      checkpoint();
      if (live.read_secondary(query) != base_->read_secondary(query)) return false;
   }
   return true;
}

void primary_overlay_cursor::set_receiver_for_notification(name receiver) {
   checked([&] { receiver_ = receiver; });
}

const primary_overlay_cursor::row_handle& primary_overlay_cursor::get_handle(int iterator) const {
   EOS_ASSERT(iterator != -1, invalid_table_iterator, "invalid iterator");
   EOS_ASSERT(iterator >= 0, table_operation_not_permitted, "dereference of end iterator");
   EOS_ASSERT(static_cast<size_t>(iterator) < rows_.size(), invalid_table_iterator, "iterator out of range");
   const auto& cached = rows_[iterator];
   // The immutable base and the query that cached this row already prove its
   // initial identity. Only this attempt's journal can retire/replace it; avoid
   // inventing another sparse capture merely to check a cached iterator.
   if (const auto* current = state_.latest_write(cached.table, cached.row.primary_key);
       current && (current->operation == primary_operation::remove || current->object_id != cached.row.object_id ||
                   current->table_object_id != cached.row.table_object_id)) throw primary_state_unsupported();
   return cached;
}

int primary_overlay_cursor::cache_table(const table_read& table) {
   const auto found = table_handles_.find(table.object_id);
   if (found != table_handles_.end()) return found->second;
   if (tables_.size() >= max_ends_ || tables_.size() > static_cast<size_t>(std::numeric_limits<int>::max() - 2))
      throw primary_state_budget_exceeded();
   const int result = -2 - static_cast<int>(tables_.size());
   tables_.push_back(table);
   table_handles_.emplace(table.object_id, result);
   return result;
}

int primary_overlay_cursor::cache_row(const table_key& table, const primary_row_metadata& row) {
   const auto found = row_handles_.find(row.object_id);
   if (found != row_handles_.end()) return found->second;
   if (rows_.size() >= max_iterators_ || rows_.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
      throw primary_state_budget_exceeded();
   const int result = static_cast<int>(rows_.size());
   rows_.push_back({table, row});
   row_handles_.emplace(row.object_id, result);
   return result;
}

int primary_overlay_cursor::seek(const primary_query& query) {
   const auto found = state_.locate_primary(query);
   if (!found.table) return -1;
   const auto end_iterator = cache_table(*found.table);
   return found.row ? cache_row(query.table, *found.row) : end_iterator;
}

int primary_overlay_cursor::find(const table_key& table, uint64_t key) {
   return checked([&] { return seek({table, primary_query_kind::find, key}); });
}

int primary_overlay_cursor::lower_bound(const table_key& table, uint64_t key) {
   return checked([&] { return seek({table, primary_query_kind::lower_bound, key}); });
}

int primary_overlay_cursor::upper_bound(const table_key& table, uint64_t key) {
   return checked([&] { return seek({table, primary_query_kind::upper_bound, key}); });
}

int primary_overlay_cursor::end(const table_key& table) {
   return checked([&] {
      const auto found = state_.read_table(table);
      return found ? cache_table(*found) : -1;
   });
}

int primary_overlay_cursor::next(int iterator, uint64_t& primary_key) {
   return checked([&] {
      if (iterator < -1) return -1; // preserve original host behaviour even for an unknown negative handle
      const auto row = get_handle(iterator);
      const auto found = state_.locate_primary({row.table, primary_query_kind::upper_bound, row.row.primary_key});
      if (!found.row) return table_handles_.at(row.row.table_object_id);
      primary_key = found.row->primary_key;
      return cache_row(row.table, *found.row);
   });
}

int primary_overlay_cursor::previous(int iterator, uint64_t& primary_key) {
   return checked([&] {
      primary_query query;
      if (iterator < -1) {
         // Wider arithmetic also safely rejects INT_MIN; never negate it as int.
         const auto index = static_cast<uint64_t>(-static_cast<int64_t>(iterator) - 2);
         EOS_ASSERT(index < tables_.size(), invalid_table_iterator, "not a valid end iterator");
         const auto current = state_.read_table(tables_[index].key);
         if (!current || current->object_id != tables_[index].object_id) throw primary_state_unsupported();
         query = {tables_[index].key, primary_query_kind::last, 0};
      } else {
         const auto& row = get_handle(iterator);
         query = {row.table, primary_query_kind::previous, row.row.primary_key};
      }
      const auto found = state_.locate_primary(query);
      if (!found.row) return -1;
      primary_key = found.row->primary_key;
      return cache_row(query.table, *found.row);
   });
}

int primary_overlay_cursor::get(int iterator, std::span<char> output) {
   return checked([&] {
      const auto row = get_handle(iterator);
      const auto read = state_.read_primary_prefix({row.table, primary_query_kind::find, row.row.primary_key}, output.size());
      if (!read.metadata.row || read.metadata.row->object_id != row.row.object_id ||
          read.metadata.row->table_object_id != row.row.table_object_id)
         throw primary_state_invalid_attempt();
      if (read.metadata.row->value_size > static_cast<size_t>(std::numeric_limits<int>::max()))
         throw primary_state_unsupported();
      if (output.empty()) return static_cast<int>(read.metadata.row->value_size);
      std::copy(read.value.begin(), read.value.end(), output.begin());
      return static_cast<int>(read.value.size());
   });
}

void primary_overlay_cursor::update(int iterator, name payer, std::span<const char> value) {
   checked([&] {
      const auto row = get_handle(iterator);
      EOS_ASSERT(row.table.code == receiver_, table_access_violation, "db access violation");
      state_.update_existing(row.table, row.row.primary_key, payer, value);
   });
}

int primary_overlay_cursor::store(const table_key& table, uint64_t primary, name payer, std::span<const char> value) {
   return checked([&] {
      EOS_ASSERT(table.code == receiver_, table_access_violation, "db access violation");
      state_.store(table, primary, payer, value);
      return seek({table, primary_query_kind::find, primary});
   });
}

void primary_overlay_cursor::remove(int iterator) {
   checked([&] {
      const auto row = get_handle(iterator);
      EOS_ASSERT(row.table.code == receiver_, table_access_violation, "db access violation");
      state_.remove(row.table, row.row.primary_key);
   });
}

secondary_overlay_cursor::secondary_overlay_cursor(primary_state_overlay& state, name receiver,
      secondary_key_kind kind, size_t max_iterators, size_t max_ends)
   : state_(state), receiver_(receiver), kind_(kind), max_iterators_(max_iterators), max_ends_(max_ends) {
   checked([&] {
      switch (kind_) {
         case secondary_key_kind::uint64: zero_ = uint64_t{0}; break;
         case secondary_key_kind::uint128: zero_ = uint128_t{0}; break;
         case secondary_key_kind::uint256: zero_ = std::array<uint128_t, 2>{}; break;
         case secondary_key_kind::float64: zero_ = secondary_float64{}; break;
         case secondary_key_kind::float128: zero_ = secondary_float128{}; break;
         default: throw secondary_state_invalid_query();
      }
   });
}

void secondary_overlay_cursor::require_kind(const secondary_key& key) const {
   if (secondary_kind(key) != kind_) throw secondary_state_invalid_query();
}

void secondary_overlay_cursor::set_receiver_for_notification(name receiver) {
   checked([&] { receiver_ = receiver; });
}

secondary_overlay_cursor::row_handle secondary_overlay_cursor::get_handle(int iterator) const {
   EOS_ASSERT(iterator != -1, invalid_table_iterator, "invalid iterator");
   EOS_ASSERT(iterator >= 0, table_operation_not_permitted, "dereference of end iterator");
   EOS_ASSERT(static_cast<size_t>(iterator) < rows_.size(), invalid_table_iterator, "iterator out of range");
   auto resolved = rows_[iterator];
   if (const auto* write = state_.latest_secondary_write(resolved.table, kind_, resolved.row.primary_key)) {
      if (write->operation == primary_operation::remove || write->object_id != resolved.row.object_id ||
          write->table_object_id != resolved.row.table_object_id) throw primary_state_unsupported();
      // Unlike the immutable primary key, this key can MOVE. A live iterator
      // follows its object, so adjacency must use the newest private key.
      resolved.row = metadata(*write);
   }
   return resolved;
}

int secondary_overlay_cursor::cache_table(const table_read& table) {
   const auto existing = table_handles_.find(table.object_id);
   if (existing != table_handles_.end()) return existing->second;
   if (tables_.size() >= max_ends_ || tables_.size() > static_cast<size_t>(std::numeric_limits<int>::max() - 2))
      throw primary_state_budget_exceeded();
   const int result = -2 - static_cast<int>(tables_.size());
   tables_.push_back(table); table_handles_.emplace(table.object_id, result);
   return result;
}

int secondary_overlay_cursor::cache_row(const table_key& table, const secondary_row_read& row) {
   require_kind(row.key);
   const auto existing = row_handles_.find(row.object_id);
   if (existing != row_handles_.end()) return existing->second;
   if (rows_.size() >= max_iterators_ || rows_.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
      throw primary_state_budget_exceeded();
   const int result = static_cast<int>(rows_.size());
   rows_.push_back({table, row}); row_handles_.emplace(row.object_id, result);
   return result;
}

int secondary_overlay_cursor::cache_result(const table_key& table, const secondary_read_result& found) {
   if (!found.table) return -1;
   const auto end = cache_table(*found.table);
   return found.row ? cache_row(table, *found.row) : end;
}

int secondary_overlay_cursor::find_primary(const table_key& table, secondary_key& key, uint64_t primary) {
   return checked([&] {
      require_kind(key); // output bits (including a NaN sentinel) are not an input key
      const auto found = state_.read_secondary({table, secondary_query_kind::find_primary, zero_, primary});
      if (found.row) key = found.row->key;
      return cache_result(table, found);
   });
}

int secondary_overlay_cursor::find_secondary(const table_key& table, const secondary_key& key, uint64_t& primary) {
   return checked([&] {
      require_kind(key);
      const auto found = state_.read_secondary({table, secondary_query_kind::find_secondary, key, 0});
      if (found.row) primary = found.row->primary_key;
      return cache_result(table, found);
   });
}

int secondary_overlay_cursor::bound(const table_key& table, secondary_key& key, uint64_t& primary,
                                     secondary_query_kind kind) {
   return checked([&] {
      require_kind(key);
      const auto found = state_.read_secondary({table, kind, key, 0});
      if (found.row) { primary = found.row->primary_key; key = found.row->key; }
      return cache_result(table, found);
   });
}
int secondary_overlay_cursor::lower_bound(const table_key& table, secondary_key& key, uint64_t& primary) {
   return bound(table, key, primary, secondary_query_kind::lower_bound);
}
int secondary_overlay_cursor::upper_bound(const table_key& table, secondary_key& key, uint64_t& primary) {
   return bound(table, key, primary, secondary_query_kind::upper_bound);
}
int secondary_overlay_cursor::end(const table_key& table) {
   return checked([&] {
      state_.secondary_accessed_ = true;
      const auto found = state_.read_table(table);
      return found ? cache_table(*found) : -1;
   });
}

int secondary_overlay_cursor::next(int iterator, uint64_t& primary) {
   return checked([&] {
      state_.secondary_accessed_ = true;
      if (iterator < -1) return -1; // original also ignores unknown negative end handles
      const auto row = get_handle(iterator);
      const auto found = state_.read_secondary({row.table, secondary_query_kind::next, row.row.key, row.row.primary_key});
      if (!found.row) return table_handles_.at(row.row.table_object_id);
      primary = found.row->primary_key;
      return cache_row(row.table, *found.row);
   });
}

int secondary_overlay_cursor::previous(int iterator, uint64_t& primary) {
   return checked([&] {
      secondary_query query;
      if (iterator < -1) {
         const auto index = static_cast<uint64_t>(-static_cast<int64_t>(iterator) - 2);
         EOS_ASSERT(index < tables_.size(), invalid_table_iterator, "not a valid end iterator");
         const auto current = state_.read_table(tables_[index].key);
         if (!current || current->object_id != tables_[index].object_id) throw primary_state_unsupported();
         query = {tables_[index].key, secondary_query_kind::last, zero_, 0};
      } else {
         const auto row = get_handle(iterator);
         query = {row.table, secondary_query_kind::previous, row.row.key, row.row.primary_key};
      }
      const auto found = state_.read_secondary(query);
      if (!found.row) return -1;
      primary = found.row->primary_key;
      return cache_row(query.table, *found.row);
   });
}

int secondary_overlay_cursor::store(const table_key& table, uint64_t primary, name payer, const secondary_key& key) {
   return checked([&] {
      require_kind(key);
      EOS_ASSERT(table.code == receiver_, table_access_violation, "db access violation");
      state_.store_secondary(table, primary, payer, key);
      return cache_result(table, state_.read_secondary({table, secondary_query_kind::find_primary, zero_, primary}));
   });
}
void secondary_overlay_cursor::update(int iterator, name payer, const secondary_key& key) {
   checked([&] {
      require_kind(key);
      const auto row = get_handle(iterator);
      EOS_ASSERT(row.table.code == receiver_, table_access_violation, "db access violation");
      state_.update_secondary(row.table, row.row.primary_key, payer, key);
   });
}
void secondary_overlay_cursor::remove(int iterator) {
   checked([&] {
      const auto row = get_handle(iterator);
      EOS_ASSERT(row.table.code == receiver_, table_access_violation, "db access violation");
      state_.remove_secondary(row.table, row.row.primary_key, zero_);
   });
}

} // namespace eosio::chain::experimental
