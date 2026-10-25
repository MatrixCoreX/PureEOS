add_executable( test_producer_plugin
        test_production_pause_vote_tracker.cpp
        test_options.cpp
        test_block_timing_util.cpp
        test_disallow_delayed_trx.cpp
        main.cpp
        )
target_link_libraries( test_producer_plugin producer_plugin eosio_testing eosio_chain_wrap )
add_p_test(NAME test_producer_plugin COMMAND plugins/producer_plugin/test/test_producer_plugin)

# The timing unit tests permanently enable fc::mock_time_traits. Keep real
# application threads in a different process: their wall clock must advance
# regardless of Boost.Test registration/execution order.
add_executable( test_producer_plugin_integration
        test_trx_full.cpp
        test_native_savanna_restart.cpp
        main.cpp
        )
target_link_libraries( test_producer_plugin_integration producer_plugin eosio_testing eosio_chain_wrap )
add_p_test(NAME test_producer_plugin_integration COMMAND plugins/producer_plugin/test/test_producer_plugin_integration)

# Deterministic two-hop timer regression, independent of mocked chain clocks.
find_package(Threads REQUIRED)
add_executable(test_production_timer_guard EXCLUDE_FROM_ALL test_production_timer_guard.cpp)
target_include_directories(test_production_timer_guard PRIVATE ../include)
target_link_libraries(test_production_timer_guard PRIVATE Boost::included_unit_test_framework Boost::asio Threads::Threads)
add_p_test(NAME test_production_timer_guard COMMAND plugins/producer_plugin/test/test_production_timer_guard)

add_executable(test_incoming_transaction_batch EXCLUDE_FROM_ALL test_incoming_transaction_batch.cpp)
target_include_directories(test_incoming_transaction_batch PRIVATE ../include ${CMAKE_SOURCE_DIR}/libraries/chain/include)
target_link_libraries(test_incoming_transaction_batch PRIVATE Boost::included_unit_test_framework)
add_p_test(NAME test_incoming_transaction_batch COMMAND plugins/producer_plugin/test/test_incoming_transaction_batch)
