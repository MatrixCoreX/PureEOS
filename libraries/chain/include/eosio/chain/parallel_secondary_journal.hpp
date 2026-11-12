#pragma once

#include <eosio/chain/parallel_state_overlay.hpp>
#include <array>
#include <variant>

namespace eosio::chain::experimental {

// One bundle per canonical cursor lifetime: shared by notification receivers,
// but independent for each root, inline group and synchronous child frame.
// Merely constructing an unused bundle must not request secondary capability.
class secondary_cursor_group {
public:
   secondary_cursor_group(primary_state_overlay&, name receiver,
                          size_t max_iterators = 4096, size_t max_end_iterators = 64);
   secondary_cursor_group(const secondary_cursor_group&) = delete;
   secondary_cursor_group& operator=(const secondary_cursor_group&) = delete;
   secondary_overlay_cursor& get(secondary_key_kind);
   void set_receiver_for_notification(name);
   name receiver() const { return receiver_; }
   size_t write_count() const { return state_.write_count(); }
private:
   primary_state_overlay& state_;
   name receiver_;
   std::array<secondary_overlay_cursor, 5> cursors_;
};

struct isolated_secondary_find_primary {
   table_key table{};
   uint64_t primary = 0;
   secondary_key key_before = uint64_t{0}, key_after = uint64_t{0};
   int32_t result = -1;
   bool operator==(const isolated_secondary_find_primary&) const = default;
};
struct isolated_secondary_find_secondary {
   table_key table{};
   secondary_key key = uint64_t{0};
   uint64_t primary_before = 0, primary_after = 0;
   int32_t result = -1;
   bool operator==(const isolated_secondary_find_secondary&) const = default;
};
struct isolated_secondary_bound {
   table_key table{};
   bool upper = false;
   secondary_key key_before = uint64_t{0}, key_after = uint64_t{0};
   uint64_t primary_before = 0, primary_after = 0;
   int32_t result = -1;
   bool operator==(const isolated_secondary_bound&) const = default;
};
struct isolated_secondary_end {
   table_key table{};
   secondary_key_kind kind = secondary_key_kind::uint64;
   int32_t result = -1;
   bool operator==(const isolated_secondary_end&) const = default;
};
struct isolated_secondary_adjacent {
   secondary_key_kind kind = secondary_key_kind::uint64;
   bool previous = false;
   int32_t iterator = -1, result = -1;
   uint64_t primary_before = 0, primary_after = 0;
   bool operator==(const isolated_secondary_adjacent&) const = default;
};
struct isolated_secondary_write {
   secondary_key_kind kind = secondary_key_kind::uint64;
   size_t write_index = 0; // absolute PRIMARY + SECONDARY transaction chronology
   int32_t iterator = -1; // store result; update/remove input
   bool operator==(const isolated_secondary_write&) const = default;
};
using isolated_secondary_cursor_event = std::variant<
   isolated_secondary_find_primary, isolated_secondary_find_secondary,
   isolated_secondary_bound, isolated_secondary_end, isolated_secondary_adjacent,
   isolated_secondary_write>;

// PRIVATE replay helper, not a proof, authorization or canonical write API.
// The caller must construct its shared replay overlay from expected.base(),
// interleave all primary/secondary/call events, and compare the final COMPLETE
// typed write log, dependencies and capability bits. In particular, matching an
// update/remove handle here does not itself prove the expected row/table/IDs.
// false may follow private effects: discard the WHOLE replay, never its tail.
// Owner cancellation checkpoints belong OUTSIDE this local-error catch.
bool replay_isolated_secondary_cursor_event(
   secondary_cursor_group&, const isolated_secondary_cursor_event&,
   const primary_overlay_result& expected, size_t& next_write,
   size_t writes_end, bool read_only);

// Same byte accounting as the shared overlay, including a removed index key.
size_t isolated_state_write_bytes(const state_mutation&);

// Complete typed effects and the dependencies reconstructed from cursor events.
// db_get/is_account do not create primary cursor events, so their additional
// ORIGINAL dependencies remain allowed and must still be revalidated live.
// Every secondary host is transcribed: its dependency set must match exactly.
// This is private structural proof only, not authorization/canonical adoption.
bool validate_isolated_replay_state(const primary_overlay_result& reconstructed,
   const primary_overlay_result& expected, const std::function<void()>& checkpoint = {});

} // namespace eosio::chain::experimental
