#include <eosio/chain/parallel_wasm_worker.hpp>
#include <eosio/chain/parallel_canonical_adoption.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/testing/tester.hpp>
#include <eosio/state_history/serialization.hpp>
#include <eosio/vm/exceptions.hpp>
#include <fc/io/raw.hpp>
#include <test_contracts.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "parallel_test_diagnostics.hpp"

using namespace eosio::chain;
using namespace eosio::chain::experimental;

namespace {
// Opt-in evidence for the original complete CDT cold production/validation
// path. Report only after the joined attempt; do not change its budget, workload
// or adoption assertions. These overlapping stage sums are not TPS or billing.
template<typename F>
auto full_cdt_measure(const char* label, controller& chain, bool production, F&& run) {
   const auto* setting = std::getenv("PUREEOS_PARALLEL_TEST_METRICS");
   if (!setting || std::string_view(setting) != "1") return std::invoke(std::forward<F>(run));
   constexpr std::array stages{performance::stage::parallel_capture, performance::stage::parallel_batch_wait,
      performance::stage::parallel_adopt, performance::stage::parallel_fallback};
   auto& metrics = performance::metrics();
   std::array<performance::sample, stages.size()> before;
   for (size_t i = 0; i < stages.size(); ++i) before[i] = metrics.snapshot(stages[i]);
   const auto stats = [&] {
      return production ? chain.get_parallel_production_stats() : chain.get_parallel_execution_stats();
   };
   const auto attempts_before = stats();
   const auto report = [&] {
      for (size_t i = 0; i < stages.size(); ++i) {
         const auto after = metrics.snapshot(stages[i]);
         std::fprintf(stderr, "FULL_CDT_STAGE %s stage=%s count=%llu us=%llu\n", label,
            performance::names[static_cast<size_t>(stages[i])],
            static_cast<unsigned long long>(after.count - before[i].count),
            static_cast<unsigned long long>((after.nanoseconds - before[i].nanoseconds) / 1000));
      }
      const auto after = stats();
      const auto diagnostic = parallel_attempt_diagnostics(attempts_before, after);
      std::fprintf(stderr, "FULL_CDT_ATTEMPTS %s %s adopted=%llu serial_fallbacks=%llu\n", label,
         diagnostic.c_str(), static_cast<unsigned long long>(after.adopted - attempts_before.adopted),
         static_cast<unsigned long long>(after.serial_fallbacks - attempts_before.serial_fallbacks));
   };
   try {
      auto result = parallel_test_measure(label, std::forward<F>(run)); report(); return result;
   } catch (...) { report(); throw; }
}

constexpr name cdt_account = "testapi"_n;
void cdt_uleb(std::vector<char>& bytes, uint32_t value) {
   do { const auto next = value >> 7; bytes.push_back(char((value & 0x7f) | (next ? 0x80 : 0))); value = next; } while (value);
}
void cdt_section(std::vector<char>& code, uint8_t id, const std::vector<char>& payload) {
   code.push_back(char(id)); cdt_uleb(code, payload.size()); code.insert(code.end(), payload.begin(), payload.end());
}
// Explicit binary lengths make each parser boundary independent of WAT
// optimizer/assembler choices. All local functions have the valid apply type;
// repeated printi imports are well-typed but never invoked by these modules.
std::vector<char> cdt_boundary_module(uint32_t imports = 0, uint32_t functions = 1, uint32_t first_body_bytes = 2) {
   BOOST_REQUIRE(functions && first_body_bytes >= 2);
   std::vector<char> code{0, 'a', 's', 'm', 1, 0, 0, 0};
   cdt_section(code, 1, {2, 0x60, 3, 0x7e, 0x7e, 0x7e, 0, 0x60, 1, 0x7e, 0});
   std::vector<char> payload;
   if (imports) {
      cdt_uleb(payload, imports);
      for (uint32_t i = 0; i < imports; ++i)
         payload.insert(payload.end(), {3, 'e', 'n', 'v', 6, 'p', 'r', 'i', 'n', 't', 'i', 0, 1});
      cdt_section(code, 2, payload); payload.clear();
   }
   cdt_uleb(payload, functions); payload.insert(payload.end(), functions, 0);
   cdt_section(code, 3, payload); payload.clear();
   cdt_section(code, 5, {1, 1, 1, 1}); // one memory, initial/max one page
   payload = {1, 5, 'a', 'p', 'p', 'l', 'y', 0}; cdt_uleb(payload, imports);
   cdt_section(code, 7, payload); payload.clear(); cdt_uleb(payload, functions);
   for (uint32_t i = 0; i < functions; ++i) {
      const auto length = i == 0 ? first_body_bytes : 2;
      cdt_uleb(payload, length); payload.push_back(0); // zero local declarations
      payload.insert(payload.end(), length - 2, 1); // nop instructions
      payload.push_back(0x0b);
   }
   cdt_section(code, 10, payload); return code;
}
std::vector<char> cdt_pad_module(std::vector<char> code, size_t size) {
   BOOST_REQUIRE(size >= code.size() + 3 && size <= isolated_wasm_limits::max_module_code_bytes);
   const auto extra = size - code.size();
   for (size_t prefix = 1; prefix <= 5; ++prefix) {
      if (extra <= prefix + 1) continue;
      const auto payload_size = extra - prefix - 1;
      std::vector<char> encoded; cdt_uleb(encoded, payload_size);
      if (encoded.size() != prefix) continue;
      // Custom section with an empty name, then inert data. This pads only a
      // synthetic boundary fixture, never the original complete CDT artifact.
      cdt_section(code, 0, std::vector<char>(payload_size, 0));
      BOOST_REQUIRE_EQUAL(code.size(), size); return code;
   }
   BOOST_FAIL("unable to encode exact module boundary"); return {};
}
std::vector<uint8_t> full_cdt_code() {
   auto code = eosio::testing::test_contracts::test_api_multi_index_wasm();
   // Exact original artifact: no stripping imports/functions or replacing it
   // with a smaller hand-written contract to evade the private parser limits.
   BOOST_REQUIRE_EQUAL(code.size(), 69287u);
   BOOST_REQUIRE_EQUAL(fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size()).str(),
      "93148d63a7d1b0b864d986cf97fe125136cf2334ba277951d307ab4d16471fac");
   return code;
}
struct full_cdt_chain : eosio::testing::tester {
   using tester::tester;
   void start() { _start_block(head().block_time() + fc::microseconds(config::block_interval_us)); }
};
struct full_cdt_corpus {
   genesis_state genesis = eosio::testing::base_tester::default_genesis();
   std::vector<uint8_t> code = full_cdt_code();
   digest_type code_hash = fc::sha256::hash(reinterpret_cast<const char*>(code.data()), code.size());
   std::vector<std::vector<char>> blocks;
   controller::config configuration(const fc::temp_directory& dir, uint16_t workers, bool console,
                                     bool production = false,
                                     isolated_vm_engine engine = isolated_vm_engine::interpreter) const {
      auto cfg = eosio::testing::base_tester::default_config(dir).first;
      cfg.parallel_engine = engine;
      cfg.wasm_runtime = engine == isolated_vm_engine::jit ?
         wasm_interface::vm_type::eos_vm_jit : wasm_interface::vm_type::eos_vm;
      cfg.contracts_console = console;
      cfg.parallel_validation_threads = workers; cfg.parallel_production_enabled = production && workers != 0;
      cfg.parallel_code_allowlist.insert(code_hash); return cfg;
   }
   full_cdt_corpus() {
      auto [key, pub, proof] = eosio::testing::get_bls_key("eosio"_n);
      genesis.initial_savanna.emplace(); genesis.initial_savanna->finalizer_key = pub;
      genesis.initial_savanna->proof_of_possession = proof;
      fc::temp_directory dir; full_cdt_chain source(configuration(dir, 0, true), genesis);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      const auto seal = [&] { blocks.push_back(fc::raw::pack(*source.produce_block())); };
      source.set_bios_contract(); seal();
      source.create_account(cdt_account); seal();
      source.set_code(cdt_account, code);
      source.set_abi(cdt_account, eosio::testing::test_contracts::test_api_multi_index_abi().c_str()); seal();
   }
   void prefix(eosio::testing::tester& chain) const {
      for (const auto& bytes : blocks) {
         chain.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(bytes)));
         chain.control->abort_block();
      }
   }
};
transaction_metadata_ptr full_cdt_transaction(eosio::testing::tester& chain, name method, uint64_t nonce) {
   signed_transaction trx;
   // These original zero-argument actions ignore trailing action data. A nonce
   // gives independent transactions without changing the original WASM bytes.
   trx.actions.push_back(action{{{cdt_account, "active"_n}}, cdt_account, method, fc::raw::pack(nonce)});
   chain.set_transaction_headers(trx); trx.sign(chain.get_private_key(cdt_account, "active"), chain.control->get_chain_id());
   return transaction_metadata::recover_keys(std::make_shared<packed_transaction>(trx, packed_transaction::compression_type::none),
      chain.control->get_chain_id(), fc::seconds(5), transaction_metadata::trx_type::input);
}
transaction_trace_ptr full_cdt_push(eosio::testing::tester& chain, const transaction_metadata_ptr& trx) {
   return chain.control->push_transaction(trx, fc::time_point::maximum(), fc::seconds(1), 0, false, 0);
}
std::vector<char> full_cdt_trace(const transaction_trace_ptr& trace) {
   BOOST_REQUIRE(trace && !trace->failed_dtrx_trace);
   auto copy = std::make_shared<transaction_trace>(*trace); copy->elapsed = fc::microseconds{};
   std::vector<std::vector<char>> explicit_fields{fc::raw::pack(trace->except ? trace->except->code() : int64_t{0})};
   for (auto& action : copy->action_traces) {
      action.elapsed = fc::microseconds{};
      explicit_fields.push_back(fc::raw::pack(action.console_markers));
      explicit_fields.push_back(fc::raw::pack(action.except ? action.except->code() : int64_t{0}));
      BOOST_REQUIRE(action.call_traces.empty()); // this original CDT contract has no sync-call import
   }
   // The original SHiP serializer binds action order, console, receipts, return
   // values and RAM in both debug modes. Copy-only elapsed normalization.
   const eosio::state_history::augmented_transaction_trace augmented{copy};
   explicit_fields.push_back(fc::raw::pack(make_history_context_wrapper(false, augmented)));
   explicit_fields.push_back(fc::raw::pack(make_history_context_wrapper(true, augmented)));
   return fc::raw::pack(explicit_fields);
}
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_full_cdt_private_tests)
BOOST_AUTO_TEST_CASE(module_byte_cap_is_distinct_from_action_data_and_function_body_caps) {
   const auto small = cdt_boundary_module();
   const auto exact = cdt_pad_module(small, isolated_wasm_limits::max_module_code_bytes);
   BOOST_CHECK_NO_THROW(isolated_wasm_module{exact});
   auto over = exact; over.push_back(0);
   BOOST_CHECK_THROW(isolated_wasm_module{over}, primary_state_budget_exceeded);
   isolated_wasm_limits restricted; restricted.max_code_bytes = 64 * 1024;
   BOOST_CHECK_THROW((isolated_wasm_module{exact, restricted}), primary_state_budget_exceeded);
   isolated_wasm_limits excessive; ++excessive.max_code_bytes;
   BOOST_CHECK_THROW((isolated_wasm_module{small, excessive}), primary_state_budget_exceeded);
   BOOST_CHECK_EQUAL(isolated_wasm_limits{}.max_action_bytes, 64 * 1024u);
   BOOST_CHECK_EQUAL(isolated_wasm_limits::max_function_body_bytes, 64 * 1024u);
   BOOST_CHECK_NO_THROW(isolated_wasm_module{small});
}
BOOST_AUTO_TEST_CASE(import_count_cap_and_cap_plus_one_preserve_linking_checks) {
   const auto exact = cdt_boundary_module(isolated_wasm_limits::max_imports);
   const auto over = cdt_boundary_module(isolated_wasm_limits::max_imports + 1);
   BOOST_CHECK_NO_THROW(isolated_wasm_module{exact});
   BOOST_CHECK_THROW(isolated_wasm_module{over}, eosio::vm::wasm_parse_exception);
   BOOST_CHECK_NO_THROW(isolated_wasm_module{exact});
}
BOOST_AUTO_TEST_CASE(local_function_count_cap_and_cap_plus_one_preserve_parser_checks) {
   const auto exact = cdt_boundary_module(0, isolated_wasm_limits::max_local_functions);
   const auto over = cdt_boundary_module(0, isolated_wasm_limits::max_local_functions + 1);
   BOOST_CHECK_NO_THROW(isolated_wasm_module{exact});
   BOOST_CHECK_THROW(isolated_wasm_module{over}, eosio::vm::wasm_parse_exception);
   BOOST_CHECK_NO_THROW(isolated_wasm_module{exact});
}
BOOST_AUTO_TEST_CASE(single_function_body_limit_is_not_increased_with_module_limit) {
   const auto exact = cdt_boundary_module(0, 1, isolated_wasm_limits::max_function_body_bytes);
   const auto over = cdt_boundary_module(0, 1, isolated_wasm_limits::max_function_body_bytes + 1);
   BOOST_REQUIRE(over.size() < isolated_wasm_limits::max_module_code_bytes);
   BOOST_CHECK_NO_THROW(isolated_wasm_module{exact});
   BOOST_CHECK_THROW(isolated_wasm_module{over}, eosio::vm::wasm_parse_exception);
   BOOST_CHECK_NO_THROW(isolated_wasm_module{exact});
}
BOOST_AUTO_TEST_CASE(unmodified_full_multi_index_module_is_admitted_without_removing_imports_or_functions) {
   const auto code = full_cdt_code();
   const std::vector<char> bytes(code.begin(), code.end());
   BOOST_CHECK_NO_THROW(isolated_wasm_module{bytes});
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(parallel_full_cdt_oracle_tests)
BOOST_AUTO_TEST_CASE(original_five_family_actions_are_actually_adopted_in_production_and_block_validation) {
   full_cdt_corpus corpus;
   for (auto engine : {isolated_vm_engine::interpreter, isolated_vm_engine::jit})
   for (bool console : {false, true}) for (uint16_t workers : {1, 2, 4})
      BOOST_TEST_CONTEXT("full CDT console=" << console << " workers=" << workers) {
      fc::temp_directory pd, sd, vd;
      full_cdt_chain producer(corpus.configuration(pd, workers, console, true, engine), corpus.genesis);
      eosio::testing::tester serial(corpus.configuration(sd, 0, console, false, engine), corpus.genesis),
         parallel(corpus.configuration(vd, workers, console, false, engine), corpus.genesis);
      corpus.prefix(producer); corpus.prefix(serial); corpus.prefix(parallel);
      producer.set_node_finalizers(std::array<name, 1>{"eosio"_n}); producer.start();
      std::vector<transaction_metadata_ptr> batch;
      uint64_t nonce = 0;
      // Original general actions each use their own table. Do not put their
      // later intentionally failing aliases over an already populated table.
      for (name method : {"s1g"_n, "s2g"_n, "s3g"_n, "sdg"_n, "sldg"_n})
         batch.push_back(full_cdt_transaction(producer, method, ++nonce));
      std::vector<std::vector<char>> expected, original, candidate;
      const auto collect = [&](auto& destination, const auto& event) {
         const auto trace = std::get<0>(event);
         if (std::any_of(batch.begin(), batch.end(), [&](const auto& tx) { return trace->id == tx->id(); }))
            destination.push_back(full_cdt_trace(trace));
      };
      boost::signals2::scoped_connection s = serial.control->applied_transaction().connect([&](const auto& event) { collect(original, event); });
      boost::signals2::scoped_connection v = parallel.control->applied_transaction().connect([&](const auto& event) { collect(candidate, event); });
      const auto before = producer.control->get_parallel_production_stats(), vb = parallel.control->get_parallel_execution_stats();
      const auto suffix = std::string(" console=") + (console ? "true" : "false") + " workers=" + std::to_string(workers);
      const auto production_label = "full-cdt-production" + suffix;
      const auto validation_label = "full-cdt-validation" + suffix;
      const auto processed = full_cdt_measure(production_label.c_str(), *producer.control, true, [&] {
         return producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& tx) {
            const auto trace = full_cdt_push(producer, tx);
            BOOST_REQUIRE_MESSAGE(trace && !trace->except && trace->receipt,
               (trace && trace->except ? trace->except->to_detail_string() : "missing full CDT receipt"));
            BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 1u);
            if (tx == batch[2] || tx == batch[3] || tx == batch[4])
               BOOST_CHECK_EQUAL(trace->action_traces[0].console.empty(), !console);
            expected.push_back(full_cdt_trace(trace)); return true;
         });
      });
      BOOST_CHECK_EQUAL(processed, batch.size());
      const auto after = producer.control->get_parallel_production_stats();
      BOOST_TEST_CONTEXT(parallel_attempt_diagnostics(before, after)) {
         BOOST_CHECK_EQUAL(after.adopted - before.adopted, batch.size());
         BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, 0u);
      }
      const auto block = producer.finish_block();
      serial.push_block(block); serial.control->abort_block();
      full_cdt_measure(validation_label.c_str(), *parallel.control, false, [&] {
         parallel.push_block(block); return 0;
      });
      parallel.control->abort_block();
      BOOST_CHECK(original == expected); BOOST_CHECK(candidate == expected);
      BOOST_CHECK_EQUAL(original.size(), batch.size()); BOOST_CHECK_EQUAL(candidate.size(), batch.size());
      BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
      BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
      BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().adopted - vb.adopted, batch.size());
      BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().serial_fallbacks - vb.serial_fallbacks, 0u);
   }
}

BOOST_AUTO_TEST_CASE(original_post_store_assertion_undo_and_healthy_reuse_keep_full_module_and_original_trace) {
   full_cdt_corpus corpus;
   for (auto engine : {isolated_vm_engine::interpreter, isolated_vm_engine::jit})
   for (bool console : {false, true}) for (uint16_t workers : {1, 2, 4})
      BOOST_TEST_CONTEXT("full CDT failure console=" << console << " workers=" << workers) {
      fc::temp_directory pd, sd;
      full_cdt_chain producer(corpus.configuration(pd, workers, console, true, engine), corpus.genesis),
         serial(corpus.configuration(sd, 0, console, false, engine), corpus.genesis);
      corpus.prefix(producer); corpus.prefix(serial);
      producer.set_node_finalizers(std::array<name, 1>{"eosio"_n}); producer.start(); serial.start();
      const auto before = producer.control->calculate_integrity_hash(), original_before = serial.control->calculate_integrity_hash();
      const auto ram_before = producer.control->get_resource_limits_manager().get_account_ram_usage(cdt_account);
      const auto bad = full_cdt_transaction(producer, "s1findfail1"_n, 100);
      const auto reference = full_cdt_push(serial, bad);
      BOOST_REQUIRE(reference && reference->except && !reference->receipt);
      transaction_trace_ptr failed;
      BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({bad}, fc::time_point::maximum(), [&](const auto& tx) {
         failed = full_cdt_push(producer, tx); return true;
      }), 1u);
      BOOST_REQUIRE(failed && failed->except && !failed->receipt);
      BOOST_CHECK_EQUAL(failed->except->code(), eosio_assert_message_exception::code_value);
      BOOST_CHECK(full_cdt_trace(failed) == full_cdt_trace(reference));
      BOOST_CHECK(producer.control->calculate_integrity_hash() == before);
      BOOST_CHECK(serial.control->calculate_integrity_hash() == original_before);
      BOOST_CHECK_EQUAL(producer.control->get_resource_limits_manager().get_account_ram_usage(cdt_account), ram_before);
      BOOST_CHECK(!producer.control->is_known_unexpired_transaction(bad->id()));
      const auto healthy = full_cdt_transaction(producer, "s1g"_n, 101);
      const auto stats = producer.control->get_parallel_production_stats(); transaction_trace_ptr accepted;
      BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({healthy}, fc::time_point::maximum(), [&](const auto& tx) {
         accepted = full_cdt_push(producer, tx); return true;
      }), 1u);
      BOOST_REQUIRE(accepted && !accepted->except && accepted->receipt);
      // Replay the actual produced block: its original receipt supplies billed
      // CPU. Two unrelated measured pushes need not have identical CPU charges.
      transaction_trace_ptr original;
      boost::signals2::scoped_connection observed = serial.control->applied_transaction().connect([&](const auto& event) {
         if (std::get<0>(event)->id == healthy->id()) original = std::get<0>(event);
      });
      serial.control->abort_block();
      const auto block = producer.finish_block(); BOOST_REQUIRE_EQUAL(block->transactions.size(), 1u);
      serial.push_block(block); serial.control->abort_block();
      BOOST_REQUIRE(original && !original->except && original->receipt);
      BOOST_CHECK(full_cdt_trace(accepted) == full_cdt_trace(original));
      BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
      const auto after = producer.control->get_parallel_production_stats();
      BOOST_TEST_CONTEXT(parallel_attempt_diagnostics(stats, after)) {
         BOOST_CHECK_EQUAL(after.adopted - stats.adopted, 1u);
         BOOST_CHECK_EQUAL(after.serial_fallbacks - stats.serial_fallbacks, 0u);
      }
   }
}
BOOST_AUTO_TEST_SUITE_END()

#include <utility>

namespace {
void full_cdt_seed_original_block(full_cdt_corpus& corpus, name seed) {
   fc::temp_directory dir;
   full_cdt_chain original(corpus.configuration(dir, 0, true), corpus.genesis);
   corpus.prefix(original);
   original.set_node_finalizers(std::array<name, 1>{"eosio"_n}); original.start();
   const auto trx = full_cdt_transaction(original, seed, 1000);
   const auto trace = full_cdt_push(original, trx);
   BOOST_REQUIRE_MESSAGE(trace && !trace->except && trace->receipt,
      (trace && trace->except ? trace->except->to_detail_string() : "missing original seed receipt"));
   const auto block = original.finish_block();
   BOOST_REQUIRE_EQUAL(block->transactions.size(), 1u);
   corpus.blocks.push_back(fc::raw::pack(*block));
}

void full_cdt_require_adoption(const controller::parallel_execution_stats& before,
                              const controller::parallel_execution_stats& after,
                              size_t adopted, size_t fallback, size_t prepared) {
   BOOST_TEST_MESSAGE(parallel_attempt_diagnostics(before, after));
   BOOST_TEST_CONTEXT(parallel_attempt_diagnostics(before, after)) {
      BOOST_CHECK_EQUAL(after.adopted - before.adopted, adopted);
      BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, fallback);
      BOOST_CHECK_EQUAL(after.prepared_successes - before.prepared_successes, prepared);
      BOOST_CHECK_EQUAL(after.discovery_exhaustions - before.discovery_exhaustions, 0u);
   }
}
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_full_cdt_oracle_tests)

BOOST_AUTO_TEST_CASE(original_multi_root_full_module_uses_one_bounded_receiver_catalog) {
   full_cdt_corpus corpus;
   const std::array<name, 5> methods{"s1g"_n, "s2g"_n, "s3g"_n, "sdg"_n, "sldg"_n};
   // Three copied modules fit the OLD per-action 256 KiB preparation bound;
   // four/five do not. The actual tree has only one unique receiver program.
   for (size_t roots : {3u, 4u, 5u})
      for (auto engine : {isolated_vm_engine::interpreter, isolated_vm_engine::jit})
   for (bool console : {false, true}) for (uint16_t workers : {1, 2, 4})
         BOOST_TEST_CONTEXT("multi-root full CDT roots=" << roots << " console=" << console << " workers=" << workers) {
         fc::temp_directory pd, sd, vd;
         full_cdt_chain producer(corpus.configuration(pd, workers, console, true, engine), corpus.genesis);
         eosio::testing::tester serial(corpus.configuration(sd, 0, console, false, engine), corpus.genesis),
            parallel(corpus.configuration(vd, workers, console, false, engine), corpus.genesis);
         corpus.prefix(producer); corpus.prefix(serial); corpus.prefix(parallel);
         producer.set_node_finalizers(std::array<name, 1>{"eosio"_n}); producer.start();
         signed_transaction signed_trx;
         for (size_t i = 0; i < roots; ++i)
            signed_trx.actions.push_back(action{{{cdt_account, "active"_n}}, cdt_account, methods[i],
                                               fc::raw::pack(uint64_t{4000} + i)});
         producer.set_transaction_headers(signed_trx);
         signed_trx.sign(producer.get_private_key(cdt_account, "active"), producer.control->get_chain_id());
         const auto trx = transaction_metadata::recover_keys(
            std::make_shared<packed_transaction>(signed_trx, packed_transaction::compression_type::none),
            producer.control->get_chain_id(), fc::seconds(5), transaction_metadata::trx_type::input);
         transaction_trace_ptr accepted, original, candidate;
         boost::signals2::scoped_connection s = serial.control->applied_transaction().connect([&](const auto& event) {
            if (std::get<0>(event)->id == trx->id()) original = std::get<0>(event);
         });
         boost::signals2::scoped_connection v = parallel.control->applied_transaction().connect([&](const auto& event) {
            if (std::get<0>(event)->id == trx->id()) candidate = std::get<0>(event);
         });
         const auto before = producer.control->get_parallel_production_stats();
         const auto vb = parallel.control->get_parallel_execution_stats();
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({trx}, fc::time_point::maximum(),
            [&](const auto& tx) { accepted = full_cdt_push(producer, tx); return true; }), 1u);
         BOOST_REQUIRE(accepted && !accepted->except && accepted->receipt);
         BOOST_REQUIRE_EQUAL(accepted->action_traces.size(), roots);
         // Count one adopted TRANSACTION, not one adoption per root action.
         full_cdt_require_adoption(before, producer.control->get_parallel_production_stats(), 1, 0, 1);
         const auto block = producer.finish_block(); BOOST_REQUIRE_EQUAL(block->transactions.size(), 1u);
         serial.push_block(block); serial.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
         BOOST_REQUIRE(original && candidate && !original->except && !candidate->except);
         BOOST_CHECK(full_cdt_trace(accepted) == full_cdt_trace(original));
         BOOST_CHECK(full_cdt_trace(accepted) == full_cdt_trace(candidate));
         BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
         BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
         full_cdt_require_adoption(vb, parallel.control->get_parallel_execution_stats(), 1, 0, 1);
      }
}

BOOST_AUTO_TEST_CASE(original_committed_store_query_and_autoincrement_use_real_sparse_discovery) {
   const std::array<std::pair<name, name>, 3> scenarios{{
      {"s1store"_n, "s1check"_n}, {"s2store"_n, "s2check"_n}, {"s2autoinc1"_n, "s2autoinc2"_n}}};
   for (const auto& [seed, query] : scenarios) {
      full_cdt_corpus corpus; full_cdt_seed_original_block(corpus, seed);
      for (auto engine : {isolated_vm_engine::interpreter, isolated_vm_engine::jit})
   for (bool console : {false, true}) for (uint16_t workers : {1, 2, 4})
         BOOST_TEST_CONTEXT("stored CDT " << seed.to_string() << " -> " << query.to_string()
                            << " console=" << console << " workers=" << workers) {
         fc::temp_directory pd, sd, vd;
         full_cdt_chain producer(corpus.configuration(pd, workers, console, true, engine), corpus.genesis);
         eosio::testing::tester serial(corpus.configuration(sd, 0, console, false, engine), corpus.genesis),
            parallel(corpus.configuration(vd, workers, console, false, engine), corpus.genesis);
         corpus.prefix(producer); corpus.prefix(serial); corpus.prefix(parallel);
         producer.set_node_finalizers(std::array<name, 1>{"eosio"_n}); producer.start();
         const auto trx = full_cdt_transaction(producer, query, 1001);
         transaction_trace_ptr accepted, original, candidate;
         boost::signals2::scoped_connection s = serial.control->applied_transaction().connect([&](const auto& event) {
            if (std::get<0>(event)->id == trx->id()) original = std::get<0>(event);
         });
         boost::signals2::scoped_connection v = parallel.control->applied_transaction().connect([&](const auto& event) {
            if (std::get<0>(event)->id == trx->id()) candidate = std::get<0>(event);
         });
         const auto before = producer.control->get_parallel_production_stats();
         const auto vb = parallel.control->get_parallel_execution_stats();
         // Exactly ONE root/transaction: do not inflate discovery allowance by
         // bundling extra roots or preload a complete snapshot in this test.
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({trx}, fc::time_point::maximum(),
            [&](const auto& tx) { accepted = full_cdt_push(producer, tx); return true; }), 1u);
         BOOST_REQUIRE(accepted && !accepted->except && accepted->receipt);
         full_cdt_require_adoption(before, producer.control->get_parallel_production_stats(), 1, 0, 1);
         const auto block = producer.finish_block(); BOOST_REQUIRE_EQUAL(block->transactions.size(), 1u);
         serial.push_block(block); serial.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
         BOOST_REQUIRE(original && candidate && !original->except && !candidate->except);
         BOOST_CHECK(full_cdt_trace(accepted) == full_cdt_trace(original));
         BOOST_CHECK(full_cdt_trace(accepted) == full_cdt_trace(candidate));
         BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
         BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
         BOOST_CHECK_EQUAL(producer.control->get_resource_limits_manager().get_account_ram_usage(cdt_account),
                           serial.control->get_resource_limits_manager().get_account_ram_usage(cdt_account));
         full_cdt_require_adoption(vb, parallel.control->get_parallel_execution_stats(), 1, 0, 1);
      }
   }
}

BOOST_AUTO_TEST_CASE(original_same_batch_store_then_check_uses_current_state_after_failed_private_attempt) {
   const std::array<std::pair<name, name>, 3> scenarios{{
      {"s1store"_n, "s1check"_n}, {"s2store"_n, "s2check"_n}, {"s2autoinc1"_n, "s2autoinc2"_n}}};
   full_cdt_corpus corpus;
   for (const auto& [seed, query] : scenarios)
      for (auto engine : {isolated_vm_engine::interpreter, isolated_vm_engine::jit})
   for (bool console : {false, true}) for (uint16_t workers : {1, 2, 4})
         BOOST_TEST_CONTEXT("same-batch CDT " << seed.to_string() << " -> " << query.to_string()
                            << " console=" << console << " workers=" << workers) {
         fc::temp_directory pd, sd, vd;
         full_cdt_chain producer(corpus.configuration(pd, workers, console, true, engine), corpus.genesis);
         eosio::testing::tester serial(corpus.configuration(sd, 0, console, false, engine), corpus.genesis),
            parallel(corpus.configuration(vd, workers, console, false, engine), corpus.genesis);
         corpus.prefix(producer); corpus.prefix(serial); corpus.prefix(parallel);
         producer.set_node_finalizers(std::array<name, 1>{"eosio"_n}); producer.start();
         const std::vector<transaction_metadata_ptr> batch{
            full_cdt_transaction(producer, seed, 2000), full_cdt_transaction(producer, query, 2001)};
         std::vector<std::vector<char>> expected, original, candidate;
         const auto collect = [&](auto& destination, const auto& event) {
            const auto trace = std::get<0>(event);
            if (trace->id == batch[0]->id() || trace->id == batch[1]->id()) destination.push_back(full_cdt_trace(trace));
         };
         boost::signals2::scoped_connection s = serial.control->applied_transaction().connect([&](const auto& event) { collect(original, event); });
         boost::signals2::scoped_connection v = parallel.control->applied_transaction().connect([&](const auto& event) { collect(candidate, event); });
         const auto before = producer.control->get_parallel_production_stats();
         const auto vb = parallel.control->get_parallel_execution_stats();
         BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& tx) {
            const auto trace = full_cdt_push(producer, tx);
            BOOST_REQUIRE_MESSAGE(trace && !trace->except && trace->receipt,
               (trace && trace->except ? trace->except->to_detail_string() : "missing dependent CDT receipt"));
            expected.push_back(full_cdt_trace(trace)); return true;
         }), 2u);
         // Current coordinator policy: original store is adopted; check's
         // failed empty-state attempt is rerun serially AFTER store commits.
         // This is not a stale-success proof; the next case covers that.
         full_cdt_require_adoption(before, producer.control->get_parallel_production_stats(), 1, 1, 1);
         const auto block = producer.finish_block(); BOOST_REQUIRE_EQUAL(block->transactions.size(), 2u);
         serial.push_block(block); serial.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
         BOOST_REQUIRE_EQUAL(expected.size(), 2u); BOOST_REQUIRE_EQUAL(original.size(), 2u); BOOST_REQUIRE_EQUAL(candidate.size(), 2u);
         BOOST_CHECK(expected == original); BOOST_CHECK(expected == candidate);
         BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
         BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
         full_cdt_require_adoption(vb, parallel.control->get_parallel_execution_stats(), 1, 1, 1);
      }
}

BOOST_AUTO_TEST_CASE(original_repeated_secondary_check_rejects_old_success_and_keeps_prior_commit) {
   // CDT table.modify changes BOTH the primary row bytes and the secondary key.
   // This proves stale-transaction rejection, not an isolated secondary-only
   // range-conflict branch; retain the existing raw-host secondary tests too.
   full_cdt_corpus corpus; full_cdt_seed_original_block(corpus, "s2store"_n);
   for (auto engine : {isolated_vm_engine::interpreter, isolated_vm_engine::jit})
   for (bool console : {false, true}) for (uint16_t workers : {1, 2, 4})
      BOOST_TEST_CONTEXT("stale CDT success console=" << console << " workers=" << workers) {
      fc::temp_directory pd, sd, vd;
      full_cdt_chain producer(corpus.configuration(pd, workers, console, true, engine), corpus.genesis),
         serial(corpus.configuration(sd, 0, console, false, engine), corpus.genesis);
      eosio::testing::tester parallel(corpus.configuration(vd, workers, console, false, engine), corpus.genesis);
      corpus.prefix(producer); corpus.prefix(serial); corpus.prefix(parallel);
      producer.set_node_finalizers(std::array<name, 1>{"eosio"_n}); producer.start(); serial.start();
      const std::vector<transaction_metadata_ptr> batch{
         full_cdt_transaction(producer, "s2check"_n, 3000), full_cdt_transaction(producer, "s2check"_n, 3001)};
      const auto before = producer.control->get_parallel_production_stats();
      std::vector<std::vector<char>> expected;
      size_t position = 0;
      BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& tx) {
         BOOST_REQUIRE(position < 2 && tx == batch[position]);
         const auto state_before = producer.control->calculate_integrity_hash();
         const auto original_before = serial.control->calculate_integrity_hash();
         const auto ram_before = producer.control->get_resource_limits_manager().get_account_ram_usage(cdt_account);
         const auto trace = full_cdt_push(producer, tx);
         BOOST_REQUIRE(trace);
         if (position == 0) {
            BOOST_REQUIRE(!trace->except && trace->receipt);
            // Original serial executor, with the actual accepted receipt's CPU
            // charge; avoid comparing unrelated measured CPU bills.
            const auto reference = serial.control->push_transaction(tx, fc::time_point::maximum(), fc::seconds(1),
               trace->receipt->cpu_usage_us, true, 0);
            BOOST_REQUIRE(reference && !reference->except && reference->receipt);
            BOOST_CHECK(full_cdt_trace(trace) == full_cdt_trace(reference));
            BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
            expected.push_back(full_cdt_trace(trace));
         } else {
            const auto reference = full_cdt_push(serial, tx);
            BOOST_REQUIRE(trace->except && !trace->receipt && reference && reference->except && !reference->receipt);
            BOOST_CHECK_EQUAL(trace->except->code(), eosio_assert_message_exception::code_value);
            BOOST_CHECK(full_cdt_trace(trace) == full_cdt_trace(reference));
            BOOST_CHECK(producer.control->calculate_integrity_hash() == state_before);
            BOOST_CHECK(serial.control->calculate_integrity_hash() == original_before);
            BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
            BOOST_CHECK_EQUAL(producer.control->get_resource_limits_manager().get_account_ram_usage(cdt_account), ram_before);
            BOOST_CHECK(!producer.control->is_known_unexpired_transaction(tx->id()));
         }
         ++position; return true;
      }), 2u);
      BOOST_CHECK_EQUAL(position, 2u);
      const auto after = producer.control->get_parallel_production_stats();
      // Both final slots really produced successful old-state private results.
      // The old aggregate `succeeded` cannot prove this because it counts
      // discarded discovery rounds too; do not replace this with >= 2 there.
      full_cdt_require_adoption(before, after, 1, 1, 2);
      // B's old success conflicts. Its fresh execution hits the original CDT
      // assertion, so it must remain a serial barrier rather than resurrect A's
      // old secondary state or pretend that reexecution itself was adoption.
      BOOST_CHECK_EQUAL(after.prefix_refresh_requests - before.prefix_refresh_requests, 1u);
      BOOST_CHECK_EQUAL(after.prefix_refresh_successes - before.prefix_refresh_successes, 0u);

      const auto healthy = full_cdt_transaction(producer, "s1g"_n, 3002);
      const auto hb = producer.control->get_parallel_production_stats();
      BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction({healthy}, fc::time_point::maximum(), [&](const auto& tx) {
         const auto trace = full_cdt_push(producer, tx);
         BOOST_REQUIRE(trace && !trace->except && trace->receipt);
         expected.push_back(full_cdt_trace(trace)); return true;
      }), 1u);
      full_cdt_require_adoption(hb, producer.control->get_parallel_production_stats(), 1, 0, 1);

      // Replay the actual block on both original serial and parallel validators.
      // The failed transaction must not be included or resurrected by replay.
      serial.control->abort_block();
      std::vector<std::vector<char>> original, candidate;
      const auto collect = [&](auto& destination, const auto& event) {
         const auto trace = std::get<0>(event);
         BOOST_CHECK(trace->id != batch[1]->id());
         if (trace->id == batch[0]->id() || trace->id == healthy->id()) destination.push_back(full_cdt_trace(trace));
      };
      boost::signals2::scoped_connection s = serial.control->applied_transaction().connect([&](const auto& event) { collect(original, event); });
      boost::signals2::scoped_connection v = parallel.control->applied_transaction().connect([&](const auto& event) { collect(candidate, event); });
      const auto vb = parallel.control->get_parallel_execution_stats();
      const auto block = producer.finish_block(); BOOST_REQUIRE_EQUAL(block->transactions.size(), 2u);
      serial.push_block(block); serial.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
      BOOST_REQUIRE_EQUAL(original.size(), 2u); BOOST_REQUIRE_EQUAL(candidate.size(), 2u);
      BOOST_CHECK(expected == original); BOOST_CHECK(expected == candidate);
      BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
      BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
      BOOST_CHECK(!serial.control->is_known_unexpired_transaction(batch[1]->id()));
      BOOST_CHECK(!parallel.control->is_known_unexpired_transaction(batch[1]->id()));
      full_cdt_require_adoption(vb, parallel.control->get_parallel_execution_stats(), 2, 0, 2);
   }
}

BOOST_AUTO_TEST_CASE(root_only_capture_keeps_action_limits_and_separate_owned_catalog_budget) {
   full_cdt_corpus corpus;
   fc::temp_directory dir;
   full_cdt_chain chain(corpus.configuration(dir, 0, false), corpus.genesis);
   corpus.prefix(chain); chain.set_node_finalizers(std::array<name, 1>{"eosio"_n}); chain.start();
   const auto transaction = [](size_t count, size_t bytes = 8) {
      signed_transaction trx;
      for (size_t i = 0; i < count; ++i)
         trx.actions.push_back(action{{{cdt_account, "active"_n}}, cdt_account, "s1g"_n,
                                     std::vector<char>(bytes, char(i))});
      return trx;
   };
   const auto capture = [&](const signed_transaction& trx) {
      return prepare_isolated_transaction_roots(*chain.control,
         packed_transaction(trx, packed_transaction::compression_type::none));
   };
   const auto original = chain.control->calculate_integrity_hash();
   for (size_t count : {1u, 3u, 4u, 5u, 16u}) {
      const auto trx = transaction(count);
      const packed_transaction packed(trx, packed_transaction::compression_type::none);
      const auto roots = prepare_isolated_transaction_roots(*chain.control, packed);
      BOOST_REQUIRE(roots); BOOST_REQUIRE_EQUAL(roots->size(), count);
      for (size_t i = 0; i < count; ++i) {
         BOOST_CHECK(roots->at(i).code_hash == corpus.code_hash);
         BOOST_CHECK(roots->at(i).input.receiver == cdt_account);
         BOOST_CHECK(roots->at(i).input.data == trx.actions[i].data);
      }
      // The old root-only executor STILL owns one full code copy per action.
      // Its safety cap must not change when the tree's capture is deduplicated.
      const auto legacy = prepare_isolated_transaction(*chain.control, packed);
      BOOST_CHECK_EQUAL(bool(legacy), count * corpus.code.size() <= isolated_transaction_executor::max_total_code_bytes);
      if (legacy) for (size_t i = 0; i < count; ++i)
         BOOST_CHECK(isolated_execution_fingerprint(roots->at(i).code_hash, roots->at(i).limits, roots->at(i).input) ==
                     isolated_execution_fingerprint(legacy->at(i).code_hash, legacy->at(i).limits, legacy->at(i).input));
   }
   BOOST_CHECK(!capture(transaction(0)));
   BOOST_CHECK(!capture(transaction(17)));
   BOOST_CHECK(capture(transaction(4, 64 * 1024)));
   BOOST_CHECK(!capture(transaction(5, 64 * 1024)));
   BOOST_CHECK(!capture(transaction(1, 64 * 1024 + 1)));
   auto max_authorizations = transaction(1);
   max_authorizations.actions.front().authorization.resize(64, {cdt_account, "active"_n});
   BOOST_CHECK(capture(max_authorizations)); // capture eligibility, not authorization approval
   for (unsigned fault = 0; fault < 4; ++fault) {
      auto trx = transaction(2);
      if (fault == 0) trx.delay_sec = 1;
      if (fault == 1) trx.context_free_actions.push_back(trx.actions.front());
      if (fault == 2) trx.actions.back().authorization.resize(65, {cdt_account, "active"_n});
      if (fault == 3) trx.actions.back().account = "eosio"_n; // reject an ineligible LATER root
      BOOST_CHECK(!capture(trx));
   }
   const auto packed = packed_transaction(transaction(2), packed_transaction::compression_type::none);
   struct capture_cancelled : std::runtime_error { capture_cancelled() : std::runtime_error("capture cancelled") {} };
   // First root, second root, and final checkpoint all propagate unchanged.
   for (size_t stop = 1; stop <= 3; ++stop) {
      size_t checks = 0;
      BOOST_CHECK_THROW(prepare_isolated_transaction_roots(*chain.control, packed, [&] {
         if (++checks == stop) throw capture_cancelled();
      }), capture_cancelled);
      BOOST_CHECK_EQUAL(checks, stop);
      BOOST_REQUIRE(prepare_isolated_transaction_roots(*chain.control, packed));
   }
   auto& db = const_cast<chainbase::database&>(chain.control->db());
   for (unsigned fault = 0; fault < 5; ++fault) {
      {
         auto undo = db.start_undo_session(true);
         db.modify(db.get<account_metadata_object, by_name>(cdt_account), [&](auto& metadata) {
            if (fault == 0) metadata.set_privileged(true);
            if (fault == 1) metadata.vm_type = 1;
            if (fault == 2) metadata.vm_version = 1;
            if (fault == 3) metadata.code_hash = fc::sha256::hash("missing full CDT code");
            if (fault == 4) metadata.code_hash = {};
         });
         BOOST_CHECK(!prepare_isolated_transaction_roots(*chain.control, packed));
      }
      BOOST_CHECK(chain.control->calculate_integrity_hash() == original);
   }
   for (size_t remaining : {size_t{0}, corpus.code.size() - 1})
      BOOST_CHECK(!prepare_isolated_tree_program(*chain.control, cdt_account, remaining));
   const auto exact = prepare_isolated_tree_program(*chain.control, cdt_account, corpus.code.size());
   BOOST_REQUIRE(exact);
   BOOST_CHECK(exact->code == std::vector<char>(corpus.code.begin(), corpus.code.end()));
   BOOST_CHECK(chain.control->calculate_integrity_hash() == original);

   // Equal code hashes at DIFFERENT receivers still own separate entries.
   const std::array names{cdt_account, "testapia"_n, "testapib"_n, "testapic"_n};
   for (size_t i = 1; i < names.size(); ++i) {
      chain.create_account(names[i]); chain.set_code(names[i], corpus.code); chain.produce_block();
   }
   size_t owned_bytes = 0;
   std::vector<isolated_receiver_program> programs;
   for (size_t i = 0; i < names.size(); ++i) {
      auto program = prepare_isolated_tree_program(*chain.control, names[i],
         isolated_transaction_executor::max_total_code_bytes - owned_bytes);
      if (i == 3) { BOOST_CHECK(!program); break; }
      BOOST_REQUIRE(program);
      owned_bytes += program->code.size(); programs.push_back(std::move(*program));
   }
   BOOST_CHECK_EQUAL(programs.size(), 3u);
   BOOST_CHECK_EQUAL(owned_bytes, 3 * corpus.code.size());
   BOOST_CHECK_LE(owned_bytes, isolated_transaction_executor::max_total_code_bytes);
}
BOOST_AUTO_TEST_SUITE_END()

#include <array>
#include <chrono>
#include <optional>
#include <set>

BOOST_AUTO_TEST_SUITE(parallel_full_cdt_oracle_tests)
BOOST_AUTO_TEST_CASE(original_cdt_sparse_private_discovery_reports_each_owned_request) {
   for (const auto& [seed, query] : std::array{
         std::pair{"s1store"_n, "s1check"_n}, std::pair{"s2store"_n, "s2check"_n},
         std::pair{"s2autoinc1"_n, "s2autoinc2"_n}}) {
      full_cdt_corpus corpus; full_cdt_seed_original_block(corpus, seed);
      for (bool console : {false, true}) BOOST_TEST_CONTEXT(
            "private discovery only: " << seed.to_string() << " -> " << query.to_string()
            << " console=" << console) {
         fc::temp_directory dir;
         full_cdt_chain chain(corpus.configuration(dir, 0, console), corpus.genesis);
         corpus.prefix(chain);
         chain.set_node_finalizers(std::array<name, 1>{"eosio"_n}); chain.start();
         const auto trx = full_cdt_transaction(chain, query, 4000);
         const auto original = chain.control->calculate_integrity_hash();
         chainbase_transaction_state_view owner(chain.control->db());
         std::set<table_key, table_key_less> tables;
         std::set<primary_query, primary_query_less> primary_queries;
         std::set<secondary_query, secondary_query_less> secondary_queries;
         std::set<name> accounts;
         std::optional<isolated_action_tree_result> successful;
         size_t rounds = 0, last_rows = 0, last_bytes = 0;
         bool deadline_reached = false;
         std::array<std::optional<state_capture_request>, 64> observed{};
         std::array<size_t, 64> observed_rows{}, observed_bytes{};
         std::array<int64_t, 64> observed_elapsed{};
         const char* phase = "root capture";
         const auto started = std::chrono::steady_clock::now();
         isolated_cancel cancel;
         cancel.deadline = started + std::chrono::milliseconds(500);
         const auto checkpoint = [&] { cancel.check(); };

         try {
            const auto prepared = prepare_isolated_transaction_roots(*chain.control, *trx->packed_trx(), checkpoint);
            BOOST_REQUIRE(prepared && prepared->size() == 1);
            checkpoint();
            const auto program = prepare_isolated_tree_program(*chain.control, cdt_account);
            BOOST_REQUIRE(program);
            const std::array roots{prepared->front().input};
            const std::array programs{*program};
            auto context = current_action_tree_context(*chain.control);
            context.context_free_data = isolated_context_free_data::capture(trx->packed_trx()->get_context_free_data());
            isolated_action_tree_executor executor;
            for (size_t round = 0; round < 64; ++round) {
               checkpoint();
               const std::vector<table_key> keys(tables.begin(), tables.end());
               const std::vector<primary_query> pq(primary_queries.begin(), primary_queries.end());
               const std::vector<secondary_query> sq(secondary_queries.begin(), secondary_queries.end());
               const std::vector<name> names(accounts.begin(), accounts.end());
               // No whole-table capture or test prefill. The owner copies only
               // queries actually requested by the previous failed attempts.
               phase = "state capture";
               const auto base = frozen_primary_state::capture_queries_with_secondary(
                  owner, keys, pq, sq, {}, checkpoint, names);
               last_rows = base->row_count(); last_bytes = base->value_bytes();
               rounds = round + 1;
               try {
                  phase = "private execution";
                  successful.emplace(executor.execute(base, roots, programs, context, {}, {}, cancel));
               } catch (const primary_state_missing& missing) {
                  observed[round] = missing.request;
                  observed_rows[round] = last_rows;
                  observed_bytes[round] = last_bytes;
                  observed_elapsed[round] = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - started).count();
                  bool extended = false;
                  if (const auto* p = std::get_if<primary_capture_request>(&missing.request)) {
                     extended = tables.insert(p->table).second;
                     if (p->query) extended = primary_queries.insert(*p->query).second || extended;
                  } else if (const auto* s = std::get_if<secondary_capture_request>(&missing.request)) {
                     const auto normalized = normalize_secondary_query(s->query);
                     extended = tables.insert(normalized.table).second;
                     extended = secondary_queries.insert(normalized).second || extended;
                  } else if (const auto* a = std::get_if<account_capture_request>(&missing.request)) {
                     extended = accounts.insert(a->account).second;
                  } else BOOST_FAIL("unrecognized owned state request");
                  BOOST_REQUIRE_MESSAGE(extended, "discovery repeated a request already in the owned capture");
                  continue;
               }
               // PRIVATE proof checks, not permission to write. Keep them
               // OUTSIDE the discovery catch: a missing proof dependency must
               // fail, never silently become a fresh execution data request.
               phase = "private proof";
               BOOST_CHECK(validate_isolated_action_tree_journal(*successful, roots, programs, context, checkpoint));
               BOOST_CHECK(successful->state.matches_base_reads(owner, checkpoint));
               phase = "complete";
               break;
            }
         } catch (const isolated_execution_interrupted& interrupted) {
            BOOST_CHECK(interrupted.reason == isolated_interrupt_reason::deadline);
            deadline_reached = true;
         }
         const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
         // Avoid synchronous stream formatting/output within the deadline.
         // These records still describe the bounded private diagnostic only.
         for (size_t i = 0; i < rounds; ++i) {
            if (!observed[i]) continue;
            BOOST_TEST_MESSAGE("owned request round=" << (i + 1) << " rows=" << observed_rows[i]
               << " bytes=" << observed_bytes[i] << " elapsed_us=" << observed_elapsed[i]);
            if (const auto* p = std::get_if<primary_capture_request>(&*observed[i])) {
               BOOST_TEST_MESSAGE("primary table=" << p->table.code.to_string() << '/'
                  << p->table.scope.to_string() << '/' << p->table.table.to_string()
                  << " has_query=" << bool(p->query) << " kind=" << (p->query ? int(p->query->kind) : -1)
                  << " primary=" << (p->query ? p->query->primary_key : 0));
            } else if (const auto* s = std::get_if<secondary_capture_request>(&*observed[i])) {
               const auto& q = s->query;
               BOOST_TEST_MESSAGE("secondary table=" << q.table.code.to_string() << '/'
                  << q.table.scope.to_string() << '/' << q.table.table.to_string()
                  << " type=" << int(secondary_kind(q.key)) << " kind=" << int(q.kind)
                  << " primary=" << q.primary_key);
            } else if (const auto* a = std::get_if<account_capture_request>(&*observed[i])) {
               BOOST_TEST_MESSAGE("account=" << a->account.to_string());
            }
         }
         BOOST_TEST_MESSAGE("private discovery summary success=" << bool(successful)
            << " rounds=" << rounds << " tables=" << tables.size()
            << " primary_queries=" << primary_queries.size() << " secondary_queries=" << secondary_queries.size()
            << " accounts=" << accounts.size() << " last_rows=" << last_rows << " last_bytes=" << last_bytes
            << " deadline=" << deadline_reached << " phase=" << phase << " elapsed_us=" << elapsed);
         BOOST_CHECK(chain.control->calculate_integrity_hash() == original);
         BOOST_CHECK_LE(rounds, 64u);
         BOOST_CHECK_MESSAGE(successful && !deadline_reached,
            "private sparse discovery did not finish within the unchanged global ceiling/envelope");
      }
   }
}
BOOST_AUTO_TEST_SUITE_END()
