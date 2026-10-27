#pragma once

#include <eosio/testing/tester.hpp>
#include <stdexcept>

namespace eosio::testing {
// Test-only selection for existing live controller fixtures. The historical
// default remains interpreter. An explicit --eos-vm-jit switches BOTH the
// canonical oracle/fallback and private workers, never only one side.
inline void configure_parallel_test_runtime(chain::controller::config& cfg) {
   const bool jit = base_tester::arguments_contains("--eos-vm-jit");
   if (base_tester::arguments_contains("--eos-vm-oc") ||
       (jit && base_tester::arguments_contains("--eos-vm")))
      throw std::invalid_argument("parallel fixture needs one matching interpreter/JIT pair");
   cfg.wasm_runtime = jit ? chain::wasm_interface::vm_type::eos_vm_jit
                          : chain::wasm_interface::vm_type::eos_vm;
   cfg.parallel_engine = jit ? chain::experimental::isolated_vm_engine::jit
                             : chain::experimental::isolated_vm_engine::interpreter;
}
} // namespace eosio::testing
