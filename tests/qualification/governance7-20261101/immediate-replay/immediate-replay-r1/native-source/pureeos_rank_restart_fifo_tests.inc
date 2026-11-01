
BOOST_AUTO_TEST_SUITE(pureeos_rank_restart_fifo_tests)
BOOST_AUTO_TEST_CASE(interrupted_snapshot_preserves_existing_waiter_tickets_fault_injection) {
 sequential_rotation_fixture f(212);f.completed(44);
 const auto before_members=f.list("active"),queue=f.list("queue");name expected;
 for(auto n:queue)if(std::find(before_members.begin(),before_members.end(),n)==before_members.end()){expected=n;break;}
 BOOST_REQUIRE(expected!=name{});
 f.until([&]{return f.rotation()["ranking"].as_bool()&&f.rotation()["ranked_count"].as_uint64()>=64;});f.block();f.chain.control->abort_block();
 const auto slot=f.chain.head().header().timestamp.slot;
 const auto when=block_timestamp_type{uint32_t((slot/12+1)*12+1)};
 const auto victim=f.chain.control->head_active_producers(when).get_scheduled_producer(when).producer_name;
 BOOST_REQUIRE(std::find(before_members.begin(),before_members.end(),victim)!=before_members.end());
 BOOST_REQUIRE_EQUAL(f.candidate(victim)["seen_epoch"].as_uint64(),f.rotation()["snapshot_epoch"].as_uint64());
 f.online.erase(std::find(f.online.begin(),f.online.end(),victim));f.restart(f.online);
 // Component-only prior-streak seed, not claimed as finalized punishment
 // evidence. Real three-window bans are covered by separate replayed cases.
 auto& db=const_cast<eosio::chain::database&>(f.chain.control->db());
 const auto& table=db.get<table_id_object,by_code_scope_table>(boost::make_tuple("eosio"_n,"eosio"_n,"rotcands"_n));
 const auto& row=db.get<key_value_object,by_scope_primary>(boost::make_tuple(table.id,victim.to_uint64_t()));
 db.modify(row,[](auto& r){std::string b(r.value.data(),r.value.size());BOOST_REQUIRE_EQUAL(b.size(),114u);b[72]=2;r.value=std::string_view(b);});
 bool interrupted=false;
 for(unsigned i=0;i<80&&!f.candidate(victim)["banned"].as_bool();++i){const auto prior=f.rotation();f.step();if(f.candidate(victim)["banned"].as_bool())interrupted=prior["ranking"].as_bool()&&prior["ranked_count"].as_uint64()>0;}
 BOOST_REQUIRE_MESSAGE(interrupted,"Seeded third miss must settle during a partially completed ranking snapshot");
 f.until([&]{return f.rotation()["stage"].as_uint64()==1;},500);
 BOOST_CHECK_MESSAGE(f.rotation()["incoming"].as_string()==expected.to_string(),"Interrupted ranking changed FIFO: expected "<<expected.to_string()<<", actual "<<f.rotation()["incoming"].as_string());
 BOOST_TEST_MESSAGE("Component-only snapshot interruption check: old waiters retain ticket priority; seeded prior streak is not canonical blacklist evidence");
}
BOOST_AUTO_TEST_SUITE_END()
