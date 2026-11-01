
BOOST_AUTO_TEST_SUITE(pureeos_blacklist_capacity_tests)
BOOST_AUTO_TEST_CASE(high_vote_ban_releases_capacity_and_cannot_return) {
 sequential_rotation_fixture f(212); f.completed(21);
 const auto victim=f.nodes[21], backfill=f.nodes[210], outside=f.nodes[211];
 BOOST_REQUIRE_EQUAL(f.rotation()["capacity"].as_uint64(),210u);
 f.until([&]{return f.rotation()["ranked_count"].as_uint64()==210 && !f.rotation()["ranking"].as_bool() && !f.rotation()["selecting"].as_bool();});
 BOOST_REQUIRE_EQUAL(f.candidate(backfill)["ticket"].as_uint64(),0u);
 f.online.erase(std::find(f.online.begin(),f.online.end(),victim)); f.restart(f.online);
 f.until([&]{return f.candidate(victim)["banned"].as_bool();},1500);
 const auto evidence=resource_json(f.candidate(victim)["evidence_slots"]);
 const auto ban=f.candidate(victim)["ban_final_block"].as_uint64();
 f.until([&]{return f.candidate(backfill)["ticket"].as_uint64()>0 && f.candidate(backfill)["seen_epoch"].as_uint64()==f.rotation()["published_epoch"].as_uint64();},1500);
 BOOST_CHECK_EQUAL(f.rotation()["ranked_count"].as_uint64(),210u);
 BOOST_CHECK_EQUAL(f.candidate(victim)["ticket"].as_uint64(),0u);
 BOOST_CHECK_EQUAL(f.candidate(outside)["ticket"].as_uint64(),0u);
 const unsigned __int128 floor=static_cast<unsigned __int128>(uint64_t(-1))<<64;
 auto rank=[&](name n){return f.candidate(n)["rank"].as_string();};
 const auto revoked=rank(victim);
 BOOST_CHECK_EQUAL(revoked,fc::variant(floor|victim.to_uint64_t()).as_string());
 f.call("unregprod"_n,fc::mutable_variant_object()("producer",victim),victim);
 f.call("regproducer"_n,fc::mutable_variant_object()("producer",victim)("producer_key",f.chain.get_public_key(victim,"replacement"))("url","")("location",0),victim);
 const auto [priv,pub,pop]=get_bls_key("rekeytest111"_n);
 f.call("regfinkey"_n,fc::mutable_variant_object()("finalizer_name",victim)("finalizer_key",pub.to_string())("proof_of_possession",pop.to_string()),victim);
 f.call("actfinkey"_n,fc::mutable_variant_object()("finalizer_name",victim)("finalizer_key",pub.to_string()),victim);
 f.stake("1250.0000 AIC","1250.0000 AIC",victim,victim); f.choose(victim,victim);f.block();
 BOOST_CHECK_EQUAL(rank(victim),revoked);BOOST_CHECK_EQUAL(f.candidate(victim)["ticket"].as_uint64(),0u);
 f.call("refreshgov"_n,fc::mutable_variant_object(),"govdevaaaaaa"_n);
 const auto& gov=*find_upgrade_governance(f.chain.control->db());
 BOOST_CHECK_EQUAL(gov.current_voters.size(),210u);
 BOOST_CHECK(std::find(gov.current_voters.begin(),gov.current_voters.end(),victim)==gov.current_voters.end());
 BOOST_CHECK(std::find(gov.current_voters.begin(),gov.current_voters.end(),backfill)!=gov.current_voters.end());
 BOOST_CHECK_EQUAL(resource_json(f.candidate(victim)["evidence_slots"]),evidence);BOOST_CHECK_EQUAL(f.candidate(victim)["ban_final_block"].as_uint64(),ban);
 f.block();f.snapshot_check();f.observer_replay();
 BOOST_TEST_MESSAGE("Actual WASM: high-vote permanent ban releases top210 slot to211; re-registration, proposer/finalizer rekey and more self votes cannot recover rank; governance excludes ban; snapshot/replay equal");
}
BOOST_AUTO_TEST_CASE(multiple_bans_backfill_multiple_rank_slots) {
 sequential_rotation_fixture f(214);f.completed(21);
 const std::vector<name> victims(f.nodes.begin()+21,f.nodes.begin()+24);
 for(auto n:victims) f.online.erase(std::find(f.online.begin(),f.online.end(),n));
 f.restart(f.online);
 f.until([&]{return f.rotation()["total_banned"].as_uint64()==3;},2000);
 f.until([&]{for(size_t i=210;i<=212;++i)if(!f.candidate(f.nodes[i])["ticket"].as_uint64() || f.candidate(f.nodes[i])["seen_epoch"].as_uint64()!=f.rotation()["published_epoch"].as_uint64())return false;return true;},2000);
 BOOST_CHECK_EQUAL(f.rotation()["ranked_count"].as_uint64(),210u);
 for(auto n:victims){BOOST_CHECK(f.candidate(n)["banned"].as_bool());BOOST_CHECK_EQUAL(f.candidate(n)["ticket"].as_uint64(),0u);BOOST_CHECK_EQUAL(f.candidate(n)["zero_windows"].as_uint64(),3u);}
 BOOST_CHECK_EQUAL(f.candidate(f.nodes[213])["ticket"].as_uint64(),0u);
 BOOST_CHECK_EQUAL(f.chain.control->head_active_finalizer_policy()->finalizers.size(),21u);
 BOOST_CHECK_EQUAL(f.chain.control->head_active_finalizer_policy()->threshold,15u);
 f.block();f.snapshot_check();
 BOOST_TEST_MESSAGE("Three real finalized bans free3 top210 slots, ranks211-213 backfill;214 outside; quorum retained21/15");
}
BOOST_AUTO_TEST_SUITE_END()
