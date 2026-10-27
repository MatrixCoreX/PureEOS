#include <array>
#include <boost/test/unit_test.hpp>
#include <eosio/chain/application.hpp>
#include <eosio/chain_plugin/chain_plugin.hpp>
#include <stdint.h>
#include <stdexcept>
#include <string>

BOOST_AUTO_TEST_CASE(chain_plugin_default_tests) {
   fc::temp_directory  tmp;
   appbase::scoped_app app;

   auto tmp_path = tmp.path().string();
   std::array          args = {
       "test_chain_plugin", "--blocks-log-stride", "10", "--data-dir", tmp_path.c_str(),
       "--config-dir", tmp_path.c_str(),
   };

   BOOST_CHECK(app->initialize<eosio::chain_plugin>(args.size(), const_cast<char**>(args.data())));
   auto& plugin = app->get_plugin<eosio::chain_plugin>();

   auto* config = std::get_if<eosio::chain::partitioned_blocklog_config>(&plugin.chain_config().blog);
   BOOST_REQUIRE(config);
   BOOST_CHECK_EQUAL(config->max_retained_files, UINT32_MAX);

   // test default eos-vm-oc-whitelist is empty
   BOOST_CHECK(!plugin.chain().is_eos_vm_oc_whitelisted(eosio::chain::name{"hello"}));
   BOOST_CHECK(!plugin.chain().is_eos_vm_oc_whitelisted(eosio::chain::name{"vault"}));
   BOOST_CHECK(!plugin.chain().is_eos_vm_oc_whitelisted(eosio::chain::name{"xs"}));
   BOOST_CHECK(!plugin.chain().is_eos_vm_oc_whitelisted(eosio::chain::name{""}));
}

BOOST_AUTO_TEST_CASE(chain_plugin_eos_vm_oc_whitelist) {
   fc::temp_directory  tmp;
   appbase::scoped_app app;

   auto tmp_path = tmp.path().string();
   std::array          args = {
      "test_chain_plugin", "--eos-vm-oc-whitelist", "hello", "--data-dir", tmp_path.c_str(),
      "--config-dir", tmp_path.c_str(),
  };

#ifdef EOSIO_EOS_VM_OC_RUNTIME_ENABLED
   BOOST_CHECK(app->initialize<eosio::chain_plugin>(args.size(), const_cast<char**>(args.data())));
   auto& plugin = app->get_plugin<eosio::chain_plugin>();
   BOOST_CHECK(plugin.chain().is_eos_vm_oc_whitelisted(eosio::chain::name{"hello"}));
   BOOST_CHECK(plugin.chain().is_eos_vm_oc_whitelisted(eosio::chain::name{"xs.hello"}));
   BOOST_CHECK(!plugin.chain().is_eos_vm_oc_whitelisted(eosio::chain::name{"goodbye"}));
#else
   // OC-only options must not silently enable unsupported execution modes.
   // This validates the disabled-build behavior; it does NOT test the OC VM.
   BOOST_CHECK_EXCEPTION(
      app->initialize<eosio::chain_plugin>(args.size(), const_cast<char**>(args.data())),
      std::runtime_error, [](const std::runtime_error& error) {
         return std::string(error.what()).find("Unknown option '--eos-vm-oc-whitelist'") != std::string::npos;
      });
#endif
}
