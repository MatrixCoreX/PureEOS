#include <eosio/chain/parallel_state_overlay.hpp>
#include <eosio/chain/exceptions.hpp>
#include <algorithm>
#include <limits>

namespace eosio::chain::experimental {
namespace {
primary_row_metadata metadata(const primary_row_read& row) {
   return {row.object_id, row.table_object_id, row.primary_key, row.payer, row.value.size()};
}
primary_row_metadata metadata(const primary_mutation& row) {
   return {row.object_id, row.table_object_id, row.primary_key, row.payer, row.value.size()};
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
      auto& table = result->tables_[key];
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
         table.rows.emplace(read.row->primary_key, std::move(*read.row));
         // Strict upper bound; no overflow at UINT64_MAX.
         query = {key, primary_query_kind::upper_bound, found.row->primary_key};
      }
   }
   return result;
}

const frozen_primary_state::table_snapshot& frozen_primary_state::get_table(const table_key& key) const {
   const auto it = tables_.find(key);
   if (it == tables_.end()) throw primary_state_missing(key);
   return it->second;
}

std::shared_ptr<const frozen_primary_state> frozen_primary_state::capture_queries(
      const chainbase_transaction_state_view& source, std::span<const table_key> keys,
      std::span<const primary_query> queries, primary_snapshot_limits limits, const std::function<void()>& checkpoint,
      std::span<const name> accounts) {
   if (keys.size() > limits.max_tables || queries.size() > limits.max_queries || accounts.size() > limits.max_accounts)
      throw primary_state_budget_exceeded();
   auto result = std::shared_ptr<frozen_primary_state>(new frozen_primary_state);
   for (const auto account : accounts) {
      if (checkpoint) checkpoint();
      if (!result->accounts_.contains(account)) result->accounts_.emplace(account, source.read_account(account));
   }
   auto capture_table = [&](const table_key& key) -> table_snapshot& {
      if (checkpoint) checkpoint();
      if (auto it = result->tables_.find(key); it != result->tables_.end()) return it->second;
      if (result->tables_.size() == limits.max_tables) throw primary_state_budget_exceeded();
      auto& table = result->tables_[key];
      table.complete = false;
      table.metadata = source.read_table(key);
      return table;
   };
   for (const auto& key : keys) capture_table(key);
   for (auto query : queries) {
      require_query(query.kind);
      if (query.kind == primary_query_kind::last) query.primary_key = 0;
      auto& table = capture_table(query.table);
      if (table.queries.contains(query)) continue;
      const auto found = source.locate_primary(query);
      if (found.table != table.metadata) throw primary_state_invalid_attempt();
      std::optional<uint64_t> selected;
      if (found.row) {
         selected = found.row->primary_key;
         if (auto existing = table.rows.find(*selected); existing != table.rows.end()) {
            if (metadata(existing->second) != *found.row) throw primary_state_invalid_attempt();
         } else {
            if (result->row_count_ == limits.max_rows ||
                found.row->value_size > limits.max_value_bytes - result->value_bytes_)
               throw primary_state_budget_exceeded();
            auto read = source.read_primary(query, limits.max_value_bytes - result->value_bytes_);
            if (!read.row || read.table != found.table || metadata(*read.row) != *found.row)
               throw primary_state_invalid_attempt();
            result->value_bytes_ += read.row->value.size();
            ++result->row_count_;
            table.rows.emplace(*selected, std::move(*read.row));
         }
      }
      table.queries.emplace(query, selected);
   }
   return result;
}

std::pair<const frozen_primary_state::table_snapshot*, const primary_row_read*>
frozen_primary_state::locate(const primary_query& query) const {
   require_query(query.kind);
   const auto captured = tables_.find(query.table);
   if (captured == tables_.end()) throw primary_state_missing(query);
   const auto& table = captured->second;
   if (!table.complete) {
      // An absent TABLE proves all its primary queries absent. Otherwise the
      // exact query must have been captured; sparse rows cannot prove ranges.
      if (!table.metadata) return {&table, nullptr};
      auto normalized = query;
      if (normalized.kind == primary_query_kind::last) normalized.primary_key = 0;
      const auto answer = table.queries.find(normalized);
      if (answer == table.queries.end()) throw primary_state_missing(normalized);
      return {&table, answer->second ? &table.rows.at(*answer->second) : nullptr};
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
   return {&table, it == rows.end() ? nullptr : &it->second};
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
      if (queries_.size() == limits_.max_read_queries) throw primary_state_budget_exceeded();
      queries_.insert(query);
   }
}

const primary_mutation* primary_state_overlay::latest_write(const table_key& table, uint64_t key) const {
   const auto t = latest_writes_.find(table);
   if (t == latest_writes_.end()) return nullptr;
   const auto row = t->second.find(key);
   return row == t->second.end() ? nullptr : &writes_[row->second];
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
         const auto& write = writes_[it->second];
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
      append({table, key, payer == name{} ? found.row->payer : payer, std::vector<char>(value.begin(), value.end()),
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
   const auto& saved = writes_.back();
   latest_writes_[saved.table][saved.primary_key] = writes_.size() - 1;
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
      append({key, primary, payer, std::vector<char>(value.begin(), value.end()), primary_operation::store,
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
      append({key, primary, found.row->payer, {}, primary_operation::remove,
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
   return {base_, std::move(reads_), std::move(queries_), std::move(writes_), std::move(accounts_)};
}

bool primary_overlay_result::matches_base_reads(const transaction_state_view& live,
                                               const std::function<void()>& checkpoint) const {
   std::set<int64_t> checked_values;
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
      if (live.locate_primary(query) != expected) return false;
      // Distinct range/point queries can select the same large row. Selection
      // is checked for each query, but compare its payload once per object ID
      // while the serial owner holds this unchanged canonical state.
      if (expected.row && checked_values.insert(expected.row->object_id).second &&
          live.read_primary(query, expected.row->value_size) !=
                          base_->read_primary(query, expected.row->value_size)) return false;
   }
   return true;
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

} // namespace eosio::chain::experimental
