#pragma once

#include <eosio/chain/execution_work_meter.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace eosio::chain {

// Common, deterministic policy. The caller must obtain this from authenticated
// chain policy; neither a worker count nor a measured clock is an input.
struct transaction_work_policy {
   uint32_t version = 0;
   uint64_t semantic_units = 0;
   eosio::vm::execution_work_memory_prices memory_prices{};
};
// LOCAL speculative/replay safeguards. Exhausting these selects the final
// canonical path; it is not a consensus transaction resource violation.
struct transaction_work_local_limits {
   uint64_t cumulative_units = 0;
   uint32_t max_trials = 0;
   uint32_t max_retryable_owner_attempts = 0;
};
// Only work already performed ONCE on immutable transaction input belongs here.
// State-dependent authorization/code/resource reads must be charged afresh on
// each canonical attempt, after checking its prefix/epoch.
struct transaction_work_seed {
   uint64_t signature_units = 0;
   uint64_t fixed_transaction_native_units = 0;
   execution_work_usage usage() const {
      return {.signature=signature_units,.native=fixed_transaction_native_units,
              .serial=fixed_transaction_native_units};
   }
};
class transaction_work_local_exhausted : public std::runtime_error {
public:
   transaction_work_local_exhausted():std::runtime_error("local transaction work budget exhausted") {}
};

// Disabled building block: does not authenticate effects, verify a receipt,
// perform DB undo, choose a production price table or admit a block.
class transaction_work_context {
   enum class trial_phase : uint8_t { unused,reserved,running,sealed,adopted,discarded };
   struct trial_record {
      trial_phase phase=trial_phase::unused;
      uint64_t allowance=0,epoch=0;
      execution_work_usage usage{};
   };
   struct slot {
      execution_work_meter meter;
      const std::thread::id owner=std::this_thread::get_id();
      std::atomic<uint32_t> borrows{0};
      bool closed=false;
      bool adopted=false;
      execution_work_usage imported{};
      explicit slot(uint64_t units):meter(units) {}
   };
   struct state {
      const transaction_work_policy policy;
      const transaction_work_local_limits local;
      const execution_work_usage seed;
      std::mutex mutex;
      std::vector<trial_record> trials;
      uint64_t available=0,reserved=0,spent=0,epoch=1,generation=0;
      uint32_t next_trial=0,retryable_attempts=0;
      bool alive=true,active=false,final_started=false,finalized=false,terminal=false;
      std::weak_ptr<slot> active_slot;
      execution_work_usage final_usage{};
      state(transaction_work_policy p,transaction_work_local_limits l,execution_work_usage s)
         :policy(p),local(l),seed(s),trials(l.max_trials),available(l.cumulative_units) {}
   };
   std::shared_ptr<state> state_;

   static void owner(const slot& s) {
      if(s.owner!=std::this_thread::get_id()) throw std::logic_error("transaction work slot used by another thread");
   }
   static void alive(const state& s) {
      if(!s.alive || s.finalized || s.terminal) throw std::logic_error("transaction work context is closed");
   }
   // Called under state::mutex; reservations guarantee every addition fits.
   static void settle(state& s,uint64_t allowance,uint64_t charge) {
      if(charge>allowance || allowance>s.reserved || charge>s.local.cumulative_units-s.spent)
         throw std::logic_error("invalid transaction work reservation settlement");
      const auto refund=allowance-charge;
      if(refund>s.local.cumulative_units-s.available) throw std::logic_error("local work refund overflow");
      s.reserved-=allowance;s.spent+=charge;s.available+=refund;
   }
   static execution_work_usage subtract(execution_work_usage a,const execution_work_usage& b) {
      if(a.wasm<b.wasm || a.host<b.host || a.signature<b.signature || a.native<b.native || a.serial<b.serial)
         throw std::logic_error("invalid transaction work provenance subtraction");
      a.wasm-=b.wasm;a.host-=b.host;a.signature-=b.signature;a.native-=b.native;a.serial-=b.serial;
      a.validate();return a;
   }

public:
   static constexpr uint32_t max_trial_records=4096,max_retryable_records=1024;
   struct snapshot {
      uint64_t local_available,local_reserved,local_spent,epoch;
      uint32_t trials_reserved,retryable_owner_attempts;
      bool active_semantic,final_attempt_started,finalized,terminal;
      execution_work_usage final_usage;
   };
   struct result {
      uint32_t policy_version;
      execution_work_usage usage;
   };
   class semantic_attempt;
   class trial_execution;
   class trial_reservation;

   // Keeps meter storage stable even if a native caller incorrectly destroys
   // its attempt before unwinding a VM guard. It does NOT extend DB/controller
   // lifetime: their owner must still join workers before destruction.
   class borrowed_meter {
      friend class transaction_work_context;
      friend class semantic_attempt;
      friend class trial_execution;
      std::shared_ptr<slot> slot_;
      explicit borrowed_meter(std::shared_ptr<slot> s):slot_(std::move(s)) {
         owner(*slot_);
         if(slot_->closed) throw std::logic_error("closed transaction work slot");
         if(slot_->meter.exhausted()) throw execution_work_exhausted();
         if(slot_->borrows.load()==UINT32_MAX) throw std::overflow_error("too many transaction work borrows");
         ++slot_->borrows;
      }
   public:
      borrowed_meter(const borrowed_meter&)=delete;
      borrowed_meter& operator=(const borrowed_meter&)=delete;
      borrowed_meter(borrowed_meter&&)=delete;
      borrowed_meter& operator=(borrowed_meter&&)=delete;
      ~borrowed_meter() {--slot_->borrows;}
      execution_work_meter& meter() const {
         owner(*slot_);
         if(slot_->closed) throw std::logic_error("closed transaction work slot");
         return slot_->meter;
      }
   };
   template<typename VMContext> class vm_binding {
      borrowed_meter lifetime_;
      static execution_work_meter::wasm_budget_view checked_budget(borrowed_meter& lifetime,VMContext& vm,
            eosio::vm::execution_work_memory_prices prices) {
         static_assert(requires {vm.get_module().execution_work_metered;},
                       "VM context must expose its immutable instrumentation marker");
         if(!vm.get_module().execution_work_metered) {
            lifetime.meter().invalidate();
            throw std::logic_error("transaction work requires an instrumented VM module");
         }
         return lifetime.meter().wasm_budget(prices);
      }
      using guard_type=decltype(std::declval<VMContext&>().bind_execution_work_budget(
         std::declval<execution_work_meter::wasm_budget_view>()));
      guard_type guard_; // destroyed BEFORE lifetime_ releases the meter
   public:
      vm_binding(std::shared_ptr<slot> s,VMContext& vm,eosio::vm::execution_work_memory_prices prices)
         :lifetime_(std::move(s)),guard_(vm.bind_execution_work_budget(checked_budget(lifetime_,vm,prices))) {}
      vm_binding(const vm_binding&)=delete;
      vm_binding& operator=(const vm_binding&)=delete;
      vm_binding(vm_binding&&)=delete;
      vm_binding& operator=(vm_binding&&)=delete;
   };

   // Opaque process-local provenance, not a producer-supplied consensus proof.
   // The owner must independently validate code, dependencies, effects and epoch
   // before adopting. Copying/serializing this token is intentionally unsupported.
   class trial_receipt {
      friend class trial_execution;
      friend class semantic_attempt;
      std::shared_ptr<state> state_;
      uint32_t id_=0;
      trial_receipt(std::shared_ptr<state> s,uint32_t id):state_(std::move(s)),id_(id) {}
   public:
      trial_receipt(const trial_receipt&)=delete;
      trial_receipt& operator=(const trial_receipt&)=delete;
      trial_receipt(trial_receipt&& other) noexcept:state_(std::move(other.state_)),id_(other.id_) {}
      trial_receipt& operator=(trial_receipt&&)=delete;
      ~trial_receipt() {
         if(state_) {std::lock_guard lock(state_->mutex);auto& r=state_->trials[id_];if(r.phase==trial_phase::sealed) r.phase=trial_phase::discarded;}
      }
      execution_work_usage usage() const {
         if(!state_) throw std::logic_error("consumed transaction work receipt");
         std::lock_guard lock(state_->mutex);return state_->trials[id_].usage;
      }
   };

   class trial_execution {
      friend class trial_reservation;
      std::shared_ptr<state> state_;
      std::shared_ptr<slot> slot_;
      uint32_t id_=0;
      bool closed_=false;
      explicit trial_execution(trial_reservation&);
      void check() const {
         owner(*slot_);if(closed_) throw std::logic_error("trial execution is closed");
      }
      void close(uint64_t charge,trial_phase phase,const execution_work_usage& usage={}) {
         std::lock_guard lock(state_->mutex);auto& r=state_->trials[id_];
         if(r.phase!=trial_phase::running) throw std::logic_error("trial reservation is not running");
         settle(*state_,r.allowance,charge);r.usage=usage;r.phase=phase;
         closed_=true;slot_->closed=true;
      }
      void fail_full() {
         slot_->meter.invalidate();
         uint64_t allowance;{std::lock_guard lock(state_->mutex);allowance=state_->trials[id_].allowance;}
         close(allowance,trial_phase::discarded);
      }
   public:
      trial_execution(const trial_execution&)=delete;
      trial_execution& operator=(const trial_execution&)=delete;
      trial_execution(trial_execution&&)=delete;
      trial_execution& operator=(trial_execution&&)=delete;
      ~trial_execution() {
         if(!closed_) {try {if(slot_->owner==std::this_thread::get_id()) slot_->meter.invalidate();
            uint64_t allowance;{std::lock_guard lock(state_->mutex);allowance=state_->trials[id_].allowance;}
            close(allowance,trial_phase::discarded);
         }catch(...) {std::terminate();}}
      }
      borrowed_meter borrow() {check();return borrowed_meter(slot_);}
      template<typename VMContext> vm_binding<VMContext> bind_vm(VMContext& vm) {
         check();return vm_binding<VMContext>(slot_,vm,state_->policy.memory_prices);
      }
      void consume(const execution_work_usage& part) {
         check();
         if(part.signature || part.native) {slot_->meter.invalidate();throw std::invalid_argument("trial cannot charge signature or owner-native work");}
         slot_->meter.consume(part);
      }
      execution_work_usage usage() const {owner(*slot_);return slot_->meter.usage();}
      bool exhausted() const {owner(*slot_);return slot_->meter.exhausted();}
      // Failed/abandoned work is conservatively charged the entire reservation;
      // M0 usage only contains successful pre-debits and cannot prove all work
      // of an incompletely unwound failed attempt. The closed ticket never reopens.
      void discard() {
         check();
         if(slot_->borrows.load()) {fail_full();throw std::logic_error("trial discarded with live VM/native binding");}
         if(slot_->meter.exhausted()) {fail_full();return;}
         const auto u=slot_->meter.usage();close(u.total(),trial_phase::discarded,u);
      }
      trial_receipt seal_success() {
         check();
         if(slot_->borrows.load()) {fail_full();throw std::logic_error("trial sealed with live VM/native binding");}
         if(slot_->meter.exhausted()) {fail_full();throw transaction_work_local_exhausted();}
         const auto u=slot_->meter.usage();
         if(u.signature || u.native) {fail_full();throw std::invalid_argument("invalid trial work provenance");}
         {std::lock_guard lock(state_->mutex);if(!state_->alive) {slot_->meter.invalidate();}}
         if(slot_->meter.exhausted()) {fail_full();throw std::logic_error("trial owner context was destroyed");}
         close(u.total(),trial_phase::sealed,u);return trial_receipt(state_,id_);
      }
   };

   class trial_reservation {
      friend class transaction_work_context;
      friend class trial_execution;
      std::shared_ptr<state> state_;
      uint32_t id_=0;
      trial_reservation(std::shared_ptr<state> s,uint32_t id):state_(std::move(s)),id_(id) {}
   public:
      trial_reservation(const trial_reservation&)=delete;
      trial_reservation& operator=(const trial_reservation&)=delete;
      trial_reservation(trial_reservation&& other) noexcept:state_(std::move(other.state_)),id_(other.id_) {}
      trial_reservation& operator=(trial_reservation&&)=delete;
      ~trial_reservation() {
         if(state_) {std::lock_guard lock(state_->mutex);auto& r=state_->trials[id_];
            if(r.phase==trial_phase::reserved) {settle(*state_,r.allowance,0);r.phase=trial_phase::discarded;}}
      }
      trial_execution start() {return trial_execution(*this);}
   };

   class semantic_attempt {
      friend class transaction_work_context;
      std::shared_ptr<state> state_;
      std::shared_ptr<slot> slot_;
      uint64_t generation_=0,epoch_=0,reservation_=0;
      bool retryable_=false,closed_=false;
      semantic_attempt(std::shared_ptr<state> s,bool retryable):state_(std::move(s)),retryable_(retryable) {
         std::lock_guard lock(state_->mutex);alive(*state_);
         if(state_->active || state_->final_started) throw std::logic_error("semantic attempt already active or final");
         const auto allowance=state_->policy.semantic_units-state_->seed.total();
         if(retryable && (state_->retryable_attempts>=state_->local.max_retryable_owner_attempts || allowance>state_->available))
            throw transaction_work_local_exhausted();
         auto storage=std::make_shared<slot>(state_->policy.semantic_units);
         storage->meter.consume(state_->seed); // allocate/seed before changing context state
         slot_=std::move(storage);epoch_=state_->epoch;generation_=++state_->generation;
         if(retryable) {reservation_=allowance;state_->available-=allowance;state_->reserved+=allowance;++state_->retryable_attempts;}
         else state_->final_started=true;
         state_->active=true;state_->active_slot=slot_;
      }
      void poison() {
         slot_->meter.invalidate();std::lock_guard lock(state_->mutex);state_->terminal=true;
      }
      void check() {
         owner(*slot_);if(closed_) throw std::logic_error("semantic attempt is closed");
         if(slot_->meter.exhausted()) {poison();throw execution_work_exhausted();}
         std::lock_guard lock(state_->mutex);alive(*state_);
         if(!state_->active || state_->generation!=generation_ || state_->epoch!=epoch_)
            throw std::logic_error("stale semantic attempt");
      }
      execution_work_usage owner_work(const execution_work_usage& u) const {
         return subtract(subtract(u,state_->seed),slot_->imported);
      }
      void closable() {
         check();if(slot_->borrows.load()) {poison();throw std::logic_error("semantic completion has a live VM/native binding");}
         if(slot_->meter.usage().signature!=state_->seed.signature) {poison();throw std::logic_error("signature work was charged more than once");}
      }
   public:
      semantic_attempt(const semantic_attempt&)=delete;
      semantic_attempt& operator=(const semantic_attempt&)=delete;
      semantic_attempt(semantic_attempt&&)=delete;
      semantic_attempt& operator=(semantic_attempt&&)=delete;
      ~semantic_attempt() {
         if(!closed_) {try {
            if(slot_->owner==std::this_thread::get_id()) slot_->meter.invalidate();
            std::lock_guard lock(state_->mutex);state_->terminal=true;
            if(retryable_) settle(*state_,reservation_,reservation_);
            state_->active=false;state_->active_slot.reset();slot_->closed=true;
         }catch(...) {std::terminate();}}
      }
      borrowed_meter borrow() {check();return borrowed_meter(slot_);}
      template<typename VMContext> vm_binding<VMContext> bind_vm(VMContext& vm) {
         check();return vm_binding<VMContext>(slot_,vm,state_->policy.memory_prices);
      }
      void consume(const execution_work_usage& part) {
         check();
         if(part.signature) {poison();throw std::invalid_argument("signature work belongs to the transaction seed");}
         try {slot_->meter.consume(part);}catch(...) {poison();throw;}
      }
      execution_work_usage usage() const {owner(*slot_);return slot_->meter.usage();}
      void adopt(trial_receipt&& receipt) {
         check();std::lock_guard lock(state_->mutex);
         if(slot_->adopted || !receipt.state_ || receipt.state_.get()!=state_.get())
            throw std::logic_error("work receipt belongs to another context or was already adopted");
         auto& r=state_->trials[receipt.id_];
         if(r.phase!=trial_phase::sealed || r.epoch!=epoch_)
            throw std::logic_error("work receipt is consumed or belongs to an old canonical epoch");
         try {slot_->meter.consume(r.usage);}catch(...) {state_->terminal=true;throw;}
         slot_->imported=r.usage;slot_->adopted=true;r.phase=trial_phase::adopted;receipt.state_.reset();
      }
      // Only a healthy, pre-reserved owner attempt can be discarded. Its seed
      // and imported private usage were performed/accounted elsewhere; charge
      // only this owner's discarded execution to the cumulative LOCAL ledger.
      execution_work_usage discard_for_retry() {
         closable();if(!retryable_) {poison();throw std::logic_error("final canonical attempt cannot be retried");}
         const auto local=owner_work(slot_->meter.usage());
         std::lock_guard lock(state_->mutex);settle(*state_,reservation_,local.total());
         state_->active=false;state_->active_slot.reset();slot_->closed=true;closed_=true;return local;
      }
      result finish() {
         closable();const auto u=slot_->meter.usage();(void)owner_work(u);
         std::lock_guard lock(state_->mutex);
         if(retryable_) settle(*state_,reservation_,0); // it became the final path, not extra retry work
         state_->final_usage=u;state_->finalized=true;state_->active=false;state_->active_slot.reset();
         slot_->closed=true;closed_=true;return {state_->policy.version,u};
      }
   };

   transaction_work_context(transaction_work_policy policy,transaction_work_local_limits local,transaction_work_seed seed={}) {
      const auto usage=seed.usage();usage.validate();
      if(!policy.version || !policy.semantic_units || usage.total()>policy.semantic_units ||
         local.max_trials>max_trial_records || local.max_retryable_owner_attempts>max_retryable_records ||
         (!local.cumulative_units && (local.max_trials || local.max_retryable_owner_attempts)))
         throw std::invalid_argument("unconfigured or invalid transaction work policy");
      state_=std::make_shared<state>(policy,local,usage);
   }
   transaction_work_context(const transaction_work_context&)=delete;
   transaction_work_context& operator=(const transaction_work_context&)=delete;
   transaction_work_context(transaction_work_context&&)=delete;
   transaction_work_context& operator=(transaction_work_context&&)=delete;
   ~transaction_work_context() {
      std::lock_guard lock(state_->mutex);state_->alive=false;
      if(auto s=state_->active_slot.lock();s && s->owner==std::this_thread::get_id()) s->meter.invalidate();
   }
   uint32_t policy_version() const noexcept {return state_->policy.version;}
   snapshot inspect() const {
      std::lock_guard lock(state_->mutex);
      return {state_->available,state_->reserved,state_->spent,state_->epoch,state_->next_trial,state_->retryable_attempts,
              state_->active,state_->final_started,state_->finalized,state_->terminal,state_->final_usage};
   }
   uint64_t advance_epoch() {
      std::lock_guard lock(state_->mutex);alive(*state_);
      if(state_->active) throw std::logic_error("cannot change canonical epoch during execution");
      if(state_->epoch==UINT64_MAX) throw std::overflow_error("canonical work epoch overflow");
      return ++state_->epoch;
   }
   trial_reservation reserve_trial(uint64_t allowance) {
      std::lock_guard lock(state_->mutex);alive(*state_);
      if(!allowance) throw std::invalid_argument("trial allowance must be positive");
      if(state_->next_trial>=state_->local.max_trials || allowance>state_->available)
         throw transaction_work_local_exhausted();
      const auto id=state_->next_trial++;
      state_->trials[id]={trial_phase::reserved,allowance,state_->epoch,{}};
      state_->available-=allowance;state_->reserved+=allowance;
      return trial_reservation(state_,id);
   }
   semantic_attempt begin_retryable_attempt() {return semantic_attempt(state_,true);}
   semantic_attempt begin_final_attempt() {return semantic_attempt(state_,false);}
};

inline transaction_work_context::trial_execution::trial_execution(trial_reservation& reservation) {
   if(!reservation.state_) throw std::logic_error("trial reservation was already started or moved");
   auto s=reservation.state_;std::lock_guard lock(s->mutex);alive(*s);
   auto& r=s->trials[reservation.id_];
   if(r.phase!=trial_phase::reserved) throw std::logic_error("trial reservation is not available");
   if(r.epoch!=s->epoch) throw std::logic_error("trial reservation belongs to an old canonical epoch");
   auto storage=std::make_shared<slot>(r.allowance); // strong failure: ticket remains reserved
   state_=std::move(s);slot_=std::move(storage);id_=reservation.id_;r.phase=trial_phase::running;reservation.state_.reset();
}

} // namespace eosio::chain
