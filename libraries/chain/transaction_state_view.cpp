#include <eosio/chain/transaction_state_view.hpp>
#include <eosio/chain/contract_table_objects.hpp>

namespace eosio::chain::experimental {
namespace {

table_read copy_table(const table_id_object& table) {
   return {{table.code, table.scope, table.table}, table.id._id, table.payer, table.count};
}

primary_row_read copy_row(const key_value_object& row, size_t max_value_bytes) {
   if (row.value.size() > max_value_bytes)
      throw state_view_read_limit_exceeded();
   primary_row_read result{row.id._id, row.t_id._id, row.primary_key, row.payer, {}};
   // Avoid null-pointer arithmetic for an empty shared_blob.
   if (row.value.size() != 0)
      result.value.assign(row.value.data(), row.value.data() + row.value.size());
   return result;
}

void require_supported_query(primary_query_kind kind) {
   switch (kind) {
      case primary_query_kind::find:
      case primary_query_kind::lower_bound:
      case primary_query_kind::upper_bound:
      case primary_query_kind::last:
         return;
   }
   throw state_view_unsupported_query();
}

} // namespace

chainbase_transaction_state_view::chainbase_transaction_state_view(const chainbase::database& db)
   : db_(db), owner_(std::this_thread::get_id()) {}

void chainbase_transaction_state_view::require_owner_thread() const {
   if (std::this_thread::get_id() != owner_)
      throw state_view_wrong_thread();
}

std::optional<table_read> chainbase_transaction_state_view::read_table(const table_key& key) const {
   require_owner_thread();
   const auto* table = db_.find<table_id_object, by_code_scope_table>(boost::make_tuple(key.code, key.scope, key.table));
   if (!table)
      return std::nullopt;
   return copy_table(*table);
}

primary_read_result chainbase_transaction_state_view::read_primary(const primary_query& query,
                                                                  size_t max_value_bytes) const {
   require_owner_thread();
   require_supported_query(query.kind); // also reject unsupported queries on missing tables
   const auto& key = query.table;
   const auto* table = db_.find<table_id_object, by_code_scope_table>(boost::make_tuple(key.code, key.scope, key.table));
   if (!table)
      return {};

   primary_read_result result{copy_table(*table), std::nullopt};
   const key_value_object* row = nullptr;
   switch (query.kind) {
      case primary_query_kind::find:
         row = db_.find<key_value_object, by_scope_primary>(boost::make_tuple(table->id, query.primary_key));
         break;
      case primary_query_kind::lower_bound:
      case primary_query_kind::upper_bound: {
         const auto& index = db_.get_index<key_value_index, by_scope_primary>();
         const auto seek = boost::make_tuple(table->id, query.primary_key);
         const auto it = query.kind == primary_query_kind::lower_bound ? index.lower_bound(seek) : index.upper_bound(seek);
         if (it != index.end() && it->t_id == table->id)
            row = &*it;
         break;
      }
      case primary_query_kind::last: {
         const auto& index = db_.get_index<key_value_index, by_scope_primary>();
         auto it = index.upper_bound(boost::make_tuple(table->id));
         if (it != index.begin()) {
            --it;
            if (it->t_id == table->id)
               row = &*it;
         }
         break;
      }
   }
   if (row)
      result.row = copy_row(*row, max_value_bytes);
   return result;
}

} // namespace eosio::chain::experimental
