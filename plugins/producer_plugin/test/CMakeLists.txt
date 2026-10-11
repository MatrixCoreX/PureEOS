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
