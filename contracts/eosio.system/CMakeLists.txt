option(PUREEOS_BP_MAINTENANCE "Enable planned BP maintenance through normal rotation handoff" OFF)
if(PUREEOS_BP_MAINTENANCE AND NOT PUREEOS_SEQUENTIAL_ROTATION)
  message(FATAL_ERROR "BP maintenance requires sequential rotation")
endif()

add_contract(
  eosio.system
  eosio.system
  ${CMAKE_CURRENT_SOURCE_DIR}/src/eosio.system.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/delegate_bandwidth.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/exchange_state.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/finalizer_key.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/name_bidding.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/native.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/producer_pay.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/powerup.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/rex.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/rotation.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/voting.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/limit_auth_changes.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/block_info.cpp)

if(PUREEOS_BP_BOND)
  if(NOT PUREEOS_SEQUENTIAL_ROTATION)
    message(FATAL_ERROR "BP bonds require sequential rotation")
  endif()
  target_sources(eosio.system PRIVATE src/bp_bond.cpp)
  target_compile_definitions(eosio.system PUBLIC PUREEOS_BP_BOND)
endif()

if(PUREEOS_BP_MAINTENANCE)
  target_sources(eosio.system PRIVATE src/maintenance.cpp)
  target_compile_definitions(eosio.system PUBLIC PUREEOS_BP_MAINTENANCE)
endif()

if(PUREEOS_BP_PEERING)
  target_sources(eosio.system PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/peer_keys.cpp)
endif()

if(PUREEOS_UPGRADE_GOVERNANCE)
  target_sources(eosio.system PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/upgrade_governance.cpp)
  target_compile_definitions(eosio.system PUBLIC PUREEOS_UPGRADE_GOVERNANCE)
endif()

if(PUREEOS_SINGLE_CHOICE_VOTING)
  target_sources(eosio.system PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/direct_voting.cpp)
  target_sources(eosio.system PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/reward_policy.cpp)
  target_compile_definitions(eosio.system PUBLIC PUREEOS_SINGLE_CHOICE_VOTING)
endif()

if(PUREEOS_RESOURCE_SPLIT)
  target_sources(eosio.system PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/resource_split.cpp)
  target_compile_definitions(eosio.system PUBLIC PUREEOS_RESOURCE_SPLIT)
endif()

if(PUREEOS_SEQUENTIAL_ROTATION)
  target_sources(eosio.system PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/rotation_state.cpp)
  target_compile_definitions(eosio.system PUBLIC PUREEOS_SEQUENTIAL_ROTATION)
endif()

if(SYSTEM_CONFIGURABLE_WASM_LIMITS)
  target_compile_definitions(eosio.system PUBLIC SYSTEM_CONFIGURABLE_WASM_LIMITS)
endif()

if(SYSTEM_BLOCKCHAIN_PARAMETERS)
  target_compile_definitions(eosio.system PUBLIC SYSTEM_BLOCKCHAIN_PARAMETERS)
endif()

target_include_directories(eosio.system PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                               ${CMAKE_CURRENT_SOURCE_DIR}/../eosio.token/include)

set_target_properties(eosio.system PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

add_contract(rex.results rex.results ${CMAKE_CURRENT_SOURCE_DIR}/src/rex.results.cpp)
add_contract(powup.results powup.results ${CMAKE_CURRENT_SOURCE_DIR}/src/powerup.results.cpp)

target_include_directories(rex.results PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_include_directories(powup.results PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(rex.results PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.rex")

set_target_properties(powup.results PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.powerup")

include(${CMAKE_CURRENT_SOURCE_DIR}/ricardian/voting-policy.cmake)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/ricardian/eosio.system.contracts.md.in
               ${CMAKE_CURRENT_BINARY_DIR}/ricardian/eosio.system.contracts.md @ONLY)
# CDT embeds Ricardian text while compiling, not just when linking. Without an
# explicit dependency an incremental ABI can retain the previous profile's text.
get_target_property(system_sources eosio.system SOURCES)
foreach(system_source IN LISTS system_sources)
  set_property(SOURCE ${system_source} APPEND PROPERTY OBJECT_DEPENDS
    ${CMAKE_CURRENT_BINARY_DIR}/ricardian/eosio.system.contracts.md)
endforeach()

target_compile_options(eosio.system PUBLIC -R${CMAKE_CURRENT_SOURCE_DIR}/ricardian
                                           -R${CMAKE_CURRENT_BINARY_DIR}/ricardian)
