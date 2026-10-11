#include <eosio/chain/parallel_canonical_adoption.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/chain/merkle.hpp>
#include <eosio/chain/snapshot.hpp>
#include <eosio/chain/transaction_context.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/performance_metrics.hpp>
#include <eosio/testing/tester.hpp>
#include <eosio/testing/bls_utils.hpp>
#include <boost/test/unit_test.hpp>
#include <fc/io/json.hpp>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace eosio::chain;
using namespace eosio::testing;

namespace {
std::vector<uint8_t> compile(const std::string& wat) { return wast_to_wasm(wat); }

std::vector<uint8_t> cdt_fixture_code() {
   const auto fixture = std::filesystem::path(__FILE__).parent_path() / "test-contracts/parallel_cdt/parallel_cdt.wasm";
   std::ifstream stream(fixture, std::ios::binary);
   BOOST_REQUIRE_MESSAGE(stream.good(), "missing versioned CDT fixture: " << fixture);
   const std::vector<uint8_t> code{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
   BOOST_REQUIRE(!code.empty());
   BOOST_REQUIRE(fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size()) ==
      fc::sha256{"0365f466b059f6985baa7a1aa1415e59139e58bdb10344a9c457b59909fe8de8"});
   return code;
}

std::vector<uint8_t> initialize_code(bool keyed = false) {
   return compile(R"((module
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32) (result i32)))
      (memory 1 1)
      (func (export "apply") (param i64 i64 i64)
         (drop (call $read (i32.const 0) (i32.const )" + std::to_string(keyed ? 40 : 32) + R"()))
         (drop (call $store (i64.load (i32.const 0)) (i64.const )" + std::to_string("rows"_n.to_uint64_t()) + R"()
            (i64.const )" + std::to_string("alice"_n.to_uint64_t()) + ") " +
            (keyed ? "(i64.load (i32.const 32))" : "(i64.const 1)") + R"( (i32.const 8) (i32.const 8))))))");
}
std::vector<uint8_t> update_code(bool extra = false, bool keyed = false) {
   return compile(R"((module
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_get_i64" (func $get (param i32 i32 i32) (result i32)))
      (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1)
      (func (export "apply") (param $receiver i64) (param i64 i64)
         (local $it i32) (local $work i32)
         (drop (call $read (i32.const 0) (i32.const )" + std::to_string(keyed ? 40 : 32) + R"()))
         (set_local $it (call $find (get_local $receiver) (i64.load (i32.const 0))
            (i64.const )" + std::to_string("rows"_n.to_uint64_t()) + ") " +
            (keyed ? "(i64.load (i32.const 32))" : "(i64.const 1)") + R"())
         (drop (call $get (get_local $it) (i32.const 64) (i32.const 8)))
         (i64.store (i32.const 64) (i64.add (i64.load (i32.const 64)) (i64.load (i32.const 8))))
         )" + (extra ? "(i64.store (i32.const 64) (i64.add (i64.load (i32.const 64)) (i64.const 1)))" : "") + R"(
         (if (i64.lt_s (i64.load (i32.const 64)) (i64.const 0)) (then (unreachable)))
         (set_local $work (i32.load (i32.const 16)))
         (block $end (loop $loop
            (br_if $end (i32.eqz (get_local $work)))
            (set_local $work (i32.sub (get_local $work) (i32.const 1))) (br $loop)))
         (call $update (get_local $it) (i64.const 0) (i32.const 64) (i32.const 8))
         (call $return (i32.const 64) (i32.const 8)))))");
}
void push(tester& chain, name scope, uint64_t delta, uint64_t nonce, uint64_t work = 0,
          std::optional<uint64_t> primary = std::nullopt, name action_name = "run"_n) {
   signed_transaction trx;
   action act; act.account = "test"_n; act.name = action_name; act.authorization = {{"alice"_n, "active"_n}};
   const std::array<uint64_t, 4> data{scope.to_uint64_t(), delta, work, nonce};
   act.data.assign(reinterpret_cast<const char*>(data.data()), reinterpret_cast<const char*>(data.data()) + sizeof(data));
   if (primary) {
      const auto* bytes = reinterpret_cast<const char*>(&*primary);
      act.data.insert(act.data.end(), bytes, bytes + sizeof(*primary));
   }
   trx.actions.push_back(std::move(act)); chain.set_transaction_headers(trx);
   trx.sign(chain.get_private_key("alice"_n, "active"), chain.control->get_chain_id());
   BOOST_REQUIRE(!chain.push_transaction(trx)->except);
}
std::vector<char> consensus_trace(const transaction_trace_ptr& trace) {
   std::vector<std::vector<char>> fields;
   fields.push_back(fc::raw::pack(trace->id));
   if (trace->receipt) fields.push_back(fc::raw::pack(*trace->receipt));
   fields.push_back(fc::raw::pack(trace->net_usage));
   for (const auto& action : trace->action_traces) {
      if (action.receipt) fields.push_back(fc::raw::pack(*action.receipt));
      fields.push_back(action.return_value);
      fields.push_back(fc::raw::pack(action.account_ram_deltas));
      fields.emplace_back(action.console.begin(), action.console.end());
   }
   return fc::raw::pack(fields);
}
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_validation_tests)

struct validation_corpus {
   genesis_state genesis = base_tester::default_genesis();
   const std::vector<uint8_t> code = update_code(), replacement = update_code(true);
   const fc::sha256 code_hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   const fc::sha256 replacement_hash = fc::sha256::hash(reinterpret_cast<const char*>(replacement.data()), replacement.size());
   std::vector<std::vector<char>> corpus;
   fc::temp_directory source_dir;
   controller::config source_config;

   validation_corpus() {
   auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->finalizer_key = public_key; genesis.initial_savanna->proof_of_possession = proof;
   {
      auto config = base_tester::default_config(source_dir).first;
      config.wasm_runtime = wasm_interface::vm_type::eos_vm;
      source_config = config;
      tester source(config, genesis);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      auto seal = [&] { corpus.push_back(fc::raw::pack(*source.produce_block())); };
      source.create_accounts({"test"_n, "alice"_n, "bob"_n, "carol"_n, "dave"_n}); seal();
      source.set_code("test"_n, initialize_code()); seal();
      uint64_t nonce = 1;
      for (auto scope : {"alice"_n, "bob"_n, "carol"_n, "dave"_n}) push(source, scope, 10, nonce++);
      seal(); source.set_code("test"_n, code); seal();
      for (auto scope : {"alice"_n, "bob"_n, "carol"_n, "dave"_n}) push(source, scope, 7, nonce++, 20000);
      seal();
      push(source, "alice"_n, 3, nonce++); push(source, "alice"_n, 5, nonce++);
      push(source, "bob"_n, 9, nonce++); seal();
      // Second private execution traps on the OLD balance (25 - 30), but must
      // succeed canonically after the first has credited 10 (35 - 30 = 5).
      push(source, "alice"_n, 10, nonce++);
      push(source, "alice"_n, uint64_t{0} - 30, nonce++); seal();
      // Entirely hot batch: one adopted result, then a bounded serial tail.
      for (unsigned i = 0; i < 6; ++i) push(source, "alice"_n, 1, nonce++);
      seal();
      // A native setcode is a serial barrier; later work must use the new code.
      push(source, "carol"_n, 2, nonce++); source.set_code("test"_n, replacement);
      push(source, "dave"_n, 2, nonce++); seal();
      for (unsigned i = 0; i < 5; ++i) {
         if (i == 1) { // real writes after the snapshot/restart checkpoint
            push(source, "bob"_n, 4, nonce++); push(source, "dave"_n, 3, nonce++);
         }
         seal();
      }
   }
   }

   controller::config configuration(const fc::temp_directory& directory, uint16_t workers) const {
      auto result = base_tester::default_config(directory).first;
      result.wasm_runtime = wasm_interface::vm_type::eos_vm;
      result.parallel_validation_threads = workers;
      result.parallel_code_allowlist = {code_hash, replacement_hash};
      return result;
   }
   void apply(tester& chain, size_t index) const {
      chain.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(corpus.at(index))));
      chain.control->abort_block();
   }
   void prefix(tester& chain, size_t count) const {
      for (size_t i = 0; i < count; ++i) apply(chain, i);
   }
};

BOOST_FIXTURE_TEST_CASE(native_savanna_parallel_adoption_matches_original_at_every_block_prefix, validation_corpus) {
   // Explicit opt-in export for a DIFFERENT, pre-change nodeos binary. Never
   // overwrite a caller's directory or export signing credentials / databases.
   std::optional<std::filesystem::path> export_directory;
   fc::variants checkpoints;
   if (const char* destination = std::getenv("PUREEOS_PARALLEL_EXPORT_DIR")) {
      export_directory = std::filesystem::path(destination);
      BOOST_REQUIRE(std::filesystem::create_directory(*export_directory));
      BOOST_REQUIRE(fc::json::save_to_file(genesis, *export_directory / "genesis.json"));
      std::filesystem::create_directory(*export_directory / "blocks");
      for (const char* file : {"blocks.log", "blocks.index"})
         std::filesystem::copy_file(source_config.blocks_dir / file, *export_directory / "blocks" / file);
   }
   for (uint16_t workers : {0, 1, 2, 4}) {
      fc::temp_directory reference_dir, observed_dir;
      auto reference_config = base_tester::default_config(reference_dir).first;
      reference_config.wasm_runtime = wasm_interface::vm_type::eos_vm;
      tester reference(reference_config, genesis);
      auto observed_config = base_tester::default_config(observed_dir).first;
      observed_config.wasm_runtime = wasm_interface::vm_type::eos_vm;
      observed_config.parallel_validation_threads = workers;
      observed_config.parallel_code_allowlist = {code_hash, replacement_hash};
      tester observed(observed_config, genesis);
      std::vector<std::vector<char>> expected_traces, actual_traces;
      auto expected_connection = reference.control->applied_transaction().connect([&](const auto& event) {
         expected_traces.push_back(consensus_trace(std::get<0>(event)));
      });
      auto actual_connection = observed.control->applied_transaction().connect([&](const auto& event) {
         actual_traces.push_back(consensus_trace(std::get<0>(event)));
      });
      for (const auto& block : corpus) {
         expected_traces.clear(); actual_traces.clear();
         reference.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(block)));
         observed.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(block)));
         reference.control->abort_block(); observed.control->abort_block();
         BOOST_CHECK(reference.control->head().id() == observed.control->head().id());
         BOOST_CHECK_EQUAL(reference.last_irreversible_block_num(), observed.last_irreversible_block_num());
         BOOST_CHECK(reference.control->calculate_integrity_hash() == observed.control->calculate_integrity_hash());
         BOOST_CHECK(expected_traces == actual_traces);
         if (export_directory && workers == 0) {
            const auto number = reference.control->head().block_num();
            const auto file = "block-" + std::to_string(number) + ".json";
            BOOST_REQUIRE(fc::json::save_to_file(fc::raw::unpack<signed_block>(block), *export_directory / file));
            checkpoints.emplace_back(fc::mutable_variant_object()
               ("block_num", number)("block_id", reference.control->head().id())
               ("lib", reference.last_irreversible_block_num())
               ("integrity_hash", reference.control->calculate_integrity_hash())("file", file));
         }
      }
      const auto stats = observed.control->get_parallel_execution_stats();
      if (workers) {
         BOOST_CHECK_GE(stats.batches, 4u);
         BOOST_CHECK_GE(stats.adopted, 7u);
         BOOST_CHECK_GE(stats.serial_fallbacks, 2u);
         BOOST_CHECK_GE(stats.succeeded, stats.adopted);
         BOOST_CHECK_GT(stats.attempts, stats.succeeded); // failed speculation is not final rejection
      } else BOOST_CHECK_EQUAL(stats.attempts, 0u);
      BOOST_TEST_MESSAGE("validation threads=" << workers << " attempts=" << stats.attempts
         << " adopted=" << stats.adopted << " serial=" << stats.serial_fallbacks);
      for (const auto& [scope, expected] : std::array<std::pair<name, uint64_t>, 4>{
         std::pair{"alice"_n, 11}, {"bob"_n, 31}, {"carol"_n, 19}, {"dave"_n, 24}}) {
         const auto data = observed.get_row_by_account("test"_n, scope, "rows"_n, name{1});
         BOOST_REQUIRE_EQUAL(data.size(), 8u);
         uint64_t value; std::memcpy(&value, data.data(), sizeof(value)); BOOST_CHECK_EQUAL(value, expected);
      }
   }
   if (export_directory) {
      // Also archive a reversible competing branch, with hashes calculated by
      // original serial execution. A process test can now test REAL fork undo
      // against the old executable rather than generating a different fork.
      fc::temp_directory first_dir, second_dir, reference_dir;
      tester first(configuration(first_dir, 0), genesis), second(configuration(second_dir, 0), genesis);
      tester reference(configuration(reference_dir, 0), genesis);
      prefix(first, 9); prefix(second, 9); prefix(reference, 9);
      fc::variants forks;
      auto export_fork = [&](const signed_block_ptr& block) {
         reference.push_block(block); reference.control->abort_block();
         BOOST_REQUIRE(reference.head().id() == block->calculate_id());
         const auto file = "fork-" + std::to_string(forks.size()) + ".json";
         BOOST_REQUIRE(fc::json::save_to_file(*block, *export_directory / file));
         forks.emplace_back(fc::mutable_variant_object()
            ("block_num", reference.head().block_num())("block_id", reference.head().id())
            ("lib", reference.last_irreversible_block_num())
            ("integrity_hash", reference.control->calculate_integrity_hash())("file", file));
      };
      push(first, "alice"_n, 100, 10001); push(first, "carol"_n, 100, 10002);
      export_fork(first.produce_block());
      push(second, "alice"_n, 200, 20001); push(second, "bob"_n, 2, 20002);
      export_fork(second.produce_block(fc::milliseconds(1000)));
      push(second, "carol"_n, 3, 20003); export_fork(second.produce_block());
      const fc::variant manifest = fc::mutable_variant_object()
         ("format", 1)("chain_id", genesis.compute_chain_id())
         ("code_hashes", std::vector<fc::sha256>{code_hash, replacement_hash})
         ("checkpoints", checkpoints)("fork_prefix_blocks", 9)("forks", forks);
      BOOST_REQUIRE(fc::json::save_to_file(manifest, *export_directory / "manifest.json"));
      BOOST_TEST_MESSAGE("Exported immutable business corpus to " << export_directory->string());
   }
}

BOOST_FIXTURE_TEST_CASE(invalid_receipts_authorization_duplicate_and_wasm_failure_undo_adopted_prefix, validation_corpus) {
   for (uint16_t workers : {1, 2, 4}) for (unsigned mutation = 0; mutation < 7; ++mutation) {
      BOOST_TEST_CONTEXT("workers=" << workers << " mutation=" << mutation) {
         fc::temp_directory reference_dir, observed_dir;
         tester reference(configuration(reference_dir, 0), genesis);
         tester observed(configuration(observed_dir, workers), genesis);
         prefix(reference, 4); prefix(observed, 4);
         const auto parent = observed.control->head().id();
         const auto before = observed.control->calculate_integrity_hash();
         auto bad = std::make_unique<signed_block>(fc::raw::unpack<signed_block>(corpus[4]));
         BOOST_REQUIRE_EQUAL(bad->transactions.size(), 4u);
         auto& last = bad->transactions.back();
         switch (mutation) {
            case 0: last.status = transaction_receipt_header::hard_fail; break;
            case 1: ++last.net_usage_words.value; break;
            case 2: last.cpu_usage_us = 0; break;
            case 3: {
               auto trx = std::get<packed_transaction>(last.trx).get_signed_transaction();
               trx.signatures.clear();
               last.trx = packed_transaction(trx, packed_transaction::compression_type::none);
               break;
            }
            case 4: {
               auto trx = std::get<packed_transaction>(last.trx).get_signed_transaction();
               const uint64_t negative = uint64_t{0} - 100;
               std::memcpy(trx.actions[0].data.data() + 8, &negative, sizeof(negative));
               trx.signatures.clear();
               trx.sign(reference.get_private_key("alice"_n, "active"), genesis.compute_chain_id());
               last.trx = packed_transaction(trx, packed_transaction::compression_type::none);
               break;
            }
            case 5: bad->transactions[1] = transaction_receipt(bad->transactions.front()); break;
            case 6: {
               // Both private withdrawals see balance 10 and independently
               // succeed (10-7). In canonical order the SECOND must fail; do
               // not adopt its stale result and spend the same balance twice.
               for (unsigned i = 0; i < 2; ++i) {
                  auto trx = std::get<packed_transaction>(bad->transactions[i].trx).get_signed_transaction();
                  const uint64_t scope = "alice"_n.to_uint64_t(), negative = uint64_t{0} - 7;
                  std::memcpy(trx.actions[0].data.data(), &scope, sizeof(scope));
                  std::memcpy(trx.actions[0].data.data() + 8, &negative, sizeof(negative));
                  trx.signatures.clear();
                  trx.sign(reference.get_private_key("alice"_n, "active"), genesis.compute_chain_id());
                  bad->transactions[i].trx = packed_transaction(trx, packed_transaction::compression_type::none);
               }
               break;
            }
         }
         std::vector<digest_type> digests;
         for (const auto& receipt : bad->transactions) digests.push_back(receipt.digest());
         bad->transaction_mroot = calculate_merkle(std::move(digests));
         // Savanna signs block ID. Re-sign the altered body commitment, so
         // rejection must happen during execution, not a trivial bad signature.
         bad->producer_signature = reference.get_private_key("eosio"_n, "active").sign(bad->calculate_id());
         const auto invalid = signed_block::create_signed_block(std::move(bad));
         auto reject = [&](tester& chain) {
            int64_t error = 0;
            try { chain.push_block(invalid); }
            catch (const fc::exception& e) { error = e.code(); }
            chain.control->abort_block();
            return error;
         };
         const auto expected = reject(reference), actual = reject(observed);
         BOOST_CHECK_NE(expected, 0);
         BOOST_CHECK_EQUAL(actual, expected);
         BOOST_CHECK(observed.control->head().id() == parent);
         BOOST_CHECK(observed.control->calculate_integrity_hash() == before);
         BOOST_CHECK(reference.control->calculate_integrity_hash() == before);
         BOOST_CHECK_GT(observed.control->get_parallel_execution_stats().adopted, 0u);
         if (mutation == 6) BOOST_CHECK_EQUAL(observed.control->get_parallel_execution_stats().succeeded, 4u);
         // A rejected batch must not leave IDs, writes, receipts or tickets
         // behind. The original valid block still works at this exact prefix.
         apply(reference, 4); apply(observed, 4);
         BOOST_CHECK(reference.control->head().id() == observed.control->head().id());
         BOOST_CHECK(reference.control->calculate_integrity_hash() == observed.control->calculate_integrity_hash());
      }
   }
}

BOOST_FIXTURE_TEST_CASE(snapshot_restart_and_checked_replay_keep_parallel_state_reconstructible, validation_corpus) {
   for (uint16_t workers : {1, 2, 4}) {
      fc::temp_directory directory, restore_dir, replay_dir;
      auto config = configuration(directory, workers);
      tester chain(config, genesis);
      prefix(chain, 9);
      BOOST_CHECK_GT(chain.control->get_parallel_execution_stats().adopted, 0u);
      const auto snapshot_id = chain.control->head().id();
      const auto snapshot_hash = chain.control->calculate_integrity_hash();
      std::ostringstream bytes;
      auto writer = std::make_shared<ostream_snapshot_writer>(bytes);
      chain.control->write_snapshot(writer); writer->finalize();
      std::istringstream input(bytes.str());
      auto reader = std::make_shared<istream_snapshot_reader>(input);
      auto restore_config = configuration(restore_dir, workers);
      tester restored(restore_config, genesis, call_startup_t::no);
      restored.close(); restored.init(restore_config, reader);
      restored.control->abort_block();
      BOOST_CHECK(restored.control->head().id() == snapshot_id);
      BOOST_CHECK(restored.control->calculate_integrity_hash() == snapshot_hash);
      chain.close(); chain.open(); chain.control->abort_block();
      BOOST_CHECK(chain.control->head().id() == snapshot_id);
      BOOST_CHECK(chain.control->calculate_integrity_hash() == snapshot_hash);
      for (size_t i = 9; i < corpus.size(); ++i) {
         apply(chain, i); apply(restored, i);
         BOOST_CHECK(chain.control->head().id() == restored.control->head().id());
         BOOST_CHECK(chain.control->calculate_integrity_hash() == restored.control->calculate_integrity_hash());
      }
      BOOST_CHECK_GT(chain.control->get_parallel_execution_stats().adopted, 0u);
      BOOST_CHECK_GT(restored.control->get_parallel_execution_stats().adopted, 0u);
      const auto lib = chain.last_irreversible_block_num();
      const auto lib_id = chain.last_irreversible_block_id();
      chain.close();
      auto replay_config = configuration(replay_dir, workers);
      replay_config.disable_replay_opts = true;
      replay_config.force_all_checks = true;
      std::filesystem::create_directories(replay_config.blocks_dir);
      for (const char* filename : {"blocks.log", "blocks.index"})
         std::filesystem::copy_file(config.blocks_dir / filename, replay_config.blocks_dir / filename);
      tester replay(replay_config, genesis);
      replay.control->abort_block();
      BOOST_CHECK_EQUAL(replay.head().block_num(), lib);
      BOOST_CHECK(replay.head().id() == lib_id);
      BOOST_CHECK_GT(replay.control->get_parallel_execution_stats().adopted, 0u);
      fc::temp_directory reference_dir;
      tester reference(configuration(reference_dir, 0), genesis);
      prefix(reference, lib - 1); // native genesis is block 1
      BOOST_CHECK(reference.control->calculate_integrity_hash() == replay.control->calculate_integrity_hash());
   }
}

BOOST_FIXTURE_TEST_CASE(fork_switch_discards_adopted_old_branch_and_reexecutes_new_prefix, validation_corpus) {
   for (uint16_t workers : {1, 2, 4}) {
      fc::temp_directory first_dir, second_dir, reference_dir, observed_dir;
      tester first(configuration(first_dir, 0), genesis), second(configuration(second_dir, 0), genesis);
      tester reference(configuration(reference_dir, 0), genesis), observed(configuration(observed_dir, workers), genesis);
      // No local finalizers on the branch builders: keep the split reversible.
      prefix(first, 9); prefix(second, 9); prefix(reference, 9); prefix(observed, 9);
      std::vector<std::vector<char>> expected_traces, actual_traces;
      auto c1 = reference.control->applied_transaction().connect([&](const auto& event) {
         expected_traces.push_back(consensus_trace(std::get<0>(event)));
      });
      auto c2 = observed.control->applied_transaction().connect([&](const auto& event) {
         actual_traces.push_back(consensus_trace(std::get<0>(event)));
      });
      auto receive = [&](const signed_block_ptr& block) {
         expected_traces.clear(); actual_traces.clear();
         reference.push_block(block); observed.push_block(block);
         reference.control->abort_block(); observed.control->abort_block();
         BOOST_CHECK(reference.head().id() == observed.head().id());
         BOOST_CHECK_EQUAL(reference.last_irreversible_block_num(), observed.last_irreversible_block_num());
         BOOST_CHECK(reference.control->calculate_integrity_hash() == observed.control->calculate_integrity_hash());
         BOOST_CHECK(expected_traces == actual_traces);
      };
      const auto before = observed.control->get_parallel_execution_stats().adopted;
      push(first, "alice"_n, 100, 10001); push(first, "carol"_n, 100, 10002);
      const auto old_branch = first.produce_block(); receive(old_branch);
      BOOST_CHECK(observed.head().id() == old_branch->calculate_id());
      BOOST_CHECK_GT(observed.control->get_parallel_execution_stats().adopted, before);
      push(second, "alice"_n, 200, 20001); push(second, "bob"_n, 2, 20002);
      // With identical QC progress, a later slot selects the competing branch.
      const auto new_branch = second.produce_block(fc::milliseconds(1000)); receive(new_branch);
      push(second, "carol"_n, 3, 20003); receive(second.produce_block());
      BOOST_CHECK(observed.head().id() == second.head().id());
      BOOST_CHECK_GT(observed.control->get_parallel_execution_stats().adopted, before + 2);
      const auto old_id = std::get<packed_transaction>(old_branch->transactions.front().trx).id();
      BOOST_CHECK(!observed.control->is_known_unexpired_transaction(old_id));
      for (const auto& [scope, expected] : std::array<std::pair<name, uint64_t>, 4>{
         std::pair{"alice"_n, 212}, {"bob"_n, 29}, {"carol"_n, 23}, {"dave"_n, 20}}) {
         const auto data = observed.get_row_by_account("test"_n, scope, "rows"_n, name{1});
         BOOST_REQUIRE_EQUAL(data.size(), 8u);
         uint64_t value; std::memcpy(&value, data.data(), 8); BOOST_CHECK_EQUAL(value, expected);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(local_allowlist_light_validation_and_batch_bounds_never_change_state, validation_corpus) {
   fc::temp_directory reference_dir;
   tester reference(configuration(reference_dir, 0), genesis);
   prefix(reference, corpus.size());
   const auto expected = reference.control->calculate_integrity_hash();
   for (unsigned mode = 0; mode < 5; ++mode) {
      fc::temp_directory directory;
      auto config = configuration(directory, 4);
      if (mode == 0) config.parallel_code_allowlist.clear();
      if (mode == 1) config.parallel_code_allowlist = {fc::sha256::hash("not deployed")};
      if (mode == 2) config.block_validation_mode = validation_mode::LIGHT;
      if (mode == 3) config.parallel_validation_batch_size = 1;
      if (mode == 4) config.parallel_validation_batch_size = 64;
      tester chain(config, genesis);
      prefix(chain, corpus.size());
      BOOST_CHECK(chain.control->calculate_integrity_hash() == expected);
      const auto stats = chain.control->get_parallel_execution_stats();
      if (mode < 3) BOOST_CHECK_EQUAL(stats.attempts, 0u);
      else BOOST_CHECK_GT(stats.adopted, 0u);
   }
}

BOOST_FIXTURE_TEST_CASE(json_blocks_finalized_before_publication_remain_restartable, validation_corpus) {
   fc::temp_directory reference_dir, observed_dir;
   tester reference(configuration(reference_dir, 0), genesis);
   tester observed(configuration(observed_dir, 2), genesis);
   for (const auto& bytes : corpus) {
      const auto original = fc::raw::unpack<signed_block>(bytes);
      // Same representation arriving at chain_api push_block. Reflection does
      // not initialize the private serialized cache; the factory MUST do it.
      auto json_block = fc::json::from_string(fc::json::to_string(original, fc::time_point::maximum())).as<signed_block>();
      auto published = signed_block::create_signed_block(std::make_unique<signed_block>(std::move(json_block)));
      BOOST_CHECK(published->packed_signed_block() == bytes);
      reference.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(bytes)));
      observed.push_block(published);
      reference.control->abort_block(); observed.control->abort_block();
      BOOST_CHECK(reference.head().id() == observed.head().id());
      BOOST_CHECK(reference.control->calculate_integrity_hash() == observed.control->calculate_integrity_hash());
   }
   const auto id = observed.head().id(), hash = observed.control->calculate_integrity_hash();
   BOOST_CHECK_GT(observed.control->get_parallel_execution_stats().adopted, 0u);
   observed.close(); observed.open(); observed.control->abort_block();
   BOOST_CHECK(observed.head().id() == id);
   BOOST_CHECK(observed.control->calculate_integrity_hash() == hash);
}

BOOST_FIXTURE_TEST_CASE(parallel_diagnostics_do_not_change_consensus_and_count_real_conflicts, validation_corpus) {
   const auto saved = performance::metrics().enabled.exchange(false);
   auto restore = fc::make_scoped_exit([&] { performance::metrics().enabled = saved; });
   fc::temp_directory off_dir, on_dir;
   tester off(configuration(off_dir, 4), genesis), on(configuration(on_dir, 4), genesis);
   const auto disabled_before = performance::metrics().snapshot(performance::stage::parallel_capture).count;
   prefix(off, corpus.size());
   BOOST_CHECK_EQUAL(performance::metrics().snapshot(performance::stage::parallel_capture).count, disabled_before);
   const auto conflicts_before = performance::metrics().snapshot(performance::stage::parallel_conflict).count;
   performance::metrics().enabled = true;
   prefix(on, corpus.size());
   BOOST_CHECK(off.control->calculate_integrity_hash() == on.control->calculate_integrity_hash());
   BOOST_CHECK_EQUAL(off.control->get_parallel_execution_stats().adopted, on.control->get_parallel_execution_stats().adopted);
   for (auto stage : {performance::stage::parallel_capture, performance::stage::parallel_batch_wait,
          performance::stage::parallel_compile, performance::stage::parallel_execute, performance::stage::parallel_adopt,
          performance::stage::parallel_fallback})
      BOOST_CHECK_GT(performance::metrics().snapshot(stage).count, 0u);
   BOOST_CHECK_GT(performance::metrics().snapshot(performance::stage::parallel_conflict).count, conflicts_before);
}

BOOST_AUTO_TEST_CASE(same_table_disjoint_rows_adopt_and_shared_balances_fall_back_in_real_savanna_blocks) {
   auto genesis = base_tester::default_genesis();
   auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->finalizer_key = public_key;
   genesis.initial_savanna->proof_of_possession = proof;
   const auto code = update_code(false, true);
   const auto code_hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   std::vector<std::vector<char>> blocks;
   {
      fc::temp_directory directory;
      auto config = base_tester::default_config(directory).first;
      config.wasm_runtime = wasm_interface::vm_type::eos_vm;
      tester source(config, genesis);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      auto seal = [&] { blocks.push_back(fc::raw::pack(*source.produce_block())); };
      source.create_accounts({"test"_n, "alice"_n}); seal();
      source.set_code("test"_n, initialize_code(true)); seal();
      uint64_t nonce = 1;
      for (uint64_t key : {1, 2, 3}) push(source, "alice"_n, 10, nonce++, 0, key);
      seal(); source.set_code("test"_n, code); seal();
      for (uint64_t key : {1, 2, 3}) push(source, "alice"_n, 7, nonce++, 20000, key);
      seal();
      push(source, "alice"_n, 3, nonce++, 0, 1);
      push(source, "alice"_n, 5, nonce++, 0, 1);
      push(source, "alice"_n, 9, nonce++, 0, 3); seal();
      // Old private balance 25 cannot pay 30. Canonical preceding credit makes
      // it possible; a worker failure MUST NOT be a final rejection.
      push(source, "alice"_n, 10, nonce++, 0, 1);
      push(source, "alice"_n, uint64_t{0} - 30, nonce++, 0, 1); seal();
      for (unsigned i = 0; i < 5; ++i) seal();
   }
   for (uint16_t workers : {0, 1, 2, 4}) {
      fc::temp_directory reference_dir, observed_dir;
      auto reference_config = base_tester::default_config(reference_dir).first;
      reference_config.wasm_runtime = wasm_interface::vm_type::eos_vm;
      auto observed_config = base_tester::default_config(observed_dir).first;
      observed_config.wasm_runtime = wasm_interface::vm_type::eos_vm;
      observed_config.parallel_validation_threads = workers;
      observed_config.parallel_code_allowlist = {code_hash};
      tester reference(reference_config, genesis), observed(observed_config, genesis);
      std::vector<std::vector<char>> expected_traces, actual_traces;
      auto expected_connection = reference.control->applied_transaction().connect([&](const auto& event) {
         expected_traces.push_back(consensus_trace(std::get<0>(event)));
      });
      auto actual_connection = observed.control->applied_transaction().connect([&](const auto& event) {
         actual_traces.push_back(consensus_trace(std::get<0>(event)));
      });
      for (size_t index = 0; index < blocks.size(); ++index) {
         expected_traces.clear(); actual_traces.clear();
         reference.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(blocks[index])));
         observed.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(blocks[index])));
         reference.control->abort_block(); observed.control->abort_block();
         BOOST_CHECK(reference.head().id() == observed.head().id());
         BOOST_CHECK_EQUAL(reference.last_irreversible_block_num(), observed.last_irreversible_block_num());
         BOOST_CHECK(reference.control->calculate_integrity_hash() == observed.control->calculate_integrity_hash());
         BOOST_CHECK(expected_traces == actual_traces);
         if (index == 4 && workers) {
            const auto stats = observed.control->get_parallel_execution_stats();
            BOOST_CHECK_EQUAL(stats.adopted, 3u);
            BOOST_CHECK_EQUAL(stats.serial_fallbacks, 0u);
         }
      }
      const auto stats = observed.control->get_parallel_execution_stats();
      BOOST_CHECK_EQUAL(stats.attempts, workers ? 16u : 0u); // discovery + complete execution
      BOOST_CHECK_EQUAL(stats.adopted, workers ? 5u : 0u);
      BOOST_CHECK_EQUAL(stats.serial_fallbacks, workers ? 3u : 0u);
      for (const auto& [key, expected] : std::array<std::pair<uint64_t, uint64_t>, 3>{{{1, 5}, {2, 17}, {3, 26}}}) {
         const auto value = observed.get_row_by_account("test"_n, "alice"_n, "rows"_n, name{key});
         BOOST_REQUIRE_EQUAL(value.size(), 8u);
         uint64_t actual; std::memcpy(&actual, value.data(), sizeof(actual));
         BOOST_CHECK_EQUAL(actual, expected);
      }
   }
}

BOOST_AUTO_TEST_CASE(sparse_discovery_ignores_unrelated_scopes_and_exhaustion_preserves_serial_acceptance) {
   auto genesis = base_tester::default_genesis();
   auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->finalizer_key = public_key;
   genesis.initial_savanna->proof_of_possession = proof;
   const auto code = update_code(false, true);
   const auto probe = compile(R"((module
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (func (export "apply") (param $receiver i64) (param i64 i64) (local $key i64)
         (set_local $key (i64.const 1))
         (loop $next
            (drop (call $find (get_local $receiver) (i64.const )" + std::to_string("alice"_n.to_uint64_t()) +
            R"() (i64.const )" + std::to_string("rows"_n.to_uint64_t()) + R"() (get_local $key)))
            (set_local $key (i64.add (get_local $key) (i64.const 1)))
            (br_if $next (i64.le_u (get_local $key) (i64.const 9)))))))");
   const auto code_hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   const auto probe_hash = fc::sha256::hash(reinterpret_cast<const char*>(probe.data()), probe.size());
   std::vector<std::vector<char>> blocks;
   {
      fc::temp_directory directory;
      auto config = base_tester::default_config(directory).first;
      config.wasm_runtime = wasm_interface::vm_type::eos_vm;
      tester source(config, genesis);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      auto seal = [&] { blocks.push_back(fc::raw::pack(*source.produce_block())); };
      source.create_accounts({"test"_n, "alice"_n}); seal();
      source.set_code("test"_n, initialize_code(true)); seal();
      uint64_t nonce = 1;
      for (uint64_t scope = 200; scope < 280; ++scope) {
         push(source, name{scope}, 10, nonce++, 0, 1);
         if (scope % 10 == 9) seal();
      }
      for (uint64_t key : {1, 2, 3}) push(source, "alice"_n, 10, nonce++, 0, key);
      seal(); source.set_code("test"_n, code); seal();
      for (uint64_t key : {1, 2, 3}) push(source, "alice"_n, 7, nonce++, 0, key);
      seal(); source.set_code("test"_n, probe); seal();
      push(source, "alice"_n, 0, nonce++); seal();
      for (unsigned i = 0; i < 5; ++i) seal();
   }
   for (uint16_t workers : {0, 1, 2, 4}) {
      fc::temp_directory reference_dir, observed_dir;
      auto reference_config = base_tester::default_config(reference_dir).first;
      reference_config.wasm_runtime = wasm_interface::vm_type::eos_vm;
      auto config = base_tester::default_config(observed_dir).first;
      config.wasm_runtime = wasm_interface::vm_type::eos_vm;
      config.parallel_validation_threads = workers;
      config.parallel_code_allowlist = {code_hash, probe_hash};
      tester reference(reference_config, genesis), observed(config, genesis);
      std::vector<std::vector<char>> expected_traces, actual_traces;
      auto first = reference.control->applied_transaction().connect([&](const auto& event) {
         expected_traces.push_back(consensus_trace(std::get<0>(event)));
      });
      auto second = observed.control->applied_transaction().connect([&](const auto& event) {
         actual_traces.push_back(consensus_trace(std::get<0>(event)));
      });
      for (const auto& block : blocks) {
         expected_traces.clear(); actual_traces.clear();
         reference.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(block)));
         observed.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(block)));
         reference.control->abort_block(); observed.control->abort_block();
         BOOST_CHECK(reference.head().id() == observed.head().id());
         BOOST_CHECK_EQUAL(reference.last_irreversible_block_num(), observed.last_irreversible_block_num());
         BOOST_CHECK(reference.control->calculate_integrity_hash() == observed.control->calculate_integrity_hash());
         BOOST_CHECK(expected_traces == actual_traces);
      }
      const auto stats = observed.control->get_parallel_execution_stats();
      BOOST_CHECK_EQUAL(stats.adopted, workers ? 3u : 0u);
      BOOST_CHECK_EQUAL(stats.succeeded, workers ? 3u : 0u);
      BOOST_CHECK_EQUAL(stats.attempts, workers ? 14u : 0u); // 3*(discovery+execution) + 8 bounded probe passes
   }
}

BOOST_FIXTURE_TEST_CASE(reviewed_but_unsupported_creation_imports_fall_back_before_any_canonical_change, validation_corpus) {
   const auto initializer = initialize_code();
   const auto initializer_hash = fc::sha256::hash(reinterpret_cast<const char*>(initializer.data()), initializer.size());
   fc::temp_directory reference_dir;
   tester reference(configuration(reference_dir, 0), genesis);
   prefix(reference, corpus.size());
   const auto expected = reference.control->calculate_integrity_hash();
   for (uint16_t workers : {1, 2, 4}) {
      fc::temp_directory directory;
      auto config = configuration(directory, workers);
      // Being on the local code-hash list is NOT permission to use unsupported
      // state operations. Four real stores hit a link-only fallback, then the original
      // transaction path creates tables/rows/payer RAM in canonical order.
      config.parallel_code_allowlist.insert(initializer_hash);
      tester chain(config, genesis);
      prefix(chain, corpus.size());
      BOOST_CHECK(chain.control->calculate_integrity_hash() == expected);
      const auto stats = chain.control->get_parallel_execution_stats();
      BOOST_CHECK_EQUAL(stats.attempts, 42u); // 19 supported inputs run discovery + execution; 4 stores fail once
      BOOST_CHECK_EQUAL(stats.succeeded, 18u);
      BOOST_CHECK_EQUAL(stats.adopted, 11u);
      BOOST_CHECK_EQUAL(stats.serial_fallbacks, 12u);
   }
}

BOOST_AUTO_TEST_CASE(real_cdt_dispatcher_multi_index_and_allocator_adopt_or_fall_back_without_semantic_changes) {
   const auto code = cdt_fixture_code();
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   auto genesis = base_tester::default_genesis();
   auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->finalizer_key = public_key;
   genesis.initial_savanna->proof_of_possession = proof;
   struct checkpoint { std::vector<char> block; uint64_t adopted; uint64_t fallbacks; };
   std::vector<checkpoint> blocks;
   {
      fc::temp_directory directory;
      auto config = base_tester::default_config(directory).first;
      config.wasm_runtime = wasm_interface::vm_type::eos_vm;
      tester source(config, genesis);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      auto seal = [&](uint64_t adopted = 0, uint64_t fallbacks = 0) {
         blocks.push_back({fc::raw::pack(*source.produce_block()), adopted, fallbacks});
      };
      source.create_accounts({"test"_n, "alice"_n, "bob"_n, "carol"_n}); seal();
      source.set_code("test"_n, code); seal();
      uint64_t nonce = 100;
      for (auto scope : {"alice"_n, "bob"_n, "carol"_n})
         push(source, scope, 10, nonce++, 0, std::nullopt, "init"_n);
      seal(0, 3); // link-only store: must use original RAM/table creation
      for (auto scope : {"alice"_n, "bob"_n, "carol"_n}) push(source, scope, 7, nonce++, 4096);
      seal(3, 0); // actual CDT multi_index + allocation, not hand-written WAT
      push(source, "alice"_n, 3, nonce++, 64); push(source, "alice"_n, 5, nonce++, 64); seal(1, 1);
      push(source, "alice"_n, 10, nonce++, 64);
      push(source, "alice"_n, uint64_t{0} - 30, nonce++, 64); seal(1, 1); // private assertion fails, serial succeeds
      push(source, "alice"_n, 2, nonce++, 2 * 1024 * 1024);
      push(source, "bob"_n, 3, nonce++, 64); push(source, "carol"_n, 4, nonce++, 64);
      seal(0, 3); // local memory exhaustion AFTER private write; discard whole batch tail
      for (auto scope : {"alice"_n, "bob"_n, "carol"_n}) push(source, scope, 1, nonce++, 4096);
      seal(3, 0); // cached machines reset even after allocation failure
      for (unsigned i = 0; i < 5; ++i) seal();
   }
   for (uint16_t workers : {0, 1, 2, 4}) {
      BOOST_TEST_CONTEXT("CDT workers=" << workers) {
         fc::temp_directory reference_dir, observed_dir;
         auto reference_config = base_tester::default_config(reference_dir).first;
         reference_config.wasm_runtime = wasm_interface::vm_type::eos_vm;
         auto config = base_tester::default_config(observed_dir).first;
         config.wasm_runtime = wasm_interface::vm_type::eos_vm;
         config.parallel_validation_threads = workers;
         config.parallel_code_allowlist = {hash};
         tester reference(reference_config, genesis), observed(config, genesis);
         std::vector<std::vector<char>> expected_traces, actual_traces;
         auto first = reference.control->applied_transaction().connect([&](const auto& event) {
            expected_traces.push_back(consensus_trace(std::get<0>(event)));
         });
         auto second = observed.control->applied_transaction().connect([&](const auto& event) {
            actual_traces.push_back(consensus_trace(std::get<0>(event)));
         });
         for (const auto& item : blocks) {
            expected_traces.clear(); actual_traces.clear();
            const auto before = observed.control->get_parallel_execution_stats();
            reference.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(item.block)));
            observed.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(item.block)));
            reference.control->abort_block(); observed.control->abort_block();
            BOOST_CHECK(reference.head().id() == observed.head().id());
            BOOST_CHECK_EQUAL(reference.last_irreversible_block_num(), observed.last_irreversible_block_num());
            BOOST_CHECK(reference.control->calculate_integrity_hash() == observed.control->calculate_integrity_hash());
            BOOST_CHECK(expected_traces == actual_traces);
            const auto after = observed.control->get_parallel_execution_stats();
            BOOST_CHECK_EQUAL(after.adopted - before.adopted, workers ? item.adopted : 0u);
            BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, workers ? item.fallbacks : 0u);
         }
         for (const auto& [scope, expected] : std::array<std::pair<name, int64_t>, 3>{{
               {"alice"_n, 8}, {"bob"_n, 21}, {"carol"_n, 22}}}) {
            const auto value = observed.get_row_by_account("test"_n, scope, "rows"_n, name{1});
            BOOST_REQUIRE_EQUAL(value.size(), 16u);
            int64_t actual; std::memcpy(&actual, value.data() + 8, sizeof(actual));
            BOOST_CHECK_EQUAL(actual, expected);
         }
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE(parallel_production_tests)
using parallel_validation_tests::validation_corpus;

struct production_chain : tester {
   using tester::tester;
   void start() { _start_block(head().block_time() + fc::microseconds(config::block_interval_us)); }
};

signed_transaction make_transaction(tester& chain, name scope, int64_t delta, uint64_t nonce,
                                    uint64_t work = 0, name signer = "alice"_n, uint8_t cpu_ms = 0) {
   signed_transaction trx;
   action act; act.account = "test"_n; act.name = "run"_n; act.authorization = {{"alice"_n, "active"_n}};
   const std::array<uint64_t, 4> data{scope.to_uint64_t(), uint64_t(delta), work, nonce};
   act.data.assign(reinterpret_cast<const char*>(data.data()), reinterpret_cast<const char*>(data.data()) + sizeof(data));
   trx.actions.push_back(std::move(act)); chain.set_transaction_headers(trx);
   trx.max_cpu_usage_ms = cpu_ms;
   trx.sign(chain.get_private_key(signer, "active"), chain.control->get_chain_id());
   return trx;
}
transaction_metadata_ptr metadata(tester& chain, const signed_transaction& trx) {
   return transaction_metadata::recover_keys(
      std::make_shared<packed_transaction>(trx, packed_transaction::compression_type::none),
      chain.control->get_chain_id(), fc::seconds(5), transaction_metadata::trx_type::input);
}
transaction_trace_ptr production_push(tester& chain, const transaction_metadata_ptr& trx,
      fc::microseconds max_time = fc::seconds(1), fc::time_point deadline = fc::time_point::maximum()) {
   return chain.control->push_transaction(trx, deadline, max_time, 0, false, 0);
}
void check_row(tester& chain, name scope, uint64_t expected) {
   auto bytes = chain.get_row_by_account("test"_n, scope, "rows"_n, name{1});
   BOOST_REQUIRE_EQUAL(bytes.size(), 8u);
   uint64_t value; std::memcpy(&value, bytes.data(), 8); BOOST_CHECK_EQUAL(value, expected);
}

BOOST_FIXTURE_TEST_CASE(nonexplicit_production_preserves_order_receipts_and_serial_validation, validation_corpus) {
   for (uint16_t workers : {0, 1, 2, 4}) {
      fc::temp_directory producer_dir, validator_dir;
      auto config = configuration(producer_dir, workers);
      config.parallel_production_enabled = workers != 0;
      production_chain producer(config, genesis);
      tester validator(configuration(validator_dir, 0), genesis);
      prefix(producer, 4); prefix(validator, 4);
      producer.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      producer.start();
      std::vector<transaction_metadata_ptr> transactions;
      for (auto scope : {"alice"_n, "bob"_n, "carol"_n, "dave"_n})
         transactions.push_back(metadata(producer, make_transaction(producer, scope, 7, 9000 + transactions.size(), 20000)));
      size_t callbacks = 0;
      uint64_t billed = 0;
      std::vector<std::vector<char>> produced_traces, verified_traces;
      auto connection = validator.control->applied_transaction().connect([&](const auto& event) {
         if (std::any_of(transactions.begin(), transactions.end(), [&](const auto& trx) { return trx->id() == std::get<0>(event)->id; }))
            verified_traces.push_back(consensus_trace(std::get<0>(event)));
      });
      BOOST_CHECK_EQUAL(producer.control->get_parallel_production_batch_size(), workers ? 8 : 0);
      BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(transactions, fc::time_point::maximum(), [&](const auto& trx) {
         BOOST_CHECK(trx == transactions.at(callbacks++));
         auto trace = production_push(producer, trx);
         BOOST_REQUIRE_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : ""));
         BOOST_REQUIRE(trace->receipt);
         BOOST_CHECK_GE(trace->receipt->cpu_usage_us, producer.control->get_global_properties().configuration.min_transaction_cpu_usage);
         billed += trace->receipt->cpu_usage_us;
         produced_traces.push_back(consensus_trace(trace));
         return true;
      }), 4u);
      const auto stats = producer.control->get_parallel_production_stats();
      BOOST_CHECK_EQUAL(stats.adopted, workers ? 4u : 0u);
      BOOST_CHECK_EQUAL(stats.serial_fallbacks, 0u);
      BOOST_CHECK_GT(billed, 0u);
      auto block = producer.finish_block();
      BOOST_REQUIRE_EQUAL(block->transactions.size(), 4u);
      for (size_t i = 0; i < 4; ++i)
         BOOST_CHECK(std::get<packed_transaction>(block->transactions[i].trx).id() == transactions[i]->id());
      validator.push_block(block); validator.control->abort_block();
      BOOST_CHECK(produced_traces == verified_traces);
      BOOST_CHECK(producer.control->calculate_integrity_hash() == validator.control->calculate_integrity_hash());
      for (auto scope : {"alice"_n, "bob"_n, "carol"_n, "dave"_n}) check_row(producer, scope, 17);
   }
}

BOOST_FIXTURE_TEST_CASE(real_cdt_production_charges_cpu_and_serial_validator_replays_results_and_fallbacks, validation_corpus) {
   const auto cdt_code = cdt_fixture_code();
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(cdt_code.data()), cdt_code.size());
   for (uint16_t workers : {0, 1, 2, 4}) {
      BOOST_TEST_CONTEXT("CDT producer workers=" << workers) {
         fc::temp_directory producer_dir, validator_dir;
         auto config = configuration(producer_dir, workers);
         config.parallel_production_enabled = workers != 0;
         config.parallel_code_allowlist = {hash};
         production_chain producer(config, genesis);
         tester validator(configuration(validator_dir, 0), genesis);
         prefix(producer, 1); prefix(validator, 1);
         producer.set_node_finalizers(std::array<name, 1>{"eosio"_n});
         producer.set_code("test"_n, cdt_code);
         uint64_t nonce = 50000;
         for (auto scope : {"alice"_n, "bob"_n, "carol"_n})
            push(producer, scope, 10, nonce++, 0, std::nullopt, "init"_n);
         auto setup = producer.produce_block();
         validator.push_block(setup); validator.control->abort_block(); producer.control->abort_block();
         for (unsigned round = 0; round < 3; ++round) {
            producer.start();
            std::vector<transaction_metadata_ptr> transactions;
            for (auto scope : {"alice"_n, "bob"_n, "carol"_n}) {
               const auto allocation = round == 1 && scope == "alice"_n ? 2 * 1024 * 1024 : 4096;
               transactions.push_back(metadata(producer, make_transaction(producer, scope, 1, nonce++, allocation)));
            }
            std::vector<std::vector<char>> produced_traces, verified_traces;
            // This subscription is per round; a plain connection does NOT
            // disconnect on destruction and would retain dead local refs.
            boost::signals2::scoped_connection connection = validator.control->applied_transaction().connect([&](const auto& event) {
               if (std::any_of(transactions.begin(), transactions.end(), [&](const auto& trx) {
                     return trx->id() == std::get<0>(event)->id;
                  })) verified_traces.push_back(consensus_trace(std::get<0>(event)));
            });
            const auto before = producer.control->get_parallel_production_stats();
            size_t callbacks = 0;
            BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(transactions,
               fc::time_point::maximum(), [&](const auto& trx) {
                  BOOST_CHECK(trx == transactions.at(callbacks++));
                  auto trace = production_push(producer, trx);
                  BOOST_REQUIRE_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : ""));
                  BOOST_REQUIRE(trace->receipt);
                  BOOST_CHECK_GE(trace->receipt->cpu_usage_us,
                     producer.control->get_global_properties().configuration.min_transaction_cpu_usage);
                  BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 1u);
                  BOOST_CHECK(trace->action_traces.front().account_ram_deltas.empty());
                  produced_traces.push_back(consensus_trace(trace));
                  return true;
               }), 3u);
            BOOST_CHECK_EQUAL(callbacks, 3u);
            const auto after = producer.control->get_parallel_production_stats();
            BOOST_CHECK_EQUAL(after.adopted - before.adopted, workers && round != 1 ? 3u : 0u);
            BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, workers && round == 1 ? 3u : 0u);
            auto block = producer.finish_block();
            BOOST_REQUIRE_EQUAL(block->transactions.size(), 3u);
            validator.push_block(block); validator.control->abort_block();
            BOOST_CHECK_EQUAL(produced_traces.size(), 3u);
            BOOST_CHECK_EQUAL(verified_traces.size(), 3u);
            BOOST_CHECK(produced_traces == verified_traces);
            BOOST_CHECK(producer.control->calculate_integrity_hash() == validator.control->calculate_integrity_hash());
            for (auto scope : {"alice"_n, "bob"_n, "carol"_n}) {
               const auto value = producer.get_row_by_account("test"_n, scope, "rows"_n, name{1});
               BOOST_REQUIRE_EQUAL(value.size(), 16u);
               int64_t actual; std::memcpy(&actual, value.data() + 8, sizeof(actual));
               BOOST_CHECK_EQUAL(actual, 11 + round);
            }
         }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(conflict_failed_private_and_auth_rejection_use_original_atomic_path, validation_corpus) {
   for (uint16_t workers : {1, 2, 4}) {
      fc::temp_directory directory;
      auto config = configuration(directory, workers); config.parallel_production_enabled = true;
      production_chain chain(config, genesis); prefix(chain, 4); chain.start();
      std::vector<transaction_metadata_ptr> batch{
         metadata(chain, make_transaction(chain, "alice"_n, 10, 9100)),
         metadata(chain, make_transaction(chain, "alice"_n, -15, 9101)), // private 10-15 fails; canonical 20-15 succeeds
         metadata(chain, make_transaction(chain, "bob"_n, 2, 9102))};
      size_t called = 0;
      chain.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& trx) {
         BOOST_CHECK(trx == batch.at(called++)); BOOST_REQUIRE(!production_push(chain, trx)->except); return true;
      });
      BOOST_CHECK_EQUAL(called, 3u);
      BOOST_CHECK_EQUAL(chain.control->get_parallel_production_stats().adopted, 1u);
      BOOST_CHECK_EQUAL(chain.control->get_parallel_production_stats().serial_fallbacks, 2u);
      check_row(chain, "alice"_n, 5); check_row(chain, "bob"_n, 12);
      batch = {metadata(chain, make_transaction(chain, "carol"_n, 1, 9200, 0, "bob"_n)),
               metadata(chain, make_transaction(chain, "dave"_n, 1, 9201))};
      called = 0;
      chain.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& trx) {
         auto trace = production_push(chain, trx);
         if (called++ == 0) { BOOST_REQUIRE(trace->except); BOOST_CHECK(!trace->receipt); }
         else BOOST_REQUIRE(!trace->except);
         return true;
      });
      BOOST_CHECK_EQUAL(called, 2u);
      check_row(chain, "carol"_n, 10); check_row(chain, "dave"_n, 11);
      BOOST_CHECK_EQUAL(chain.control->get_parallel_production_stats().adopted, 1u);
      BOOST_CHECK_EQUAL(chain.control->get_parallel_production_stats().serial_fallbacks, 4u);
      BOOST_CHECK(!chain.control->is_known_unexpired_transaction(batch.front()->id()));
      // Ordinary duplicate rejection cannot replay a previous accepted result.
      auto duplicate = production_push(chain, batch.back());
      BOOST_REQUIRE(duplicate->except); BOOST_CHECK_EQUAL(duplicate->except->code(), tx_duplicate::code_value);
      check_row(chain, "dave"_n, 11);
   }
}

BOOST_FIXTURE_TEST_CASE(batch_stop_abort_exception_deadline_and_identity_never_reuse_private_results, validation_corpus) {
   for (unsigned mode = 0; mode < 7; ++mode) {
      fc::temp_directory directory;
      auto config = configuration(directory, 2); config.parallel_production_enabled = true;
      production_chain chain(config, genesis); prefix(chain, 4); chain.start();
      std::vector<transaction_metadata_ptr> batch{
         metadata(chain, make_transaction(chain, "alice"_n, 1, 9300)),
         metadata(chain, make_transaction(chain, "bob"_n, 1, 9301))};
      const auto before = chain.control->calculate_integrity_hash();
      size_t callbacks = 0;
      const auto deadline = mode == 3 ? fc::time_point::now() - fc::seconds(1) : fc::time_point::maximum();
      auto run = [&] {
         return chain.control->for_each_parallel_production_transaction(batch, deadline, [&](const auto& trx) {
            ++callbacks;
            if (mode == 0) return false; // queue declines before push
            if (mode == 1) { chain.control->abort_block(); chain.start(); return true; }
            if (mode == 2) throw std::runtime_error("queue callback failed");
            if (mode == 4) {
               BOOST_CHECK_THROW(chain.control->for_each_parallel_production_transaction(batch, deadline, [](const auto&) { return true; }), transaction_exception);
               return false;
            }
            if (mode == 5) {
               // Same signed bytes but a different queue metadata capability.
               auto other = metadata(chain, trx->packed_trx()->get_signed_transaction());
               BOOST_REQUIRE(!production_push(chain, other)->except);
               return true;
            }
            BOOST_REQUIRE(!production_push(chain, trx)->except);
            chain.control->interrupt_transaction(controller::interrupt_t::speculative_block_trx);
            return true;
         });
      };
      if (mode == 2) BOOST_CHECK_THROW(run(), std::runtime_error);
      else BOOST_CHECK_EQUAL(run(), mode == 3 ? 0u : mode == 5 ? 2u : 1u);
      BOOST_CHECK_EQUAL(callbacks, mode == 3 ? 0u : mode == 5 ? 2u : 1u);
      if (mode <= 4) {
         check_row(chain, "alice"_n, 10); check_row(chain, "bob"_n, 10);
         if (mode != 1) BOOST_CHECK(chain.control->calculate_integrity_hash() == before);
      }
      if (mode == 5) {
         BOOST_CHECK_EQUAL(chain.control->get_parallel_production_stats().adopted, 0u);
         BOOST_CHECK_EQUAL(chain.control->get_parallel_production_stats().serial_fallbacks, 2u);
      }
      // Close/abort that pending context and process fresh work. No old slot or
      // reentrancy flag survives any of the above outcomes.
      chain.control->abort_block(); chain.start();
      auto fresh = metadata(chain, make_transaction(chain, "carol"_n, 3, 9400));
      BOOST_CHECK_EQUAL(chain.control->for_each_parallel_production_transaction({fresh}, fc::time_point::maximum(), [&](const auto& trx) {
         BOOST_REQUIRE(!production_push(chain, trx)->except); return true;
      }), 1u);
      check_row(chain, "carol"_n, 13);
   }
}

BOOST_FIXTURE_TEST_CASE(private_cpu_is_billed_once_and_obeys_original_deadlines, validation_corpus) {
   fc::temp_directory directory;
   production_chain chain(configuration(directory, 0), genesis); prefix(chain, 4); chain.start();
   const auto snapshot = chain.control->calculate_integrity_hash();
   for (unsigned mode = 0; mode < 7; ++mode) {
      auto trx = make_transaction(chain, "alice"_n, 1, 9500, 0, "alice"_n, mode == 2 ? 20 : 0);
      packed_transaction packed(trx, packed_transaction::compression_type::none);
      {
         platform_timer timer;
         const auto start = fc::time_point::now();
         transaction_context context(*chain.control, packed, packed.id(), transaction_checktime_timer(timer),
            action_digests_t::store_which_t::savanna, start, transaction_metadata::trx_type::input);
         context.block_deadline = start + fc::seconds(5);
         if (mode == 1) context.block_deadline = start + fc::milliseconds(20);
         context.explicit_billed_cpu_time = mode == 3;
         if (mode == 3) context.billed_cpu_time_us = 100;
         context.init_for_input_trx(packed.get_unprunable_size(), packed.get_prunable_size());
         if (mode == 0 || mode == 1) {
            const int64_t private_us = mode == 1 ? 30000 : 10000;
            BOOST_CHECK_NO_THROW(context.charge_isolated_cpu(fc::microseconds(private_us)));
            BOOST_CHECK_EQUAL(context.isolated_cpu_charged().count(), private_us);
            // Measure at a fixed instant: exact extra CPU, not another trial's
            // noisy wall time. Existing min CPU still applies below this value.
            BOOST_CHECK_EQUAL(context.update_billed_cpu_time(start + fc::milliseconds(12)), 12000u + private_us);
            BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::microseconds(1)), transaction_exception);
            BOOST_CHECK_NO_THROW(context.checktime());
         } else if (mode == 2) {
            BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::milliseconds(21)), tx_cpu_usage_exceeded);
            BOOST_CHECK_EQUAL(context.isolated_cpu_charged().count(), 21000);
         } else if (mode == 3) {
            BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::milliseconds(1)), transaction_exception);
            BOOST_CHECK_EQUAL(context.update_billed_cpu_time(start + fc::seconds(1)), 100u);
         } else if (mode == 4) {
            BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::microseconds(-1)), transaction_exception);
            BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::seconds(2)), transaction_exception);
            BOOST_CHECK_EQUAL(context.isolated_cpu_charged().count(), 0);
         } else if (mode == 5) {
            timer.interrupt_timer();
            BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::milliseconds(1)), interrupt_exception);
            BOOST_CHECK_EQUAL(context.isolated_cpu_charged().count(), 0);
         } else {
            // The original wall deadline must remain unchanged: private CPU
            // does not make the remaining real wall window negative twice.
            context.block_deadline = start - fc::microseconds(1);
            BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::milliseconds(1)), deadline_exception);
         }
      }
      BOOST_CHECK(chain.control->calculate_integrity_hash() == snapshot); // init + charge all undone
   }
}

BOOST_FIXTURE_TEST_CASE(cpu_minimum_block_and_subjective_account_budgets_are_not_bypassed, validation_corpus) {
   fc::temp_directory directory;
   production_chain chain(configuration(directory, 0), genesis); prefix(chain, 4); chain.start();
   const auto before = chain.control->calculate_integrity_hash();
   for (unsigned mode = 0; mode < 3; ++mode) {
      auto& limits = chain.control->get_mutable_resource_limits_manager();
      auto trx = make_transaction(chain, "alice"_n, 1, 9600);
      packed_transaction packed(trx, packed_transaction::compression_type::none);
      platform_timer timer;
      const auto start = fc::time_point::now();
      transaction_context context(*chain.control, packed, packed.id(), transaction_checktime_timer(timer),
         action_digests_t::store_which_t::savanna, start, transaction_metadata::trx_type::input);
      // These fixture-only resource changes are covered by the context's
      // original undo session; no mutable controller DB capability is exposed.
      if (mode == 1) limits.add_transaction_usage({}, limits.get_block_cpu_limit() - 5000, 0,
         block_timestamp_type(chain.control->pending_block_time()).slot);
      if (mode == 2) {
         limits.set_account_limits("alice"_n, -1, -1, 1, false);
         limits.set_account_limits("bob"_n, -1, -1, 100, false);
         limits.process_account_limit_updates();
      }
      context.block_deadline = start + fc::seconds(5); context.leeway = fc::microseconds(0);
      if (mode == 2) {
         auto [available, ignored] = limits.get_account_cpu_limit("alice"_n);
         BOOST_REQUIRE_GT(available, 10000);
         context.subjective_cpu_bill_us = available - 10000;
      }
      context.init_for_input_trx(packed.get_unprunable_size(), packed.get_prunable_size());
      if (mode == 0) {
         context.charge_isolated_cpu(fc::microseconds(0));
         BOOST_CHECK_EQUAL(context.update_billed_cpu_time(start), chain.control->get_global_properties().configuration.min_transaction_cpu_usage);
      } else if (mode == 1) {
         BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::microseconds(6000)), block_cpu_usage_exceeded);
      } else {
         BOOST_CHECK_THROW(context.charge_isolated_cpu(fc::microseconds(11000)), tx_cpu_usage_exceeded);
      }
   }
   BOOST_CHECK(chain.control->calculate_integrity_hash() == before);
}

BOOST_FIXTURE_TEST_CASE(slow_private_queue_head_falls_back_and_does_not_starve_following_transaction, validation_corpus) {
   for (uint16_t workers : {1, 2, 4}) for (auto wall_ms : {100, 500}) {
      fc::temp_directory directory;
      auto config = configuration(directory, workers); config.parallel_production_enabled = true;
      production_chain chain(config, genesis); prefix(chain, 4); chain.start();
      std::vector<transaction_metadata_ptr> batch{
         metadata(chain, make_transaction(chain, "alice"_n, 1, 9700, 1'000'000'000)),
         metadata(chain, make_transaction(chain, "bob"_n, 1, 9701))};
      const auto deadline = fc::time_point::now() + fc::milliseconds(wall_ms);
      size_t callbacks = 0;
      const auto count = chain.control->for_each_parallel_production_transaction(batch, deadline, [&](const auto& trx) {
         auto trace = production_push(chain, trx, fc::milliseconds(20), deadline);
         if (callbacks++ == 0) {
            BOOST_REQUIRE(trace->except);
            BOOST_CHECK_EQUAL(trace->except->code(), tx_cpu_usage_exceeded::code_value);
            BOOST_CHECK(!trace->receipt);
         } else BOOST_REQUIRE(!trace->except);
         return true;
      });
      BOOST_CHECK_EQUAL(count, 2u); BOOST_CHECK_EQUAL(callbacks, 2u);
      BOOST_CHECK(!chain.control->is_known_unexpired_transaction(batch.front()->id()));
      BOOST_CHECK(chain.control->is_known_unexpired_transaction(batch.back()->id()));
      check_row(chain, "alice"_n, 10); check_row(chain, "bob"_n, 11);
   }
}

BOOST_AUTO_TEST_CASE(export_fresh_live_producer_fixture) {
   const char* destination = std::getenv("PUREEOS_PARALLEL_PRODUCER_EXPORT_DIR");
   if (!destination) { BOOST_TEST_MESSAGE("Live process fixture export not requested"); return; }
   const auto directory = std::filesystem::path(destination);
   BOOST_REQUIRE(std::filesystem::create_directory(directory));
   std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
   auto genesis = base_tester::default_genesis();
   // FC JSON timestamps retain milliseconds, not arbitrary microseconds.
   // Normalize BEFORE chain-ID calculation, otherwise the exported genesis
   // round trip creates a different chain than the in-memory signed blocks.
   genesis.initial_timestamp = block_timestamp_type(fc::time_point::now() - fc::seconds(5)).to_time_point();
   auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->finalizer_key = public_key; genesis.initial_savanna->proof_of_possession = proof;
   BOOST_REQUIRE(fc::json::from_string(fc::json::to_string(genesis, fc::time_point::maximum()))
                    .as<genesis_state>().compute_chain_id() == genesis.compute_chain_id());
   fc::temp_directory source_dir;
   auto config = base_tester::default_config(source_dir).first;
   config.wasm_runtime = wasm_interface::vm_type::eos_vm;
   production_chain source(config, genesis);
   source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
   fc::variants checkpoints;
   auto seal = [&] {
      auto block = source.produce_block(); source.control->abort_block();
      auto file = "block-" + std::to_string(block->block_num()) + ".json";
      BOOST_REQUIRE(fc::json::save_to_file(*block, directory / file));
      checkpoints.emplace_back(fc::mutable_variant_object()
         ("block_num", block->block_num())("block_id", source.head().id())("lib", source.last_irreversible_block_num())
         ("integrity_hash", source.control->calculate_integrity_hash())("file", file));
   };
   source.create_accounts({"test"_n, "alice"_n, "bob"_n, "carol"_n, "dave"_n}); seal();
   source.set_code("test"_n, initialize_code()); seal();
   unsigned nonce = 1;
   for (auto scope : {"alice"_n, "bob"_n, "carol"_n, "dave"_n}) push(source, scope, 10, nonce++);
   seal();
   auto code = update_code(); source.set_code("test"_n, code); seal();
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   source.start();
   fc::variants transactions;
   unsigned transaction_count = 16;
   if (const char* count_text = std::getenv("PUREEOS_PARALLEL_PRODUCER_TRANSACTION_COUNT")) {
      char* end = nullptr;
      const auto count = std::strtoul(count_text, &end, 10);
      BOOST_REQUIRE(end && *end == '\0' && count >= 16 && count <= 4096);
      transaction_count = count;
   }
   for (unsigned i = 0; i < transaction_count; ++i) {
      const std::array scopes{"alice"_n, "bob"_n, "carol"_n, "dave"_n};
      auto trx = make_transaction(source, scopes[i % 4], 1, 10000 + i, 20000);
      trx.expiration = fc::time_point_sec(fc::time_point::now() + fc::seconds(1800));
      trx.signatures.clear(); trx.sign(source.get_private_key("alice"_n, "active"), source.control->get_chain_id());
      packed_transaction packed(trx, packed_transaction::compression_type::none);
      transactions.emplace_back(fc::mutable_variant_object()("id", packed.id())("packed", packed));
   }
   BOOST_REQUIRE(fc::json::save_to_file(genesis, directory / "genesis.json"));
   BOOST_REQUIRE(fc::json::save_to_file(transactions, directory / "transactions.json"));
   // Public, deterministic TEST credentials only. Confined to the new private
   // fixture directory, never printed, installed or used on an existing chain.
   std::vector<std::string> providers{
      base_tester::get_public_key("eosio"_n, "active").to_string({}) + "=KEY:" +
         base_tester::get_private_key("eosio"_n, "active").to_string({}),
      public_key.to_string() + "=KEY:" + private_key.to_string()};
   BOOST_REQUIRE(fc::json::save_to_file(providers, directory / "TEST-ONLY-keys.json"));
   BOOST_REQUIRE(fc::json::save_to_file(fc::mutable_variant_object()("format", 1)
      ("chain_id", source.control->get_chain_id())("code_hashes", std::vector{hash})("checkpoints", checkpoints), directory / "manifest.json"));
}

BOOST_AUTO_TEST_SUITE_END()

// Explicit, bounded offline measurement fixture. Registered separately so an
// ordinary correctness run does not silently claim this opt-in export ran.
BOOST_AUTO_TEST_SUITE(parallel_benchmark_fixtures)
BOOST_AUTO_TEST_CASE(export_fixed_replay_workloads) {
   using namespace parallel_production_tests;
   const char* destination = std::getenv("PUREEOS_PARALLEL_BENCHMARK_EXPORT_DIR");
   if (!destination) { BOOST_TEST_MESSAGE("Benchmark fixture export not requested"); return; }
   const auto root = std::filesystem::path(destination);
   BOOST_REQUIRE(std::filesystem::create_directory(root));
   std::filesystem::permissions(root, std::filesystem::perms::owner_all);
   const auto code = update_code(), replacement = update_code(true);
   const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   for (const std::string workload : {"independent", "hot", "mixed", "serial"}) {
      const auto directory = root / workload;
      BOOST_REQUIRE(std::filesystem::create_directory(directory));
      auto genesis = base_tester::default_genesis();
      auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
      genesis.initial_savanna.emplace();
      genesis.initial_savanna->finalizer_key = public_key; genesis.initial_savanna->proof_of_possession = proof;
      fc::temp_directory source_dir;
      auto config = base_tester::default_config(source_dir).first;
      config.wasm_runtime = wasm_interface::vm_type::eos_vm;
      production_chain source(config, genesis);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      fc::variants checkpoints;
      auto seal = [&] {
         auto block = source.produce_block(); source.control->abort_block();
         auto file = "block-" + std::to_string(block->block_num()) + ".json";
         BOOST_REQUIRE(fc::json::save_to_file(*block, directory / file));
         checkpoints.emplace_back(fc::mutable_variant_object()
            ("block_num", block->block_num())("block_id", source.head().id())("lib", source.last_irreversible_block_num())
            ("integrity_hash", source.control->calculate_integrity_hash())("file", file));
      };
      source.create_accounts({"test"_n, "alice"_n}); seal();
      source.set_code("test"_n, initialize_code()); seal();
      unsigned nonce = 1;
      for (unsigned i = 0; i < 16; ++i) push(source, name{uint64_t{100} + i}, 10, nonce++);
      seal(); source.set_code("test"_n, workload == "serial" ? replacement : code); seal();
      for (unsigned block = 0; block < 32; ++block) {
         source.start();
         for (unsigned i = 0; i < 16; ++i) {
            // Native code changes are true serial barriers, not worker hints.
            if (workload == "mixed" && i == 4) source.set_code("test"_n, replacement);
            if (workload == "mixed" && i == 12) source.set_code("test"_n, code);
            auto trx = metadata(source, make_transaction(source, name{uint64_t{100} + (workload == "hot" ? 0 : i)},
                                                          1, nonce++, 80000));
            auto trace = production_push(source, trx);
            BOOST_REQUIRE_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : ""));
            BOOST_REQUIRE(trace->receipt);
         }
         seal();
      }
      for (unsigned i = 0; i < 5; ++i) seal();
      const auto replay_height = source.last_irreversible_block_num();
      BOOST_REQUIRE_GE(replay_height, 37u); // every measured business block is finalized
      const auto replay_point = checkpoints.at(replay_height - 2);
      source.close();
      std::filesystem::create_directory(directory / "blocks");
      for (const char* file : {"blocks.log", "blocks.index"})
         std::filesystem::copy_file(config.blocks_dir / file, directory / "blocks" / file);
      BOOST_REQUIRE(fc::json::save_to_file(genesis, directory / "genesis.json"));
      BOOST_REQUIRE(fc::json::save_to_file(fc::mutable_variant_object()("format", 1)
         ("chain_id", genesis.compute_chain_id())("code_hashes", std::vector{hash})("checkpoints", checkpoints)
         ("replay_checkpoint", replay_point)("workload", workload)("business_transactions", 512)
         ("business_blocks", 32)("loop_iterations", 80000), directory / "manifest.json"));
   }
}
BOOST_AUTO_TEST_SUITE_END()
