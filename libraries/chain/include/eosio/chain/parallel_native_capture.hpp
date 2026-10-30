#pragma once
#include <eosio/chain/parallel_state_overlay.hpp>
#include <eosio/chain/parallel_wasm_worker.hpp>
#include <memory>

namespace eosio::chain::experimental {
struct native_capture_partition_limit : std::runtime_error {
   native_capture_partition_limit():std::runtime_error("native capture local partition limit"){}
};
// Separate native-read capability, never submitted to restricted WASM workers.
// Caller owns the current exclusive chain write window. No application callback
// is pumped during capture; every reader joins before return, unwind or undo.
// Results contain only the existing immutable frozen snapshot values.
class bounded_native_capture_pool {
public:
   explicit bounded_native_capture_pool(size_t participants);
   ~bounded_native_capture_pool();
   bounded_native_capture_pool(const bounded_native_capture_pool&)=delete;
   bounded_native_capture_pool& operator=(const bounded_native_capture_pool&)=delete;
   std::shared_ptr<const frozen_primary_state> capture(
      chainbase::database&,std::span<const table_key>,std::span<const primary_query>,
      primary_snapshot_limits,const isolated_cancel&,std::span<const name>,
      primary_snapshot_capture_stats* = nullptr);
   bool active()const noexcept; // observation only; does not grant write access
private:
   struct impl;
   std::unique_ptr<impl> impl_;
};
}
