#pragma once

#include <eosio/chain/transaction_state_view.hpp>
#include <eosio/chain/types.hpp>
#include <array>
#include <variant>

namespace eosio::chain::experimental {

// Owned exact bits, not native floating point. Equality deliberately distinguishes
// +0/-0; ordering uses the same SoftFloat comparators as the canonical indices.
struct secondary_float64 {
   uint64_t bits = 0;
   bool operator==(const secondary_float64&) const = default;
};
struct secondary_float128 {
   uint128_t bits = 0;
   bool operator==(const secondary_float128&) const = default;
};
using secondary_key = std::variant<uint64_t, uint128_t, std::array<uint128_t, 2>,
                                   secondary_float64, secondary_float128>;
enum class secondary_key_kind : uint8_t { uint64, uint128, uint256, float64, float128 };

struct secondary_state_invalid_query : std::logic_error {
   secondary_state_invalid_query() : std::logic_error("unsupported secondary state query or key") {}
};
secondary_key_kind secondary_kind(const secondary_key&);
bool secondary_key_is_nan(const secondary_key&);
// Reject unlike types and NaN. Equality of exact keys above is NOT this ordering.
bool secondary_key_less(const secondary_key&, const secondary_key&);
bool secondary_key_equivalent(const secondary_key&, const secondary_key&);
size_t secondary_key_bytes(const secondary_key&);

enum class secondary_query_kind : uint8_t {
   find_primary, find_secondary, lower_bound, upper_bound, first, last, next, previous
};
struct secondary_query {
   table_key table;
   secondary_query_kind kind = secondary_query_kind::find_primary;
   secondary_key key = uint64_t{0}; // also identifies the index; ignored by primary/first/last
   uint64_t primary_key = 0;        // used by find_primary and full-tuple adjacency only
   bool operator==(const secondary_query&) const = default;
};
// Canonicalize only ignored input fields. Exact float bits are retained for
// value-taking queries, even when two keys have equivalent database ordering.
secondary_query normalize_secondary_query(secondary_query);
// Dictionary order for exact query identities, NOT database secondary order.
// A captured query must be normalized before insertion or lookup.
struct secondary_query_less {
   bool operator()(const secondary_query&, const secondary_query&) const;
};
struct secondary_row_read {
   int64_t object_id = 0;
   int64_t table_object_id = 0;
   uint64_t primary_key = 0;
   name payer;
   secondary_key key = uint64_t{0}; // type is part of the object's identity
   bool operator==(const secondary_row_read&) const = default;
};
struct secondary_read_result {
   std::optional<table_read> table;
   std::optional<secondary_row_read> row;
   bool operator==(const secondary_read_result&) const = default;
};

// Live OWNER-ONLY foundation, not a worker capability or permission to adopt.
// The database must outlive this view, register all required indices and not be
// concurrently mutated. The table metadata is exactly the primary view's shared
// metadata, not an independently counted "secondary table".
// next/previous are predicate reads relative to a complete (secondary,primary)
// tuple. A future iterator consumer MUST additionally prove its anchor object's
// identity/generation; this API alone cannot validate an iterator.
class chainbase_secondary_state_view {
public:
   explicit chainbase_secondary_state_view(const chainbase::database&);
   // Preserve the existing owner's thread and database identity; do not allow
   // pairing primary and secondary reads from unrelated live databases.
   explicit chainbase_secondary_state_view(const chainbase_transaction_state_view&);
   chainbase_secondary_state_view(const chainbase_secondary_state_view&) = delete;
   chainbase_secondary_state_view& operator=(const chainbase_secondary_state_view&) = delete;
   chainbase_secondary_state_view(chainbase_secondary_state_view&&) = delete;
   chainbase_secondary_state_view& operator=(chainbase_secondary_state_view&&) = delete;
   secondary_read_result read_secondary(const secondary_query&) const;
private:
   const chainbase::database& db_;
   chainbase_transaction_state_view primary_;
   const std::thread::id owner_;
};

} // namespace eosio::chain::experimental
