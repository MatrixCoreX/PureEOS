#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

namespace eosio::chain::experimental {

enum class isolated_vm_engine : uint8_t { interpreter, jit };
constexpr bool isolated_vm_engine_supported(isolated_vm_engine engine) noexcept {
   if (engine == isolated_vm_engine::interpreter) return true;
#ifdef __x86_64__
   return engine == isolated_vm_engine::jit;
#else
   return false;
#endif
}
constexpr const char* isolated_vm_engine_name(isolated_vm_engine engine) noexcept {
   return engine == isolated_vm_engine::interpreter ? "interpreter" :
          engine == isolated_vm_engine::jit ? "jit" : "invalid";
}

class parallel_deadline_service;
// Immutable, process-local execution policy. Shared infrastructure owns only
// timers; modules, memory, globals and host frames remain worker-exclusive.
// This selection never enters a receipt, fingerprint or consensus rule.
class isolated_execution_runtime {
public:
   explicit isolated_execution_runtime(isolated_vm_engine engine, size_t maximum_frames);
   ~isolated_execution_runtime();
   isolated_execution_runtime(const isolated_execution_runtime&) = delete;
   isolated_execution_runtime& operator=(const isolated_execution_runtime&) = delete;
   isolated_vm_engine engine() const noexcept { return engine_; }
   parallel_deadline_service& deadlines() const;
private:
   const isolated_vm_engine engine_;
   std::unique_ptr<parallel_deadline_service> deadlines_;
};

} // namespace eosio::chain::experimental
