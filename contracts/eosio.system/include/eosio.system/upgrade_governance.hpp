#pragma once
#include <cstdint>

// PureEOS profile-2 host extensions. Keep the upstream CDT installation intact.
extern "C" {
   __attribute__((eosio_wasm_import)) uint64_t get_upgrade_developer();
   __attribute__((eosio_wasm_import)) void set_upgrade_voters(const char* packed_voters, uint32_t byte_size);
}
