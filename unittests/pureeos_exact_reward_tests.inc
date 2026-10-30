// Exact token-unit regression; real WASM at a legal half-second block time.
BOOST_AUTO_TEST_SUITE(pureeos_exact_rate_tests)

BOOST_AUTO_TEST_CASE(one_percent_claim_uses_exact_smallest_units_at_legal_block_time) {
 positive_vote_reward_fixture f(rotation_contracts_for_test(), "1100.0000 SYS");
 f.seal(); f.vote(); f.one_unit(); f.settle_single_producer();
 const auto from=reward_fixture::when(f.global()["last_pervote_bucket_fill"]);
 const auto target=from+fc::microseconds(2280096000000LL);
 const auto delta=target-f.chain.head().block_time()-fc::milliseconds(500);
 BOOST_REQUIRE_GT(delta.count(),0);BOOST_REQUIRE_EQUAL(delta.count()%500000,0);
 f.chain.produce_block(delta);
 BOOST_REQUIRE(f.chain.control->pending_block_time()==target);
 const auto supply=f.supply(),before=f.balance(f.nodes[0]);
 BOOST_REQUIRE_EQUAL(supply,11000000);
 const auto expected=int64_t(static_cast<unsigned __int128>(supply)*2280096000000ULL/(100ULL*364*86400*1000000));
 BOOST_REQUIRE_EQUAL(expected,7975);
 auto result=f.call("claimrewards"_n,fc::mutable_variant_object()("owner",f.nodes[0]),f.nodes[0]);
 BOOST_REQUIRE(!result->except);
 BOOST_CHECK_EQUAL(f.supply()-supply,expected);
 BOOST_CHECK_EQUAL(f.balance(f.nodes[0])-before,expected);
 BOOST_CHECK_EQUAL(f.balance("eosio.saving"_n),0);
 BOOST_CHECK_EQUAL(f.balance("eosio.vpay"_n),0);
}
BOOST_AUTO_TEST_SUITE_END()
