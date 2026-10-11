#include <eosio/chain/transaction_state_view.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/testing/tester.hpp>
#include <eosio/testing/bls_utils.hpp>
#include <test_contracts.hpp>
#include <boost/test/unit_test.hpp>
#include <array>

using namespace eosio::chain;
using namespace eosio::chain::experimental;
using namespace eosio::testing;

namespace {
struct trace_summary {
   std::vector<char> transaction_id;
   std::vector<char> receipt;
   std::vector<std::vector<char>> action_receipts;
   std::vector<std::vector<char>> return_values;
   std::vector<std::vector<char>> ram_deltas;
   bool operator==(const trace_summary&) const = default;
};

trace_summary summarize(const transaction_trace_ptr& trace) {
   trace_summary result;
   result.transaction_id = fc::raw::pack(trace->id);
   if (trace->receipt) result.receipt = fc::raw::pack(*trace->receipt);
   for (const auto& action : trace->action_traces) {
      result.action_receipts.push_back(action.receipt ? fc::raw::pack(*action.receipt) : std::vector<char>{});
      result.return_values.push_back(action.return_value);
      result.ram_deltas.push_back(fc::raw::pack(action.account_ram_deltas));
   }
   return result;
}
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_serial_reference_tests)

// Both controllers remain serial. The observer calls the experimental read
// adapter; the reference uses the existing tester/database APIs. Blocks are
// serialized once and independently decoded for each validator. This is a
// reusable prefix/state/output oracle, NOT a parallel-executor acceptance test.
BOOST_AUTO_TEST_CASE(native_savanna_token_corpus_preserves_state_and_receipts) {
   auto genesis = base_tester::default_genesis();
   auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace();
   genesis.initial_savanna->finalizer_key = public_key;
   genesis.initial_savanna->proof_of_possession = proof;

   std::vector<std::vector<char>> corpus;
   fc::temp_directory source_dir;
   {
      tester source(base_tester::default_config(source_dir).first, genesis);
      source.set_node_finalizers(std::array<account_name, 1>{"eosio"_n});
      auto seal = [&] { corpus.push_back(fc::raw::pack(*source.produce_block())); };
      source.create_accounts({"token"_n, "alice"_n, "bob"_n, "carol"_n});
      seal();
      source.set_code("token"_n, test_contracts::eosio_token_wasm());
      source.set_abi("token"_n, test_contracts::eosio_token_abi());
      seal();
      source.push_action("token"_n, "create"_n, "token"_n,
         fc::mutable_variant_object()("issuer", "token")("maximum_supply", "1000000.0000 PAR"));
      source.issue("token"_n, "1000.0000 PAR", "token"_n, "reference corpus");
      seal();
      source.transfer("token"_n, "alice"_n, "100.0000 PAR", "fund alice", "token"_n);
      source.transfer("token"_n, "bob"_n, "50.0000 PAR", "fund bob", "token"_n);
      seal();
      source.transfer("alice"_n, "bob"_n, "10.0000 PAR", "dependency one", "token"_n);
      source.transfer("bob"_n, "carol"_n, "5.0000 PAR", "dependency two", "token"_n);
      seal();
      source.transfer("bob"_n, "alice"_n, "55.0000 PAR", "empty bob", "token"_n);
      source.push_action("token"_n, "close"_n, "bob"_n,
         fc::mutable_variant_object()("owner", "bob")("symbol", "4,PAR"));
      seal();
      source.transfer("alice"_n, "bob"_n, "1.0000 PAR", "recreate row", "token"_n);
      seal();
      // Validate a rejected input restores the source pending state. It is not
      // inserted into the accepted-block corpus or counted as an accepted trx.
      const auto before_failure = source.control->calculate_integrity_hash();
      BOOST_CHECK_EXCEPTION(
         source.transfer("alice"_n, "carol"_n, "5000.0000 PAR", "overdraw", "token"_n),
         eosio_assert_message_exception,
         [](const auto& error) { return error.to_detail_string().find("overdrawn balance") != std::string::npos; });
      BOOST_CHECK(source.control->calculate_integrity_hash() == before_failure);
      for (unsigned i = 0; i < 12; ++i) seal();
   }

   fc::temp_directory reference_dir, observed_dir;
   tester reference(base_tester::default_config(reference_dir).first, genesis);
   tester observed(base_tester::default_config(observed_dir).first, genesis);
   chainbase_transaction_state_view view(observed.control->db());
   std::vector<trace_summary> reference_traces, observed_traces;
   auto reference_connection = reference.control->applied_transaction().connect([&](const auto& event) {
      reference_traces.push_back(summarize(std::get<0>(event)));
   });
   auto observed_connection = observed.control->applied_transaction().connect([&](const auto& event) {
      observed_traces.push_back(summarize(std::get<0>(event)));
   });

   const auto token_symbol = symbol::from_string("4,PAR");
   const auto primary = token_symbol.to_symbol_code().value;
   size_t compared_traces = 0;
   size_t compared_actions = 0;
   for (size_t block_index = 0; block_index < corpus.size(); ++block_index) {
      reference_traces.clear();
      observed_traces.clear();
      reference.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(corpus[block_index])));
      observed.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(corpus[block_index])));
      reference.control->abort_block();
      observed.control->abort_block();
      BOOST_CHECK(reference.control->head().id() == observed.control->head().id());
      BOOST_CHECK_EQUAL(reference.last_irreversible_block_num(), observed.last_irreversible_block_num());
      BOOST_CHECK(reference_traces == observed_traces);
      compared_traces += reference_traces.size();
      for (const auto& trace : reference_traces) compared_actions += trace.action_receipts.size();
      const auto state_before_reads = observed.control->calculate_integrity_hash();
      BOOST_CHECK(reference.control->calculate_integrity_hash() == state_before_reads);

      for (name owner : {"token"_n, "alice"_n, "bob"_n, "carol"_n, "missing"_n}) {
         const table_key key{"token"_n, owner, "accounts"_n};
         const auto result = view.read_primary({key, primary_query_kind::find, primary}, 1024);
         const auto* table = reference.control->db().find<table_id_object, by_code_scope_table>(
            boost::make_tuple(key.code, key.scope, key.table));
         BOOST_CHECK_EQUAL(bool(result.table), table != nullptr);
         if (table) {
            BOOST_CHECK_EQUAL(result.table->object_id, table->id._id);
            BOOST_CHECK_EQUAL(result.table->count, table->count);
            BOOST_CHECK(result.table->payer == table->payer);
         }
         const auto old_api_value = reference.get_row_by_account(key.code, key.scope, key.table, name(primary));
         BOOST_CHECK_EQUAL(bool(result.row), !old_api_value.empty()); // token rows are non-empty assets
         if (result.row) BOOST_CHECK(result.row->value == old_api_value);
         if (block_index == 5 && owner == "bob"_n) BOOST_CHECK(!result.table && !result.row);
         if (block_index >= 6 && owner == "bob"_n) BOOST_REQUIRE(result.row);
      }
      BOOST_CHECK(observed.control->calculate_integrity_hash() == state_before_reads);
   }
   BOOST_CHECK_EQUAL(corpus.size(), 19u);
   BOOST_CHECK_GE(compared_traces, 15u); // guard against two disconnected/empty observers passing
   BOOST_CHECK_GE(compared_actions, compared_traces);
   BOOST_CHECK_GT(observed.last_irreversible_block_num(), 0u);
   for (const auto& [owner, expected] : std::array<std::pair<name, int64_t>, 4>{
         std::pair{"token"_n, 8500000}, {"alice"_n, 1440000}, {"bob"_n, 10000}, {"carol"_n, 50000}}) {
      const auto result = view.read_primary({{"token"_n, owner, "accounts"_n}, primary_query_kind::find, primary}, 1024);
      BOOST_REQUIRE(result.row);
      const auto balance = fc::raw::unpack<asset>(result.row->value);
      BOOST_CHECK_EQUAL(balance.get_amount(), expected);
      BOOST_CHECK(balance.get_symbol() == token_symbol);
   }
}

BOOST_AUTO_TEST_SUITE_END()
