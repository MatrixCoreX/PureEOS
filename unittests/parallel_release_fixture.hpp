#pragma once
#include <algorithm>

// Same-version, fresh profile-2 fixture. No old program or state is involved.
// Opt-in export only: running this is NOT a throughput measurement.
BOOST_AUTO_TEST_SUITE(parallel_release_fixtures)
using namespace parallel_production_tests;

// A data-dependent arithmetic workload, not a sleep or an unused empty loop.
// Its checksum contributes to persistent state and is checked independently by
// the Python harness. Each action still performs a real DB read and write.
std::vector<uint8_t> release_compute_code() {
   return compile(R"((module
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_get_i64" (func $get (param i32 i32 i32) (result i32)))
      (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1)
      (func (export "apply") (param $receiver i64) (param i64 i64)
         (local $it i32) (local $work i32) (local $acc i64)
         (drop (call $read (i32.const 0) (i32.const 32)))
         (set_local $it (call $find (get_local $receiver) (i64.load (i32.const 0))
            (i64.const )" + std::to_string("rows"_n.to_uint64_t()) + R"() (i64.const 1)))
         (drop (call $get (get_local $it) (i32.const 64) (i32.const 8)))
         (set_local $acc (i64.load (i32.const 8)))
         (set_local $work (i32.load (i32.const 16)))
         (block $end (loop $loop
            (br_if $end (i32.eqz (get_local $work)))
            (set_local $acc (i64.xor (get_local $acc) (i64.shr_u (get_local $acc) (i64.const 12))))
            (set_local $acc (i64.xor (get_local $acc) (i64.shl (get_local $acc) (i64.const 25))))
            (set_local $acc (i64.xor (get_local $acc) (i64.shr_u (get_local $acc) (i64.const 27))))
            (set_local $acc (i64.mul (get_local $acc) (i64.const 2685821657736338717)))
            (set_local $work (i32.sub (get_local $work) (i32.const 1))) (br $loop)))
         (i64.store (i32.const 64) (i64.add (i64.load (i32.const 64))
            (i64.add (i64.const 1) (i64.and (get_local $acc) (i64.const 65535)))))
         (call $update (get_local $it) (i64.const 0) (i32.const 64) (i32.const 8))
         (call $return (i32.const 64) (i32.const 8)))))");
}

BOOST_AUTO_TEST_CASE(export_fresh_jit_throughput_fixture) {
   const auto* destination = std::getenv("PUREEOS_PARALLEL_RELEASE_EXPORT_DIR");
   if (!destination) { BOOST_TEST_MESSAGE("Release fixture export not requested"); return; }
   const auto bounded_setting = [](const char* key, unsigned fallback, unsigned maximum) {
      const auto* value = std::getenv(key);
      if (!value) return fallback;
      char* end = nullptr;
      const auto parsed = std::strtoul(value, &end, 10);
      BOOST_REQUIRE(end != value && *end == '\0' && parsed <= maximum);
      return static_cast<unsigned>(parsed);
   };
   const auto count = bounded_setting("PUREEOS_PARALLEL_RELEASE_TRANSACTIONS", 32768, 262144);
   const auto iterations = bounded_setting("PUREEOS_PARALLEL_RELEASE_WORK", 0, 10000000);
   const auto hot_percent = bounded_setting("PUREEOS_PARALLEL_RELEASE_HOT_PERCENT", 0, 100);
   const auto actions = bounded_setting("PUREEOS_PARALLEL_RELEASE_ACTIONS", 1, 100);
   const auto compute = bounded_setting("PUREEOS_PARALLEL_RELEASE_COMPUTE", 0, 1);
   const auto unique_scopes = bounded_setting("PUREEOS_PARALLEL_RELEASE_UNIQUE_SCOPES", 0, 1);
   const auto inline_actions = bounded_setting("PUREEOS_PARALLEL_RELEASE_INLINE_ACTIONS", 0, 100);
   const auto* diagnostic_cpu = std::getenv("PUREEOS_PARALLEL_RELEASE_TEST_BLOCK_CPU_US");
   if (diagnostic_cpu) {
      BOOST_REQUIRE(*diagnostic_cpu && std::all_of(diagnostic_cpu,
         diagnostic_cpu + std::strlen(diagnostic_cpu), [](char c) { return c >= '0' && c <= '9'; }));
   }
   // Test fixture only. Set BEFORE chain creation/signing: modifying an exported
   // genesis would invalidate the chain ID, bootstrap blocks and signed corpus.
   const auto block_cpu = bounded_setting("PUREEOS_PARALLEL_RELEASE_TEST_BLOCK_CPU_US",
      eosio::chain::config::default_max_block_cpu_usage, 800000);
   BOOST_REQUIRE_GE(block_cpu, 200000u);
   const auto* cdt_wasm = std::getenv("PUREEOS_PARALLEL_RELEASE_CDT_WASM");
   BOOST_REQUIRE_GE(count, 64u);
   BOOST_REQUIRE_GE(actions, 1u);
   BOOST_REQUIRE(!unique_scopes || count <= 8192);
   BOOST_REQUIRE(!unique_scopes || hot_percent == 0);
   BOOST_REQUIRE(inline_actions == 0 || (inline_actions == 100 && actions == 1 && cdt_wasm));
   BOOST_REQUIRE(!cdt_wasm || compute);
   BOOST_REQUIRE_LE(uint64_t(count) * actions * (inline_actions ? inline_actions + 1 : 1), 2000000u);
   const auto directory = std::filesystem::path(destination);
   BOOST_REQUIRE(std::filesystem::create_directory(directory));
   std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
   auto genesis = base_tester::default_genesis();
   const auto default_configuration = genesis.initial_configuration;
   genesis.initial_configuration.max_block_cpu_usage = block_cpu;
   genesis.initial_configuration.validate();
   // Leave room for bounded bootstrap blocks when every transaction gets its
   // own table scope. No bootstrap checkpoint is in the future at live start.
   genesis.initial_timestamp = block_timestamp_type(fc::time_point::now() -
      fc::seconds(5 + (unique_scopes ? (count + 63) / 64 : 0))).to_time_point();
   const auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->version = 2;
   genesis.initial_savanna->finalizer_key = public_key;
   genesis.initial_savanna->proof_of_possession = proof;
   BOOST_REQUIRE(fc::json::from_string(fc::json::to_string(genesis, fc::time_point::maximum()))
      .as<genesis_state>().compute_chain_id() == genesis.compute_chain_id());
   fc::temp_directory source_dir;
   auto config = base_tester::default_config(source_dir).first;
   config.wasm_runtime = wasm_interface::vm_type::eos_vm_jit;
   production_chain source(config, genesis);
   source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
   fc::variants checkpoints;
   const auto seal = [&] {
      const auto block = source.produce_block(); source.control->abort_block();
      const auto file = "block-" + std::to_string(block->block_num()) + ".json";
      BOOST_REQUIRE(fc::json::save_to_file(*block, directory / file));
      checkpoints.emplace_back(fc::mutable_variant_object()
         ("block_num", block->block_num())("block_id", source.head().id())
         ("lib", source.last_irreversible_block_num())
         ("integrity_hash", source.control->calculate_integrity_hash())("file", file));
   };
   source.create_accounts({"test"_n, "alice"_n}); seal();
   source.set_code("test"_n, initialize_code()); seal();
   if (inline_actions)
      source.set_authority("alice"_n, "active"_n, authority{1,
         {{source.get_public_key("alice"_n, "active"), 1}}, {{{"test"_n, eosio::chain::config::eosio_code_name}, 1}}});
   std::vector<name> scopes(unique_scopes ? count : 64);
   for (size_t i = 0; i < scopes.size(); ++i) {
      scopes[i] = unique_scopes ? name{std::string("row") + char('a' + i / 676) +
         char('a' + (i / 26) % 26) + char('a' + i % 26)} :
         name{std::string("row") + char('a' + i / 26) + char('a' + i % 26)};
      push(source, scopes[i], 10, i + 1);
      if ((i + 1) % 64 == 0) seal();
   }
   if (scopes.size() % 64) seal();
   auto code = compute ? release_compute_code() : update_code();
   if (cdt_wasm) {
      std::ifstream input(cdt_wasm, std::ios::binary);
      BOOST_REQUIRE(input.good());
      code.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
      BOOST_REQUIRE(!code.empty() && code.size() <= 256 * 1024);
   }
   source.set_code("test"_n, code);
   source.set_abi("test"_n, R"({"version":"eosio::abi/1.1","types":[],
      "structs":[{"name":"row","base":"","fields":[{"name":"value","type":"uint64"}]}],
      "actions":[],"tables":[{"name":"rows","index_type":"i64","key_names":[],"key_types":[],"type":"row"}],
      "ricardian_clauses":[],"error_messages":[],"abi_extensions":[]})");
   seal(); source.start();
   fc::variants transactions;
   for (unsigned i = 0; i < count; ++i) {
      const auto scope = scopes[(i % 100 < hot_percent) ? 0 : i % scopes.size()];
      auto trx = make_transaction(source, scope, 1, 1000000 + i, iterations);
      if (inline_actions) trx.actions.front().name = "batch"_n;
      trx.actions.resize(actions, trx.actions.front());
      trx.expiration = fc::time_point_sec(fc::time_point::now() + fc::seconds(1800));
      trx.signatures.clear();
      trx.sign(source.get_private_key("alice"_n, "active"), source.control->get_chain_id());
      const packed_transaction packed(trx, packed_transaction::compression_type::none);
      transactions.emplace_back(fc::mutable_variant_object()("id", packed.id())("packed", packed)
         ("actions", actions)("executed_actions", actions * (inline_actions ? inline_actions + 1 : 1))
         ("row_updates", actions * (inline_actions ? inline_actions : 1))("scope", scope.to_string()));
   }
   BOOST_REQUIRE(fc::json::save_to_file(genesis, directory / "genesis.json"));
   BOOST_REQUIRE(fc::json::save_to_file(transactions, directory / "transactions.json"));
   const std::vector<std::string> providers{
      base_tester::get_public_key("eosio"_n, "active").to_string({}) + "=KEY:" +
         base_tester::get_private_key("eosio"_n, "active").to_string({}),
      public_key.to_string() + "=KEY:" + private_key.to_string()};
   BOOST_REQUIRE(fc::json::save_to_file(providers, directory / "TEST-ONLY-keys.json"));
   const auto code_hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   BOOST_REQUIRE(fc::json::save_to_file(fc::mutable_variant_object()("format", 1)
      ("fixture_kind", "fresh-profile2-jit-row-update")("chain_id", genesis.compute_chain_id())
      ("code_hashes", std::vector{code_hash})("checkpoints", checkpoints)
      ("business_transactions", count)("loop_iterations", iterations)("hot_percent", hot_percent)
      ("actions_per_transaction", actions)("compute_checksum", bool(compute))
      ("unique_scopes", bool(unique_scopes))("inline_actions", inline_actions)("cdt_contract", bool(cdt_wasm))
      ("initial_row_value", 10)("scopes", std::vector<name>(scopes.begin(), scopes.end()))
      ("test_capacity_diagnostic", diagnostic_cpu != nullptr)
      ("default_initial_configuration", default_configuration)
      ("resource_policy", diagnostic_cpu
         ? "TEST ONLY block CPU override; unchanged per-tx CPU, NET and billing; no system-contract account quota"
         : "unchanged genesis block CPU/NET; no system-contract account quota"),
      directory / "manifest.json"));
}
BOOST_AUTO_TEST_SUITE_END()
