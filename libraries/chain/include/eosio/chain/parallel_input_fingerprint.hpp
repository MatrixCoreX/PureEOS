#pragma once
#include <eosio/chain/parallel_action_tree.hpp>
namespace eosio::chain::experimental {
// Owned immutable observations made by the current canonical owner.
// No controller/native state is reachable by a fingerprint worker.
struct root_fingerprint_binding { fc::sha256 code_hash; isolated_wasm_limits limits; };
struct owned_fingerprint_input {
   std::vector<isolated_action_input> roots;
   std::vector<root_fingerprint_binding> bindings;
   isolated_action_tree_context context;
   fc::sha256 execution_fingerprint,hint_fingerprint;
};
class owned_fingerprint_pool {
public:
   explicit owned_fingerprint_pool(size_t participants);
   ~owned_fingerprint_pool();
   owned_fingerprint_pool(const owned_fingerprint_pool&)=delete;
   owned_fingerprint_pool& operator=(const owned_fingerprint_pool&)=delete;
   std::vector<owned_fingerprint_input> prepare(std::vector<owned_fingerprint_input>,const isolated_cancel&);
private:
   struct impl;std::unique_ptr<impl> impl_;
};
}
