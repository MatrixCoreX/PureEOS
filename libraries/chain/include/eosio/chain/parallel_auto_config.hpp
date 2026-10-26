#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eosio::chain::experimental {

// Local startup sizing only. Never stored in chain state, fingerprints,
// receipts or snapshots; manual configuration remains authoritative.
struct parallel_cpu_info {
   size_t allowed_logical = 1;
   size_t allowed_physical = 1;
   std::optional<size_t> quota_cores;
   bool affinity_known = false;
   bool topology_known = false;
   bool quota_known = false;
   std::vector<std::string> warnings;
   size_t usable_cores() const noexcept;
};

// Alternate read-only roots/CPU IDs allow deterministic filesystem tests.
// nodeos always uses the defaults; no environment-variable override exists.
struct parallel_cpu_probe {
   std::filesystem::path proc_root = "/proc";
   std::filesystem::path cpu_root = "/sys/devices/system/cpu";
   std::optional<std::vector<unsigned>> allowed_cpus;
   unsigned hardware_fallback = 0;
};

parallel_cpu_info detect_parallel_cpu_info(const parallel_cpu_probe& = {});

struct resolved_parallel_config {
   uint16_t workers = 0;
   uint16_t batch = 8;
   bool workers_auto = false;
   bool batch_auto = false;
};

// Defaults stay 0/8. Explicit workers=auto also sizes a defaulted batch;
// an explicitly configured numeric batch always overrides that heuristic.
resolved_parallel_config resolve_parallel_config(std::string_view workers,
   std::string_view batch, bool batch_explicit, const parallel_cpu_info&);

} // namespace eosio::chain::experimental
