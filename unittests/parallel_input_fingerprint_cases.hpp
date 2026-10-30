#pragma once
#include <eosio/chain/parallel_input_fingerprint.hpp>
#include <future>
BOOST_AUTO_TEST_SUITE(parallel_input_fingerprint_tests)
using namespace eosio::chain::experimental;
std::vector<owned_fingerprint_input> make_inputs(size_t count=24) {
   std::vector<owned_fingerprint_input> result(count);
   for(size_t i=0;i<count;++i) {
      auto& input=result[i];input.context.max_inline_action_depth=4+(i%3);
      input.context.console_enabled=i%2;input.context.context_free_data=isolated_context_free_data::capture({});
      for(size_t r=0;r<1+i%3;++r) {
         isolated_action_input action;action.receiver=action.account="tokenone"_n;
         action.action="transfer"_n;action.data.assign(64+i,char(i+r));
         action.authorizations={{"alice"_n,"active"_n}};action.max_return_bytes=256+i;
         input.roots.push_back(action);root_fingerprint_binding binding;
         binding.code_hash=fc::sha256::hash(std::to_string(i+r));
         binding.limits.max_pages=16+i;input.bindings.push_back(binding);
      }
   }return result;
}
std::pair<fc::sha256,fc::sha256> original_owner_fingerprints(const owned_fingerprint_input& input) {
   std::vector<fc::sha256> execution,hints;
   for(size_t i=0;i<input.roots.size();++i) {
      const auto& action=input.roots[i];const auto& binding=input.bindings[i];
      execution.push_back(isolated_execution_fingerprint(binding.code_hash,binding.limits,action));
      fc::sha256::encoder shape;constexpr char domain[]="pureeos/access-path-shape/v1";
      shape.write(domain,sizeof(domain));fc::raw::pack(shape,binding.code_hash);
      fc::raw::pack(shape,binding.limits.max_pages);fc::raw::pack(shape,binding.limits.max_call_depth);
      fc::raw::pack(shape,action.receiver);fc::raw::pack(shape,action.account);fc::raw::pack(shape,action.action);
      fc::raw::pack(shape,action.authorizations);fc::raw::pack(shape,action.data.size());
      fc::raw::pack(shape,action.max_return_bytes);hints.push_back(shape.result());
   }
   return {isolated_action_tree_fingerprint(execution,input.context),isolated_action_tree_fingerprint(hints,input.context)};
}
BOOST_AUTO_TEST_CASE(original_full_formats_and_owned_payloads_match_for_every_participant_count) {
   for(size_t n:{1u,2u,4u,8u}) {
      owned_fingerprint_pool pool(n);auto original=make_inputs();std::vector<std::pair<fc::sha256,fc::sha256>> expected;
      for(const auto& input:original)expected.push_back(original_owner_fingerprints(input));
      auto input_copy=original;auto actual=pool.prepare(std::move(input_copy),{});
      BOOST_REQUIRE_EQUAL(actual.size(),original.size());
      for(size_t i=0;i<actual.size();++i) {
         BOOST_CHECK(actual[i].execution_fingerprint==expected[i].first);
         BOOST_CHECK(actual[i].hint_fingerprint==expected[i].second);
         BOOST_CHECK(actual[i].context==original[i].context);
         BOOST_REQUIRE_EQUAL(actual[i].roots.size(),original[i].roots.size());
         for(size_t r=0;r<actual[i].roots.size();++r) {
            BOOST_CHECK(actual[i].roots[r].data==original[i].roots[r].data);
            BOOST_CHECK(actual[i].roots[r].authorizations==original[i].roots[r].authorizations);
            BOOST_CHECK(actual[i].bindings[r].code_hash==original[i].bindings[r].code_hash);
            BOOST_CHECK(actual[i].bindings[r].limits==original[i].bindings[r].limits);
         }
      }
      original.clear();BOOST_CHECK(!actual.front().roots.front().data.empty());
      BOOST_CHECK(pool.prepare({},{}).empty());
   }
}
BOOST_AUTO_TEST_CASE(bounds_wrong_owner_cancel_deadline_and_healthy_reuse) {
   owned_fingerprint_pool pool(4);
   auto wrong=std::async(std::launch::async,[&]{
      try{pool.prepare(make_inputs(),{});}catch(const state_view_wrong_thread&){return true;}return false;
   });BOOST_CHECK(wrong.get());
   auto mismatch=make_inputs();mismatch[0].bindings.clear();
   BOOST_CHECK_THROW(pool.prepare(std::move(mismatch),{}),primary_state_invalid_attempt);
   auto data=make_inputs();data[0].roots[0].data.resize(65537);
   BOOST_CHECK_THROW(pool.prepare(std::move(data),{}),primary_state_budget_exceeded);
   auto auth=make_inputs();auth[0].roots[0].authorizations.resize(65);
   BOOST_CHECK_THROW(pool.prepare(std::move(auth),{}),primary_state_budget_exceeded);
   auto too_many=make_inputs(257);
   BOOST_CHECK_THROW(pool.prepare(std::move(too_many),{}),primary_state_budget_exceeded);
   auto large=make_inputs(80);for(auto& item:large)for(auto& root:item.roots)root.data.resize(65536);
   BOOST_CHECK_THROW(pool.prepare(std::move(large),{}),primary_state_budget_exceeded);
   auto generation=std::make_shared<std::atomic<uint64_t>>(1);isolated_cancel cancel;cancel.generation=generation;
   BOOST_CHECK_THROW(pool.prepare(make_inputs(),cancel),isolated_execution_interrupted);
   cancel.observed=1;cancel.deadline=std::chrono::steady_clock::now();
   BOOST_CHECK_THROW(pool.prepare(make_inputs(),cancel),isolated_execution_interrupted);
   cancel.deadline=std::chrono::steady_clock::time_point::max();
   BOOST_CHECK_EQUAL(pool.prepare(make_inputs(),cancel).size(),24u);
}
BOOST_AUTO_TEST_SUITE_END()
