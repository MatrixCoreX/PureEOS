#include <eosio/chain/parallel_canonical_adoption.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/chain/merkle.hpp>
#include <eosio/chain/snapshot.hpp>
#include <eosio/testing/tester.hpp>
#include <eosio/testing/bls_utils.hpp>
#include <boost/test/unit_test.hpp>
#include <array>
#include <cstring>
#include <sstream>

using namespace eosio::chain;
using namespace eosio::testing;

namespace {
std::vector<uint8_t> compile(const std::string& wat) { return wast_to_wasm(wat); }

std::vector<uint8_t> initialize_code() {
   return compile(R"((module
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32) (result i32)))
      (memory 1 1)
      (func (export "apply") (param i64 i64 i64)
         (drop (call $read (i32.const 0) (i32.const 32)))
         (drop (call $store (i64.load (i32.const 0)) (i64.const )" + std::to_string("rows"_n.to_uint64_t()) + R"()
            (i64.const )" + std::to_string("alice"_n.to_uint64_t()) + R"() (i64.const 1) (i32.const 8) (i32.const 8))))))");
}
std::vector<uint8_t> update_code(bool extra = false) {
   return compile(R"((module
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_get_i64" (func $get (param i32 i32 i32) (result i32)))
      (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1)
      (func (export "apply") (param $receiver i64) (param i64 i64)
         (local $it i32) (local $work i32)
         (drop (call $read (i32.const 0) (i32.const 32)))
         (set_local $it (call $find (get_local $receiver) (i64.load (i32.const 0))
            (i64.const )" + std::to_string("rows"_n.to_uint64_t()) + R"() (i64.const 1)))
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
void push(tester& chain, name scope, uint64_t delta, uint64_t nonce, uint64_t work = 0) {
   signed_transaction trx;
   action act; act.account = "test"_n; act.name = "run"_n; act.authorization = {{"alice"_n, "active"_n}};
   const std::array<uint64_t, 4> data{scope.to_uint64_t(), delta, work, nonce};
   act.data.assign(reinterpret_cast<const char*>(data.data()), reinterpret_cast<const char*>(data.data()) + sizeof(data));
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
}

BOOST_FIXTURE_TEST_CASE(invalid_receipts_authorization_duplicate_and_wasm_failure_undo_adopted_prefix, validation_corpus) {
   for (uint16_t workers : {1, 2, 4}) for (unsigned mutation = 0; mutation < 6; ++mutation) {
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

BOOST_AUTO_TEST_SUITE_END()
