#pragma once

#include <eosio/chain/execution_work_meter.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <span>

namespace eosio::chain {

// Canonical ordered budget component, NOT active node admission. Callers must
// supply complete, canonical access observations and calibrated deterministic
// work. A user's declaration or a partial private read set is not that proof.
struct execution_work_key {
   uint8_t domain = 0;
   std::array<uint64_t,5> identity{};
   bool operator==(const execution_work_key&) const = default;
   bool operator<(const execution_work_key& other) const noexcept {
      if(domain != other.domain) return domain < other.domain;
      return identity < other.identity;
   }
};

struct canonical_work_access {
   std::span<const execution_work_key> reads, writes;
   // Incomplete coverage MUST be an explicit whole-order barrier. No purported
   // keys may accompany an unknown footprint. Ranges/negative queries and code,
   // permissions/native effects remain unknown until separately proven covered.
   bool complete = false;
};

struct parallel_work_limits {
   // Component memory bounds, not activated chain/genesis settings. Leave room
   // for future block sizes without imposing a new 4096-transaction ceiling.
   static constexpr size_t hard_max_transactions=65536, hard_max_keys=262144, hard_max_transaction_keys=4096;
   uint64_t total=0, parallel_path=0, serial=0, span=0, transaction=0;
   uint64_t scheduled=0;
   // A COMMON policy input, never std::thread::hardware_concurrency(), local
   // affinity, CPU quota or the actual number of configured workers.
   uint32_t lanes=0;
   size_t max_transactions=hard_max_transactions, max_keys=hard_max_keys,
          max_transaction_keys=hard_max_transaction_keys;
   void validate() const {
      if(!total || !span || !scheduled || !lanes || lanes>65536 || !transaction || transaction>total ||
         !max_transactions || max_transactions>hard_max_transactions ||
         !max_keys || max_keys>hard_max_keys || !max_transaction_keys ||
         max_transaction_keys>hard_max_transaction_keys)
         throw std::invalid_argument("invalid parallel execution work limits");
   }
};

struct parallel_work_assessment {
   enum violation : uint32_t {
      total_limit=1, path_limit=2, serial_limit=4, span_limit=8,
      transaction_limit=16, transaction_count_limit=32, key_count_limit=64,
      transaction_key_limit=128, scheduled_limit=256
   };
   uint32_t violations=0;
   uint64_t total=0, parallel_path=0, serial=0, span=0, completion=0, scheduled=0;
   size_t transactions=0, keys=0;
   bool accepted() const {return !violations;}
   bool operator==(const parallel_work_assessment&) const = default;
};

class parallel_work_budget {
   struct frontier {uint64_t writer=0, reader=0;};
   parallel_work_limits limits_;
   std::map<execution_work_key,frontier> keys_;
   parallel_work_assessment state_;
   uint64_t barrier_=0;

   static void validate_keys(std::span<const execution_work_key> keys) {
      for(size_t i=1;i<keys.size();++i)
         if(!(keys[i-1]<keys[i])) throw std::invalid_argument("work access keys must be sorted and unique");
   }
   template<typename F> static void unique_keys(const canonical_work_access& access,F&& visit) {
      size_t r=0,w=0;
      while(r<access.reads.size() || w<access.writes.size()) {
         if(w==access.writes.size() || (r<access.reads.size() && access.reads[r]<access.writes[w])) {
            visit(access.reads[r++],false);
         } else {
            const auto& key=access.writes[w++];
            if(r<access.reads.size() && access.reads[r]==key) ++r;
            visit(key,true);
         }
      }
   }
public:
   explicit parallel_work_budget(parallel_work_limits limits) : limits_(limits) {limits_.validate();}
   const parallel_work_assessment& state() const {return state_;}

   // Rejected candidates and invalid input leave the block unchanged. Work is
   // processed only in canonical order; local worker count is never an input.
   parallel_work_assessment try_add(const execution_work_usage& work,const canonical_work_access& access) {
      work.validate(); const auto total=work.total();
      if(!total) throw std::invalid_argument("semantic transaction work must be positive");
      if(!access.complete && (!access.reads.empty() || !access.writes.empty()))
         throw std::invalid_argument("unknown work access must be an explicit barrier");
      auto next=state_; next.violations=0; ++next.transactions;
      if(next.transactions>limits_.max_transactions) next.violations|=parallel_work_assessment::transaction_count_limit;
      // Check lengths before walking attacker-sized input or allocating memory.
      if(access.reads.size()>limits_.max_transaction_keys || access.writes.size()>limits_.max_transaction_keys) {
         next.violations|=parallel_work_assessment::transaction_key_limit; return next;
      }
      validate_keys(access.reads); validate_keys(access.writes);
      uint64_t predecessor=access.complete?barrier_:state_.parallel_path;
      size_t transaction_keys=0;
      unique_keys(access,[&](const auto& key,bool write) {
         ++transaction_keys; const auto it=keys_.find(key);
         if(it==keys_.end()) {++next.keys; return;}
         predecessor=std::max(predecessor,it->second.writer);
         if(write) predecessor=std::max(predecessor,it->second.reader);
      });
      if(transaction_keys>limits_.max_transaction_keys) next.violations|=parallel_work_assessment::transaction_key_limit;
      if(next.keys>limits_.max_keys) next.violations|=parallel_work_assessment::key_count_limit;
      next.total=execution_work_usage::add(state_.total,total);
      // Unknown/native-barrier work reserves its full cost on the owner lane.
      // Known serial cost is a subset, never counted again in the parallel DAG.
      const auto serial=access.complete?work.serial:total;
      next.serial=execution_work_usage::add(state_.serial,serial);
      next.completion=execution_work_usage::add(predecessor,total-serial);
      next.parallel_path=std::max(state_.parallel_path,next.completion);
      // Conservative bound: all mandatory serial work PLUS the longest
      // non-serial dependency path. Separate maxima alone can underbudget a
      // block. Actual native/VM phase capture and finite-lane calibration are
      // required before these abstract units may become consensus limits.
      next.span=execution_work_usage::add(next.parallel_path,next.serial);
      // Finite-lane, work-conserving DAG bound with identical abstract lanes:
      // S + D + ceil((W - D) / P), where W excludes serial work S. This is not
      // a wall-clock guarantee. Complete dependencies, calibrated costs and a
      // suitable scheduler remain mandatory; retries/load/communication and
      // unaccounted owner work cannot be assumed free.
      const auto parallel=next.total-next.serial;
      if(next.parallel_path>parallel) throw std::logic_error("parallel path exceeds non-serial work");
      const auto remainder=parallel-next.parallel_path;
      const auto extra=remainder/limits_.lanes + (remainder%limits_.lanes!=0);
      next.scheduled=execution_work_usage::add(next.span,extra);
      if(total>limits_.transaction) next.violations|=parallel_work_assessment::transaction_limit;
      if(next.total>limits_.total) next.violations|=parallel_work_assessment::total_limit;
      if(next.parallel_path>limits_.parallel_path) next.violations|=parallel_work_assessment::path_limit;
      if(next.serial>limits_.serial) next.violations|=parallel_work_assessment::serial_limit;
      if(next.span>limits_.span) next.violations|=parallel_work_assessment::span_limit;
      if(next.scheduled>limits_.scheduled) next.violations|=parallel_work_assessment::scheduled_limit;
      if(!next.accepted()) return next;

      // Allocate every new map node BEFORE changing committed state. Merging
      // nodes with the same allocator/comparator does not allocate; frontier
      // assignments cannot throw. Per-TX results belong to the caller; this
      // frontier does not allocate a second copy of every transaction record.
      std::map<execution_work_key,frontier> staged;
      unique_keys(access,[&](const auto& key,bool) {
         if(!keys_.contains(key)) staged.emplace(key,frontier{});
      });
      keys_.merge(staged);
      unique_keys(access,[&](const auto& key,bool write) {
         auto& f=keys_.find(key)->second;
         // A dependent execution needs CANONICALLY COMMITTED state. An earlier
         // unrelated slow execution can delay this transaction's commit too.
         // Record the zero-cost ordered-commit prefix, not just its VM end.
         if(write) {f.writer=next.parallel_path; f.reader=0;}
         else f.reader=std::max(f.reader,next.parallel_path);
      });
      if(!access.complete) barrier_=next.completion;
      state_=next;
      return next;
   }
};

} // namespace eosio::chain
