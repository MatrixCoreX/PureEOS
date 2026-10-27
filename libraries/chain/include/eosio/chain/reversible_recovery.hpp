#pragma once
#include <eosio/chain/block.hpp>
#include <eosio/chain/chain_id_type.hpp>
#include <filesystem>
#include <map>
#include <mutex>

namespace eosio::chain {

// Local crash-recovery data, NOT consensus state or permission to vote. Each
// immutable block is atomically installed and synced before advancing finalizer
// safety information. Recovery still verifies headers, QCs and executes blocks.
class reversible_recovery {
public:
   reversible_recovery(const std::filesystem::path& directory, const chain_id_type& chain_id);
   std::vector<signed_block_ptr> load(); // startup only; throws on committed corruption
   void store(const signed_block_ptr& block); // thread safe; throws on I/O failure
   // Caller must first durably sync the irreversible block log through this height.
   void prune(uint32_t durable_lib);
   static void sync_directory(const std::filesystem::path& directory);

private:
   const std::filesystem::path directory;
   const chain_id_type chain_id;
   std::mutex mutex;
   std::map<block_id_type, uint32_t> saved;
   uint32_t pruned_through = 0;
};
} // namespace eosio::chain
