#include <eosio/chain/parallel_secondary_journal.hpp>
#include <type_traits>

namespace eosio::chain::experimental {

size_t isolated_state_write_bytes(const state_mutation& mutation) {
   if (const auto* primary = std::get_if<primary_mutation>(&mutation)) return primary->value.size();
   return secondary_key_bytes(std::get<secondary_mutation>(mutation).key);
}

bool validate_isolated_replay_state(const primary_overlay_result& actual,
      const primary_overlay_result& expected, const std::function<void()>& checkpoint) {
   const auto check = [&] { if (checkpoint) checkpoint(); };
   check();
   if (actual.base() != expected.base() || actual.primary_only() != expected.primary_only() ||
       actual.writes().size() != expected.writes().size() ||
       actual.secondary_read_queries().size() != expected.secondary_read_queries().size()) return false;
   for (size_t i = 0; i < actual.writes().size(); ++i) {
      check(); if (actual.writes()[i] != expected.writes()[i]) return false;
   }
   for (const auto& table : actual.read_tables()) {
      check(); if (!expected.read_tables().contains(table)) return false;
   }
   for (const auto& query : actual.read_queries()) {
      check(); if (!expected.read_queries().contains(query)) return false;
   }
   for (const auto& query : actual.secondary_read_queries()) {
      check(); if (!expected.secondary_read_queries().contains(query)) return false;
   }
   for (const auto& account : actual.read_accounts()) {
      check(); if (!expected.read_accounts().contains(account)) return false;
   }
   check();
   return true;
}

secondary_cursor_group::secondary_cursor_group(primary_state_overlay& state, name receiver,
      size_t max_iterators, size_t max_ends)
   : state_(state), receiver_(receiver),
     cursors_{{{state, receiver, secondary_key_kind::uint64, max_iterators, max_ends},
               {state, receiver, secondary_key_kind::uint128, max_iterators, max_ends},
               {state, receiver, secondary_key_kind::uint256, max_iterators, max_ends},
               {state, receiver, secondary_key_kind::float64, max_iterators, max_ends},
               {state, receiver, secondary_key_kind::float128, max_iterators, max_ends}}} {}

secondary_overlay_cursor& secondary_cursor_group::get(secondary_key_kind kind) {
   const auto index = static_cast<size_t>(kind);
   if (index >= cursors_.size()) throw secondary_state_invalid_query();
   return cursors_[index];
}

void secondary_cursor_group::set_receiver_for_notification(name receiver) {
   for (auto& cursor : cursors_) cursor.set_receiver_for_notification(receiver);
   receiver_ = receiver;
}

bool replay_isolated_secondary_cursor_event(secondary_cursor_group& group,
      const isolated_secondary_cursor_event& event, const primary_overlay_result& expected,
      size_t& next_write, size_t writes_end, bool read_only) {
   try {
      if (next_write > writes_end || writes_end > expected.writes().size() ||
          group.write_count() != next_write) return false;
      return std::visit([&](const auto& item) -> bool {
         using T = std::decay_t<decltype(item)>;
         if constexpr (std::is_same_v<T, isolated_secondary_find_primary>) {
            auto key = item.key_before;
            const auto result = group.get(secondary_kind(key)).find_primary(item.table, key, item.primary);
            return result == item.result && key == item.key_after;
         } else if constexpr (std::is_same_v<T, isolated_secondary_find_secondary>) {
            auto primary = item.primary_before;
            const auto result = group.get(secondary_kind(item.key)).find_secondary(item.table, item.key, primary);
            return result == item.result && primary == item.primary_after;
         } else if constexpr (std::is_same_v<T, isolated_secondary_bound>) {
            auto key = item.key_before;
            auto primary = item.primary_before;
            auto& cursor = group.get(secondary_kind(key));
            const auto result = item.upper ? cursor.upper_bound(item.table, key, primary)
                                           : cursor.lower_bound(item.table, key, primary);
            return result == item.result && key == item.key_after && primary == item.primary_after;
         } else if constexpr (std::is_same_v<T, isolated_secondary_end>) {
            return group.get(item.kind).end(item.table) == item.result;
         } else if constexpr (std::is_same_v<T, isolated_secondary_adjacent>) {
            auto primary = item.primary_before;
            auto& cursor = group.get(item.kind);
            const auto result = item.previous ? cursor.previous(item.iterator, primary)
                                              : cursor.next(item.iterator, primary);
            return result == item.result && primary == item.primary_after;
         } else {
            static_assert(std::is_same_v<T, isolated_secondary_write>);
            if (read_only || item.write_index != next_write || next_write >= writes_end) return false;
            const auto* write = std::get_if<secondary_mutation>(&expected.writes()[next_write]);
            if (!write || secondary_kind(write->key) != item.kind || write->table.code != group.receiver()) return false;
            auto& cursor = group.get(item.kind);
            switch (write->operation) {
               case primary_operation::store:
                  if (cursor.store(write->table, write->primary_key, write->payer, write->key) != item.iterator) return false;
                  break;
               case primary_operation::update: cursor.update(item.iterator, write->payer, write->key); break;
               case primary_operation::remove: cursor.remove(item.iterator); break;
               default: return false;
            }
            if (group.write_count() != next_write + 1) return false;
            ++next_write;
            return true;
         }
      }, event);
   } catch (const std::exception&) { return false; }
}

} // namespace eosio::chain::experimental
