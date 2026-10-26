### Build contracts with cdt if available ###
include(ExternalProject)

if( EOSIO_COMPILE_TEST_CONTRACTS )
   set(EOSIO_WASM_OLD_BEHAVIOR "Off")
   if( USE_EOSIO_CDT_1_7_X OR USE_EOSIO_CDT_1_8_X )
      find_package( eosio.cdt REQUIRED )
      set(CMAKE_ARGS_VAL -DCMAKE_TOOLCHAIN_FILE=${EOSIO_CDT_ROOT}/lib/cmake/eosio.cdt/EosioWasmToolchain.cmake -DEOSIO_COMPILE_TEST_CONTRACTS=${EOSIO_COMPILE_TEST_CONTRACTS} )
   else()
      find_package( cdt REQUIRED )
      set(CMAKE_ARGS_VAL -DCMAKE_TOOLCHAIN_FILE=${CDT_ROOT}/lib/cmake/cdt/CDTWasmToolchain.cmake -DEOSIO_COMPILE_TEST_CONTRACTS=${EOSIO_COMPILE_TEST_CONTRACTS} )
   endif()

   if( USE_EOSIO_CDT_1_7_X )
      list(APPEND CMAKE_ARGS_VAL -DUSE_EOSIO_CDT_1_7_X=${USE_EOSIO_CDT_1_7_X})
   elseif( USE_EOSIO_CDT_1_8_X )
      list(APPEND CMAKE_ARGS_VAL -DUSE_EOSIO_CDT_1_8_X=${USE_EOSIO_CDT_1_8_X})
   endif()

   message( STATUS "Building contracts in directory `./unittests/contracts/`" )
   ExternalProject_Add(
           contracts_project
           SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/contracts
           BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/contracts
           CMAKE_ARGS ${CMAKE_ARGS_VAL}
           UPDATE_COMMAND ""
           PATCH_COMMAND ""
           TEST_COMMAND ""
           INSTALL_COMMAND ""
           BUILD_ALWAYS 1
   )

   message( STATUS "Building contracts in directory `./unittests/test-contracts/`" )
   ExternalProject_Add(
      test_contracts_project
      SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/test-contracts
      BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/test-contracts
      CMAKE_ARGS ${CMAKE_ARGS_VAL}
      UPDATE_COMMAND ""
      PATCH_COMMAND ""
      TEST_COMMAND ""
      INSTALL_COMMAND ""
      BUILD_ALWAYS 1
   )
else()
  message( STATUS "Not building contracts in directory `./unittests/contracts/`" )
  add_subdirectory(contracts)
  message( STATUS "Not building contracts in directory `./unittests/test-contracts/`" )
  add_subdirectory(test-contracts)
endif()


if (NOT DISABLE_WASM_SPEC_TESTS)
add_subdirectory( wasm-spec-tests/generated-tests )
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/test_contracts.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/test_contracts.hpp ESCAPE_QUOTES)

add_subdirectory(snapshots)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/snapshots.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/snapshots.hpp ESCAPE_QUOTES)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/deep-mind/deep-mind.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/deep-mind.hpp ESCAPE_QUOTES)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/test-data/test-data.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/test-data.hpp ESCAPE_QUOTES)

add_custom_command(
   OUTPUT protocol_feature_digest_tests.cpp
   COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/gen_protocol_feature_digest_tests.py ${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol_feature_manager.cpp > protocol_feature_digest_tests.cpp
   DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol_feature_manager.cpp ${CMAKE_CURRENT_SOURCE_DIR}/gen_protocol_feature_digest_tests.py
)

### BUILD UNIT TEST EXECUTABLE ###
file(GLOB UNIT_TESTS "*.cpp") # find all unit test suites
set(PUREEOS_STANDALONE_FIXTURE_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/thread_cpu_billing_tests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/chain_config_tests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/native_vm_guard_tests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/oc_memory_resource_failure_tests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/parallel_deadline_service_tests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/parallel_jit_backend_tests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/parallel_jit_finalization_tests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/parallel_production_admission_tests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/parallel_runtime_engine_tests.cpp")
list(REMOVE_ITEM UNIT_TESTS ${PUREEOS_STANDALONE_FIXTURE_SOURCES})
add_executable( unit_test ${UNIT_TESTS} protocol_feature_digest_tests.cpp) # build unit tests as one executable

# Exercise the actual manager and all existing resource-limit regressions
# without building the unrelated monolithic unit_test translation units.
add_executable(resource_limits_boundary_test main.cpp resource_limits_test.cpp)
target_link_libraries(resource_limits_boundary_test PRIVATE eosio_chain_wrap state_history chainbase
  eosio_testing fc custom_appbase ${PLATFORM_SPECIFIC_LIBS})
target_compile_options(resource_limits_boundary_test PRIVATE -DDISABLE_EOSLIB_SERIALIZE)
target_include_directories(resource_limits_boundary_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/include)
add_test(NAME pureeos_resource_limits_boundary COMMAND resource_limits_boundary_test
  --run_test=resource_limits_test --report_level=detailed --color_output=no)
set_tests_properties(pureeos_resource_limits_boundary PROPERTIES TIMEOUT 120 LABELS "resources;integer-boundary")

target_link_libraries( unit_test eosio_chain_wrap state_history chainbase eosio_testing fc custom_appbase ${PLATFORM_SPECIFIC_LIBS} )
target_link_libraries(unit_test pureeos_state_view_experimental pureeos_wasm_worker_experimental
  pureeos_secondary_state_experimental)

# Small real-Chainbase test target; no controller, sockets or deployed chain.
add_executable(parallel_state_view_test ../tests/main.cpp parallel_state_view_tests.cpp)
target_link_libraries(parallel_state_view_test PRIVATE pureeos_state_view_experimental Boost::included_unit_test_framework)
add_test(NAME pureeos_parallel_state_view COMMAND parallel_state_view_test --report_level=detailed --color_output=no)
set_tests_properties(pureeos_parallel_state_view PROPERTIES TIMEOUT 60 LABELS "parallel-px1")

add_executable(vm_entry_validation_test ../tests/vm_entry_validation_test.cpp)
target_link_libraries(vm_entry_validation_test PRIVATE eos-vm ${PLATFORM_SPECIFIC_LIBS})
add_test(NAME pureeos_vm_entry_validation COMMAND vm_entry_validation_test)
set_tests_properties(pureeos_vm_entry_validation PROPERTIES TIMEOUT 60 LABELS "parallel;vm-entry")

add_executable(parallel_secondary_state_test ../tests/main.cpp parallel_secondary_state_tests.cpp)
target_link_libraries(parallel_secondary_state_test PRIVATE pureeos_secondary_state_experimental Boost::included_unit_test_framework)
target_compile_definitions(parallel_secondary_state_test PRIVATE SOFTFLOAT_FAST_INT64)
add_test(NAME pureeos_parallel_secondary_state COMMAND parallel_secondary_state_test --report_level=detailed --color_output=no)
set_tests_properties(pureeos_parallel_secondary_state PROPERTIES TIMEOUT 60 LABELS "parallel-px7;secondary-state;owner-only")

add_executable(parallel_serial_reference_test main.cpp parallel_serial_reference_tests.cpp)
target_link_libraries(parallel_serial_reference_test PRIVATE eosio_chain_wrap eosio_testing
  state_history fc custom_appbase pureeos_state_view_experimental ${PLATFORM_SPECIFIC_LIBS})
target_compile_options(parallel_serial_reference_test PRIVATE -DDISABLE_EOSLIB_SERIALIZE)
target_include_directories(parallel_serial_reference_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/include)
add_test(NAME pureeos_parallel_serial_reference COMMAND parallel_serial_reference_test --report_level=detailed --color_output=no)
set_tests_properties(pureeos_parallel_serial_reference PROPERTIES TIMEOUT 180 LABELS "parallel-px1")

add_executable(parallel_softfloat_test ../tests/main.cpp parallel_softfloat_tests.cpp)
target_link_libraries(parallel_softfloat_test PRIVATE pureeos_state_view_experimental Boost::included_unit_test_framework)
target_compile_definitions(parallel_softfloat_test PRIVATE SOFTFLOAT_FAST_INT64)
target_compile_definitions(unit_test PRIVATE SOFTFLOAT_FAST_INT64)
add_test(NAME pureeos_parallel_softfloat COMMAND parallel_softfloat_test --report_level=detailed --color_output=no)
set_tests_properties(pureeos_parallel_softfloat PROPERTIES TIMEOUT 60 LABELS "parallel-px0")

# Common synchronous-call resource ownership, independent of the VM/controller.
add_executable(sync_call_resource_pool_test ../tests/main.cpp sync_call_resource_pool_tests.cpp sync_call_memory_pool_tests.cpp)
target_link_libraries(sync_call_resource_pool_test PRIVATE fc Boost::included_unit_test_framework)
target_include_directories(sync_call_resource_pool_test PRIVATE ${CMAKE_SOURCE_DIR}/libraries/chain/include)
add_test(NAME pureeos_sync_call_resource_pool COMMAND sync_call_resource_pool_test --report_level=detailed --color_output=no)
set_tests_properties(pureeos_sync_call_resource_pool PROPERTIES TIMEOUT 60 LABELS "parallel-px7;sync-call")

# Compile the actual changed OC call sites even in an ENABLE_OC=OFF build.
# This OBJECT target is a syntax/type check only: it does NOT link LLVM, run OC,
# or claim that the complete optional runtime was tested.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
add_library(sync_call_oc_compile_check OBJECT EXCLUDE_FROM_ALL
  ../libraries/chain/webassembly/runtimes/eos-vm-oc.cpp
  ../libraries/chain/webassembly/runtimes/eos-vm-oc/memory_pools.cpp
  ../libraries/chain/webassembly/runtimes/eos-vm-oc/memory.cpp
  ../libraries/chain/wasm_interface.cpp)
target_link_libraries(sync_call_oc_compile_check PRIVATE eosio_chain)
target_compile_definitions(sync_call_oc_compile_check PRIVATE EOSIO_EOS_VM_OC_RUNTIME_ENABLED)

# Run the actual mmap constructor/cleanup with syscall failure injection and an
# empty intrinsic directory. This is allocator lifecycle coverage, NOT OC WASM.
add_executable(sync_call_oc_memory_failure_test ../tests/main.cpp
  oc_memory_resource_failure_tests.cpp ../libraries/chain/webassembly/runtimes/eos-vm-oc/memory.cpp)
target_link_libraries(sync_call_oc_memory_failure_test PRIVATE fc Boost::included_unit_test_framework)
target_include_directories(sync_call_oc_memory_failure_test PRIVATE ${CMAKE_SOURCE_DIR}/libraries/chain/include)
target_compile_definitions(sync_call_oc_memory_failure_test PRIVATE PUREEOS_OC_MEMORY_FAILURE_FIXTURE)
target_link_options(sync_call_oc_memory_failure_test PRIVATE "-Wl,--wrap=mmap" "-Wl,--wrap=munmap")
add_test(NAME pureeos_sync_call_oc_memory_failure COMMAND sync_call_oc_memory_failure_test --report_level=detailed --color_output=no)
set_tests_properties(pureeos_sync_call_oc_memory_failure PROPERTIES TIMEOUT 60 LABELS "parallel-px7;sync-call;oc-allocator-only")
endif()

# Actual native VM headers with test-only syscall failure injection. No live node
# or user process receives deliberate faults. The OS fault probe is a bounded
# child of the normal test, and is excluded from the sanitizer variants.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  function(pureeos_add_native_vm_guard_target target)
    add_executable(${target} ../tests/main.cpp native_vm_guard_tests.cpp)
    target_link_libraries(${target} PRIVATE fc eos-vm Boost::included_unit_test_framework)
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/libraries/chain/include)
    target_compile_definitions(${target} PRIVATE PUREEOS_NATIVE_VM_GUARD_FIXTURE)
    target_link_options(${target} PRIVATE "-Wl,--wrap=mmap" "-Wl,--wrap=mprotect" "-Wl,--wrap=munmap")
  endfunction()
  pureeos_add_native_vm_guard_target(native_vm_guard_test)
  add_test(NAME pureeos_native_vm_guard COMMAND native_vm_guard_test --report_level=detailed --color_output=no)
  set_tests_properties(pureeos_native_vm_guard PROPERTIES TIMEOUT 60 LABELS "parallel-px7;sync-call;native-vm")
endif()

# Standalone C++ watchdog ownership checks: no controller, chain data, sockets,
# VM code-page revocation, or claim that the private JIT is already enabled.
add_executable(parallel_chain_config_test chain_config_tests.cpp)
target_compile_definitions(parallel_chain_config_test PRIVATE PUREEOS_CHAIN_CONFIG_FIXTURE SOFTFLOAT_FAST_INT64)
target_include_directories(parallel_chain_config_test PRIVATE ${CMAKE_SOURCE_DIR}/libraries/chain/include)
target_link_libraries(parallel_chain_config_test PRIVATE fc chainbase softfloat eos-vm Boost::included_unit_test_framework)
add_test(NAME pureeos_parallel_chain_config COMMAND parallel_chain_config_test --report_level=detailed --color_output=no)
set_tests_properties(pureeos_parallel_chain_config PROPERTIES TIMEOUT 30 LABELS "parallel-px7;capacity;standalone")

find_package(Threads REQUIRED)
add_executable(parallel_immutable_input_test ../tests/parallel_immutable_input_test.cpp)
target_include_directories(parallel_immutable_input_test PRIVATE ${CMAKE_SOURCE_DIR}/libraries/chain/include)
target_link_libraries(parallel_immutable_input_test PRIVATE Threads::Threads)
add_test(NAME pureeos_parallel_immutable_input COMMAND parallel_immutable_input_test)
set_tests_properties(pureeos_parallel_immutable_input PROPERTIES TIMEOUT 30 LABELS "parallel-px7;standalone;immutable-input")
add_executable(parallel_access_hints_test ../tests/parallel_access_hints_test.cpp)
target_include_directories(parallel_access_hints_test PRIVATE ${CMAKE_SOURCE_DIR}/libraries/chain/include)
target_link_libraries(parallel_access_hints_test PRIVATE Threads::Threads)
add_test(NAME pureeos_parallel_access_hints COMMAND parallel_access_hints_test)
set_tests_properties(pureeos_parallel_access_hints PROPERTIES TIMEOUT 30 LABELS "parallel-px7;standalone;prefetch")
add_executable(parallel_worker_signals_test ../tests/parallel_worker_signals_test.cpp)
add_executable(parallel_auto_config_test ../tests/parallel_auto_config_test.cpp ../libraries/chain/parallel_auto_config.cpp)
target_include_directories(parallel_auto_config_test PRIVATE ${CMAKE_SOURCE_DIR}/libraries/chain/include)
target_link_libraries(parallel_auto_config_test PRIVATE Threads::Threads)
add_test(NAME pureeos_parallel_auto_config COMMAND parallel_auto_config_test)
set_tests_properties(pureeos_parallel_auto_config PROPERTIES TIMEOUT 30 LABELS "parallel-px7;standalone;auto-config")
target_include_directories(parallel_worker_signals_test PRIVATE ${CMAKE_SOURCE_DIR}/libraries/chain/include)
target_link_libraries(parallel_worker_signals_test PRIVATE Threads::Threads)
add_test(NAME pureeos_parallel_worker_signals COMMAND parallel_worker_signals_test)
set_tests_properties(pureeos_parallel_worker_signals PROPERTIES TIMEOUT 30 LABELS "parallel-px7;standalone;wakeup")
add_executable(parallel_production_admission_test parallel_production_admission_tests.cpp)
target_compile_definitions(parallel_production_admission_test PRIVATE PUREEOS_PARALLEL_PRODUCTION_ADMISSION_FIXTURE)
target_link_libraries(parallel_production_admission_test PRIVATE Boost::included_unit_test_framework)
target_include_directories(parallel_production_admission_test PRIVATE ${CMAKE_SOURCE_DIR}/libraries/chain/include)
add_test(NAME pureeos_parallel_production_admission COMMAND parallel_production_admission_test --report_level=detailed --color_output=no)
set_tests_properties(pureeos_parallel_production_admission PROPERTIES TIMEOUT 30 LABELS "parallel-px7;production;standalone")

function(pureeos_add_parallel_deadline_target target)
  add_executable(${target} parallel_deadline_service_tests.cpp)
  target_link_libraries(${target} PRIVATE Threads::Threads Boost::included_unit_test_framework)
  target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/libraries/chain/include)
  target_compile_definitions(${target} PRIVATE PUREEOS_PARALLEL_DEADLINE_FIXTURE)
endfunction()
pureeos_add_parallel_deadline_target(parallel_deadline_service_test)
add_test(NAME pureeos_parallel_deadline_service COMMAND parallel_deadline_service_test --report_level=detailed --color_output=no)
set_tests_properties(pureeos_parallel_deadline_service PROPERTIES TIMEOUT 60 LABELS "parallel-px7;deadline-service;standalone")

# Native private-JIT prototype, not an engine switch in controller or nodeos.
# Check instruction byte bounds even in the Release build of this fixture.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  add_executable(parallel_jit_backend_test parallel_jit_backend_tests.cpp)
  target_link_libraries(parallel_jit_backend_test PRIVATE eos-vm WAST WASM Threads::Threads Boost::included_unit_test_framework)
  target_include_directories(parallel_jit_backend_test PRIVATE
    ${CMAKE_SOURCE_DIR}/libraries/chain/include ${CMAKE_SOURCE_DIR}/libraries/wasm-jit/Include)
  target_compile_definitions(parallel_jit_backend_test PRIVATE PUREEOS_PARALLEL_JIT_FIXTURE EOS_VM_VALIDATE_JIT_SIZE)
  target_compile_options(parallel_jit_backend_test PRIVATE -UNDEBUG)
  add_test(NAME pureeos_parallel_jit_backend COMMAND parallel_jit_backend_test --report_level=detailed --color_output=no)
  set_tests_properties(pureeos_parallel_jit_backend PROPERTIES TIMEOUT 90 LABELS "parallel-px7;native-jit;standalone")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(parallel_jit_finalization_test parallel_jit_finalization_tests.cpp)
    target_link_libraries(parallel_jit_finalization_test PRIVATE eos-vm Threads::Threads Boost::included_unit_test_framework)
    target_include_directories(parallel_jit_finalization_test PRIVATE ${CMAKE_SOURCE_DIR}/libraries/chain/include)
    target_compile_definitions(parallel_jit_finalization_test PRIVATE PUREEOS_PARALLEL_JIT_FINALIZATION_FIXTURE EOS_VM_VALIDATE_JIT_SIZE)
    target_compile_options(parallel_jit_finalization_test PRIVATE -UNDEBUG)
    target_link_options(parallel_jit_finalization_test PRIVATE "-Wl,--wrap=mprotect")
    add_test(NAME pureeos_parallel_jit_finalization COMMAND parallel_jit_finalization_test --report_level=detailed --color_output=no)
    set_tests_properties(pureeos_parallel_jit_finalization PROPERTIES TIMEOUT 60 LABELS "parallel-px7;native-jit;fault-injection")
  endif()
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  add_executable(parallel_runtime_engine_test parallel_runtime_engine_tests.cpp)
  target_compile_definitions(parallel_runtime_engine_test PRIVATE PUREEOS_PARALLEL_RUNTIME_FIXTURE)
  target_include_directories(parallel_runtime_engine_test PRIVATE ${CMAKE_SOURCE_DIR}/libraries/wasm-jit/Include)
  target_link_libraries(parallel_runtime_engine_test PRIVATE pureeos_wasm_worker_experimental WAST WASM Boost::included_unit_test_framework)
  add_test(NAME pureeos_parallel_runtime_engine COMMAND parallel_runtime_engine_test --report_level=detailed --color_output=no)
  set_tests_properties(pureeos_parallel_runtime_engine PROPERTIES TIMEOUT 180 LABELS "parallel-px7;runtime-engines;private")
endif()

add_executable(parallel_wasm_worker_test main.cpp parallel_wasm_worker_tests.cpp parallel_ordered_batch_tests.cpp
  parallel_inline_action_tests.cpp parallel_action_tree_tests.cpp parallel_action_tree_journal_tests.cpp parallel_action_tree_pool_tests.cpp
  parallel_context_free_tests.cpp parallel_module_cache_tests.cpp parallel_sync_call_tests.cpp parallel_mixed_tree_tests.cpp
  parallel_secondary_transcript_tests.cpp parallel_secondary_wasm_tests.cpp parallel_pure_host_tests.cpp parallel_integer_host_tests.cpp
  parallel_float_host_tests.cpp parallel_console_host_tests.cpp parallel_full_cdt_tests.cpp parallel_transaction_environment_tests.cpp)
# Only this new TU reads the generated original-contract loader. Avoid changing
# all existing target compile flags merely to add one fixture.
set_source_files_properties(parallel_full_cdt_tests.cpp PROPERTIES
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/include")
target_link_libraries(parallel_wasm_worker_test PRIVATE eosio_chain_wrap eosio_testing
  state_history fc custom_appbase pureeos_wasm_worker_experimental ${PLATFORM_SPECIFIC_LIBS})
target_compile_options(parallel_wasm_worker_test PRIVATE -DDISABLE_EOSLIB_SERIALIZE)
add_test(NAME pureeos_parallel_wasm_worker COMMAND parallel_wasm_worker_test --report_level=detailed --color_output=no)
add_test(NAME pureeos_parallel_sync_private COMMAND parallel_wasm_worker_test
  --run_test=parallel_sync_private_tests:parallel_sync_oracle_tests:parallel_sync_owner_tests:parallel_sync_pool_tests:parallel_sync_adoption_tests --report_level=detailed --color_output=no -- --eos-vm)
set_tests_properties(pureeos_parallel_sync_private PROPERTIES TIMEOUT 120 LABELS "parallel-px7;sync-call")
add_test(NAME pureeos_parallel_mixed_private COMMAND parallel_wasm_worker_test
  --run_test=parallel_mixed_tree_private_tests:parallel_mixed_tree_pool_tests:parallel_mixed_tree_journal_tests:parallel_mixed_tree_oracle_tests --report_level=detailed --color_output=no -- --eos-vm)
set_tests_properties(pureeos_parallel_mixed_private PROPERTIES TIMEOUT 180 LABELS "parallel-px7;private-only;mixed-tree")
add_test(NAME pureeos_parallel_mixed_owner COMMAND parallel_wasm_worker_test
  --run_test=parallel_mixed_tree_owner_tests --report_level=detailed --color_output=no -- --eos-vm)
set_tests_properties(pureeos_parallel_mixed_owner PROPERTIES TIMEOUT 180 LABELS "parallel-px7;owner-only;mixed-tree")
add_test(NAME pureeos_parallel_mixed_adoption COMMAND parallel_wasm_worker_test
  --run_test=parallel_mixed_tree_adoption_tests --report_level=detailed --color_output=no -- --eos-vm)
set_tests_properties(pureeos_parallel_mixed_adoption PROPERTIES TIMEOUT 180 LABELS "parallel-px7;canonical;mixed-tree")
# Larger pools and both engines expand the matrix; per-job deadlines remain unchanged.
set_tests_properties(pureeos_parallel_wasm_worker PROPERTIES TIMEOUT 600 LABELS "parallel-px2")

add_executable(unapplied_queue_test EXCLUDE_FROM_ALL main.cpp unapplied_transaction_queue_tests.cpp)
target_link_libraries(unapplied_queue_test PRIVATE eosio_chain_wrap eosio_testing
  state_history fc custom_appbase ${PLATFORM_SPECIFIC_LIBS})
target_compile_options(unapplied_queue_test PRIVATE -DDISABLE_EOSLIB_SERIALIZE)
add_test(NAME pureeos_unapplied_queue COMMAND unapplied_queue_test --report_level=detailed --color_output=no)

add_executable(parallel_validation_test main.cpp parallel_validation_tests.cpp parallel_inline_adoption_tests.cpp parallel_context_free_tests.cpp)
target_link_libraries(parallel_validation_test PRIVATE eosio_chain_wrap eosio_testing
  state_history fc custom_appbase ${PLATFORM_SPECIFIC_LIBS})
target_compile_options(parallel_validation_test PRIVATE -DDISABLE_EOSLIB_SERIALIZE)
add_test(NAME pureeos_parallel_validation COMMAND parallel_validation_test --run_test=parallel_validation_tests --report_level=detailed --color_output=no)
set_tests_properties(pureeos_parallel_validation PROPERTIES TIMEOUT 300 LABELS "parallel-px3")
add_test(NAME pureeos_parallel_production COMMAND parallel_validation_test --run_test=parallel_production_tests --report_level=detailed --color_output=no)
set_tests_properties(pureeos_parallel_production PROPERTIES TIMEOUT 300 LABELS "parallel-px5")
add_test(NAME pureeos_parallel_inline_adoption COMMAND parallel_validation_test --run_test=parallel_inline_adoption_tests --report_level=detailed --color_output=no)
add_test(NAME pureeos_parallel_context_free COMMAND parallel_validation_test
  --run_test=parallel_context_free_tests:parallel_context_free_oracle_tests:parallel_context_free_adoption_tests
  --report_level=detailed --color_output=no)
set_tests_properties(pureeos_parallel_context_free PROPERTIES TIMEOUT 300 LABELS "parallel-px7;context-free")
set_tests_properties(pureeos_parallel_inline_adoption PROPERTIES TIMEOUT 300 LABELS "parallel-px7")

# Release-profile-2 fixtures explicitly select the same JIT on the serial
# oracle/fallback and private workers. These are correctness tests, not TPS.
add_test(NAME pureeos_parallel_release_jit_inline COMMAND parallel_validation_test
  --run_test=parallel_inline_adoption_tests "--run_test=!*/export_*"
  --report_level=detailed --color_output=no -- --eos-vm-jit)
add_test(NAME pureeos_parallel_release_jit_sync COMMAND parallel_wasm_worker_test
  --run_test=parallel_sync_adoption_tests "--run_test=!*/export_*"
  --report_level=detailed --color_output=no -- --eos-vm-jit)
add_test(NAME pureeos_parallel_release_jit_mixed COMMAND parallel_wasm_worker_test
  --run_test=parallel_mixed_tree_adoption_tests "--run_test=!*/export_*"
  --report_level=detailed --color_output=no -- --eos-vm-jit)
add_test(NAME pureeos_parallel_release_jit_recovery COMMAND parallel_validation_test
  "--run_test=*/native_savanna_parallel_adoption_matches_original_at_every_block_prefix,invalid_receipts_authorization_duplicate_and_wasm_failure_undo_adopted_prefix,snapshot_restart_and_checked_replay_keep_parallel_state_reconstructible,fork_switch_discards_adopted_old_branch_and_reexecutes_new_prefix,lifecycle_fork_undo_snapshot_and_reopen_keep_fresh_canonical_id_allocation,nonexplicit_production_preserves_order_receipts_and_serial_validation,conflict_failed_private_and_auth_rejection_use_original_atomic_path"
  --report_level=detailed --color_output=no -- --eos-vm-jit)
set_tests_properties(pureeos_parallel_release_jit_inline pureeos_parallel_release_jit_sync
  pureeos_parallel_release_jit_mixed pureeos_parallel_release_jit_recovery PROPERTIES
  TIMEOUT 1200 RUN_SERIAL TRUE LABELS "parallel-px7;release-profile2;jit;canonical")

# Existing original-path suites, rebuilt against the current core. This is a
# bounded compatibility target, not a replacement for the complete unit_test.
add_executable(parallel_compatibility_test main.cpp read_only_trx_tests.cpp
  sync_call_cpp_tests.cpp sync_call_wasm_tests.cpp resource_limits_test.cpp
  database_tests.cpp delay_tests.cpp ram_tests.cpp checktime_tests.cpp api_tests.cpp
  subjective_billing_tests.cpp unapplied_transaction_queue_tests.cpp)
target_link_libraries(parallel_compatibility_test PRIVATE eosio_chain_wrap eosio_testing
  state_history chainbase fc custom_appbase ${PLATFORM_SPECIFIC_LIBS})
target_compile_options(parallel_compatibility_test PRIVATE -DDISABLE_EOSLIB_SERIALIZE)
target_include_directories(parallel_compatibility_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/contracts ${CMAKE_CURRENT_BINARY_DIR}/contracts ${CMAKE_CURRENT_BINARY_DIR}/include)
foreach(PARALLEL_COMPAT_SUITE read_only_trx_tests sync_call_cpp_tests sync_call_wasm_tests
    resource_limits_test database_tests delay_tests ram_tests checktime_tests subjective_billing_test
    unapplied_transaction_queue_tests)
  add_test(NAME pureeos_parallel_compat_${PARALLEL_COMPAT_SUITE} COMMAND parallel_compatibility_test
    --run_test=${PARALLEL_COMPAT_SUITE} --report_level=detailed --color_output=no -- --eos-vm)
  set_tests_properties(pureeos_parallel_compat_${PARALLEL_COMPAT_SUITE} PROPERTIES
    TIMEOUT 1800 LABELS "parallel-compatibility" RUN_SERIAL TRUE)
endforeach()
add_test(NAME pureeos_sync_call_timeout_reuse COMMAND parallel_compatibility_test
  --run_test=sync_call_cpp_tests/timeout_cleanup_allows_same_parent_and_child_modules_to_execute_again
  --report_level=detailed --color_output=no -- --eos-vm)
set_tests_properties(pureeos_sync_call_timeout_reuse PROPERTIES TIMEOUT 30 LABELS "parallel-px7;sync-call;native-vm" RUN_SERIAL TRUE)
foreach(PARALLEL_API_CASE db_tests db_notify_tests multi_index_tests permission_tests resource_limits_tests)
  add_test(NAME pureeos_parallel_compat_api_${PARALLEL_API_CASE} COMMAND parallel_compatibility_test
    --run_test=api_part2_tests/${PARALLEL_API_CASE}* --report_level=detailed --color_output=no -- --eos-vm)
  set_tests_properties(pureeos_parallel_compat_api_${PARALLEL_API_CASE} PROPERTIES
    TIMEOUT 1800 LABELS "parallel-compatibility" RUN_SERIAL TRUE)
endforeach()
foreach(PARALLEL_API_CASE action_ordinal_test action_ordinal_failtest1 action_ordinal_failtest2 action_ordinal_failtest3
    permission_usage_tests action_results_tests)
  add_test(NAME pureeos_parallel_compat_api_${PARALLEL_API_CASE} COMMAND parallel_compatibility_test
    --run_test=api_part3_tests/${PARALLEL_API_CASE}* --report_level=detailed --color_output=no -- --eos-vm)
  set_tests_properties(pureeos_parallel_compat_api_${PARALLEL_API_CASE} PROPERTIES
    TIMEOUT 1800 LABELS "parallel-compatibility" RUN_SERIAL TRUE)
endforeach()

# Opt-in native isolation checks, without pretending that non-instrumented
# Chainbase/FC or the complete nodeos/JIT have been covered by sanitizers.
set(PUREEOS_PARALLEL_SANITIZERS "" CACHE STRING "Native prototype checks: address,undefined or thread; empty disables")
if(PUREEOS_PARALLEL_SANITIZERS)
  include(${CMAKE_SOURCE_DIR}/cmake/ParallelSanitizerOptions.cmake)
  if(NOT PUREEOS_PARALLEL_SANITIZERS STREQUAL "address,undefined" AND
     NOT PUREEOS_PARALLEL_SANITIZERS STREQUAL "thread")
    message(FATAL_ERROR "PUREEOS_PARALLEL_SANITIZERS must be address,undefined or thread")
  endif()

  pureeos_add_parallel_deadline_target(parallel_deadline_service_sanitizer_test)
  set_target_properties(parallel_deadline_service_sanitizer_test PROPERTIES EXCLUDE_FROM_ALL TRUE)
  target_compile_options(parallel_deadline_service_sanitizer_test PRIVATE ${PUREEOS_PARALLEL_SANITIZER_OPT_FLAG} -g1 -fno-omit-frame-pointer
    "-fsanitize=${PUREEOS_PARALLEL_SANITIZERS}")
  target_link_options(parallel_deadline_service_sanitizer_test PRIVATE "-fsanitize=${PUREEOS_PARALLEL_SANITIZERS}")
  add_test(NAME pureeos_parallel_deadline_service_sanitizer COMMAND parallel_deadline_service_sanitizer_test --report_level=detailed --color_output=no)
  set_tests_properties(pureeos_parallel_deadline_service_sanitizer PROPERTIES TIMEOUT 60 LABELS "parallel-sanitizer;deadline-service;standalone")

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pureeos_add_native_vm_guard_target(native_vm_guard_sanitizer_test)
    set_target_properties(native_vm_guard_sanitizer_test PROPERTIES EXCLUDE_FROM_ALL TRUE)
    target_compile_definitions(native_vm_guard_sanitizer_test PRIVATE PUREEOS_NATIVE_VM_SANITIZER)
    target_compile_options(native_vm_guard_sanitizer_test PRIVATE ${PUREEOS_PARALLEL_SANITIZER_OPT_FLAG} -g1 -fno-omit-frame-pointer
      "-fsanitize=${PUREEOS_PARALLEL_SANITIZERS}")
    target_link_options(native_vm_guard_sanitizer_test PRIVATE "-fsanitize=${PUREEOS_PARALLEL_SANITIZERS}")
    if(PUREEOS_PARALLEL_SANITIZERS STREQUAL "thread")
      # Same test-only longjmp interceptor compatibility as the private worker.
      target_compile_options(native_vm_guard_sanitizer_test PRIVATE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
    endif()
    add_test(NAME pureeos_native_vm_guard_sanitizer COMMAND native_vm_guard_sanitizer_test --report_level=detailed --color_output=no)
    set_tests_properties(pureeos_native_vm_guard_sanitizer PROPERTIES TIMEOUT 60 LABELS "parallel-sanitizer;sync-call;native-vm")
  endif()

  add_executable(sync_call_resource_pool_sanitizer_test EXCLUDE_FROM_ALL ../tests/main.cpp sync_call_resource_pool_tests.cpp sync_call_memory_pool_tests.cpp)
  target_link_libraries(sync_call_resource_pool_sanitizer_test PRIVATE fc Boost::included_unit_test_framework)
  target_include_directories(sync_call_resource_pool_sanitizer_test PRIVATE ${CMAKE_SOURCE_DIR}/libraries/chain/include)
  target_compile_options(sync_call_resource_pool_sanitizer_test PRIVATE ${PUREEOS_PARALLEL_SANITIZER_OPT_FLAG} -g1 -fno-omit-frame-pointer
    "-fsanitize=${PUREEOS_PARALLEL_SANITIZERS}")
  target_link_options(sync_call_resource_pool_sanitizer_test PRIVATE "-fsanitize=${PUREEOS_PARALLEL_SANITIZERS}")
  add_test(NAME pureeos_sync_call_resource_pool_sanitizer COMMAND sync_call_resource_pool_sanitizer_test --report_level=detailed --color_output=no)
  set_tests_properties(pureeos_sync_call_resource_pool_sanitizer PROPERTIES TIMEOUT 60 LABELS "parallel-sanitizer;sync-call")
  # Instrument the actual dependency implementation too, not merely its callers.
  get_target_property(PUREEOS_SOFTFLOAT_SOURCES softfloat SOURCES)
  get_target_property(PUREEOS_SOFTFLOAT_SOURCE_DIR softfloat SOURCE_DIR)
  get_target_property(PUREEOS_SOFTFLOAT_INCLUDES softfloat INCLUDE_DIRECTORIES)
  set(PUREEOS_SOFTFLOAT_ABSOLUTE_SOURCES)
  foreach(PUREEOS_SOFTFLOAT_SOURCE IN LISTS PUREEOS_SOFTFLOAT_SOURCES)
    if(IS_ABSOLUTE "${PUREEOS_SOFTFLOAT_SOURCE}")
      list(APPEND PUREEOS_SOFTFLOAT_ABSOLUTE_SOURCES "${PUREEOS_SOFTFLOAT_SOURCE}")
    else()
      list(APPEND PUREEOS_SOFTFLOAT_ABSOLUTE_SOURCES "${PUREEOS_SOFTFLOAT_SOURCE_DIR}/${PUREEOS_SOFTFLOAT_SOURCE}")
    endif()
  endforeach()
  add_library(pureeos_softfloat_sanitized STATIC EXCLUDE_FROM_ALL ${PUREEOS_SOFTFLOAT_ABSOLUTE_SOURCES})
  target_include_directories(pureeos_softfloat_sanitized PUBLIC ${PUREEOS_SOFTFLOAT_INCLUDES})
  target_compile_definitions(pureeos_softfloat_sanitized PUBLIC THREAD_LOCAL=__thread
    PRIVATE SOFTFLOAT_FAST_INT64 SOFTFLOAT_ROUND_EVEN INLINE_LEVEL=5 SOFTFLOAT_FAST_DIV32TO16 SOFTFLOAT_FAST_DIV64TO32)
  target_compile_options(pureeos_softfloat_sanitized PRIVATE ${PUREEOS_PARALLEL_SANITIZER_OPT_FLAG} -g1 -fno-omit-frame-pointer
    "-fsanitize=${PUREEOS_PARALLEL_SANITIZERS}")
  add_executable(parallel_native_sanitizer_test EXCLUDE_FROM_ALL ../tests/main.cpp
    parallel_state_view_tests.cpp parallel_softfloat_tests.cpp
    ../libraries/chain/transaction_state_view.cpp ../libraries/chain/parallel_state_overlay.cpp
    ../libraries/chain/parallel_secondary_state.cpp)
  target_link_libraries(parallel_native_sanitizer_test PRIVATE chainbase fc pureeos_softfloat_sanitized
    Boost::included_unit_test_framework)
  target_include_directories(parallel_native_sanitizer_test PRIVATE
    ${CMAKE_SOURCE_DIR}/libraries/chain/include ${CMAKE_BINARY_DIR}/libraries/chain/include)
  target_compile_definitions(parallel_native_sanitizer_test PRIVATE SOFTFLOAT_FAST_INT64)
  target_compile_options(parallel_native_sanitizer_test PRIVATE ${PUREEOS_PARALLEL_SANITIZER_OPT_FLAG} -g1 -fno-omit-frame-pointer
    "-fsanitize=${PUREEOS_PARALLEL_SANITIZERS}")
  target_link_options(parallel_native_sanitizer_test PRIVATE "-fsanitize=${PUREEOS_PARALLEL_SANITIZERS}")
  add_test(NAME pureeos_parallel_native_sanitizer COMMAND parallel_native_sanitizer_test --report_level=detailed --color_output=no)
  set_tests_properties(pureeos_parallel_native_sanitizer PROPERTIES TIMEOUT 180 LABELS "parallel-sanitizer")

  add_executable(parallel_secondary_sanitizer_test EXCLUDE_FROM_ALL ../tests/main.cpp
    parallel_secondary_state_tests.cpp ../libraries/chain/parallel_secondary_state.cpp
    ../libraries/chain/transaction_state_view.cpp ../libraries/chain/parallel_state_overlay.cpp
    ../libraries/chain/parallel_secondary_journal.cpp)
  target_link_libraries(parallel_secondary_sanitizer_test PRIVATE chainbase fc pureeos_softfloat_sanitized
    Boost::included_unit_test_framework)
  target_include_directories(parallel_secondary_sanitizer_test PRIVATE
    ${CMAKE_SOURCE_DIR}/libraries/chain/include ${CMAKE_BINARY_DIR}/libraries/chain/include)
  target_compile_definitions(parallel_secondary_sanitizer_test PRIVATE SOFTFLOAT_FAST_INT64)
  target_compile_options(parallel_secondary_sanitizer_test PRIVATE ${PUREEOS_PARALLEL_SANITIZER_OPT_FLAG} -g1 -fno-omit-frame-pointer
    "-fsanitize=${PUREEOS_PARALLEL_SANITIZERS}")
  target_link_options(parallel_secondary_sanitizer_test PRIVATE "-fsanitize=${PUREEOS_PARALLEL_SANITIZERS}")
  add_test(NAME pureeos_parallel_secondary_sanitizer COMMAND parallel_secondary_sanitizer_test --report_level=detailed --color_output=no)
  set_tests_properties(pureeos_parallel_secondary_sanitizer PROPERTIES TIMEOUT 120 LABELS "parallel-sanitizer;secondary-state;owner-only")

  # Instrument the actual PRIVATE numeric C definitions, under the same prefix
  # as the normal private target. Canonical ___ symbols remain in builtins.
  get_target_property(PUREEOS_NUMERIC_C_SOURCES pureeos_numeric_builtins_experimental SOURCES)
  get_target_property(PUREEOS_NUMERIC_C_RENAMES pureeos_numeric_builtins_experimental COMPILE_DEFINITIONS)
  add_library(pureeos_numeric_builtins_sanitized STATIC EXCLUDE_FROM_ALL ${PUREEOS_NUMERIC_C_SOURCES})
  target_compile_definitions(pureeos_numeric_builtins_sanitized PRIVATE ${PUREEOS_NUMERIC_C_RENAMES})
  target_compile_options(pureeos_numeric_builtins_sanitized PRIVATE -Wall ${PUREEOS_PARALLEL_SANITIZER_OPT_FLAG} -g1 -fno-omit-frame-pointer
    "-fsanitize=${PUREEOS_PARALLEL_SANITIZERS}")

  # Instrument the worker, its VM header implementation and owned state layer.
  # The existing canonical controller/Chainbase/FC remain uninstrumented; this
  # is not a whole-node sanitizer build. Whole-archive selects every instrumented
  # SoftFloat definition before the normal chain dependency archive is searched.
  add_executable(parallel_wasm_sanitizer_test EXCLUDE_FROM_ALL main.cpp parallel_wasm_worker_tests.cpp parallel_ordered_batch_tests.cpp
    parallel_inline_action_tests.cpp parallel_action_tree_tests.cpp parallel_action_tree_journal_tests.cpp parallel_action_tree_pool_tests.cpp
    parallel_inline_adoption_tests.cpp parallel_context_free_tests.cpp parallel_module_cache_tests.cpp parallel_sync_call_tests.cpp parallel_mixed_tree_tests.cpp
    parallel_secondary_transcript_tests.cpp parallel_secondary_wasm_tests.cpp parallel_pure_host_tests.cpp parallel_integer_host_tests.cpp
    parallel_float_host_tests.cpp parallel_console_host_tests.cpp parallel_full_cdt_tests.cpp parallel_transaction_environment_tests.cpp
    ../libraries/chain/parallel_inline_action.cpp ../libraries/chain/parallel_action_tree.cpp ../libraries/chain/parallel_sync_call.cpp
    ../libraries/chain/parallel_mixed_tree_journal.cpp ../libraries/chain/parallel_transaction_environment.cpp
    ../libraries/chain/parallel_canonical_adoption.cpp ../libraries/chain/apply_context.cpp ../libraries/chain/host_context.cpp ../libraries/chain/transaction_context.cpp
    ../libraries/chain/transaction_state_view.cpp ../libraries/chain/parallel_state_overlay.cpp ../libraries/chain/parallel_secondary_state.cpp
    ../libraries/chain/parallel_secondary_journal.cpp
    ../libraries/chain/parallel_wasm_worker.cpp ../libraries/chain/parallel_ordered_batch.cpp ../libraries/chain/parallel_worker_pool.cpp
    ../libraries/chain/platform_timer_accuracy.cpp)
  # A mixed-instrumentation run hung in the original POSIX timer calibration
  # spin loop. Include the actual configured timer backend in this test build;
  # do not disable signals or timer checks to bypass startup.
  get_target_property(PUREEOS_CHAIN_SOURCES eosio_chain SOURCES)
  get_target_property(PUREEOS_CHAIN_SOURCE_DIR eosio_chain SOURCE_DIR)
  foreach(PUREEOS_CHAIN_SOURCE IN LISTS PUREEOS_CHAIN_SOURCES)
    if(PUREEOS_CHAIN_SOURCE MATCHES "^platform_timer_(posix|asio_fallback|kqueue)\\.cpp$")
      target_sources(parallel_wasm_sanitizer_test PRIVATE "${PUREEOS_CHAIN_SOURCE_DIR}/${PUREEOS_CHAIN_SOURCE}")
    endif()
  endforeach()
  target_link_libraries(parallel_wasm_sanitizer_test PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,pureeos_softfloat_sanitized>"
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,pureeos_numeric_builtins_sanitized>"
    eosio_chain_wrap eosio_testing state_history fc custom_appbase eos-vm ${PLATFORM_SPECIFIC_LIBS})
  target_compile_options(parallel_wasm_sanitizer_test PRIVATE -DDISABLE_EOSLIB_SERIALIZE
    ${PUREEOS_PARALLEL_SANITIZER_OPT_FLAG} -g1 -fno-omit-frame-pointer "-fsanitize=${PUREEOS_PARALLEL_SANITIZERS}")
  target_link_options(parallel_wasm_sanitizer_test PRIVATE "-fsanitize=${PUREEOS_PARALLEL_SANITIZERS}")
  if(PUREEOS_PARALLEL_SANITIZERS STREQUAL "thread")
    # GCC libtsan here intercepts siglongjmp, but not glibc __longjmp_chk.
    # Fortified VM signal jumps otherwise bypass TSan's signal-frame cleanup.
    # Test-only compatibility option; production hardening is unchanged.
    target_compile_options(parallel_wasm_sanitizer_test PRIVATE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
  endif()
  if(PUREEOS_PARALLEL_SANITIZERS STREQUAL "thread")
    # Private path plus the adopter/action/transaction contexts are instrumented,
    # but NOT the complete controller/authorization database used by oracle and
    # integration cases. ASan includes those cases to check trace relocation;
    # TSan still excludes mixed-controller execution. Keep that limitation in
    # the test name and output; do not suppress mixed-build race diagnostics.
    add_test(NAME pureeos_parallel_wasm_private_tsan COMMAND parallel_wasm_sanitizer_test
      "--run_test=!parallel_wasm_worker_tests/fixed_input_and_authorization_hosts_match_original_wasm_execution:!parallel_wasm_worker_tests/private_wasm_write_matches_original_chain_execution:!parallel_cdt_host_oracle_tests:!parallel_transaction_oracle_tests:!parallel_notification_oracle_tests:!parallel_action_tree_oracle_tests:!parallel_inline_adoption_tests:!parallel_context_free_oracle_tests:!parallel_context_free_adoption_tests:!parallel_sync_oracle_tests:!parallel_sync_owner_tests:!parallel_sync_adoption_tests:!parallel_mixed_tree_oracle_tests:!parallel_mixed_tree_owner_tests:!parallel_mixed_tree_adoption_tests:!parallel_secondary_wasm_oracle_tests:!parallel_pure_hash_oracle_tests:!parallel_integer_oracle_tests:!parallel_float_oracle_tests:!parallel_console_oracle_tests:!parallel_exit_oracle_tests:!parallel_environment_oracle_tests:!parallel_full_cdt_oracle_tests"
      --report_level=detailed --color_output=no)
    set_tests_properties(pureeos_parallel_wasm_private_tsan PROPERTIES TIMEOUT 180 LABELS "parallel-sanitizer;private-only")
  else()
    add_test(NAME pureeos_parallel_wasm_sanitizer COMMAND parallel_wasm_sanitizer_test --report_level=detailed --color_output=no)
    # Expanded controller/oracle corpus outgrew the old whole-suite watchdog
    # (the previous ASan corpus already needed 163s). This is ONLY the aggregate
    # test watchdog; production 50ms and private-attempt 500ms stay unchanged.
    set_tests_properties(pureeos_parallel_wasm_sanitizer PROPERTIES TIMEOUT 600 LABELS "parallel-sanitizer")
  endif()

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Only auto-register for the dynamic GNU ELF runtimes understood by this
    # checker. Do not reject Clang or non-ELF toolchains by a GNU-name heuristic.
    # A cache switch can leave a newer executable from the OTHER sanitizer even
    # though CMake reports its target up-to-date. Verify actual ELF dependencies
    # before each family; a failed fixture prevents the dependent test running.
    # This guard proves runtime identity, not complete instrumentation coverage.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    function(pureeos_require_sanitizer_runtime test_name target_name)
      add_test(NAME ${test_name}_runtime COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_SOURCE_DIR}/scripts/parallel_sanitizer_check.py
        --sanitizer ${PUREEOS_PARALLEL_SANITIZERS} $<TARGET_FILE:${target_name}>)
      set_tests_properties(${test_name}_runtime PROPERTIES TIMEOUT 60
        FIXTURES_SETUP ${target_name}_runtime LABELS "parallel-sanitizer-runtime")
      set_tests_properties(${test_name} PROPERTIES FIXTURES_REQUIRED ${target_name}_runtime)
    endfunction()
    pureeos_require_sanitizer_runtime(pureeos_parallel_deadline_service_sanitizer parallel_deadline_service_sanitizer_test)
    pureeos_require_sanitizer_runtime(pureeos_native_vm_guard_sanitizer native_vm_guard_sanitizer_test)
    pureeos_require_sanitizer_runtime(pureeos_sync_call_resource_pool_sanitizer sync_call_resource_pool_sanitizer_test)
    pureeos_require_sanitizer_runtime(pureeos_parallel_native_sanitizer parallel_native_sanitizer_test)
    pureeos_require_sanitizer_runtime(pureeos_parallel_secondary_sanitizer parallel_secondary_sanitizer_test)
    if(PUREEOS_PARALLEL_SANITIZERS STREQUAL "thread")
      pureeos_require_sanitizer_runtime(pureeos_parallel_wasm_private_tsan parallel_wasm_sanitizer_test)
    else()
      pureeos_require_sanitizer_runtime(pureeos_parallel_wasm_sanitizer parallel_wasm_sanitizer_test)
    endif()
    add_test(NAME pureeos_sanitizer_runtime_checker COMMAND ${Python3_EXECUTABLE} -B
      ${CMAKE_SOURCE_DIR}/tests/parallel_sanitizer_check_test.py)
    set_tests_properties(pureeos_sanitizer_runtime_checker PROPERTIES TIMEOUT 30 LABELS "parallel-sanitizer-runtime")
  endif()
endif()

target_compile_options(unit_test PUBLIC -DDISABLE_EOSLIB_SERIALIZE)

# Focused consensus/genesis regression target; reuse the real controller/tester,
# without rebuilding every unrelated unit test translation unit.
add_executable(native_savanna_test main.cpp pureeos_genesis_tests.cpp ../tests/native_policy_boundary_fixture.cpp performance_metrics_tests.cpp block_log_extract.cpp ../tests/block_log.cpp)
target_link_libraries(native_savanna_test eosio_chain_wrap state_history chainbase eosio_testing fc custom_appbase ${PLATFORM_SPECIFIC_LIBS})
target_compile_options(native_savanna_test PRIVATE -DDISABLE_EOSLIB_SERIALIZE)
add_executable(cdt_runtime_parameter_test EXCLUDE_FROM_ALL main.cpp ../tests/cdt_runtime_parameter_probe.cpp)
target_link_libraries(cdt_runtime_parameter_test eosio_chain_wrap eosio_testing chainbase fc custom_appbase ${PLATFORM_SPECIFIC_LIBS})
target_compile_options(cdt_runtime_parameter_test PRIVATE -DDISABLE_EOSLIB_SERIALIZE)
target_include_directories(cdt_runtime_parameter_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/include
  ${CMAKE_SOURCE_DIR}/libraries/testing/include)
target_include_directories(native_savanna_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/include
  ${CMAKE_SOURCE_DIR}/libraries/testing/include)
add_test(NAME native_savanna_genesis COMMAND native_savanna_test --run_test=pureeos_genesis_tests --report_level=detailed)
add_test(NAME native_performance_metrics COMMAND native_savanna_test --run_test=performance_metrics_tests --report_level=detailed)
add_test(NAME native_upgrade_governance COMMAND native_savanna_test --run_test=pureeos_governance_tests --report_level=detailed)
set(PUREEOS_GOVERNANCE_CONTRACTS_DIR "" CACHE PATH "CDT profile-2 contract build directory for real WASM integration tests")
if(PUREEOS_GOVERNANCE_CONTRACTS_DIR)
  target_compile_definitions(native_savanna_test PRIVATE PUREEOS_GOVERNANCE_CONTRACTS_DIR="${PUREEOS_GOVERNANCE_CONTRACTS_DIR}")
  target_compile_definitions(unit_test PRIVATE PUREEOS_GOVERNANCE_CONTRACTS_DIR="${PUREEOS_GOVERNANCE_CONTRACTS_DIR}")
  add_test(NAME native_upgrade_governance_contract COMMAND native_savanna_test --run_test=pureeos_governance_contract_tests --report_level=detailed)
endif()
set(PUREEOS_SINGLE_CHOICE_CONTRACTS_DIR "" CACHE PATH "Experimental fresh-chain single-choice contract build for WASM tests")
if(PUREEOS_SINGLE_CHOICE_CONTRACTS_DIR)
  if(NOT PUREEOS_GOVERNANCE_CONTRACTS_DIR)
    message(FATAL_ERROR "Single-choice tests also need the baseline PUREEOS_GOVERNANCE_CONTRACTS_DIR")
  endif()
  target_compile_definitions(native_savanna_test PRIVATE PUREEOS_SINGLE_CHOICE_CONTRACTS_DIR="${PUREEOS_SINGLE_CHOICE_CONTRACTS_DIR}")
  target_compile_definitions(unit_test PRIVATE PUREEOS_SINGLE_CHOICE_CONTRACTS_DIR="${PUREEOS_SINGLE_CHOICE_CONTRACTS_DIR}")
  target_link_libraries(native_savanna_test chain_plugin)
  target_link_libraries(unit_test chain_plugin)
  add_test(NAME native_single_choice_contract COMMAND native_savanna_test --run_test=pureeos_single_choice_contract_tests --report_level=detailed)
endif()
add_test(NAME native_genesis_blocklog_compat COMMAND native_savanna_test --run_test=blog_file_tests,block_log_extraction_tests --report_level=detailed)
set(PUREEOS_RESOURCE_SPLIT_CONTRACTS_DIR "" CACHE PATH "Fresh-chain 10/90 resource split contract build for WASM tests")
if(PUREEOS_RESOURCE_SPLIT_CONTRACTS_DIR)
  if(NOT PUREEOS_GOVERNANCE_CONTRACTS_DIR)
    message(FATAL_ERROR "Resource split tests need PUREEOS_GOVERNANCE_CONTRACTS_DIR")
  endif()
  target_compile_definitions(native_savanna_test PRIVATE PUREEOS_RESOURCE_SPLIT_CONTRACTS_DIR="${PUREEOS_RESOURCE_SPLIT_CONTRACTS_DIR}")
  target_compile_definitions(unit_test PRIVATE PUREEOS_RESOURCE_SPLIT_CONTRACTS_DIR="${PUREEOS_RESOURCE_SPLIT_CONTRACTS_DIR}")
  add_test(NAME native_resource_split_contract COMMAND native_savanna_test --run_test=pureeos_resource_split_tests --report_level=detailed)
  if(PUREEOS_SINGLE_CHOICE_CONTRACTS_DIR AND PUREEOS_ROTATION_CONTRACTS_DIR)
    add_test(NAME native_committee_boundary_contract COMMAND native_savanna_test
      --run_test=pureeos_committee_boundary_tests --report_level=detailed --log_level=message --color_output=no)
    # Real BLS verification/replay is considerably slower in unoptimized builds
    # and when both VM regression matrices run concurrently. This is a test
    # watchdog, not a consensus block-time or finality parameter.
    set_tests_properties(native_committee_boundary_contract PROPERTIES TIMEOUT 3600)
  endif()
endif()
set(PUREEOS_ROTATION_CONTRACTS_DIR "" CACHE PATH "Fresh-chain sequential rotation contract build for WASM tests")
set(PUREEOS_PREVIOUS_ROTATION_CONTRACTS_DIR "" CACHE PATH "Optional pre-upgrade rotation v2 contracts for real state compatibility testing")
set(PUREEOS_PREVIOUS_REWARD_CONTRACTS_DIR "" CACHE PATH "Optional block-only contracts with the former reward activation threshold")
if(PUREEOS_PREVIOUS_REWARD_CONTRACTS_DIR)
  target_compile_definitions(native_savanna_test PRIVATE PUREEOS_PREVIOUS_REWARD_CONTRACTS_DIR="${PUREEOS_PREVIOUS_REWARD_CONTRACTS_DIR}")
endif()
if(PUREEOS_PREVIOUS_ROTATION_CONTRACTS_DIR)
  target_compile_definitions(native_savanna_test PRIVATE PUREEOS_PREVIOUS_ROTATION_CONTRACTS_DIR="${PUREEOS_PREVIOUS_ROTATION_CONTRACTS_DIR}")
endif()
if(PUREEOS_ROTATION_CONTRACTS_DIR)
  if(NOT PUREEOS_RESOURCE_SPLIT_CONTRACTS_DIR OR NOT PUREEOS_SINGLE_CHOICE_CONTRACTS_DIR)
    message(FATAL_ERROR "Rotation tests require the resource-split and single-choice fixtures")
  endif()
  target_compile_definitions(native_savanna_test PRIVATE PUREEOS_ROTATION_CONTRACTS_DIR="${PUREEOS_ROTATION_CONTRACTS_DIR}")
  target_compile_definitions(unit_test PRIVATE PUREEOS_ROTATION_CONTRACTS_DIR="${PUREEOS_ROTATION_CONTRACTS_DIR}")
  add_test(NAME native_sequential_rotation_contract COMMAND native_savanna_test
    --run_test=pureeos_sequential_rotation_tests --report_level=detailed --log_level=message --color_output=no)
  set_tests_properties(native_sequential_rotation_contract PROPERTIES TIMEOUT 10800)
  add_test(NAME native_reward_halving_contract COMMAND native_savanna_test
    --run_test=pureeos_reward_halving_tests --report_level=detailed --log_level=message --color_output=no)
  set_tests_properties(native_reward_halving_contract PROPERTIES TIMEOUT 1800)
  add_test(NAME native_reward_activation_contract COMMAND native_savanna_test
    --run_test=pureeos_reward_activation_tests --report_level=detailed --log_level=message --color_output=no)
  set_tests_properties(native_reward_activation_contract PROPERTIES TIMEOUT 1800)
endif()
target_include_directories( unit_test PUBLIC
                            ${CMAKE_CURRENT_SOURCE_DIR}
                            ${CMAKE_SOURCE_DIR}/libraries/testing/include
                            ${CMAKE_SOURCE_DIR}/test-contracts
                            ${CMAKE_BINARY_DIR}/contracts
                            ${CMAKE_CURRENT_SOURCE_DIR}/contracts
                            ${CMAKE_CURRENT_BINARY_DIR}/contracts
                            ${CMAKE_CURRENT_BINARY_DIR}/include)

### MARK TEST SUITES FOR EXECUTION ###
add_p_test(NAME protocol_feature_digest_unit_test COMMAND unit_test --run_test=protocol_feature_digest_tests --report_level=detailed --log_level=all --color_output)
add_p_test(NAME protocol_feature_digest_generator_test COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/test_gen_protocol_feature_digest_tests.py)
set(ctest_tests "protocol_feature_digest_tests")
foreach(TEST_SUITE ${UNIT_TESTS}) # create an independent target for each test suite
  execute_process(COMMAND sh -c "grep -E 'BOOST_AUTO_TEST_SUITE\\s*[(]' '${TEST_SUITE}' | grep -vE '//.*BOOST_AUTO_TEST_SUITE\\s*[(]' | cut -d ')' -f 1 | cut -d '(' -f 2" OUTPUT_VARIABLE SUITE_NAMES OUTPUT_STRIP_TRAILING_WHITESPACE) # get the test suite name from the *.cpp file
  string(REPLACE "\n" ";" SUITE_NAMES "${SUITE_NAMES}")
  # Boost permits reopening a suite; CTest still needs only one entry for it.
  list(REMOVE_DUPLICATES SUITE_NAMES)
  foreach(SUITE_NAME IN LISTS SUITE_NAMES)
    if (NOT "" STREQUAL "${SUITE_NAME}") # ignore empty lines
      execute_process(COMMAND sh -c "echo ${SUITE_NAME} | sed -e 's/s$//' | sed -e 's/_test$//'" OUTPUT_VARIABLE TRIMMED_SUITE_NAME OUTPUT_STRIP_TRAILING_WHITESPACE) # trim "_test" or "_tests" from the end of ${SUITE_NAME}
      # to run unit_test with all log from blockchain displayed, put "--verbose" after "--", i.e. "unit_test -- --verbose"
      foreach(RUNTIME ${EOSIO_WASM_RUNTIMES})
        add_p_test(NAME ${TRIMMED_SUITE_NAME}_unit_test_${RUNTIME} COMMAND unit_test --run_test=${SUITE_NAME} --report_level=detailed --log_level=all --color_output -- --${RUNTIME})
        # add a duplicate test to run without fsgsbase instructions for a limited number of OC wasm tests
        if(RUNTIME STREQUAL "eos-vm-oc" AND TRIMMED_SUITE_NAME MATCHES "^wasm_part")
           add_p_test(NAME ${TRIMMED_SUITE_NAME}_unit_test_${RUNTIME}-nofsgs COMMAND unit_test --run_test=${SUITE_NAME} --report_level=detailed --log_level=all --color_output -- --${RUNTIME} COST 5000)
           set_tests_properties(${TRIMMED_SUITE_NAME}_unit_test_${RUNTIME}-nofsgs PROPERTIES ENVIRONMENT "HELIOS_DISABLE_FSGSBASE=1")
        endif()
        # build list of tests to run during coverage testing
        if(ctest_tests)
          string(APPEND ctest_tests "|")
        endif()
        string(APPEND ctest_tests ${TRIMMED_SUITE_NAME}_unit_test_$RUNTIME)
      endforeach()
    endif()
  endforeach()
endforeach()
set(ctest_tests "'${ctest_tests}' -j8") # surround test list string in apostrophies

# The following tests are known to take the longest, bump up their cost (priority) so that they'll run first
# even on fresh first time test runs before ctest auto-detects costs
foreach(RUNTIME ${EOSIO_WASM_RUNTIMES})
   set_tests_properties(api_part1_unit_test_${RUNTIME} PROPERTIES COST 5000)
   # Interpreter crypto tests also run in unoptimized developer builds. This is
   # a harness watchdog only; transaction CPU limits remain unchanged.
   set_tests_properties(api_part1_unit_test_${RUNTIME} PROPERTIES TIMEOUT 10800)
   # These suites assert wall-clock limits. Concurrent compiler/VM workloads
   # must not turn CPU contention into a spurious correctness failure.
   set_tests_properties(abi_unit_test_${RUNTIME} getpeerkeys_unit_test_${RUNTIME}
                        checktime_unit_test_${RUNTIME} PROPERTIES RUN_SERIAL TRUE COST 10000)
   set_tests_properties(api_part2_unit_test_${RUNTIME} PROPERTIES COST 5000)
   set_tests_properties(api_part3_unit_test_${RUNTIME} PROPERTIES COST 5000)
   set_tests_properties(wasm_part1_unit_test_${RUNTIME} PROPERTIES COST 5000)
   set_tests_properties(wasm_part2_unit_test_${RUNTIME} PROPERTIES COST 5000)
   set_tests_properties(wasm_part3_unit_test_${RUNTIME} PROPERTIES COST 5000)
   set_tests_properties(wasm_config_part1_unit_test_${RUNTIME} PROPERTIES COST 5000)
   set_tests_properties(wasm_config_part2_unit_test_${RUNTIME} PROPERTIES COST 5000)
   set_tests_properties(wasm_config_part3_unit_test_${RUNTIME} PROPERTIES COST 5000)
   set_tests_properties(wasm_config_part4_unit_test_${RUNTIME} PROPERTIES COST 5000)
   set_tests_properties(wasm_config_part5_unit_test_${RUNTIME} PROPERTIES COST 5000)
   set_tests_properties(snapshot_part1_unit_test_${RUNTIME} PROPERTIES COST 4000)
   set_tests_properties(snapshot_part2_unit_test_${RUNTIME} PROPERTIES COST 4000)
   set_tests_properties(finality_unit_test_${RUNTIME} PROPERTIES COST 4000)
   set_tests_properties(delay_unit_test_${RUNTIME} PROPERTIES COST 3000)
endforeach()

### COVERAGE TESTING ###
if(ENABLE_COVERAGE_TESTING)
  set(Coverage_NAME ${PROJECT_NAME}_ut_coverage)
  # check for dependencies
  if(NOT LCOV_PATH)
    message(FATAL_ERROR "lcov not found! Aborting...")
  endif()
  if(NOT LLVMCOV_PATH)
    message(FATAL_ERROR "llvm-cov not found! Aborting...")
  endif()
  if(NOT GENHTML_PATH)
    message(FATAL_ERROR "genhtml not found! Aborting...")
  endif()
  # tests to skip during coverage testing
  set(ctest_exclude_tests '') # no spaces allowed within tests list
  # setup target
  add_custom_target(${Coverage_NAME}
    # cleanup lcov
    COMMAND ${LCOV_PATH} --directory . --zerocounters
    # run tests
    COMMAND ./tools/ctestwrapper.sh -R ${ctest_tests} -E ${ctest_exclude_tests}
    COMMAND ${LCOV_PATH} --directory . --capture --gcov-tool ${CMAKE_SOURCE_DIR}/tools/llvm-gcov.sh --output-file ${Coverage_NAME}.info
    COMMAND ${LCOV_PATH} -remove ${Coverage_NAME}.info '*/boost/*' '/usr/lib/*' '/usr/include/*' '*/externals/*' '*/libfc/*' '*/wasm-jit/*' --output-file ${Coverage_NAME}_filtered.info
    COMMAND ${GENHTML_PATH} -o ${Coverage_NAME} ${PROJECT_BINARY_DIR}/${Coverage_NAME}_filtered.info
    COMMAND if [ "$CI" != "true" ]\; then ${CMAKE_COMMAND} -E remove ${Coverage_NAME}.base ${Coverage_NAME}.info ${Coverage_NAME}_filtered.info ${Coverage_NAME}.total ${PROJECT_BINARY_DIR}/${Coverage_NAME}.info.cleaned ${PROJECT_BINARY_DIR}/${Coverage_NAME}_filtered.info.cleaned\; fi
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
    COMMENT "Resetting code coverage counters to zero. Processing code coverage counters and generating report. Report published in ./${Coverage_NAME}"
  )
  # show info where to find the report
  add_custom_command(TARGET ${Coverage_NAME} POST_BUILD
    COMMAND ;
    COMMENT "Open ./${Coverage_NAME}/index.html in your browser to view the coverage report."
  )
endif()

# Dedicated CPU accounting check; no Boost/controller fixture needed.
add_executable(thread_cpu_billing_test thread_cpu_billing_tests.cpp)
target_link_libraries(thread_cpu_billing_test PRIVATE ${PLATFORM_SPECIFIC_LIBS})
target_include_directories(thread_cpu_billing_test PRIVATE ${CMAKE_SOURCE_DIR}/libraries/chain/include)
add_test(NAME pureeos_thread_cpu_billing COMMAND thread_cpu_billing_test)
set_tests_properties(pureeos_thread_cpu_billing PROPERTIES TIMEOUT 60 LABELS "parallel;cpu-billing")

add_test(NAME pureeos_parallel_multitoken COMMAND parallel_validation_test --run_test=parallel_multitoken_tests --report_level=detailed --color_output=no)
set_tests_properties(pureeos_parallel_multitoken PROPERTIES TIMEOUT 300 LABELS "parallel;multitoken")
