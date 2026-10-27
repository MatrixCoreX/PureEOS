#pragma once

#include <fc/crypto/sha256.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace eosio::chain {
struct transaction;
namespace experimental {

// One immutable, bounded canonical serialization per transaction, shared by
// roots, notifications, inline and sync children. No live chain references.
// Action ranges point INTO owned bytes, not a second copy of every action.
class isolated_transaction_environment final {
public:
   static constexpr size_t max_wire_bytes = 512 * 1024;
   static constexpr size_t max_actions = 1024, max_authorizations = 64;
   static constexpr size_t max_action_bytes = 64 * 1024, max_extensions = 1024;
   static std::shared_ptr<const isolated_transaction_environment> capture(
      const transaction&, uint64_t pending_time_us, uint64_t published_time_us,
      uint32_t pending_block_num, const std::function<void()>& checkpoint = {},
      size_t wire_budget = max_wire_bytes);
   isolated_transaction_environment(const isolated_transaction_environment&) = delete;
   isolated_transaction_environment& operator=(const isolated_transaction_environment&) = delete;

   const fc::sha256& fingerprint() const { return fingerprint_; }
   const fc::sha256& transaction_id() const { return id_; }
   size_t owned_wire_bytes() const { return bytes_.size(); }
   size_t action_count(uint32_t type) const;
   uint64_t current_time() const { return pending_time_us_; }
   uint64_t publication_time() const { return published_time_us_; }
   uint32_t block_num() const { return pending_block_num_; }
   uint32_t expiration() const { return expiration_; }
   uint32_t tapos_block_num() const { return ref_block_num_; }
   uint32_t tapos_block_prefix() const { return ref_block_prefix_; }
   int32_t transaction_size() const { return static_cast<int32_t>(bytes_.size()); }
   int32_t read_transaction(std::span<char> output) const;
   int32_t get_action(uint32_t type, uint32_t index, std::span<char> output) const;
   // Original packed_transaction::id hashes the reserialized transaction, not
   // its compressed envelope. Owner also checks ACTUAL publication/block data.
   bool matches(const fc::sha256& id, uint64_t pending_time_us,
                uint64_t published_time_us, uint32_t pending_block_num) const;

private:
   struct action_range { size_t offset, size; };
   isolated_transaction_environment(std::vector<char>,
      std::array<std::vector<action_range>, 2>, const transaction&,
      uint64_t pending_time_us, uint64_t published_time_us, uint32_t pending_block_num);
   const std::vector<char> bytes_;
   const std::array<std::vector<action_range>, 2> actions_;
   const fc::sha256 id_, fingerprint_;
   const uint64_t pending_time_us_, published_time_us_;
   const uint32_t pending_block_num_, expiration_, ref_block_prefix_;
   const uint16_t ref_block_num_;
};

inline bool same_isolated_environment(const std::shared_ptr<const isolated_transaction_environment>& a,
      const std::shared_ptr<const isolated_transaction_environment>& b) {
   return bool(a) == bool(b) && (!a || a->fingerprint() == b->fingerprint());
}

} // namespace experimental
} // namespace eosio::chain
