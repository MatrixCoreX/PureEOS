#include <eosio/chain/transaction_work_context.hpp>
#include <array>
#include <barrier>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <string_view>
#include <type_traits>

// Allocation failures are injected only into this standalone test executable.
static thread_local int fail_after=-1;
void* operator new(std::size_t bytes) {
   if(fail_after==0) {fail_after=-1;throw std::bad_alloc();}
   if(fail_after>0) --fail_after;
   if(void* p=std::malloc(bytes?bytes:1)) return p;
   throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void* operator new[](std::size_t bytes) {return ::operator new(bytes);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}

namespace {
using namespace eosio::chain;
using context=transaction_work_context;
void require(bool value,const char* text) {if(!value) throw std::runtime_error(text);}
template<typename E,typename F> void rejects(F&& f) {
   try {f();}catch(const E&) {return;}
   throw std::runtime_error("expected transaction work rejection");
}
transaction_work_policy policy(uint64_t units=100) {return {7,units,{11,2}};}
transaction_work_local_limits local(uint64_t units=300,uint32_t trials=8,uint32_t retries=2) {return {units,trials,retries};}
void conserved(const context& c,uint64_t limit) {
   const auto s=c.inspect();
   require(static_cast<unsigned __int128>(s.local_available)+s.local_reserved+s.local_spent==limit,"local ledger conservation violated");
}
// Real VM contexts expose the same scoped capability. This fake is intentional:
// isolate ownership/lifetime/reset semantics without pretending to test WASM.
struct fake_vm {
   struct module {bool execution_work_metered=true;} compiled;
   module& get_module() {return compiled;}
   eosio::vm::execution_work_budget_view current{};
   class guard {
      fake_vm& vm_;
      eosio::vm::execution_work_budget_view previous_;
   public:
      guard(fake_vm& vm,eosio::vm::execution_work_budget_view b):vm_(vm),previous_(vm.current) {b.charge(0);vm.current=b;}
      guard(const guard&)=delete;guard(guard&&)=delete;
      ~guard() {vm_.current=previous_;}
   };
   guard bind_execution_work_budget(eosio::vm::execution_work_budget_view b) {return guard(*this,b);}
};
}
int main(int argc,char** argv) {
   static_assert(!std::is_copy_constructible_v<context> && !std::is_move_constructible_v<context>);
   static_assert(!std::is_copy_constructible_v<context::trial_receipt> && std::is_move_constructible_v<context::trial_receipt>);
   const std::vector<std::pair<const char*,std::function<void()>>> cases{
      {"unconfigured_policy_and_invalid_bounds_fail_before_execution",[] {
         rejects<std::invalid_argument>([] {context c({},{});});
         rejects<std::invalid_argument>([] {context c(policy(10),local(),{11,0});});
         rejects<std::overflow_error>([] {context c(policy(UINT64_MAX),local(),{UINT64_MAX,1});});
         rejects<std::invalid_argument>([] {context c(policy(),{0,1,0});});
         rejects<std::invalid_argument>([] {context c(policy(),{1,context::max_trial_records+1,0});});
         context serial_only(policy(),{});auto attempt=serial_only.begin_final_attempt();attempt.consume({.wasm=10});
         require(attempt.finish().usage.total()==10,"explicit serial-only policy did not work");
      }},
      {"signature_and_fixed_seed_appear_once_in_final_invoice_across_retries",[] {
         context c(policy(),local(),{25,7});
         {auto attempt=c.begin_retryable_attempt();attempt.consume({.native=12,.serial=12});
          require(attempt.discard_for_retry()==execution_work_usage{.native=12,.serial=12},"retry charged seed again");}
         c.advance_epoch();
         auto final=c.begin_final_attempt();final.consume({.native=9,.serial=9});final.consume({.wasm=20});
         const auto r=final.finish();
         require(r.policy_version==7 && r.usage==execution_work_usage{.wasm=20,.signature=25,.native=16,.serial=16},"state-dependent work leaked across epoch or seed was duplicated");
         require(c.inspect().local_spent==12 && c.inspect().finalized,"discarded owner work was not local-only");conserved(c,300);
      }},
      {"healthy_discard_keeps_spent_trial_work_and_never_bills_it",[] {
         context c(policy(),local());auto ticket=c.reserve_trial(60);auto trial=ticket.start();
         trial.consume({.wasm=20,.host=7,.serial=3});trial.discard();
         require(c.inspect().local_spent==27 && c.inspect().local_available==273,"unused trial reservation was not refunded exactly");
         rejects<std::logic_error>([&] {ticket.start();});
         rejects<std::logic_error>([&] {trial.consume({.wasm=1});});
         auto final=c.begin_final_attempt();final.consume({.wasm=5});require(final.finish().usage.wasm==5,"discarded trial entered semantic bill");conserved(c,300);
      }},
      {"failed_predebit_consumes_full_local_reservation_but_allows_final_canonical",[] {
         context c(policy(),local(10,2,0));auto ticket=c.reserve_trial(10);auto trial=ticket.start();
         trial.consume({.wasm=5});rejects<execution_work_exhausted>([&] {trial.consume({.wasm=6});});
         require(trial.usage().total()==5 && trial.exhausted(),"failed debit changed successful usage");trial.discard();
         require(c.inspect().local_spent==10 && c.inspect().local_available==0 && !c.inspect().terminal,"failed local trial rejected semantic TX or refunded its failed allowance");
         rejects<transaction_work_local_exhausted>([&] {c.reserve_trial(1);});
         auto final=c.begin_final_attempt();final.consume({.wasm=100});require(final.finish().usage.total()==100,"local exhaustion prevented final canonical path");conserved(c,10);
      }},
      {"attempt_count_does_not_refund_when_zero_work_trials_are_cancelled",[] {
         context c(policy(),local(1,3,0));
         for(unsigned i=0;i<3;++i) {auto reservation=c.reserve_trial(1);}
         require(c.inspect().local_available==1 && c.inspect().trials_reserved==3,"cancelled ticket changed attempt count or leaked quota");
         rejects<transaction_work_local_exhausted>([&] {c.reserve_trial(1);});conserved(c,1);
      }},
      {"adopted_whole_transaction_usage_is_once_and_import_has_no_signature",[] {
         context c(policy(),local(),{11,3});auto ticket=c.reserve_trial(60);auto trial=ticket.start();
         trial.consume({.wasm=20,.host=5,.serial=2});auto receipt=trial.seal_success();
         auto final=c.begin_final_attempt();final.adopt(std::move(receipt));
         rejects<std::logic_error>([&] {final.adopt(std::move(receipt));});
         final.consume({.native=7,.serial=7});
         require(final.finish().usage==execution_work_usage{.wasm=20,.host=5,.signature=11,.native=10,.serial=12},"adopted VM was billed twice or seed lost");
         require(c.inspect().local_spent==25,"trial work was unbounded or refunded after adoption");conserved(c,300);
      }},
      {"canonical_retry_does_not_charge_imported_private_work_to_local_twice",[] {
         context c(policy(),local(),{10,4});auto ticket=c.reserve_trial(30);auto trial=ticket.start();trial.consume({.wasm=20});auto receipt=trial.seal_success();
         {auto a=c.begin_retryable_attempt();a.adopt(std::move(receipt));a.consume({.native=5,.serial=5});
          require(a.discard_for_retry().total()==5,"discard counted seed/imported private work twice");}
         require(c.inspect().local_spent==25,"owner retry local cost incorrect");
         auto b=c.begin_final_attempt();b.consume({.wasm=20,.native=5,.serial=5});require(b.finish().usage.total()==39,"final canonical invoice retained abandoned import");conserved(c,300);
      }},
      {"stale_and_cross_context_receipts_fail_without_partial_import",[] {
         context a(policy(),local()),b(policy(),local());auto ticket=a.reserve_trial(20);auto trial=ticket.start();trial.consume({.wasm=9});auto r=trial.seal_success();
         {auto attempt=b.begin_final_attempt();rejects<std::logic_error>([&] {attempt.adopt(std::move(r));});require(attempt.finish().usage.total()==0,"cross-context import partially billed");}
         a.advance_epoch();auto attempt=a.begin_final_attempt();rejects<std::logic_error>([&] {attempt.adopt(std::move(r));});
         attempt.consume({.wasm=3});require(attempt.finish().usage.total()==3,"stale private work entered new epoch");conserved(a,300);conserved(b,300);
      }},
      {"queued_old_epoch_ticket_cannot_start_but_running_old_epoch_is_bounded",[] {
         context c(policy(),local());auto queued=c.reserve_trial(20),running=c.reserve_trial(20);auto trial=running.start();
         c.advance_epoch();rejects<std::logic_error>([&] {queued.start();});
         trial.consume({.wasm=5});auto receipt=trial.seal_success();auto final=c.begin_final_attempt();
         rejects<std::logic_error>([&] {final.adopt(std::move(receipt));});require(final.finish().usage.total()==0,"old running trial escaped its epoch");conserved(c,300);
      }},
      {"semantic_resource_exhaustion_cannot_be_revived_by_retry",[] {
         context c(policy(10),local(100));
         {auto a=c.begin_retryable_attempt();a.consume({.wasm=8});rejects<execution_work_exhausted>([&] {a.consume({.wasm=3});});
          rejects<execution_work_exhausted>([&] {a.discard_for_retry();});rejects<execution_work_exhausted>([&] {a.finish();});}
         require(c.inspect().terminal,"semantic resource failure was not terminal");
         rejects<std::logic_error>([&] {c.begin_final_attempt();});conserved(c,100);
      }},
      {"retryable_owner_reserves_worst_case_before_work_and_final_cannot_retry",[] {
         context c(policy(100),local(50,0,1),{10,0});
         rejects<transaction_work_local_exhausted>([&] {c.begin_retryable_attempt();});
         auto a=c.begin_final_attempt();a.consume({.wasm=1});
         rejects<std::logic_error>([&] {a.discard_for_retry();});require(c.inspect().terminal,"final retry misuse was not rejected closed");conserved(c,50);
      }},
      {"nested_VM_bindings_share_semantic_budget_and_restore_parent",[] {
         context c(policy(100),local(),{10,0});auto a=c.begin_final_attempt();fake_vm vm,child;
         {auto root=a.bind_vm(vm);vm.current.charge(3);
          {auto reentrant=a.bind_vm(vm);vm.current.charge(1);}
          {auto nested=a.bind_vm(child);child.current.charge(7);require(child.current.remaining==vm.current.remaining,"sync used a new budget");}
          require(!child.current.remaining,"nested VM binding did not restore predecessor");vm.current.charge(2);}
         require(!vm.current.remaining && a.finish().usage==execution_work_usage{.wasm=13,.signature=10},"nested work lost/doubled or binding outlived parent");
      }},
      {"live_borrow_forbids_finish_and_scope_storage_survives_attempt_destruction",[] {
         context c(policy(),local());auto* a=new context::semantic_attempt(c.begin_final_attempt());fake_vm vm;
         auto* binding=new context::vm_binding<fake_vm>(a->bind_vm(vm));
         rejects<std::logic_error>([&] {a->finish();});require(c.inspect().terminal,"live VM finish was not terminal");
         delete a;rejects<execution_work_exhausted>([&] {vm.current.charge(1);});
         delete binding;require(!vm.current.remaining,"destroyed binding did not restore original view");
      }},
      {"raw_native_borrow_cannot_smuggle_a_second_signature_charge",[] {
         context c(policy(),local(),{5,0});auto a=c.begin_final_attempt();
         {auto lease=a.borrow();lease.meter().consume({.signature=5});}
         rejects<std::logic_error>([&] {a.finish();});require(c.inspect().terminal,"second signature escaped final provenance check");
      }},
      {"binding_rejects_uninstrumented_modules_before_replacing_a_VM_view",[] {
         context c(policy(),local());auto a=c.begin_final_attempt();fake_vm vm;vm.compiled.execution_work_metered=false;
         rejects<std::logic_error>([&] {a.bind_vm(vm);});require(!vm.current.remaining,"unmetered module changed active VM binding");
         rejects<execution_work_exhausted>([&] {a.finish();});require(c.inspect().terminal,"unmetered execution could finish with an empty bill");
      }},
      {"failed_trial_with_live_VM_is_closed_and_cannot_export_result",[] {
         context c(policy(),local(20));auto ticket=c.reserve_trial(20);auto trial=ticket.start();fake_vm vm;
         {auto b=trial.bind_vm(vm);vm.current.charge(3);rejects<std::logic_error>([&] {trial.seal_success();});
          rejects<execution_work_exhausted>([&] {vm.current.charge(1);});}
         require(c.inspect().local_spent==20 && !c.inspect().terminal,"failed trial live binding leaked quota or poisoned canonical execution");
         rejects<std::logic_error>([&] {trial.seal_success();});auto final=c.begin_final_attempt();final.finish();conserved(c,20);
      }},
      {"reservation_can_move_to_worker_and_context_lifetime_is_not_borrowed",[] {
         context c(policy(),local());auto reservation=c.reserve_trial(20);std::optional<context::trial_receipt> receipt;std::exception_ptr error;
         std::thread worker([r=std::move(reservation),&receipt,&error]() mutable {
            try {auto trial=r.start();trial.consume({.wasm=7});receipt.emplace(trial.seal_success());}catch(...) {error=std::current_exception();}
         });worker.join();if(error) std::rethrow_exception(error);
         auto final=c.begin_final_attempt();final.adopt(std::move(*receipt));require(final.finish().usage.wasm==7,"worker reservation/receipt lost provenance");conserved(c,300);
      }},
      {"concurrent_trials_cannot_oversubscribe_shared_local_reservations",[] {
         context c(policy(),local(100,8,0));std::barrier gate(4);std::array<std::exception_ptr,4> errors{};std::vector<std::thread> threads;
         for(unsigned i=0;i<4;++i) threads.emplace_back([&,i] {
            try {auto ticket=c.reserve_trial(25);auto trial=ticket.start();gate.arrive_and_wait();trial.consume({.wasm=5+i});trial.discard();}
            catch(...) {errors[i]=std::current_exception();gate.arrive_and_drop();}
         });
         for(auto& thread:threads) thread.join();for(auto error:errors) if(error) std::rethrow_exception(error);
         require(c.inspect().local_spent==26 && !c.inspect().local_reserved && c.inspect().local_available==74,"concurrent local ledger drifted");conserved(c,100);
      }},
      {"near_uint64_limits_preserve_reservation_and_invoice_conservation",[] {
         context c(policy(UINT64_MAX),local(UINT64_MAX,2,1),{UINT64_MAX-20,0});
         {auto ticket=c.reserve_trial(UINT64_MAX);auto trial=ticket.start();trial.consume({.wasm=1});trial.discard();}
         {auto a=c.begin_retryable_attempt();a.consume({.wasm=2});a.discard_for_retry();}
         auto final=c.begin_final_attempt();final.consume({.wasm=20});require(final.finish().usage.total()==UINT64_MAX,"uint64 semantic boundary overflowed");
         require(c.inspect().local_spent==3,"uint64 local settlement overflowed");conserved(c,UINT64_MAX);
      }},
      {"slot_allocation_failure_leaves_ticket_and_owner_reservations_unchanged",[] {
         context c(policy(),local(200));auto ticket=c.reserve_trial(20);
         fail_after=0;rejects<std::bad_alloc>([&] {ticket.start();});
         require(c.inspect().local_available==180 && c.inspect().local_reserved==20 && !c.inspect().local_spent,"failed trial slot allocation lost quota");
         {auto trial=ticket.start();trial.consume({.wasm=5});trial.discard();}
         fail_after=0;rejects<std::bad_alloc>([&] {c.begin_retryable_attempt();});
         require(c.inspect().local_available==195 && !c.inspect().local_reserved && !c.inspect().active_semantic && !c.inspect().retryable_owner_attempts,
            "failed owner slot allocation partially reserved quota or consumed retry count");
         auto final=c.begin_final_attempt();final.finish();conserved(c,200);
      }},
      {"local_limits_change_speculation_but_never_the_final_semantic_invoice",[] {
         for(const auto limits:{transaction_work_local_limits{},local(100,2,1),local(1000,16,4)}) {
            context c(policy(),limits,{7,3});
            if(limits.max_trials) {auto ticket=c.reserve_trial(10);auto trial=ticket.start();trial.consume({.wasm=5});trial.discard();}
            auto final=c.begin_final_attempt();final.consume({.wasm=13,.host=5,.native=4,.serial=6});
            require(final.finish().usage==execution_work_usage{.wasm=13,.host=5,.signature=7,.native=7,.serial=9},"local quota leaked into consensus work/invoice");
            conserved(c,limits.cumulative_units);
         }
      }},
      {"thousand_seeded_local_histories_match_independent_spend_oracle",[] {
         uint64_t seed=0x915e1234;auto random=[&] {seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;};
         for(unsigned iteration=0;iteration<1000;++iteration) {
            context c(policy(),local(1000,128,0));uint64_t spent=0;
            for(unsigned i=0;i<64 && spent<1000;++i) {
               const uint64_t available=1000-spent,allowance=1+random()%std::min<uint64_t>(available,50),used=random()%(allowance+1);
               auto ticket=c.reserve_trial(allowance);auto trial=ticket.start();trial.consume({.wasm=used});
               if(random()%5==0) {
                  rejects<execution_work_exhausted>([&] {trial.consume({.wasm=allowance+1});});trial.discard();spent+=allowance;
               }else if(random()%2) {auto receipt=trial.seal_success();spent+=used;}
               else {trial.discard();spent+=used;}
               const auto s=c.inspect();require(s.local_spent==spent && s.local_available==1000-spent && !s.local_reserved,"random local work ledger differs from oracle");
               conserved(c,1000);
            }
            auto final=c.begin_final_attempt();final.consume({.host=17});require(final.finish().usage.host==17,"random abandoned trials entered final invoice");
         }
      }}
   };
   if(argc>2) return 2;
   unsigned failed=0,executed=0;
   for(const auto& [name,test]:cases) {
      if(argc==2 && std::string_view(argv[1])!=name) continue;
      ++executed;
      try {test();std::cerr<<name<<": passed\n";}
      catch(const std::exception& e) {++failed;std::cerr<<name<<": FAILED: "<<e.what()<<'\n';}
   }
   if(!executed) return 2;
   std::cout<<"{\"status\":\""<<(failed?"failed":"passed")<<"\",\"cases\":"<<executed<<",\"passed\":"<<executed-failed<<",\"failed\":"<<failed<<"}\n";
   return failed?1:0;
}
