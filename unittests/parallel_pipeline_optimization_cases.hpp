#pragma once
#include <eosio/chain/parallel_immutable_code.hpp>
#include <eosio/chain/parallel_production_budget.hpp>

BOOST_AUTO_TEST_SUITE(parallel_pipeline_optimization_tests)
BOOST_AUTO_TEST_CASE(owned_code_replacements_preserve_sealed_worker_bytes_and_digest) {
   using eosio::chain::experimental::immutable_code;
   std::vector<char> external{'a','b','c'};
   immutable_code source(external);
   const auto frozen=source;
   const auto original_digest=frozen.content_hash();
   external[0]='x';
   source[0]^=1;
   BOOST_CHECK(frozen.view()==std::vector<char>({'a','b','c'}));
   BOOST_CHECK(frozen.content_hash()==original_digest);
   source.resize(10,'z');
   BOOST_CHECK_EQUAL(frozen.size(),3u);
   BOOST_CHECK(frozen.view()==std::vector<char>({'a','b','c'}));
   BOOST_CHECK(frozen.content_hash()==original_digest);
   BOOST_CHECK(original_digest==fc::sha256::hash(frozen.data(),frozen.size()));
   source.assign(external.begin(),external.end());
   BOOST_CHECK(source.content_hash()!=original_digest);
   source.clear();
   BOOST_CHECK(source.empty());
   BOOST_CHECK(frozen.content_hash()==original_digest);
   BOOST_CHECK(frozen.view()==std::vector<char>({'a','b','c'}));
}
BOOST_AUTO_TEST_CASE(serial_reserve_uses_actual_producer_limit_without_changing_private_cap) {
   using eosio::chain::experimental::production_preparation_budget_us;
   constexpr auto unlimited=std::numeric_limits<int64_t>::max();
   BOOST_CHECK_EQUAL(production_preparation_budget_us(155000,150000,3200000,unlimited),0);
   BOOST_CHECK_EQUAL(production_preparation_budget_us(155001,150000,3200000,unlimited),1);
   BOOST_CHECK_EQUAL(production_preparation_budget_us(125000,150000,3200000,unlimited),0);
   BOOST_CHECK_EQUAL(production_preparation_budget_us(125000,150000,3200000,100000),20000);
   BOOST_CHECK_EQUAL(production_preparation_budget_us(105000,150000,3200000,100000),0);
   BOOST_CHECK_EQUAL(production_preparation_budget_us(105001,150000,3200000,100000),1);
   BOOST_CHECK_EQUAL(production_preparation_budget_us(1000000,150000,3200000,100000),50000);
   BOOST_CHECK_EQUAL(production_preparation_budget_us(1000000,150000,6400000,100000),50000);
   BOOST_CHECK_EQUAL(production_preparation_budget_us(6000,150000,1000,unlimited),0);
   BOOST_CHECK_EQUAL(production_preparation_budget_us(6001,150000,1000,unlimited),1);
}
BOOST_AUTO_TEST_CASE(transfer_prefetch_never_assumes_contract_semantics_or_creates_tables) {
   using parallel_production_tests::production_chain;
   using parallel_production_tests::metadata;
   using parallel_production_tests::production_push;
   const auto code=compile("(module (func (export \"apply\") (param i64 i64 i64)))");
   const auto hash=fc::sha256::hash(reinterpret_cast<const char*>(code.data()),code.size());
   const auto genesis=parallel_multitoken_tests::genesis();
   fc::temp_directory producer_directory,validator_directory;
   auto cfg=base_tester::default_config(producer_directory).first;
   auto oracle=base_tester::default_config(validator_directory).first;
   eosio::testing::configure_parallel_test_runtime(cfg);
   eosio::testing::configure_parallel_test_runtime(oracle);
   cfg.parallel_validation_threads=2;cfg.parallel_validation_batch_size=24;
   cfg.parallel_production_enabled=true;cfg.parallel_code_allowlist={hash};
   production_chain producer(cfg,genesis);tester validator(oracle,genesis);
   producer.set_node_finalizers(std::array<name,1>{"eosio"_n});
   producer.create_accounts({"test"_n,"alice"_n,"bob"_n});producer.set_code("test"_n,code);
   auto setup=producer.produce_block();producer.control->abort_block();
   validator.push_block(setup);validator.control->abort_block();producer.start();
   signed_transaction trx;action call;
   call.account="test"_n;call.name="transfer"_n;call.authorization={{"alice"_n,"active"_n}};
   const uint64_t symbol=uint64_t{4}|(uint64_t{'T'}<<8)|(uint64_t{'K'}<<16)|(uint64_t{'N'}<<24);
   const std::array<uint64_t,4> fields{"alice"_n.to_uint64_t(),"bob"_n.to_uint64_t(),1,symbol};
   call.data.assign(reinterpret_cast<const char*>(fields.data()),reinterpret_cast<const char*>(fields.data())+sizeof(fields));
   call.data.push_back(0);trx.actions.push_back(call);producer.set_transaction_headers(trx);
   trx.sign(producer.get_private_key("alice"_n,"active"),producer.control->get_chain_id());
   std::vector<transaction_metadata_ptr> batch{metadata(producer,trx)};
   BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch,fc::time_point::maximum(),[&](const auto& m){
      auto trace=production_push(producer,m);BOOST_REQUIRE(!trace->except);return true;
   }),1u);
   auto block=producer.finish_block();validator.push_block(block);validator.control->abort_block();
   BOOST_CHECK(producer.control->calculate_integrity_hash()==validator.control->calculate_integrity_hash());
   const name symbol_code{symbol>>8};
   for(const auto owner:{"alice"_n,"bob"_n})
      BOOST_CHECK(producer.get_row_by_account("test"_n,owner,"accounts"_n,symbol_code).empty());
   BOOST_CHECK(producer.get_row_by_account("test"_n,symbol_code,"stat"_n,symbol_code).empty());
}

BOOST_AUTO_TEST_CASE(byte_digest_preserves_empty_raw_hash_and_frozen_copy_semantics) {
   using eosio::chain::experimental::immutable_code;
   const auto empty_hash=fc::sha256::hash("",0);
   immutable_code code;
   BOOST_CHECK(code.content_hash()==fc::sha256{});
   BOOST_CHECK(code.byte_hash()==empty_hash);
   BOOST_CHECK(code.byte_hash()!=code.content_hash());
   for (size_t size : {size_t{0},size_t{1},size_t{63},size_t{64},size_t{65},size_t{4096}}) {
      code.resize(size,static_cast<char>(0xa5));
      const auto frozen=code;
      const auto expected=fc::sha256::hash(code.data(),code.size());
      BOOST_CHECK(code.byte_hash()==expected);
      code.resize(size+1,'x');
      BOOST_CHECK(frozen.byte_hash()==expected);
      code[0]^=1;
      BOOST_CHECK(code.byte_hash()==fc::sha256::hash(code.data(),code.size()));
      BOOST_CHECK(frozen.byte_hash()==expected);
      code.clear();
      BOOST_CHECK(code.byte_hash()==empty_hash);
      BOOST_CHECK(code.content_hash()==fc::sha256{});
   }
}
BOOST_AUTO_TEST_SUITE_END()
