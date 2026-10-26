#pragma once

// Real EOS token WASM deployed under two independent contract accounts.
// Same symbols and users intentionally exercise contract-scoped isolation.
BOOST_AUTO_TEST_SUITE(parallel_multitoken_tests)
using parallel_production_tests::production_chain;
using parallel_production_tests::metadata;
using parallel_production_tests::production_push;

const std::array<name, 2> token_contracts{"tokenone"_n, "tokentwo"_n};
const std::array<name, 4> token_users{"alice"_n, "bob"_n, "carol"_n, "dave"_n};

signed_transaction transfer(tester& chain, name contract, name from, name to,
                            int64_t units, uint64_t nonce) {
   signed_transaction trx;
   trx.actions.push_back(chain.get_action(contract, "transfer"_n, {{from, "active"_n}},
      fc::mutable_variant_object()("from", from)("to", to)
         ("quantity", asset{units, symbol::from_string("4,TKN")}.to_string())
         ("memo", std::to_string(nonce))));
   chain.set_transaction_headers(trx);
   trx.sign(chain.get_private_key(from, "active"), chain.control->get_chain_id());
   return trx;
}

int64_t row_amount(tester& chain, name contract, name scope, name table) {
   const auto row=chain.get_row_by_account(contract, scope, table,
      name{symbol::from_string("4,TKN").to_symbol_code().value});
   BOOST_REQUIRE_EQUAL(row.size(), table=="stat"_n ? 40u : 16u);
   int64_t units; std::memcpy(&units,row.data(),sizeof(units));return units;
}
int64_t balance(tester& chain,name contract,name owner) {
   return row_amount(chain,contract,owner,"accounts"_n);
}
int64_t supply(tester& chain,name contract) {
   return row_amount(chain,contract,name{symbol::from_string("4,TKN").to_symbol_code().value},"stat"_n);
}

void setup(tester& chain,const std::vector<uint8_t>& code) {
   chain.create_accounts({token_contracts[0],token_contracts[1],token_users[0],token_users[1],token_users[2],token_users[3]});
   std::ifstream stream(std::filesystem::path(__FILE__).parent_path()/"contracts/eosio.token/eosio.token.abi");
   BOOST_REQUIRE(stream.good());
   const std::string abi{std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
   for(size_t i=0;i<token_contracts.size();++i) {
      const auto contract=token_contracts[i];
      chain.set_code(contract,code);chain.set_abi(contract,abi);
      BOOST_REQUIRE(!chain.push_action(contract,"create"_n,contract,
         fc::mutable_variant_object()("issuer","alice")("maximum_supply","1000000.0000 TKN"))->except);
      BOOST_REQUIRE(!chain.push_action(contract,"issue"_n,"alice"_n,
         fc::mutable_variant_object()("to","alice")("quantity",i ? "20000.0000 TKN" : "10000.0000 TKN")("memo","multitoken setup"))->except);
      for(size_t u=1;u<token_users.size();++u) {
         BOOST_REQUIRE(!chain.push_action(contract,"open"_n,token_users[u],
            fc::mutable_variant_object()("owner",token_users[u])("symbol","4,TKN")("ram_payer",token_users[u]))->except);
         auto funding=transfer(chain,contract,"alice"_n,token_users[u],1000000,100+i*10+u);
         BOOST_REQUIRE(!chain.push_transaction(funding)->except);
      }
      BOOST_CHECK_EQUAL(supply(chain,contract),i ? 200000000 : 100000000);
   }
}

genesis_state genesis() {
   auto result=base_tester::default_genesis();
   result.initial_configuration.max_block_cpu_usage=3200000;
   result.initial_configuration.max_block_net_usage=33554432;
   result.initial_configuration.validate();
   auto [private_key,public_key,proof]=get_bls_key("eosio"_n);
   result.initial_savanna.emplace();result.initial_savanna->finalizer_key=public_key;
   result.initial_savanna->proof_of_possession=proof;
   return result;
}

BOOST_AUTO_TEST_CASE(deployed_token_contracts_keep_same_symbol_balances_and_supply_isolated) {
   const auto code=token_fixture_code();
   const auto hash=fc::sha256::hash(reinterpret_cast<const char*>(code.data()),code.size());
   const auto chain_genesis=genesis();
   std::vector<std::vector<char>> blocks;
   std::array<std::array<int64_t,4>,2> expected_balances{};
   {
      fc::temp_directory directory;
      auto cfg=base_tester::default_config(directory).first;
      eosio::testing::configure_parallel_test_runtime(cfg);
      production_chain source(cfg,chain_genesis);
      source.set_node_finalizers(std::array<name,1>{"eosio"_n});
      auto seal=[&] {blocks.push_back(fc::raw::pack(*source.produce_block()));source.control->abort_block();};
      setup(source,code);seal();
      uint64_t nonce=1000;
      auto push=[&](name contract,name from,name to,int64_t units) {
         auto signed_transfer=transfer(source,contract,from,to,units,nonce++);
         auto trace=source.push_transaction(signed_transfer);
         BOOST_REQUIRE(!trace->except);BOOST_REQUIRE_EQUAL(trace->action_traces.size(),3u);
         BOOST_CHECK(trace->action_traces[0].receiver==contract);
         BOOST_CHECK(trace->action_traces[1].receiver==from);
         BOOST_CHECK(trace->action_traces[2].receiver==to);
      };
      push(token_contracts[0],"alice"_n,"bob"_n,10000);
      push(token_contracts[1],"alice"_n,"bob"_n,20000);seal();
      push(token_contracts[0],"bob"_n,"carol"_n,3000);
      push(token_contracts[1],"bob"_n,"dave"_n,5000);
      push(token_contracts[0],"carol"_n,"bob"_n,1000);
      push(token_contracts[1],"dave"_n,"bob"_n,2000);seal();
      auto combined=combine_actions(source,{transfer(source,token_contracts[0],"alice"_n,"carol"_n,700,nonce++),
         transfer(source,token_contracts[1],"alice"_n,"carol"_n,1100,nonce++)});
      BOOST_REQUIRE(!source.push_transaction(combined)->except);seal();
      for(size_t c=0;c<2;++c)for(size_t u=0;u<4;++u)
         expected_balances[c][u]=balance(source,token_contracts[c],token_users[u]);
      BOOST_CHECK_EQUAL(expected_balances[0][1],1008000);
      BOOST_CHECK_EQUAL(expected_balances[1][1],1017000);
      for(unsigned i=0;i<5;++i)seal();
   }
   for(uint16_t workers:{0,1,2,8}) {
      fc::temp_directory reference_directory,observed_directory;
      auto reference_cfg=base_tester::default_config(reference_directory).first;
      auto observed_cfg=base_tester::default_config(observed_directory).first;
      eosio::testing::configure_parallel_test_runtime(reference_cfg);
      eosio::testing::configure_parallel_test_runtime(observed_cfg);
      observed_cfg.parallel_validation_threads=workers;
      observed_cfg.parallel_validation_batch_size=24;
      observed_cfg.parallel_code_allowlist={hash};
      tester reference(reference_cfg,chain_genesis),observed(observed_cfg,chain_genesis);
      std::vector<std::vector<char>> expected_traces,actual_traces;
      auto a=reference.control->applied_transaction().connect([&](const auto& event){expected_traces.push_back(consensus_trace(std::get<0>(event)));});
      auto b=observed.control->applied_transaction().connect([&](const auto& event){actual_traces.push_back(consensus_trace(std::get<0>(event)));});
      for(const auto& packed:blocks) {
         expected_traces.clear();actual_traces.clear();
         reference.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(packed)));
         observed.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(packed)));
         reference.control->abort_block();observed.control->abort_block();
         BOOST_CHECK(expected_traces==actual_traces);
         BOOST_CHECK(reference.head().id()==observed.head().id());
         BOOST_CHECK_EQUAL(reference.last_irreversible_block_num(),observed.last_irreversible_block_num());
         BOOST_CHECK(reference.control->calculate_integrity_hash()==observed.control->calculate_integrity_hash());
      }
      if(workers)BOOST_CHECK_GT(observed.control->get_parallel_execution_stats().adopted,0u);
      for(size_t c=0;c<2;++c) {
         BOOST_CHECK_EQUAL(supply(observed,token_contracts[c]),c ? 200000000 : 100000000);
         for(size_t u=0;u<4;++u) {
            BOOST_CHECK_EQUAL(balance(reference,token_contracts[c],token_users[u]),expected_balances[c][u]);
            BOOST_CHECK_EQUAL(balance(observed,token_contracts[c],token_users[u]),expected_balances[c][u]);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(multi_contract_production_checks_authority_and_rolls_back_cross_contract_failure) {
   const auto code=token_fixture_code();
   const auto hash=fc::sha256::hash(reinterpret_cast<const char*>(code.data()),code.size());
   for(uint16_t workers:{0,2,8}) {
      fc::temp_directory directory;
      auto cfg=base_tester::default_config(directory).first;
      eosio::testing::configure_parallel_test_runtime(cfg);
      cfg.parallel_validation_threads=workers;cfg.parallel_validation_batch_size=24;
      cfg.parallel_production_enabled=workers!=0;cfg.parallel_code_allowlist={hash};
      production_chain chain(cfg,genesis());
      chain.set_node_finalizers(std::array<name,1>{"eosio"_n});
      setup(chain,code);chain.produce_block();chain.control->abort_block();chain.start();
      uint64_t nonce=9000;
      const auto before_a=balance(chain,token_contracts[0],"alice"_n);
      const auto before_b=balance(chain,token_contracts[1],"alice"_n);
      std::vector<transaction_metadata_ptr> batch{
         metadata(chain,transfer(chain,token_contracts[0],"alice"_n,"bob"_n,10000,nonce++)),
         metadata(chain,transfer(chain,token_contracts[1],"alice"_n,"bob"_n,20000,nonce++))};
      size_t called=0;
      BOOST_CHECK_EQUAL(chain.control->for_each_parallel_production_transaction(batch,fc::time_point::maximum(),[&](const auto& trx){
         BOOST_CHECK(trx==batch.at(called++));auto trace=production_push(chain,trx);
         BOOST_REQUIRE(!trace->except);BOOST_REQUIRE(trace->receipt);
         BOOST_CHECK_GE(trace->receipt->cpu_usage_us,100u);return true;
      }),2u);
      BOOST_CHECK_EQUAL(called,2u);
      if(workers)BOOST_CHECK_GE(chain.control->get_parallel_production_stats().adopted,2u);
      BOOST_CHECK_EQUAL(balance(chain,token_contracts[0],"alice"_n),before_a-10000);
      BOOST_CHECK_EQUAL(balance(chain,token_contracts[1],"alice"_n),before_b-20000);
      const auto saved_a=balance(chain,token_contracts[0],"alice"_n);
      const auto saved_b=balance(chain,token_contracts[1],"alice"_n);
      const auto saved_bob=balance(chain,token_contracts[0],"bob"_n);
      auto bad=combine_actions(chain,{transfer(chain,token_contracts[0],"alice"_n,"bob"_n,1,nonce++),
         transfer(chain,token_contracts[1],"alice"_n,"bob"_n,1000000000000LL,nonce++)});
      auto failed=production_push(chain,metadata(chain,bad));BOOST_REQUIRE(failed->except);
      BOOST_CHECK_EQUAL(balance(chain,token_contracts[0],"alice"_n),saved_a);
      BOOST_CHECK_EQUAL(balance(chain,token_contracts[1],"alice"_n),saved_b);
      BOOST_CHECK_EQUAL(balance(chain,token_contracts[0],"bob"_n),saved_bob);
      auto wrong=transfer(chain,token_contracts[1],"alice"_n,"bob"_n,1,nonce++);
      wrong.signatures.clear();wrong.sign(chain.get_private_key("bob"_n,"active"),chain.control->get_chain_id());
      auto unauthorized=production_push(chain,metadata(chain,wrong));BOOST_REQUIRE(unauthorized->except);
      BOOST_CHECK_EQUAL(balance(chain,token_contracts[1],"alice"_n),saved_b);
      BOOST_CHECK_EQUAL(supply(chain,token_contracts[0]),100000000);
      BOOST_CHECK_EQUAL(supply(chain,token_contracts[1]),200000000);
   }
}
BOOST_AUTO_TEST_SUITE_END()
