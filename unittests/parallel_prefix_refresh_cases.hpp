#pragma once

// Included by parallel_validation_tests.cpp after its shared oracle helpers.
// These are real controller/VM tests, not synthetic coordinator callbacks.
BOOST_AUTO_TEST_SUITE(parallel_prefix_refresh_tests)
using namespace parallel_production_tests;

struct prefix_chain_set {
   genesis_state genesis;
   fc::temp_directory producer_dir, serial_dir, parallel_dir;
   std::unique_ptr<production_chain> producer;
   std::unique_ptr<tester> serial, parallel;
   uint64_t nonce = 800000;

   prefix_chain_set(uint16_t workers, const std::vector<uint8_t>& code,
                    experimental::isolated_vm_engine engine = experimental::isolated_vm_engine::interpreter) {
      genesis = base_tester::default_genesis();
      const auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
      genesis.initial_savanna.emplace();
      genesis.initial_savanna->version = 2;
      genesis.initial_savanna->finalizer_key = public_key;
      genesis.initial_savanna->proof_of_possession = proof;
      const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
      auto configuration = [&](const fc::temp_directory& directory, uint16_t count, bool production) {
         auto config = base_tester::default_config(directory).first;
         config.parallel_engine = engine;
         config.wasm_runtime = engine == experimental::isolated_vm_engine::jit ?
            wasm_interface::vm_type::eos_vm_jit : wasm_interface::vm_type::eos_vm;
         config.parallel_validation_threads = count;
         config.parallel_production_enabled = production;
         config.parallel_code_allowlist = {hash};
         return config;
      };
      producer = std::make_unique<production_chain>(configuration(producer_dir, workers, workers != 0), genesis);
      serial = std::make_unique<tester>(configuration(serial_dir, 0, false), genesis);
      parallel = std::make_unique<tester>(configuration(parallel_dir, workers, false), genesis);
      producer->set_node_finalizers(std::array<name, 1>{"eosio"_n});
      producer->create_accounts({"test"_n, "alice"_n, "bob"_n, "carol"_n, "dave"_n});
      seal_setup();
      producer->set_code("test"_n, initialize_code());
      seal_setup();
      for (auto scope : {"alice"_n, "bob"_n, "carol"_n, "dave"_n}) push(*producer, scope, 10, nonce++);
      seal_setup();
      producer->set_code("test"_n, code);
      seal_setup();
      producer->control->abort_block();
      producer->start();
   }

   void validate(const signed_block_ptr& block) {
      serial->push_block(block); serial->control->abort_block();
      parallel->push_block(block); parallel->control->abort_block();
      BOOST_CHECK(producer->control->calculate_integrity_hash() == serial->control->calculate_integrity_hash());
      BOOST_CHECK(serial->control->calculate_integrity_hash() == parallel->control->calculate_integrity_hash());
      BOOST_CHECK(serial->control->head().id() == parallel->control->head().id());
   }
   void seal_setup() {
      const auto block = producer->produce_block();
      producer->control->abort_block();
      validate(block);
      producer->start();
   }
   transaction_metadata_ptr transaction(name scope, int64_t delta, uint64_t work = 0) {
      return metadata(*producer, make_transaction(*producer, scope, delta, nonce++, work));
   }
   void finish() { validate(producer->finish_block()); }
};

BOOST_AUTO_TEST_CASE(warm_access_paths_recapture_changed_values_and_reduce_real_worker_attempts) {
   for (auto engine : {experimental::isolated_vm_engine::interpreter, experimental::isolated_vm_engine::jit})
   for (uint16_t workers : {1, 4, 16}) {
      prefix_chain_set chain(workers, update_code(), engine);
      uint64_t cold_attempts = 0;
      for (unsigned round = 0; round < 3; ++round) {
         std::vector<transaction_metadata_ptr> batch;
         for (auto scope : {"alice"_n, "bob"_n}) {
            // Distinct payload nonces and transaction headers/signatures.
            // Access hints must never cache the old row VALUE (10/11/12).
            auto trx = make_transaction(*chain.producer, scope, 1, 840000 + round);
            trx.expiration = fc::time_point_sec{trx.expiration.sec_since_epoch() + round};
            trx.signatures.clear();
            trx.sign(chain.producer->get_private_key("alice"_n, "active"), chain.genesis.compute_chain_id());
            batch.push_back(metadata(*chain.producer, trx));
         }
         const auto before = chain.producer->control->get_parallel_production_stats();
         BOOST_CHECK_EQUAL(chain.producer->control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(),
            [&](const auto& trx) { BOOST_REQUIRE(!production_push(*chain.producer, trx)->except); return true; }), batch.size());
         const auto after = chain.producer->control->get_parallel_production_stats();
         const auto attempts = after.attempts - before.attempts;
         if (!round) cold_attempts = attempts;
         else BOOST_CHECK_LT(attempts, cold_attempts);
         BOOST_CHECK_EQUAL(after.adopted - before.adopted, 2u);
         BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, 0u);
         chain.finish();
         check_row(*chain.producer, "alice"_n, 11 + round);
         check_row(*chain.serial, "bob"_n, 11 + round);
         check_row(*chain.parallel, "alice"_n, 11 + round);
         chain.producer->start();
      }
   }
}

std::vector<uint8_t> prefix_branch_code(bool fail_after_branch = false) {
   // The old B reads alice=10 and writes carol. After A, B must discover bob
   // instead. C then depends on the NEW write set, not B's old carol journal.
   return compile(R"((module
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_get_i64" (func $get (param i32 i32 i32) (result i32)))
      (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1)
      (func (export "apply") (param $receiver i64) (param i64 i64)
         (local $it i32) (local $old i64) (local $target i64)
         (drop (call $read (i32.const 0) (i32.const 32)))
         (set_local $it (call $find (get_local $receiver) (i64.load (i32.const 0))
            (i64.const )" + std::to_string("rows"_n.to_uint64_t()) + R"() (i64.const 1)))
         (drop (call $get (get_local $it) (i32.const 64) (i32.const 8)))
         (set_local $old (i64.load (i32.const 64)))
         (i64.store (i32.const 64) (i64.add (get_local $old) (i64.load (i32.const 8))))
         (call $update (get_local $it) (i64.const 0) (i32.const 64) (i32.const 8))
         (if (i64.eq (i64.load (i32.const 16)) (i64.const 1)) (then
            )" + (fail_after_branch ? R"((if (i64.eq (get_local $old) (i64.const 11)) (then unreachable)))" : "") + R"(
            (set_local $target (select (i64.const )" + std::to_string("bob"_n.to_uint64_t()) +
            ") (i64.const " + std::to_string("carol"_n.to_uint64_t()) + R"() (i64.eq (get_local $old) (i64.const 11))))
            (set_local $it (call $find (get_local $receiver) (get_local $target)
               (i64.const )" + std::to_string("rows"_n.to_uint64_t()) + R"() (i64.const 1)))
            (drop (call $get (get_local $it) (i32.const 72) (i32.const 8)))
            (i64.store (i32.const 72) (i64.add (i64.load (i32.const 72)) (i64.const 7)))
            (call $update (get_local $it) (i64.const 0) (i32.const 72) (i32.const 8))))
         (call $return (i32.const 64) (i32.const 16)))))");
}

BOOST_AUTO_TEST_CASE(warm_access_hint_for_old_branch_cannot_hide_new_state_dependent_path) {
   for (auto engine : {experimental::isolated_vm_engine::interpreter, experimental::isolated_vm_engine::jit})
   for (uint16_t workers : {1, 4, 16}) {
      prefix_chain_set chain(workers, prefix_branch_code(), engine);
      for (unsigned round = 0; round < 3; ++round) {
         auto trx = make_transaction(*chain.producer, "alice"_n, 1, 845000 + round, 1);
         trx.expiration = fc::time_point_sec{trx.expiration.sec_since_epoch() + round};
         trx.signatures.clear();
         trx.sign(chain.producer->get_private_key("alice"_n, "active"), chain.genesis.compute_chain_id());
         const std::vector batch{metadata(*chain.producer, trx)};
         const auto before = chain.producer->control->get_parallel_production_stats();
         BOOST_CHECK_EQUAL(chain.producer->control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(),
            [&](const auto& item) { BOOST_REQUIRE(!production_push(*chain.producer, item)->except); return true; }), 1u);
         const auto after = chain.producer->control->get_parallel_production_stats();
         BOOST_CHECK_EQUAL(after.adopted - before.adopted, 1u);
         BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, 0u);
         chain.finish();
         check_row(*chain.producer, "alice"_n, 11 + round);
         check_row(*chain.serial, "bob"_n, round ? 17 : 10);
         check_row(*chain.parallel, "carol"_n, round < 2 ? 17 : 24);
         chain.producer->start();
      }
   }
}

BOOST_AUTO_TEST_CASE(changed_branch_rediscovers_writes_and_preserves_independent_tail) {
   for (auto engine : {experimental::isolated_vm_engine::interpreter, experimental::isolated_vm_engine::jit})
   for (uint16_t workers : {0, 1, 2, 4, 8, 12, 16}) {
      BOOST_TEST_CONTEXT("prefix branch engine=" << experimental::isolated_vm_engine_name(engine) << " workers=" << workers) {
         prefix_chain_set chain(workers, prefix_branch_code(), engine);
         auto& producer = *chain.producer;
         const std::vector batch{chain.transaction("alice"_n, 1), chain.transaction("alice"_n, 2, 1),
                                 chain.transaction("bob"_n, 3), chain.transaction("dave"_n, 5)};
         const auto before = producer.control->get_parallel_production_stats();
         const auto validation_before = chain.parallel->control->get_parallel_execution_stats();
         std::vector<std::vector<char>> produced, verified, concurrent;
         auto collect = [&](auto& output, const auto& event) {
            const auto& trace = std::get<0>(event);
            if (std::any_of(batch.begin(), batch.end(), [&](const auto& item) { return item->id() == trace->id; }))
               output.push_back(consensus_trace(trace));
         };
         boost::signals2::scoped_connection a = chain.serial->control->applied_transaction().connect(
            [&](const auto& e) { collect(verified, e); });
         boost::signals2::scoped_connection b = chain.parallel->control->applied_transaction().connect(
            [&](const auto& e) { collect(concurrent, e); });
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(),
            [&](const auto& trx) {
               const auto trace = production_push(producer, trx);
               BOOST_REQUIRE_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : ""));
               produced.push_back(consensus_trace(trace));
               return true;
            }), batch.size());
         const auto after = producer.control->get_parallel_production_stats();
         BOOST_CHECK_EQUAL(after.adopted - before.adopted, workers ? 4u : 0u);
         BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, 0u);
         BOOST_CHECK_EQUAL(after.prefix_refresh_requests - before.prefix_refresh_requests, workers ? 2u : 0u);
         BOOST_CHECK_EQUAL(after.prefix_refresh_successes - before.prefix_refresh_successes, workers ? 2u : 0u);
         // The independent D retains its result; only B and C may be prepared again.
         BOOST_CHECK_EQUAL(after.prepared_successes - before.prepared_successes, workers ? 4u : 0u);
         chain.finish();
         const auto validated = chain.parallel->control->get_parallel_execution_stats();
         BOOST_CHECK_EQUAL(validated.adopted - validation_before.adopted, workers ? 4u : 0u);
         BOOST_CHECK_EQUAL(validated.prefix_refresh_requests - validation_before.prefix_refresh_requests, workers ? 2u : 0u);
         BOOST_CHECK(produced == verified); BOOST_CHECK(produced == concurrent);
         for (const auto& [scope, value] : std::array<std::pair<name, uint64_t>, 4>{
               std::pair{"alice"_n, 13}, {"bob"_n, 20}, {"carol"_n, 10}, {"dave"_n, 15}}) {
            check_row(producer, scope, value); check_row(*chain.serial, scope, value); check_row(*chain.parallel, scope, value);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(fresh_branch_trap_rolls_back_and_stops_tail_speculation) {
   for (auto engine : {experimental::isolated_vm_engine::interpreter, experimental::isolated_vm_engine::jit})
   for (uint16_t workers : {1, 4, 16}) {
      prefix_chain_set chain(workers, prefix_branch_code(true), engine);
      auto& producer = *chain.producer;
      const std::vector batch{chain.transaction("alice"_n, 1), chain.transaction("alice"_n, 2, 1),
                              chain.transaction("dave"_n, 5)};
      const auto before = producer.control->get_parallel_production_stats();
      size_t index = 0;
      BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(),
         [&](const auto& trx) {
            const auto trace = production_push(producer, trx);
            BOOST_CHECK_EQUAL(bool(trace->except), index == 1);
            ++index;
            return true;
         }), 3u);
      const auto after = producer.control->get_parallel_production_stats();
      BOOST_CHECK_EQUAL(after.adopted - before.adopted, 1u);
      BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, 2u);
      BOOST_CHECK_EQUAL(after.prefix_refresh_requests - before.prefix_refresh_requests, 1u);
      BOOST_CHECK_EQUAL(after.prefix_refresh_successes - before.prefix_refresh_successes, 0u);
      chain.finish();
      check_row(producer, "alice"_n, 11); check_row(producer, "bob"_n, 10);
      check_row(producer, "carol"_n, 10); check_row(producer, "dave"_n, 15);
   }
}

BOOST_AUTO_TEST_CASE(extra_public_push_is_a_barrier_even_after_expected_slot_was_consumed) {
   for (auto engine : {experimental::isolated_vm_engine::interpreter, experimental::isolated_vm_engine::jit})
   for (uint16_t workers : {1, 4, 16}) {
      prefix_chain_set chain(workers, update_code(), engine);
      auto& producer = *chain.producer;
      const std::vector batch{chain.transaction("alice"_n, 1), chain.transaction("bob"_n, 2)};
      const auto extra = chain.transaction("carol"_n, 3);
      const auto before = producer.control->get_parallel_production_stats();
      size_t index = 0;
      BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(),
         [&](const auto& trx) {
            BOOST_REQUIRE(!production_push(producer, trx)->except);
            if (index++ == 0) BOOST_REQUIRE(!production_push(producer, extra)->except);
            return true;
         }), 2u);
      const auto after = producer.control->get_parallel_production_stats();
      BOOST_CHECK_EQUAL(after.adopted - before.adopted, 1u);
      BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, 1u);
      BOOST_CHECK_EQUAL(after.prefix_refresh_requests - before.prefix_refresh_requests, 0u);
      chain.finish();
      check_row(producer, "alice"_n, 11); check_row(producer, "bob"_n, 12); check_row(producer, "carol"_n, 13);
   }
}

BOOST_AUTO_TEST_CASE(successful_private_refresh_does_not_bypass_canonical_authorization) {
   for (auto engine : {experimental::isolated_vm_engine::interpreter, experimental::isolated_vm_engine::jit})
   for (uint16_t workers : {1, 4, 16}) {
      prefix_chain_set chain(workers, update_code(), engine);
      auto& producer = *chain.producer;
      const auto wrong_signature = metadata(producer,
         make_transaction(producer, "alice"_n, 2, chain.nonce++, 0, "bob"_n));
      const std::vector batch{chain.transaction("alice"_n, 1), wrong_signature, chain.transaction("dave"_n, 5)};
      const auto before = producer.control->get_parallel_production_stats();
      size_t index = 0;
      BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(),
         [&](const auto& trx) {
            const auto trace = production_push(producer, trx);
            BOOST_CHECK_EQUAL(bool(trace->except), index == 1);
            ++index;
            return true;
         }), 3u);
      const auto after = producer.control->get_parallel_production_stats();
      BOOST_CHECK_EQUAL(after.adopted - before.adopted, 1u);
      BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, 2u);
      BOOST_CHECK_EQUAL(after.prefix_refresh_requests - before.prefix_refresh_requests, 1u);
      BOOST_CHECK_EQUAL(after.prefix_refresh_successes - before.prefix_refresh_successes, 1u);
      chain.finish();
      check_row(producer, "alice"_n, 11); check_row(producer, "dave"_n, 15);
   }
}

BOOST_AUTO_TEST_CASE(prefix_retry_cannot_renew_the_production_private_deadline) {
   for (auto engine : {experimental::isolated_vm_engine::interpreter, experimental::isolated_vm_engine::jit})
   for (uint16_t workers : {1, 4, 16}) {
      prefix_chain_set chain(workers, update_code(), engine);
      auto& producer = *chain.producer;
      const std::vector batch{chain.transaction("alice"_n, 1), chain.transaction("alice"_n, 2),
                              chain.transaction("dave"_n, 5)};
      const auto before = producer.control->get_parallel_production_stats();
      size_t index = 0;
      BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(),
         [&](const auto& trx) {
            BOOST_REQUIRE(!production_push(producer, trx)->except);
            // Deliberate boundary injection, not a workload/TPS measurement.
            // The REAL block deadline remains infinite, but the original 50ms
            // preparation deadline must have elapsed before refreshing B.
            if (index++ == 0) std::this_thread::sleep_for(std::chrono::milliseconds(70));
            return true;
         }), 3u);
      const auto after = producer.control->get_parallel_production_stats();
      BOOST_CHECK_EQUAL(after.adopted - before.adopted, 1u);
      BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, 2u);
      BOOST_CHECK_EQUAL(after.prefix_refresh_requests - before.prefix_refresh_requests, 1u);
      BOOST_CHECK_EQUAL(after.prefix_refresh_successes - before.prefix_refresh_successes, 0u);
      BOOST_CHECK_EQUAL(after.preparation_deadlines - before.preparation_deadlines, 1u);
      chain.finish();
      check_row(producer, "alice"_n, 13); check_row(producer, "dave"_n, 15);
   }
}

BOOST_AUTO_TEST_CASE(prefix_retry_cannot_renew_exhausted_discovery_rounds) {
   const auto code = compile(R"((module
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1)
      (func (export "apply") (param $receiver i64) (param i64 i64)
         (local $key i64) (local $it i32) (local $first i32)
         (drop (call $read (i32.const 0) (i32.const 32)))
         (set_local $key (i64.const 1))
         (loop $next
            (set_local $it (call $find (get_local $receiver) (i64.load (i32.const 0))
               (i64.const )" + std::to_string("rows"_n.to_uint64_t()) + R"() (get_local $key)))
            (if (i64.eq (get_local $key) (i64.const 1)) (then (set_local $first (get_local $it))))
            (set_local $key (i64.add (get_local $key) (i64.const 1)))
            (br_if $next (i64.le_u (get_local $key) (i64.load (i32.const 8)))))
         (call $update (get_local $first) (i64.const 0) (i32.const 8) (i32.const 8))
         (call $return (i32.const 8) (i32.const 8)))))");
   for (auto engine : {experimental::isolated_vm_engine::interpreter, experimental::isolated_vm_engine::jit})
   for (uint16_t workers : {1, 4, 16}) {
      prefix_chain_set chain(workers, code, engine);
      const auto before = chain.parallel->control->get_parallel_execution_stats();
      // Generate canonically, then exercise the 500ms validation preparation
      // phase. Sixty-three distinct queries require all sixty-four waves.
      BOOST_REQUIRE(!production_push(*chain.producer, chain.transaction("alice"_n, 63))->except);
      BOOST_REQUIRE(!production_push(*chain.producer, chain.transaction("alice"_n, 1))->except);
      chain.finish();
      const auto after = chain.parallel->control->get_parallel_execution_stats();
      BOOST_TEST_CONTEXT(experimental::parallel_attempt_diagnostics(before, after)) {
         BOOST_CHECK_EQUAL(after.prepared_successes - before.prepared_successes, 2u);
         BOOST_CHECK_EQUAL(after.adopted - before.adopted, 1u);
         BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, 1u);
         BOOST_CHECK_EQUAL(after.prefix_refresh_requests - before.prefix_refresh_requests, 1u);
         BOOST_CHECK_EQUAL(after.prefix_refresh_successes - before.prefix_refresh_successes, 0u);
         BOOST_CHECK_EQUAL(after.discovery_exhaustions - before.discovery_exhaustions, 1u);
         BOOST_CHECK_EQUAL(after.preparation_deadlines - before.preparation_deadlines, 0u);
      }
      check_row(*chain.parallel, "alice"_n, 1);
   }
}

BOOST_AUTO_TEST_CASE(formal_callback_interrupt_must_propagate_before_and_after_expected_push) {
   for (auto engine : {experimental::isolated_vm_engine::interpreter, experimental::isolated_vm_engine::jit})
   for (uint16_t workers : {0, 1, 4, 16}) for (bool committed : {false, true}) {
      BOOST_TEST_CONTEXT("formal interrupt engine=" << experimental::isolated_vm_engine_name(engine) << " workers=" << workers << " committed=" << committed) {
         prefix_chain_set chain(workers, update_code(), engine);
         auto& producer = *chain.producer;
         const std::vector batch{chain.transaction("alice"_n, 1), chain.transaction("bob"_n, 2)};
         size_t callbacks = 0;
         BOOST_CHECK_THROW(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(),
            [&](const auto& trx) -> bool {
               ++callbacks;
               if (committed) BOOST_REQUIRE(!production_push(producer, trx)->except);
               producer.control->interrupt_transaction(controller::interrupt_t::speculative_block_trx);
               EOS_THROW(interrupt_exception, "injected formal production callback interrupt");
            }), interrupt_exception);
         BOOST_CHECK_EQUAL(callbacks, 1u);
         check_row(producer, "alice"_n, committed ? 11 : 10);
         check_row(producer, "bob"_n, 10);
         if (committed) chain.finish();
         else producer.control->abort_block();
         producer.start();
         // Scope cleanup must also release the callback-order pointer. A new
         // batch observes the new interrupt generation and owns a fresh epoch.
         const std::vector healthy{chain.transaction("carol"_n, 3)};
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(healthy, fc::time_point::maximum(),
            [&](const auto& trx) { BOOST_REQUIRE(!production_push(producer, trx)->except); return true; }), 1u);
         chain.finish();
         check_row(producer, "carol"_n, 13);
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
