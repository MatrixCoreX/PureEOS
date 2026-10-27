#ifdef PUREEOS_CHAIN_CONFIG_FIXTURE
#define BOOST_TEST_MODULE PureEOS_Chain_Config
#include <boost/test/included/unit_test.hpp>
#else
#include <boost/test/unit_test.hpp>
#endif
#include <eosio/chain/chain_config.hpp>

using namespace eosio::chain;

BOOST_AUTO_TEST_SUITE(chain_configuration_comparison)

BOOST_AUTO_TEST_CASE(minimum_cpu_change_is_not_equal_at_any_configuration_version) {
   const auto check = []<typename Configuration>() {
      const Configuration original{};
      auto changed = original;
      ++changed.min_transaction_cpu_usage;
      BOOST_CHECK(!(original == changed));
      BOOST_CHECK(!(changed == original));
      BOOST_CHECK(original != changed);
      BOOST_CHECK(changed != original);
   };
   check.operator()<chain_config_v0>();
   check.operator()<chain_config_v1>();
   check.operator()<chain_config_v2>();
}

BOOST_AUTO_TEST_CASE(every_current_configuration_field_participates_in_comparison) {
   const chain_config original{};
   const auto check = [&](auto member) {
      auto changed = original;
      ++(changed.*member);
      BOOST_CHECK(original != changed);
      BOOST_CHECK(changed != original);
      BOOST_CHECK(original == original);
      BOOST_CHECK(changed == changed);
   };
#define CHECK_CONFIG_FIELD(field) check(&chain_config::field)
   CHECK_CONFIG_FIELD(max_block_net_usage);
   CHECK_CONFIG_FIELD(target_block_net_usage_pct);
   CHECK_CONFIG_FIELD(max_transaction_net_usage);
   CHECK_CONFIG_FIELD(base_per_transaction_net_usage);
   CHECK_CONFIG_FIELD(net_usage_leeway);
   CHECK_CONFIG_FIELD(context_free_discount_net_usage_num);
   CHECK_CONFIG_FIELD(context_free_discount_net_usage_den);
   CHECK_CONFIG_FIELD(max_block_cpu_usage);
   CHECK_CONFIG_FIELD(target_block_cpu_usage_pct);
   CHECK_CONFIG_FIELD(max_transaction_cpu_usage);
   CHECK_CONFIG_FIELD(min_transaction_cpu_usage);
   CHECK_CONFIG_FIELD(max_transaction_lifetime);
   CHECK_CONFIG_FIELD(deferred_trx_expiration_window);
   CHECK_CONFIG_FIELD(max_transaction_delay);
   CHECK_CONFIG_FIELD(max_inline_action_size);
   CHECK_CONFIG_FIELD(max_inline_action_depth);
   CHECK_CONFIG_FIELD(max_authority_depth);
   CHECK_CONFIG_FIELD(max_action_return_value_size);
   CHECK_CONFIG_FIELD(max_sync_call_depth);
   CHECK_CONFIG_FIELD(max_sync_call_data_size);
#undef CHECK_CONFIG_FIELD
}

BOOST_AUTO_TEST_SUITE_END()
