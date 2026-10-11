#include <eosio/chain/parallel_state_overlay.hpp>
#include <algorithm>

namespace eosio::chain::experimental {
namespace {
primary_row_metadata metadata(const primary_row_read& row) {
   return {row.object_id, row.table_object_id, row.primary_key, row.payer, row.value.size()};
}
void require_query(primary_query_kind kind) {
   switch (kind) {
      case primary_query_kind::find:
      case primary_query_kind::lower_bound:
      case primary_query_kind::upper_bound:
      case primary_query_kind::last: return;
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
   if (it == tables_.end()) throw primary_state_unsupported();
   return it->second;
}

std::pair<const frozen_primary_state::table_snapshot*, const primary_row_read*>
frozen_primary_state::locate(const primary_query& query) const {
   require_query(query.kind);
   const auto& table = get_table(query.table);
   const auto& rows = table.rows;
   auto it = rows.end();
   switch (query.kind) {
      case primary_query_kind::find: it = rows.find(query.primary_key); break;
      case primary_query_kind::lower_bound: it = rows.lower_bound(query.primary_key); break;
      case primary_query_kind::upper_bound: it = rows.upper_bound(query.primary_key); break;
      case primary_query_kind::last: if (!rows.empty()) it = std::prev(rows.end()); break;
   }
   return {&table, it == rows.end() ? nullptr : &it->second};
}

std::optional<table_read> frozen_primary_state::read_table(const table_key& key) const {
   return get_table(key).metadata;
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
   // Conservatively track the entire captured table, including absent tables,
   // absent rows and empty/bounded range results. Never erase earlier reads.
   if (!reads_.contains(key)) {
      if (reads_.size() == limits_.max_read_tables) throw primary_state_budget_exceeded();
      reads_.insert(key);
   }
}

const std::vector<char>* primary_state_overlay::written_value(const table_key& table, uint64_t key) const {
   const auto t = latest_writes_.find(table);
   if (t == latest_writes_.end()) return nullptr;
   const auto row = t->second.find(key);
   return row == t->second.end() ? nullptr : &writes_[row->second].value;
}

std::optional<table_read> primary_state_overlay::read_table(const table_key& key) const {
   check_owner_and_valid();
   try {
      depend_on(key);
      return base_->read_table(key);
   } catch (...) { valid_ = false; throw; }
}

primary_metadata_result primary_state_overlay::locate_primary(const primary_query& query) const {
   check_owner_and_valid();
   try {
      depend_on(query.table);
      // Supported updates never change row identity, size, payer or ordering.
      return base_->locate_primary(query);
   } catch (...) { valid_ = false; throw; }
}

primary_prefix_result primary_state_overlay::read_primary_prefix(const primary_query& query, size_t max_bytes) const {
   check_owner_and_valid();
   try {
      auto found = locate_primary(query);
      if (found.row) {
         if (const auto* value = written_value(query.table, found.row->primary_key)) {
            primary_prefix_result result{std::move(found), {}};
            result.value.assign(value->begin(), value->begin() + std::min(max_bytes, value->size()));
            return result;
         }
      }
      return base_->read_primary_prefix(query, max_bytes);
   } catch (...) { valid_ = false; throw; }
}

primary_read_result primary_state_overlay::read_primary(const primary_query& query, size_t max_value_bytes) const {
   check_owner_and_valid();
   try {
      auto found = locate_primary(query);
      if (!found.row) return {found.table, std::nullopt};
      if (found.row->value_size > max_value_bytes) throw state_view_read_limit_exceeded();
      if (const auto* value = written_value(query.table, found.row->primary_key)) {
         const auto& row = *found.row;
         return {found.table, primary_row_read{row.object_id, row.table_object_id, row.primary_key, row.payer, *value}};
      }
      return base_->read_primary(query, max_value_bytes);
   } catch (...) { valid_ = false; throw; }
}

void primary_state_overlay::update_existing(const table_key& table, uint64_t key, name payer, std::span<const char> value) {
   check_owner_and_valid();
   try {
      const auto found = locate_primary({table, primary_query_kind::find, key});
      if (!found.row || (payer != name{} && payer != found.row->payer) || value.size() != found.row->value_size)
         throw primary_state_unsupported();
      if (writes_.size() == limits_.max_write_operations || value.size() > limits_.max_write_value_bytes - written_bytes_)
         throw primary_state_budget_exceeded();
      writes_.push_back({table, key, found.row->payer, std::vector<char>(value.begin(), value.end())});
      latest_writes_[table][key] = writes_.size() - 1;
      written_bytes_ += value.size();
   } catch (...) { valid_ = false; throw; }
}

primary_overlay_result primary_state_overlay::seal() {
   check_owner_and_valid();
   valid_ = false;
   return {base_, std::move(reads_), std::move(writes_)};
}

} // namespace eosio::chain::experimental
