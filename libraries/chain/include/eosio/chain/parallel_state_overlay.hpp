#pragma once

#include <eosio/chain/transaction_state_view.hpp>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <tuple>

namespace eosio::chain::experimental {

struct table_key_less {
   bool operator()(const table_key& a, const table_key& b) const {
      return std::tie(a.code, a.scope, a.table) < std::tie(b.code, b.scope, b.table);
   }
};

struct primary_snapshot_limits {
   size_t max_tables = 64;
   size_t max_rows = 4096;
   size_t max_value_bytes = 4 * 1024 * 1024;
};

struct primary_overlay_limits {
   size_t max_read_tables = 64;
   size_t max_write_operations = 4096;
   size_t max_write_value_bytes = 4 * 1024 * 1024;
};

struct primary_state_unsupported : std::runtime_error {
   primary_state_unsupported() : std::runtime_error("primary state operation requires serial fallback") {}
};
struct primary_state_budget_exceeded : std::runtime_error {
   primary_state_budget_exceeded() : std::runtime_error("primary state local budget exceeded") {}
};
struct primary_state_invalid_attempt : std::logic_error {
   primary_state_invalid_attempt() : std::logic_error("primary attempt is failed or already sealed") {}
};

// Bounded, explicitly selected tables copied by the serial owner, then exposed
// as immutable owned values. No Chainbase references survive capture. The
// controller's capture/quiescence/epoch lifecycle is NOT implemented here.
// Unknown tables mean unsupported, NEVER "table does not exist".
class frozen_primary_state final : public transaction_state_view {
public:
   static std::shared_ptr<const frozen_primary_state> capture(
      const chainbase_transaction_state_view&, std::span<const table_key>, primary_snapshot_limits = {});
   frozen_primary_state(const frozen_primary_state&) = delete;
   frozen_primary_state& operator=(const frozen_primary_state&) = delete;

   std::optional<table_read> read_table(const table_key&) const override;
   primary_metadata_result locate_primary(const primary_query&) const override;
   primary_prefix_result read_primary_prefix(const primary_query&, size_t max_bytes) const override;
   primary_read_result read_primary(const primary_query&, size_t max_value_bytes) const override;

   size_t table_count() const { return tables_.size(); }
   size_t row_count() const { return row_count_; }
   size_t value_bytes() const { return value_bytes_; }

private:
   frozen_primary_state() = default;
   struct table_snapshot {
      std::optional<table_read> metadata;
      std::map<uint64_t, primary_row_read> rows;
   };
   const table_snapshot& get_table(const table_key&) const;
   std::pair<const table_snapshot*, const primary_row_read*> locate(const primary_query&) const;
   std::map<table_key, table_snapshot, table_key_less> tables_;
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
   const auto& writes() const { return writes_; }
private:
   friend class primary_state_overlay;
   primary_overlay_result(std::shared_ptr<const frozen_primary_state> base,
                          std::set<table_key, table_key_less> reads, std::vector<primary_update> writes)
      : base_(std::move(base)), reads_(std::move(reads)), writes_(std::move(writes)) {}
   std::shared_ptr<const frozen_primary_state> base_;
   std::set<table_key, table_key_less> reads_;
   std::vector<primary_update> writes_;
};

// One thread owns each attempt. First capability: existing-row updates only,
// unchanged payer and payload size. No create/delete, RAM accounting or IDs are
// simulated. Unsupported operations poison the WHOLE attempt for fallback.
class primary_state_overlay final : public transaction_state_view {
public:
   explicit primary_state_overlay(std::shared_ptr<const frozen_primary_state>, primary_overlay_limits = {});
   primary_state_overlay(const primary_state_overlay&) = delete;
   primary_state_overlay& operator=(const primary_state_overlay&) = delete;
   std::optional<table_read> read_table(const table_key&) const override;
   primary_metadata_result locate_primary(const primary_query&) const override;
   primary_prefix_result read_primary_prefix(const primary_query&, size_t max_bytes) const override;
   primary_read_result read_primary(const primary_query&, size_t max_value_bytes) const override;
   void update_existing(const table_key&, uint64_t primary_key, name payer, std::span<const char> value);
   primary_overlay_result seal();

private:
   void check_owner_and_valid() const;
   void depend_on(const table_key&) const;
   const std::vector<char>* written_value(const table_key&, uint64_t primary_key) const;
   std::shared_ptr<const frozen_primary_state> base_;
   primary_overlay_limits limits_;
   const std::thread::id owner_;
   mutable bool valid_ = true;
   mutable std::set<table_key, table_key_less> reads_;
   std::vector<primary_update> writes_;
   std::map<table_key, std::map<uint64_t, size_t>, table_key_less> latest_writes_;
   size_t written_bytes_ = 0;
};

} // namespace eosio::chain::experimental
