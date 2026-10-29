#pragma once

#include <eosio/chain/name.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace chainbase { class database; }

namespace eosio::chain::experimental {

// Owned values for the restricted validation experiment. The original host
// remains the canonical writer. This boundary is not a general MVCC database.
struct table_key {
   name code;
   name scope;
   name table;
   bool operator==(const table_key&) const = default;
};

struct table_read {
   table_key key;
   int64_t object_id = 0;
   name payer;
   uint32_t count = 0;
   bool operator==(const table_read&) const = default;
};

// Account identity only. Receipt/auth sequences, ABI and executable capability
// are NOT dependencies of is_account; notification capability is checked by
// the canonical owner separately, before any mutation.
struct account_read {
   name account;
   int64_t object_id = 0;
   bool operator==(const account_read&) const = default;
};

struct primary_row_read {
   int64_t object_id = 0;
   int64_t table_object_id = 0;
   uint64_t primary_key = 0;
   name payer;
   std::vector<char> value; // never shared_blob or a pointer into Chainbase
   bool operator==(const primary_row_read&) const = default;
};

// Locating a row or querying its size must not allocate/copy its payload.
struct primary_row_metadata {
   int64_t object_id = 0;
   int64_t table_object_id = 0;
   uint64_t primary_key = 0;
   name payer;
   size_t value_size = 0;
   bool operator==(const primary_row_metadata&) const = default;
};

enum class primary_query_kind : uint8_t { find, lower_bound, upper_bound, last, previous };

struct primary_query {
   table_key table;
   primary_query_kind kind = primary_query_kind::find;
   uint64_t primary_key = 0; // ignored for last (no arithmetic on UINT64_MAX)
};

struct primary_read_result {
   // Missing table and existing table with no matching row are different reads.
   std::optional<table_read> table;
   std::optional<primary_row_read> row;
   bool operator==(const primary_read_result&) const = default;
};

struct primary_metadata_result {
   std::optional<table_read> table;
   std::optional<primary_row_metadata> row;
   bool operator==(const primary_metadata_result&) const = default;
};

struct primary_prefix_result {
   primary_metadata_result metadata;
   std::vector<char> value; // at most the requested prefix; metadata retains full size
   bool operator==(const primary_prefix_result&) const = default;
};

// Local experimental limits/misuse, NOT consensus transaction exceptions.
// A future speculative caller must fall back rather than reject a block on a
// local read budget failure.
struct state_view_read_limit_exceeded : std::runtime_error {
   state_view_read_limit_exceeded() : std::runtime_error("state view row exceeds local copy budget") {}
};
struct state_view_wrong_thread : std::logic_error {
   state_view_wrong_thread() : std::logic_error("serial state view used from a different thread") {}
};
struct state_view_unsupported_query : std::logic_error {
   state_view_unsupported_query() : std::logic_error("unsupported primary state query") {}
};

struct secondary_query;
struct secondary_read_result;

class transaction_state_view {
public:
   virtual ~transaction_state_view() = default;
   virtual std::optional<account_read> read_account(name) const = 0;
   virtual std::optional<table_read> read_table(const table_key&) const = 0;
   virtual primary_metadata_result locate_primary(const primary_query&) const = 0;
   virtual primary_prefix_result read_primary_prefix(const primary_query&, size_t max_bytes) const = 0;
   virtual primary_read_result read_primary(const primary_query&, size_t max_value_bytes) const = 0;
   // Old/read-limited adapters must explicitly decline this capability. An
   // unsupported secondary query is NEVER evidence that no row exists.
   virtual secondary_read_result read_secondary(const secondary_query&) const;
};

// Live, serial adapter. The database must outlive the view, have the table and
// primary indices registered, and MUST NOT be concurrently mutated. Thread
// affinity rejects accidental dispatch to workers; it does not lock the DB or
// stop other owners mutating it. A future frozen-base adapter needs its own
// lifetime/epoch proof. Each call sees current state, including undo changes.
class chainbase_transaction_state_view final : public transaction_state_view {
public:
   explicit chainbase_transaction_state_view(const chainbase::database&);
   chainbase_transaction_state_view(const chainbase_transaction_state_view&) = delete;
   chainbase_transaction_state_view& operator=(const chainbase_transaction_state_view&) = delete;
   chainbase_transaction_state_view(chainbase_transaction_state_view&&) = delete;
   chainbase_transaction_state_view& operator=(chainbase_transaction_state_view&&) = delete;

   std::optional<account_read> read_account(name) const override;
   std::optional<table_read> read_table(const table_key&) const override;
   primary_metadata_result locate_primary(const primary_query&) const override;
   primary_prefix_result read_primary_prefix(const primary_query&, size_t max_bytes) const override;
   primary_read_result read_primary(const primary_query&, size_t max_value_bytes) const override;
   // Synchronous complete-byte comparison against an immutable owned snapshot.
   // Never caches canonical pointers or retains the borrowed expected bytes.
   bool matches_primary(const primary_query&,const primary_metadata_result&,
                        std::span<const char> expected_value) const;
   secondary_read_result read_secondary(const secondary_query&) const override;

private:
   friend class frozen_primary_state;
   friend class frozen_primary_capture_session; // source/owner identity only
   // Restricted to the immutable snapshot builder. All supplied rows are owned
   // immutable values. No callback, live pointer or iterator crosses this call.
   struct capture_primary_result {
      primary_metadata_result metadata;
      std::optional<primary_row_read> owned_row;
      bool changed = false;
      bool budget_exceeded = false;
   };
   capture_primary_result read_primary_for_capture(const primary_query&,
      const std::optional<table_read>& expected_table,
      const std::map<uint64_t,std::shared_ptr<const primary_row_read>>& existing_rows,
      bool can_copy_row,size_t remaining_value_bytes) const;
   friend class chainbase_secondary_state_view; // owner-affine same-database read adapter only
   void require_owner_thread() const;
   const chainbase::database& db_;
   const std::thread::id owner_;
};

} // namespace eosio::chain::experimental
