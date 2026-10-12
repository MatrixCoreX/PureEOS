#include <eosio/chain/webassembly/eos-vm-oc/memory_pools.hpp>

namespace eosio::chain::eosvmoc {

memory_pools::memory_pools()
   : pools([](uint32_t slices) { return new eosvmoc::memory(slices); }) {}

eosvmoc::memory* memory_pools::acquire_mem(uint32_t call_depth) {
   return pools.acquire(call_depth);
}

void memory_pools::release_mem(uint32_t call_depth, eosvmoc::memory* m) noexcept {
   pools.release(call_depth, m);
}

void memory_pools::set_num_threads(uint32_t nthreads) {
   pools.set_num_threads(nthreads);
}

void memory_pools::set_max_call_depth(uint32_t max_depth) {
   pools.set_max_call_depth(max_depth);
}

} // namespace eosio::chain::eosvmoc
