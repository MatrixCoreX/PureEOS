#include <eosio/chain/parallel_canonical_adoption.hpp>
#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/testing/tester.hpp>
#include <eosio/testing/bls_utils.hpp>
#include <boost/test/unit_test.hpp>
#include <fc/io/raw.hpp>
#include <fc/io/json.hpp>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>

using namespace eosio::chain;
using namespace eosio::testing;
using namespace eosio::chain::experimental;

#include "parallel_test_diagnostics.hpp"

namespace {
const std::array contracts{"test"_n, "bob"_n, "carol"_n};
std::string literal(name n) { return "(i64.const " + std::to_string(n.to_uint64_t()) + ")"; }
std::string wat_bytes(std::span<const char> bytes) {
   const char hex[] = "0123456789abcdef";
   std::string out;
   for (unsigned char c : bytes) { out += '\\'; out += hex[c >> 4]; out += hex[c & 15]; }
   return out;
}

// Three real WASM contracts. Notification receivers read the sending contract;
// ordinary child actions mutate their OWN rows. Sibling inline destroys and
// recreates its table, proving group-local iterators and original object IDs/RAM.
std::vector<uint8_t> inline_code(unsigned role, unsigned variant = 0) {
   std::string data, sends;
   const std::array targets{"bob"_n, "bob"_n, "carol"_n, "carol"_n, "eosio"_n};
   const std::array methods{"child"_n, "sibling"_n, "grand"_n, "grand"_n, "linkauth"_n};
   for (size_t i = 0; i < targets.size(); ++i) {
      action act; act.account = targets[i]; act.name = methods[i];
      act.authorization = {{i == 4 ? "alice"_n : targets[i], i == 3 ? "missing"_n : "active"_n}};
      act.data.resize(32);
      if (i == 4) act.data = fc::raw::pack(std::tuple{"alice"_n, "bob"_n, "child"_n, "active"_n});
      auto bytes = fc::raw::pack(act);
      const size_t payload = bytes.size() - act.data.size(), offset = 1024 + 256 * i;
      if (i != 4) bytes.insert(bytes.end(), {char(0xff), '\0'}); // preserve exact send bytes
      data += "(data (i32.const " + std::to_string(offset) + ") \"" + wat_bytes(bytes) + "\")";
      sends += "(func $send" + std::to_string(i);
      if (i != 4) for (size_t j : {0u, 8u, 16u, 24u})
         sends += " (i64.store (i32.const " + std::to_string(offset + payload + j) +
            ") (i64.load (i32.const " + std::to_string(j) + ")))";
      sends += " (call $inline (i32.const " + std::to_string(offset) + ") (i32.const " +
         std::to_string(bytes.size()) + ")))";
   }
   const auto notify = [](name n) { return "(call $notify " + literal(n) + ")"; };
   std::string body;
   if (role == 0) {
      body = notify("carol"_n) + "(call $send0)" + notify("blank"_n) + "(call $send1)";
      if (variant == 1) body += "(call $send4)"; // native/system receiver must select serial before writes
   } else if (role == 1) {
      body = "(if (i64.eq (get_local $method) " + literal("child"_n) + ") (then " + notify("carol"_n) +
         "(if (i64.eq (i64.load (i32.const 16)) (i64.const 3)) (then (call $send3)) (else (call $send2)))))";
   } else {
      body = "(if (i64.eq (i64.load (i32.const 16)) (i64.const 1)) (then (unreachable)))" + notify("blank"_n);
   }
   return wast_to_wasm(R"((module
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "db_find_i64" (func $find (param i64 i64 i64 i64) (result i32)))
      (import "env" "db_get_i64" (func $get (param i32 i32 i32) (result i32)))
      (import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32) (result i32)))
      (import "env" "db_update_i64" (func $update (param i32 i64 i32 i32)))
      (import "env" "db_remove_i64" (func $erase (param i32)))
      (import "env" "require_recipient" (func $notify (param i64)))
      (import "env" "send_inline" (func $inline (param i32 i32)))
      (import "env" "get_sender" (func $sender (result i64)))
      (import "env" "set_action_return_value" (func $return (param i32 i32)))
      (memory 1 1) )" + data + sends + R"(
      (func (export "apply") (param $receiver i64) (param $account i64) (param $method i64)
         (local $it i32) (local $scope i64) (local $payer i64)
         (drop (call $read (i32.const 0) (i32.const 32)))
         (set_local $scope (i64.load (i32.const 0)))
         (if (i64.eq (get_local $method) )" + literal("init"_n) + R"() (then
            (drop (call $store (get_local $scope) )" + literal("rows"_n) + R"( (get_local $receiver)
               (i64.const 1) (i32.const 8) (i32.const 8))) (return)))
         (set_local $it (call $find (get_local $account) (get_local $scope) )" + literal("rows"_n) + R"( (i64.const 1)))
         (drop (call $get (get_local $it) (i32.const 64) (i32.const 8)))
         (if (i64.eq (get_local $receiver) (get_local $account)) (then
            (i64.store (i32.const 64) (i64.add (i64.load (i32.const 64)) (i64.load (i32.const 8))))
            (if (i64.eq (get_local $method) )" + literal("sibling"_n) + R"() (then
               (call $erase (get_local $it))
               (set_local $it (call $store (get_local $scope) )" + literal("rows"_n) + R"( (get_local $receiver)
                  (i64.const 1) (i32.const 64) (i32.const 8))))
            (else
               (set_local $payer (i64.const 0))
               (if (i32.and (i64.eq (get_local $method) )" + literal("child"_n) + R"()
                     (i64.eq (i64.load (i32.const 16)) (i64.const 2)))
                  (then (set_local $payer )" + literal("alice"_n) + R"()))
               (call $update (get_local $it) (get_local $payer) (i32.const 64) (i32.const 8))))
            )" + body + R"(
         ))
         (i32.store (i32.const 72) (get_local $it))
         (i64.store (i32.const 80) (call $sender))
         (i64.store (i32.const 88) (get_local $method))
         (call $return (i32.const 64) (i32.const 32)))))");
}

struct inline_chain : tester {
   using tester::tester;
   void start() { _start_block(head().block_time() + fc::microseconds(config::block_interval_us)); }
};
signed_transaction inline_trx(tester& chain, name account, name method, name scope, uint64_t delta,
                              uint64_t mode, uint64_t nonce, size_t roots = 1) {
   signed_transaction trx;
   for (size_t i = 0; i < roots; ++i) {
      action act; act.account = account; act.name = method;
      act.authorization = {{method == "init"_n ? account : "alice"_n, "active"_n}};
      const std::array<uint64_t, 4> args{scope.to_uint64_t(), delta, mode, nonce + i};
      act.data.assign(reinterpret_cast<const char*>(args.data()), reinterpret_cast<const char*>(args.data()) + sizeof(args));
      trx.actions.push_back(std::move(act));
   }
   chain.set_transaction_headers(trx);
   trx.sign(chain.get_private_key(method == "init"_n ? account : "alice"_n, "active"), chain.control->get_chain_id());
   return trx;
}
transaction_metadata_ptr inline_metadata(tester& chain, const signed_transaction& trx) {
   return transaction_metadata::recover_keys(std::make_shared<packed_transaction>(trx, packed_transaction::compression_type::none),
      chain.control->get_chain_id(), fc::seconds(5), transaction_metadata::trx_type::input);
}
transaction_trace_ptr inline_push(tester& chain, const transaction_metadata_ptr& trx) {
   return chain.control->push_transaction(trx, fc::time_point::maximum(), fc::seconds(1), 0, false, 0);
}
transaction_trace_ptr inline_seed(tester& chain, signed_transaction trx) {
   return chain.push_transaction(trx); // tester requires a mutable owned lvalue
}
std::vector<char> inline_consensus_trace(const transaction_trace_ptr& trace, bool include_cpu = true) {
   std::vector<std::vector<char>> fields{fc::raw::pack(trace->id), fc::raw::pack(trace->net_usage),
      fc::raw::pack(trace->except ? trace->except->code() : int64_t{0})};
   if (trace->receipt) {
      auto receipt = *trace->receipt;
      if (!include_cpu) receipt.cpu_usage_us = 0; // two independent production clocks; validators retain exact CPU
      fields.push_back(fc::raw::pack(receipt));
   }
   for (const auto& a : trace->action_traces) {
      fields.push_back(fc::raw::pack(a.action_ordinal)); fields.push_back(fc::raw::pack(a.creator_action_ordinal));
      fields.push_back(fc::raw::pack(a.closest_unnotified_ancestor_action_ordinal));
      fields.push_back(fc::raw::pack(a.receiver)); fields.push_back(fc::raw::pack(a.act));
      fields.push_back(fc::raw::pack(a.context_free));
      if (a.receipt) fields.push_back(fc::raw::pack(*a.receipt));
      fields.push_back(a.return_value); fields.push_back(fc::raw::pack(a.account_ram_deltas));
      fields.push_back(fc::raw::pack(a.error_code)); fields.push_back(fc::raw::pack(a.except ? a.except->code() : int64_t{0}));
      fields.emplace_back(a.console.begin(), a.console.end());
   }
   return fc::raw::pack(fields);
}
uint64_t inline_row(tester& chain, name contract, name scope) {
   const auto bytes = chain.get_row_by_account(contract, scope, "rows"_n, name{1});
   BOOST_REQUIRE_EQUAL(bytes.size(), 8u);
   uint64_t value; std::memcpy(&value, bytes.data(), 8); return value;
}
void setup_inline_chain(tester& source, const std::array<std::vector<uint8_t>, 3>& codes, const std::function<void()>& seal) {
   source.create_accounts({"test"_n, "bob"_n, "carol"_n, "alice"_n, "dave"_n, "blank"_n}); seal();
   for (size_t i = 0; i < contracts.size(); ++i) source.set_code(contracts[i], codes[i]);
   seal();
   uint64_t nonce = 1;
   for (name contract : contracts) for (name scope : {"alice"_n, "dave"_n})
      BOOST_REQUIRE(!inline_seed(source, inline_trx(source, contract, "init"_n, scope, 10, 0, nonce++))->except);
   seal();
   source.set_authority("bob"_n, "active"_n, authority{1, {{source.get_public_key("bob"_n, "active"), 1}},
      {{{"test"_n, config::eosio_code_name}, 1}}});
   source.set_authority("carol"_n, "active"_n, authority{1, {{source.get_public_key("carol"_n, "active"), 1}},
      {{{"bob"_n, config::eosio_code_name}, 1}}});
   seal();
}
struct inline_corpus {
   genesis_state genesis = base_tester::default_genesis();
   const std::array<std::vector<uint8_t>, 3> codes{inline_code(0), inline_code(1), inline_code(2)};
   const std::vector<uint8_t> native_root = inline_code(0, 1);
   flat_set<digest_type> reviewed;
   std::vector<std::vector<char>> blocks;
   inline_corpus() {
      auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
      genesis.initial_savanna.emplace();
      genesis.initial_savanna->finalizer_key = public_key; genesis.initial_savanna->proof_of_possession = proof;
      for (const auto& code : codes) reviewed.insert(fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size()));
      reviewed.insert(fc::sha256::hash(reinterpret_cast<const char*>(native_root.data()), native_root.size()));
      fc::temp_directory dir;
      tester source(configuration(dir, 0), genesis);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      auto seal = [&] { blocks.push_back(fc::raw::pack(*source.produce_block())); };
      setup_inline_chain(source, codes, seal);
   }
   controller::config configuration(const fc::temp_directory& dir, uint16_t workers, bool production = false) const {
      auto cfg = base_tester::default_config(dir).first;
      cfg.wasm_runtime = wasm_interface::vm_type::eos_vm;
      cfg.parallel_validation_threads = workers; cfg.parallel_production_enabled = production && workers != 0;
      cfg.parallel_code_allowlist = reviewed; return cfg;
   }
   void prefix(tester& chain) const {
      for (const auto& block : blocks) {
         chain.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(block)));
         chain.control->abort_block();
      }
   }
};
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_inline_adoption_tests)

BOOST_FIXTURE_TEST_CASE(mixed_inline_tree_production_validation_and_conflicting_lifecycle_keep_original_receipts, inline_corpus) {
   for (uint16_t workers : {0, 1, 2, 4}) {
      BOOST_TEST_CONTEXT("ordinary inline workers=" << workers) {
         fc::temp_directory pd, sd, vd;
         inline_chain producer(configuration(pd, workers, true), genesis);
         tester serial(configuration(sd, 0), genesis), parallel(configuration(vd, workers), genesis);
         prefix(producer); prefix(serial); prefix(parallel);
         producer.set_node_finalizers(std::array<name, 1>{"eosio"_n}); producer.start();
         uint64_t nonce = 1000;
         auto run = [&](std::vector<name> scopes, size_t roots, size_t adopted, size_t fallback) {
            std::vector<transaction_metadata_ptr> batch;
            for (name scope : scopes) batch.push_back(inline_metadata(producer,
               inline_trx(producer, "test"_n, "run"_n, scope, 1, 0, nonce += 10, roots)));
            std::vector<std::vector<char>> expected, serial_traces, parallel_traces;
            auto collect = [&](auto& output, const auto& event) {
               if (std::any_of(batch.begin(), batch.end(), [&](const auto& trx) { return trx->id() == std::get<0>(event)->id; }))
                  output.push_back(inline_consensus_trace(std::get<0>(event)));
            };
            boost::signals2::scoped_connection a = serial.control->applied_transaction().connect([&](const auto& e) { collect(serial_traces, e); });
            boost::signals2::scoped_connection b = parallel.control->applied_transaction().connect([&](const auto& e) { collect(parallel_traces, e); });
            const auto before = producer.control->get_parallel_production_stats();
            const auto vb = parallel.control->get_parallel_execution_stats();
            BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& trx) {
               const auto trace = inline_push(producer, trx);
               BOOST_REQUIRE_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : ""));
               BOOST_REQUIRE(trace->receipt); BOOST_REQUIRE_EQUAL(trace->action_traces.size(), roots * 8);
               uint64_t sequence = 0; int64_t elapsed = 0;
               // Per root: root, notification, blank, child, its notification,
               // grandchild, grandchild's blank, sibling. Ordinals are allocated
               // on SEND, so the sibling executes AFTER larger grandchildren.
               for (size_t root = 0; root < roots; ++root) {
                  const size_t base = roots + root * 7;
                  for (size_t index : {root, base, base + 2, base + 1, base + 4, base + 5, base + 6, base + 3}) {
                     const auto& act = trace->action_traces.at(index);
                     BOOST_REQUIRE(act.receipt); BOOST_CHECK_GT(act.receipt->global_sequence, sequence);
                     sequence = act.receipt->global_sequence; elapsed += act.elapsed.count();
                     if (act.receiver == "blank"_n) BOOST_CHECK(act.return_value.empty());
                     else {
                        BOOST_REQUIRE_EQUAL(act.return_value.size(), 32u);
                        uint64_t sender; std::memcpy(&sender, act.return_value.data() + 16, 8);
                        const auto creator = act.creator_action_ordinal.value;
                        BOOST_CHECK_EQUAL(sender, creator ? trace->action_traces.at(creator - 1).receiver.to_uint64_t() : 0u);
                     }
                  }
               }
               BOOST_CHECK_LE(elapsed, trace->elapsed.count());
               BOOST_CHECK_GE(trace->receipt->cpu_usage_us, producer.control->get_global_properties().configuration.min_transaction_cpu_usage);
               expected.push_back(inline_consensus_trace(trace)); return true;
            }), batch.size());
            const auto after = producer.control->get_parallel_production_stats();
            BOOST_TEST_CONTEXT(parallel_attempt_diagnostics(before, after)) {
               BOOST_CHECK_EQUAL(after.adopted - before.adopted, workers ? adopted : 0u);
               BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, workers ? fallback : 0u);
            }
            const auto block = producer.finish_block();
            serial.push_block(block); serial.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
            BOOST_CHECK(expected == serial_traces); BOOST_CHECK(expected == parallel_traces);
            BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
            BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
            BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().adopted - vb.adopted, workers ? adopted : 0u);
            producer.start();
         };
         run({"alice"_n, "dave"_n}, 2, 2, 0);
         run({"alice"_n, "alice"_n, "dave"_n}, 1, 1, 2);
         for (name scope : {"alice"_n, "dave"_n}) {
            const uint64_t increments = scope == "alice"_n ? 4 : 3;
            BOOST_CHECK_EQUAL(inline_row(producer, "test"_n, scope), 10 + increments);
            BOOST_CHECK_EQUAL(inline_row(producer, "bob"_n, scope), 10 + 2 * increments);
            BOOST_CHECK_EQUAL(inline_row(producer, "carol"_n, scope), 10 + increments);
         }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(send_time_authority_ram_and_deep_trap_failures_undo_original_prefix_and_clear_batch_tail, inline_corpus) {
   for (uint16_t workers : {0, 1, 2, 4}) for (uint64_t failure : {1u, 2u, 3u, 4u}) {
      BOOST_TEST_CONTEXT("inline rollback workers=" << workers << " failure=" << failure) {
         fc::temp_directory pd, sd;
         inline_chain producer(configuration(pd, workers, true), genesis), serial(configuration(sd, 0), genesis);
         prefix(producer); prefix(serial); producer.set_node_finalizers(std::array<name, 1>{"eosio"_n});
         producer.start(); serial.start();
         if (failure == 4) {
            producer.set_authority("bob"_n, "active"_n, authority{1, {{producer.get_public_key("bob"_n, "active"), 1}}, {}});
            serial.control->abort_block(); serial.push_block(producer.produce_block()); serial.control->abort_block();
            producer.control->abort_block(); producer.start(); serial.start();
         }
         const auto before_state = producer.control->calculate_integrity_hash(), original_state = serial.control->calculate_integrity_hash();
         auto bad = inline_trx(producer, "test"_n, "run"_n, "alice"_n, 1, failure == 4 ? 0 : failure, 5000);
         const auto original = inline_push(serial, inline_metadata(serial, bad));
         BOOST_REQUIRE(original->except); BOOST_CHECK(serial.control->calculate_integrity_hash() == original_state);
         // A failing authority applies to both scopes. Use a plain init on a new
         // table as the valid tail in that variant; other failures are per-input.
         auto good = failure == 4 ? inline_trx(producer, "bob"_n, "init"_n, "blank"_n, 10, 0, 5001) :
            inline_trx(producer, "test"_n, "run"_n, "dave"_n, 1, 0, 5001);
         const std::vector<transaction_metadata_ptr> batch{inline_metadata(producer, bad), inline_metadata(producer, good)};
         const auto before = producer.control->get_parallel_production_stats();
         size_t calls = 0; std::vector<char> accepted;
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& trx) {
            const auto trace = inline_push(producer, trx);
            if (calls++ == 0) {
               BOOST_REQUIRE(trace->except); BOOST_CHECK(inline_consensus_trace(trace, false) == inline_consensus_trace(original, false));
               BOOST_CHECK(producer.control->calculate_integrity_hash() == before_state);
               for (name contract : contracts) BOOST_CHECK_EQUAL(inline_row(producer, contract, "alice"_n), 10u);
            } else {
               BOOST_REQUIRE_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : ""));
               accepted = inline_consensus_trace(trace);
            }
            return true;
         }), 2u);
         const auto after = producer.control->get_parallel_production_stats();
         BOOST_TEST_CONTEXT(parallel_attempt_diagnostics(before, after)) {
            BOOST_CHECK_EQUAL(after.adopted - before.adopted, 0u);
            BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, workers ? 2u : 0u);
            if (workers && failure != 1) BOOST_CHECK_GT(after.succeeded - before.succeeded, 0u);
         }
         serial.control->abort_block(); std::vector<char> verified;
         boost::signals2::scoped_connection c = serial.control->applied_transaction().connect([&](const auto& event) {
            if (std::get<0>(event)->id == batch[1]->id()) verified = inline_consensus_trace(std::get<0>(event));
         });
         const auto block = producer.finish_block(); BOOST_REQUIRE_EQUAL(block->transactions.size(), 1u);
         serial.push_block(block); serial.control->abort_block();
         BOOST_CHECK(accepted == verified);
         BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
      }
   }
}

BOOST_FIXTURE_TEST_CASE(unreviewed_child_and_native_inline_remain_whole_transaction_serial_barriers, inline_corpus) {
   for (uint16_t workers : {1, 2, 4}) for (bool native : {false, true}) {
      BOOST_TEST_CONTEXT("inline barrier workers=" << workers << " native=" << native) {
         fc::temp_directory pd, sd, vd;
         auto pcfg = configuration(pd, workers, true), vcfg = configuration(vd, workers);
         if (!native) {
            const auto hash = fc::sha256::hash(reinterpret_cast<const char*>(codes[2].data()), codes[2].size());
            pcfg.parallel_code_allowlist.erase(hash); vcfg.parallel_code_allowlist.erase(hash);
         }
         inline_chain producer(pcfg, genesis);
         tester serial(configuration(sd, 0), genesis), parallel(vcfg, genesis);
         prefix(producer); prefix(serial); prefix(parallel);
         producer.set_node_finalizers(std::array<name, 1>{"eosio"_n}); producer.start();
         if (native) {
            producer.set_code("test"_n, native_root);
            producer.set_authority("alice"_n, "active"_n, authority{1, {{producer.get_public_key("alice"_n, "active"), 1}},
               {{{"test"_n, config::eosio_code_name}, 1}}});
            const auto setup = producer.produce_block();
            serial.push_block(setup); serial.control->abort_block(); parallel.push_block(setup); parallel.control->abort_block();
            producer.control->abort_block(); producer.start();
            BOOST_REQUIRE(producer.control->find_apply_handler("eosio"_n, "eosio"_n, "linkauth"_n));
         }
         const auto trx = inline_metadata(producer, inline_trx(producer, "test"_n, "run"_n, "alice"_n, 1, 0, 6000));
         std::vector<char> expected, serial_trace, parallel_trace;
         boost::signals2::scoped_connection a = serial.control->applied_transaction().connect([&](const auto& e) {
            if (std::get<0>(e)->id == trx->id()) serial_trace = inline_consensus_trace(std::get<0>(e));
         });
         boost::signals2::scoped_connection b = parallel.control->applied_transaction().connect([&](const auto& e) {
            if (std::get<0>(e)->id == trx->id()) parallel_trace = inline_consensus_trace(std::get<0>(e));
         });
         const auto before = producer.control->get_parallel_production_stats();
         const auto vb = parallel.control->get_parallel_execution_stats();
         BOOST_REQUIRE_EQUAL(producer.control->for_each_parallel_production_transaction({trx}, fc::time_point::maximum(), [&](const auto& item) {
            const auto trace = inline_push(producer, item);
            BOOST_REQUIRE_MESSAGE(!trace->except, (trace->except ? trace->except->to_detail_string() : ""));
            BOOST_CHECK_EQUAL(trace->action_traces.size(), native ? 9u : 8u);
            expected = inline_consensus_trace(trace); return true;
         }), 1u);
         const auto after = producer.control->get_parallel_production_stats();
         BOOST_TEST_CONTEXT(parallel_attempt_diagnostics(before, after)) {
            BOOST_CHECK_EQUAL(after.adopted - before.adopted, 0u);
            BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, 1u);
         }
         const auto block = producer.finish_block();
         serial.push_block(block); serial.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
         BOOST_CHECK(expected == serial_trace); BOOST_CHECK(expected == parallel_trace);
         BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
         BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
         BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().adopted - vb.adopted, 0u);
         BOOST_CHECK_EQUAL(inline_row(producer, "test"_n, "alice"_n), 11u);
         BOOST_CHECK_EQUAL(inline_row(producer, "bob"_n, "alice"_n), 12u);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(adopted_parent_keeps_scheduled_inline_and_fresh_child_context_on_late_defensive_fallback, inline_corpus) {
   fc::temp_directory dir; inline_chain chain(configuration(dir, 0), genesis); prefix(chain); chain.start();
   const packed_transaction packed(inline_trx(chain, "test"_n, "run"_n, "alice"_n, 1, 0, 7000),
                                   packed_transaction::compression_type::none);
   const auto prepared = prepare_isolated_transaction(*chain.control, packed); BOOST_REQUIRE(prepared);
   const std::vector roots{prepared->front().input};
   std::vector<isolated_receiver_program> programs;
   for (name account : {"test"_n, "bob"_n, "carol"_n, "blank"_n}) {
      auto program = prepare_isolated_tree_program(*chain.control, account); BOOST_REQUIRE(program); programs.push_back(*program);
   }
   const std::array<table_key, 3> tables{{{"test"_n, "alice"_n, "rows"_n},
      {"bob"_n, "alice"_n, "rows"_n}, {"carol"_n, "alice"_n, "rows"_n}}};
   chainbase_transaction_state_view live(chain.control->db()); isolated_action_tree_executor executor;
   const auto result = executor.execute(frozen_primary_state::capture(live, tables), roots, programs,
                                       current_action_tree_context(*chain.control));
   BOOST_REQUIRE_EQUAL(result.steps.size(), 8u);
   const auto original_state = chain.control->calculate_integrity_hash();
   std::vector<char> expected_root, expected_child, expected_trace; digest_type expected_state;
   for (unsigned mode = 0; mode < 7; ++mode) {
      BOOST_TEST_CONTEXT("inline original-host boundary mode=" << mode) {
         {
            platform_timer timer; const auto start = fc::time_point::now();
            transaction_context transaction(*chain.control, packed, packed.id(), transaction_checktime_timer(timer),
               action_digests_t::store_which_t::savanna, start, transaction_metadata::trx_type::input);
            transaction.block_deadline = start + fc::seconds(5);
            transaction.explicit_billed_cpu_time = true; transaction.billed_cpu_time_us = 100;
            transaction.init_for_input_trx(packed.get_unprunable_size(), packed.get_prunable_size());
            // Deliberately do NOT reserve trace capacity: original scheduling
            // must refresh act pointers across vector reallocation.
            transaction.trace->action_traces.emplace_back(*transaction.trace, packed.get_transaction().actions.front(),
                                                          "test"_n, false, 1, 0, 0);
            apply_context parent(*chain.control, transaction, 1);
            if (mode == 0) chain.control->get_wasm_interface().execute(result.steps[0].code_hash, 0, 0, parent);
            else BOOST_REQUIRE(try_adopt_action_tree_result(parent, result.steps[0].code_hash, 0, 0, programs[0].limits, result, 0));
            BOOST_REQUIRE_EQUAL(transaction.trace->action_traces.size(), 5u);
            if (mode == 0) expected_root = parent.action_return_value;
            else BOOST_CHECK(parent.action_return_value == expected_root);
            // Direct host-boundary test, not a replacement for the full receipt
            // tests above: the read-only notifications do not mutate rows here.
            // Construct the ACTUAL queued child trace with its new group cache.
            const auto& child_step = result.steps[3];
            apply_context child(*chain.control, transaction, child_step.action_ordinal, 1);
            if (mode == 1) {
               BOOST_REQUIRE(try_adopt_action_tree_result(child, child_step.code_hash, 0, 0, programs[1].limits, result, 3));
            } else {
               if (mode > 1) {
                  auto rejected = result; auto limits = programs[1].limits;
                  if (mode == 2) rejected.steps[3].effects.execution_fingerprint = fc::sha256::hash("bad step");
                  if (mode == 3) --limits.max_pages;
                  if (mode == 4) rejected.steps[3].sender = "alice"_n;
                  if (mode == 5) ++rejected.groups[rejected.steps[3].group_index].depth;
                  const auto prefix_state = chain.control->calculate_integrity_hash();
                  BOOST_CHECK(!try_adopt_action_tree_result(child, child_step.code_hash, 0, mode == 6 ? 1 : 0,
                                                            limits, rejected, 3));
                  BOOST_CHECK(chain.control->calculate_integrity_hash() == prefix_state);
                  BOOST_CHECK_EQUAL(transaction.trace->action_traces.size(), 5u);
                  BOOST_CHECK(child.action_return_value.empty());
               }
               chain.control->get_wasm_interface().execute(child_step.code_hash, 0, 0, child);
            }
            BOOST_REQUIRE_EQUAL(transaction.trace->action_traces.size(), 7u);
            BOOST_CHECK_EQUAL(inline_row(chain, "test"_n, "alice"_n), 11u); // prefix was NOT re-executed
            BOOST_CHECK_EQUAL(inline_row(chain, "bob"_n, "alice"_n), 11u);
            BOOST_REQUIRE_EQUAL(child.action_return_value.size(), 32u);
            int32_t iterator; std::memcpy(&iterator, child.action_return_value.data() + 8, 4);
            BOOST_CHECK_EQUAL(iterator, 0); // inline, unlike notification, has a fresh cursor cache
            if (mode == 0) {
               expected_child = child.action_return_value; expected_trace = inline_consensus_trace(transaction.trace, false);
               expected_state = chain.control->calculate_integrity_hash();
            } else {
               BOOST_CHECK(child.action_return_value == expected_child);
               BOOST_CHECK(inline_consensus_trace(transaction.trace, false) == expected_trace);
               BOOST_CHECK(chain.control->calculate_integrity_hash() == expected_state);
            }
         }
         BOOST_CHECK(chain.control->calculate_integrity_hash() == original_state);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(whole_tree_preflight_rejects_deep_current_metadata_stale_reads_and_forged_sends_before_events, inline_corpus) {
   fc::temp_directory dir; inline_chain chain(configuration(dir, 0), genesis); prefix(chain); chain.start();
   const packed_transaction packed(inline_trx(chain, "test"_n, "run"_n, "alice"_n, 1, 0, 7100),
                                   packed_transaction::compression_type::none);
   const auto prepared = prepare_isolated_transaction(*chain.control, packed); BOOST_REQUIRE(prepared);
   const std::vector roots{prepared->front().input}; std::vector<isolated_receiver_program> programs;
   for (name account : {"test"_n, "bob"_n, "carol"_n, "blank"_n}) {
      auto program = prepare_isolated_tree_program(*chain.control, account); BOOST_REQUIRE(program); programs.push_back(*program);
   }
   const std::array<table_key, 3> tables{{{"test"_n, "alice"_n, "rows"_n},
      {"bob"_n, "alice"_n, "rows"_n}, {"carol"_n, "alice"_n, "rows"_n}}};
   chainbase_transaction_state_view live(chain.control->db()); isolated_action_tree_executor executor;
   const auto result = executor.execute(frozen_primary_state::capture(live, tables), roots, programs,
                                       current_action_tree_context(*chain.control));
   const auto original = chain.control->calculate_integrity_hash();
   auto& db = const_cast<chainbase::database&>(chain.control->db()); // test-only faults, always inside original undo
   for (unsigned fault = 0; fault < 6; ++fault) {
      {
         platform_timer timer; const auto start = fc::time_point::now();
         transaction_context transaction(*chain.control, packed, packed.id(), transaction_checktime_timer(timer),
            action_digests_t::store_which_t::savanna, start, transaction_metadata::trx_type::input);
         transaction.block_deadline = start + fc::seconds(5); transaction.explicit_billed_cpu_time = true;
         transaction.billed_cpu_time_us = 100; transaction.init_for_input_trx(packed.get_unprunable_size(), packed.get_prunable_size());
         transaction.trace->action_traces.emplace_back(*transaction.trace, packed.get_transaction().actions.front(), "test"_n, false, 1, 0, 0);
         auto rejected = result;
         if (fault == 0) db.modify(db.get<account_metadata_object, by_name>("bob"_n), [](auto& m) { m.set_privileged(true); });
         if (fault == 1) db.modify(db.get<account_metadata_object, by_name>("carol"_n), [](auto& m) { m.code_hash = fc::sha256::hash("not captured"); });
         if (fault == 2) {
            const auto& table = db.get<table_id_object, by_code_scope_table>(boost::make_tuple("bob"_n, "alice"_n, "rows"_n));
            const auto& row = db.get<key_value_object, by_scope_primary>(boost::make_tuple(table.id, uint64_t{1}));
            db.modify(row, [](auto& row) { const uint64_t changed = 99; row.value.assign(reinterpret_cast<const char*>(&changed), 8); });
         }
         if (fault == 3) rejected.root_fingerprint = fc::sha256::hash("wrong tree context");
         if (fault == 4) {
            bool changed = false;
            for (auto& event : rejected.steps[0].events) if (auto* send = std::get_if<isolated_tree_inline_event>(&event)) {
               send->sent.packed[0] ^= 1; changed = true; break;
            }
            BOOST_REQUIRE(changed);
         }
         if (fault == 5) rejected.steps.back().creator_action_ordinal = 0;
         const auto before = chain.control->calculate_integrity_hash();
         apply_context context(*chain.control, transaction, 1);
         BOOST_CHECK(!try_adopt_action_tree_result(context, result.steps[0].code_hash, 0, 0, programs[0].limits, rejected, 0));
         BOOST_CHECK(chain.control->calculate_integrity_hash() == before);
         BOOST_CHECK_EQUAL(transaction.trace->action_traces.size(), 1u);
         BOOST_CHECK(context.action_return_value.empty());
      }
      BOOST_CHECK(chain.control->calculate_integrity_hash() == original);
   }
}

BOOST_AUTO_TEST_CASE(export_fresh_inline_producer_fixture) {
   const char* destination = std::getenv("PUREEOS_PARALLEL_PRODUCER_INLINE_EXPORT_DIR");
   if (!destination) { BOOST_TEST_MESSAGE("Inline process fixture export not requested"); return; }
   const std::filesystem::path directory(destination);
   BOOST_REQUIRE(std::filesystem::create_directory(directory));
   std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
   auto genesis = base_tester::default_genesis();
   genesis.initial_timestamp = block_timestamp_type(fc::time_point::now() - fc::seconds(5)).to_time_point();
   auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->finalizer_key = public_key; genesis.initial_savanna->proof_of_possession = proof;
   BOOST_REQUIRE(fc::json::from_string(fc::json::to_string(genesis, fc::time_point::maximum()))
      .as<genesis_state>().compute_chain_id() == genesis.compute_chain_id());
   fc::temp_directory source_dir;
   auto cfg = base_tester::default_config(source_dir).first; cfg.wasm_runtime = wasm_interface::vm_type::eos_vm;
   inline_chain source(cfg, genesis); source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
   const std::array<std::vector<uint8_t>, 3> codes{inline_code(0), inline_code(1), inline_code(2)};
   fc::variants checkpoints;
   auto seal = [&] {
      const auto block = source.produce_block(); source.control->abort_block();
      const auto file = "block-" + std::to_string(block->block_num()) + ".json";
      BOOST_REQUIRE(fc::json::save_to_file(*block, directory / file));
      checkpoints.emplace_back(fc::mutable_variant_object()("block_num", block->block_num())
         ("block_id", source.head().id())("lib", source.last_irreversible_block_num())
         ("integrity_hash", source.control->calculate_integrity_hash())("file", file));
   };
   setup_inline_chain(source, codes, seal); source.start();
   unsigned count = 16;
   if (const char* text = std::getenv("PUREEOS_PARALLEL_PRODUCER_TRANSACTION_COUNT")) {
      char* end = nullptr; const auto parsed = std::strtoul(text, &end, 10);
      BOOST_REQUIRE(end && *end == '\0' && parsed >= 16 && parsed <= 4096); count = parsed;
   }
   fc::variants transactions;
   for (unsigned i = 0; i < count; ++i) {
      auto trx = inline_trx(source, "test"_n, "run"_n, i % 2 ? "dave"_n : "alice"_n, 1, 0, 10000 + i * 2, 2);
      trx.expiration = fc::time_point_sec(fc::time_point::now() + fc::seconds(1800));
      trx.signatures.clear(); trx.sign(source.get_private_key("alice"_n, "active"), source.control->get_chain_id());
      const packed_transaction packed(trx, packed_transaction::compression_type::none);
      transactions.emplace_back(fc::mutable_variant_object()("id", packed.id())("packed", packed)("actions", trx.actions.size()));
   }
   BOOST_REQUIRE(fc::json::save_to_file(genesis, directory / "genesis.json"));
   BOOST_REQUIRE(fc::json::save_to_file(transactions, directory / "transactions.json"));
   // Deterministic disposable TEST credentials only; never print or install.
   const std::vector<std::string> providers{
      base_tester::get_public_key("eosio"_n, "active").to_string({}) + "=KEY:" + base_tester::get_private_key("eosio"_n, "active").to_string({}),
      public_key.to_string() + "=KEY:" + private_key.to_string()};
   BOOST_REQUIRE(fc::json::save_to_file(providers, directory / "TEST-ONLY-keys.json"));
   std::vector<fc::sha256> hashes;
   for (const auto& code : codes) hashes.push_back(fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size()));
   BOOST_REQUIRE(fc::json::save_to_file(fc::mutable_variant_object()("format", 1)
      ("fixture_kind", "ordinary-inline-multiple-actions")("chain_id", source.control->get_chain_id())
      ("code_hashes", hashes)("checkpoints", checkpoints), directory / "manifest.json"));
}

BOOST_AUTO_TEST_CASE(export_checked_inline_recovery_corpus) {
   const char* destination = std::getenv("PUREEOS_PARALLEL_INLINE_RECOVERY_EXPORT_DIR");
   if (!destination) { BOOST_TEST_MESSAGE("Inline recovery corpus export not requested"); return; }
   const std::filesystem::path directory(destination);
   BOOST_REQUIRE(std::filesystem::create_directory(directory));
   std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
   inline_corpus fixture;
   BOOST_REQUIRE(fc::json::save_to_file(fixture.genesis, directory / "genesis.json"));
   std::filesystem::create_directory(directory / "blocks");
   fc::variants checkpoints, forks; std::vector<std::vector<char>> corpus;
   fc::temp_directory source_dir; const auto cfg = fixture.configuration(source_dir, 0);
   {
      inline_chain source(cfg, fixture.genesis);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      auto record = [&](const signed_block_ptr& block) {
         source.control->abort_block();
         const auto file = "block-" + std::to_string(block->block_num()) + ".json";
         BOOST_REQUIRE(fc::json::save_to_file(*block, directory / file));
         corpus.push_back(fc::raw::pack(*block));
         checkpoints.emplace_back(fc::mutable_variant_object()("block_num", block->block_num())
            ("block_id", source.head().id())("lib", source.last_irreversible_block_num())
            ("integrity_hash", source.control->calculate_integrity_hash())("file", file));
      };
      for (const auto& data : fixture.blocks) {
         auto block = std::make_shared<signed_block>(fc::raw::unpack<signed_block>(data));
         source.push_block(block); record(block);
      }
      uint64_t nonce = 20000;
      for (unsigned i = 0; i < 16; ++i) {
         source.start();
         for (name scope : {"alice"_n, i % 3 == 0 ? "alice"_n : "dave"_n})
            BOOST_REQUIRE(!inline_seed(source, inline_trx(source, "test"_n, "run"_n, scope, 1, 0, nonce += 10, 2))->except);
         record(source.produce_block());
      }
   }
   for (const char* file : {"blocks.log", "blocks.index"})
      BOOST_REQUIRE(std::filesystem::copy_file(cfg.blocks_dir / file, directory / "blocks" / file));
   for (uint16_t workers : {0, 1, 2, 4}) {
      fc::temp_directory sd, pd;
      tester serial(fixture.configuration(sd, 0), fixture.genesis), parallel(fixture.configuration(pd, workers), fixture.genesis);
      std::vector<std::vector<char>> original, observed;
      boost::signals2::scoped_connection a = serial.control->applied_transaction().connect([&](const auto& e) {
         original.push_back(inline_consensus_trace(std::get<0>(e)));
      });
      boost::signals2::scoped_connection b = parallel.control->applied_transaction().connect([&](const auto& e) {
         observed.push_back(inline_consensus_trace(std::get<0>(e)));
      });
      for (const auto& data : corpus) {
         original.clear(); observed.clear();
         serial.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(data)));
         parallel.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(data)));
         serial.control->abort_block(); parallel.control->abort_block();
         BOOST_CHECK(original == observed);
         BOOST_CHECK(serial.head().id() == parallel.head().id());
         BOOST_CHECK_EQUAL(serial.last_irreversible_block_num(), parallel.last_irreversible_block_num());
         BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
      }
      if (workers) {
         const auto stats = parallel.control->get_parallel_execution_stats();
         BOOST_CHECK_GT(stats.adopted, 0u); BOOST_CHECK_GT(stats.serial_fallbacks, 0u);
      }
   }
   constexpr size_t prefix_blocks = 7;
   fc::temp_directory fd, sd, rd;
   tester first(fixture.configuration(fd, 0), fixture.genesis), second(fixture.configuration(sd, 0), fixture.genesis),
      reference(fixture.configuration(rd, 0), fixture.genesis);
   for (tester* chain : {&first, &second, &reference}) for (size_t i = 0; i < prefix_blocks; ++i) {
      chain->push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(corpus[i]))); chain->control->abort_block();
   }
   first.set_node_finalizers(std::array<name, 1>{"eosio"_n}); second.set_node_finalizers(std::array<name, 1>{"eosio"_n});
   auto fork = [&](const signed_block_ptr& block) {
      reference.push_block(block); reference.control->abort_block();
      BOOST_REQUIRE(reference.head().id() == block->calculate_id());
      const auto file = "fork-" + std::to_string(forks.size()) + ".json";
      BOOST_REQUIRE(fc::json::save_to_file(*block, directory / file));
      forks.emplace_back(fc::mutable_variant_object()("block_num", reference.head().block_num())
         ("block_id", reference.head().id())("lib", reference.last_irreversible_block_num())
         ("integrity_hash", reference.control->calculate_integrity_hash())("file", file));
   };
   BOOST_REQUIRE(!inline_seed(first, inline_trx(first, "test"_n, "run"_n, "alice"_n, 3, 0, 30000, 2))->except);
   fork(first.produce_block());
   BOOST_REQUIRE(!inline_seed(second, inline_trx(second, "test"_n, "run"_n, "dave"_n, 5, 0, 30100, 2))->except);
   fork(second.produce_block(fc::milliseconds(1000)));
   BOOST_REQUIRE(!inline_seed(second, inline_trx(second, "test"_n, "run"_n, "alice"_n, 7, 0, 30200, 2))->except);
   fork(second.produce_block());
   std::vector<fc::sha256> hashes;
   for (const auto& code : fixture.codes) hashes.push_back(fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size()));
   BOOST_REQUIRE(fc::json::save_to_file(fc::mutable_variant_object()("format", 1)
      ("fixture_kind", "ordinary-inline-recovery")("chain_id", fixture.genesis.compute_chain_id())
      ("code_hashes", hashes)("checkpoints", checkpoints)("fork_prefix_blocks", prefix_blocks)("forks", forks), directory / "manifest.json"));
}

BOOST_AUTO_TEST_SUITE_END()
