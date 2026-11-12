#include <eosio/testing/tester.hpp>
#include <eosio/testing/bls_utils.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <boost/test/unit_test.hpp>
#include <fc/io/json.hpp>
#include <cstdlib>
#include <array>
#include <filesystem>
#include <fstream>

using namespace eosio::chain;
using namespace eosio::testing;

namespace {
void parameter_round_trip(wasm_interface::vm_type runtime, bool custom) {
   const char* input = std::getenv("PUREEOS_CDT_PARAMETER_PROBE_DIR");
   BOOST_REQUIRE_MESSAGE(input && *input, "Supply the real compiled runtimeparams WASM/ABI directory");
   const std::filesystem::path root(input);
   std::ifstream wasm(root / "runtimeparams.wasm", std::ios::binary);
   std::ifstream abi(root / "runtimeparams.abi");
   BOOST_REQUIRE(wasm.good() && abi.good());
   const std::vector<uint8_t> code{std::istreambuf_iterator<char>(wasm), std::istreambuf_iterator<char>()};
   const std::string schema{std::istreambuf_iterator<char>(abi), std::istreambuf_iterator<char>()};
   BOOST_REQUIRE(!code.empty() && !schema.empty());
   auto genesis = base_tester::default_genesis();
   const auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->version = 2;
   genesis.initial_savanna->finalizer_key = public_key;
   genesis.initial_savanna->proof_of_possession = proof;
   if (custom) {
      genesis.initial_configuration.max_block_cpu_usage = 321000;
      genesis.initial_configuration.max_block_net_usage = 2097152;
   }
   genesis.initial_configuration.validate();
   fc::temp_directory directory;
   auto configuration = base_tester::default_config(directory).first;
   configuration.wasm_runtime = runtime;
   tester chain(configuration, genesis);
   chain.set_node_finalizers(std::array<name, 1>{"eosio"_n});
   chain.produce_blocks(2);
   chain.set_code("eosio"_n, code);
   chain.set_abi("eosio"_n, schema);
   const auto before = chain.control->get_global_properties().configuration;
   // Nonfatal catch: the old-library negative control must fail assertions,
   // not be mistaken for a useful counterexample after an unrelated abort.
   try {
      chain.push_action("eosio"_n, "verify"_n, "eosio"_n,
         fc::mutable_variant_object()("expected_cpu", before.max_block_cpu_usage)
                                     ("expected_net", before.max_block_net_usage));
   } catch (const fc::exception& error) {
      // Preserve the actual WASM assertion. A generic "unexpected exception"
      // cannot distinguish the known old-runtime defect from a setup failure.
      BOOST_ERROR(error.to_detail_string());
   } catch (const std::exception& error) {
      BOOST_ERROR(error.what());
   }
   BOOST_CHECK(chain.control->get_global_properties().configuration == before);
   chain.produce_block();
   BOOST_CHECK(chain.control->get_global_properties().configuration == before);
   chain.close();
   chain.open();
   BOOST_CHECK(chain.control->get_global_properties().configuration == before);
}
}

BOOST_AUTO_TEST_SUITE(cdt_runtime_parameter_tests)
BOOST_AUTO_TEST_CASE(jit_default_parameters) { parameter_round_trip(wasm_interface::vm_type::eos_vm_jit, false); }
BOOST_AUTO_TEST_CASE(jit_custom_parameters) { parameter_round_trip(wasm_interface::vm_type::eos_vm_jit, true); }
BOOST_AUTO_TEST_CASE(interpreter_default_parameters) { parameter_round_trip(wasm_interface::vm_type::eos_vm, false); }
BOOST_AUTO_TEST_CASE(interpreter_custom_parameters) { parameter_round_trip(wasm_interface::vm_type::eos_vm, true); }
BOOST_AUTO_TEST_SUITE_END()
