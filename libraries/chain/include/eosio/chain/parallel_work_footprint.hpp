#pragma once

#include <eosio/chain/parallel_work_budget.hpp>
#include <compare>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <tuple>
#include <variant>
#include <vector>

namespace eosio::chain {

// Owner observations are logical identities, NEVER Chainbase IDs, native
// addresses, producer-supplied declarations or shortened hashes. Permissions
// are deliberately coarse per actor, so parent/child and absence queries share
// a guard. Resource fields use the same keys as DB payer derivation below.
enum class owner_work_access_kind : uint8_t {
   account=0x41, code=0x42, permissions=0x43, ram=0x44, cpu=0x45, net=0x46,
   account_sequences=0x47, configuration=0x48, protocol=0x49,
   producer_policy=0x4a, native_global=0x4b
};
enum class work_access_mode : uint8_t { read, write };
struct owner_work_observation {
   owner_work_access_kind kind{};
   uint64_t account=0; // zero only for the global domains
   work_access_mode mode=work_access_mode::read;
};

// Trusted owner contract, not a proof object. Existing canonical validation,
// authorization and original-host replay must establish ALL these facts and
// record their real accesses before setting the corresponding bits. Nothing
// here authenticates a worker result or authorizes adopting it. In particular,
// overlay.matches_base_reads alone cannot establish this coverage.
struct canonical_work_coverage {
   enum part : uint16_t {
      semantic_success=1, same_canonical_prefix=2, complete_action_tree=4,
      code_and_native_handlers=8, authorization=16, transaction_resources=32,
      other_native_state=64, protocol_and_commit=128
   };
   static constexpr uint16_t all=255;
   uint16_t owner_verified=0;
   bool complete() const noexcept {return owner_verified==all;}
};

struct work_footprint_limits {
   static constexpr size_t hard_max_observations=32768;
   size_t max_keys=parallel_work_limits::hard_max_transaction_keys;
   size_t max_observations=16384;
   void validate() const {
      if(!max_keys || max_keys>parallel_work_limits::hard_max_transaction_keys ||
         !max_observations || max_observations>hard_max_observations)
         throw std::invalid_argument("invalid work footprint limits");
   }
};

namespace work_footprint_detail {class collector;}
class canonical_work_footprint {
public:
   enum unknown_reason : uint32_t {
      owner_coverage_missing=1, observation_limit=2, key_limit=4,
      invalid_observation=8, incomplete_mutation_history=16
   };
private:
   friend class work_footprint_detail::collector;
   std::vector<execution_work_key> reads_,writes_;
   uint32_t unknown_=owner_coverage_missing;
   static canonical_work_footprint known(std::vector<execution_work_key> reads,
                                          std::vector<execution_work_key> writes) {
      canonical_work_footprint result;result.reads_=std::move(reads);result.writes_=std::move(writes);result.unknown_=0;return result;
   }
public:
   static canonical_work_footprint barrier(uint32_t reason) {
      if(!reason) throw std::invalid_argument("work barrier needs a reason");
      canonical_work_footprint result;result.unknown_=reason;return result;
   }
   bool complete() const noexcept {return !unknown_;}
   uint32_t unknown_reasons() const noexcept {return unknown_;}
   const auto& reads() const noexcept {return reads_;}
   const auto& writes() const noexcept {return writes_;}
   canonical_work_access view() const & {return {reads_,writes_,complete()};}
   canonical_work_access view() const && = delete;
   bool operator==(const canonical_work_footprint&) const = default;
};

namespace work_footprint_detail {
struct unavailable {uint32_t reason;};
inline void need(bool value,uint32_t reason=canonical_work_footprint::incomplete_mutation_history) {
   if(!value) throw unavailable{reason};
}
struct logical_table {
   uint64_t code,scope,table;
   auto operator<=>(const logical_table&) const = default;
};
template<typename Table> logical_table table_identity(const Table& table) {
   return {table.code.to_uint64_t(),table.scope.to_uint64_t(),table.table.to_uint64_t()};
}
inline execution_work_key table_guard(logical_table t) {return {0x40,{t.code,t.scope,t.table,0,0}};}
inline execution_work_key account_guard(uint64_t account) {return {uint8_t(owner_work_access_kind::account),{account,0,0,0,0}};}
inline execution_work_key owner_key(owner_work_access_kind kind,uint64_t account) {
   return {uint8_t(kind),{account,0,0,0,0}};
}
struct table_state {bool present=false;int64_t generation=0;uint64_t payer=0;uint32_t count=0;};
struct row_identity {
   logical_table table;
   uint8_t family=0; // primary0, secondary variant-index+1; never key bits
   uint64_t primary=0;
   auto operator<=>(const row_identity&) const = default;
};
struct row_state {
   bool present=false;int64_t object=0,generation=0;uint64_t payer=0;size_t bytes=0;
   bool operator==(const row_state&) const = default;
};
inline size_t secondary_bytes(size_t variant_index) {
   constexpr size_t sizes[]{8,16,32,8,16};
   need(variant_index<std::size(sizes),canonical_work_footprint::invalid_observation);
   return sizes[variant_index];
}
class collector {
   work_footprint_limits limits_;
   std::vector<execution_work_key> reads_,writes_;
public:
   explicit collector(work_footprint_limits limits):limits_(limits) {}
   void add(execution_work_key key,work_access_mode mode) {
      auto& values=mode==work_access_mode::read?reads_:writes_;
      // Bound the intermediate list too, not just the eventual deduplicated
      // output. This avoids allocating unbounded memory for repeated accesses.
      need(reads_.size()+writes_.size()<limits_.max_observations*4,
           canonical_work_footprint::observation_limit);
      values.push_back(key);
   }
   void account(uint64_t account,work_access_mode mode=work_access_mode::read) {
      need(account!=0,canonical_work_footprint::invalid_observation);add(account_guard(account),mode);
   }
   void ram(uint64_t account) {this->account(account);add(owner_key(owner_work_access_kind::ram,account),work_access_mode::write);}
   void table(logical_table table,work_access_mode mode) {
      account(table.code);add(table_guard(table),mode);
   }
   void owner(const owner_work_observation& observation) {
      need(observation.mode==work_access_mode::read || observation.mode==work_access_mode::write,
           canonical_work_footprint::invalid_observation);
      switch(observation.kind) {
         case owner_work_access_kind::account:account(observation.account,observation.mode);return;
         case owner_work_access_kind::code:case owner_work_access_kind::permissions:
         case owner_work_access_kind::ram:case owner_work_access_kind::cpu:
         case owner_work_access_kind::net:case owner_work_access_kind::account_sequences:
            account(observation.account);break;
         case owner_work_access_kind::configuration:case owner_work_access_kind::protocol:
         case owner_work_access_kind::producer_policy:case owner_work_access_kind::native_global:
            need(!observation.account,canonical_work_footprint::invalid_observation);break;
         default:throw unavailable{canonical_work_footprint::invalid_observation};
      }
      add(owner_key(observation.kind,observation.account),observation.mode);
   }
   canonical_work_footprint finish() {
      const auto normalize=[](auto& values) {std::sort(values.begin(),values.end());values.erase(std::unique(values.begin(),values.end()),values.end());};
      normalize(reads_);normalize(writes_);
      size_t unique=reads_.size()+writes_.size();
      for(const auto& key:reads_) if(std::binary_search(writes_.begin(),writes_.end(),key)) --unique;
      need(unique<=limits_.max_keys,canonical_work_footprint::key_limit);
      return canonical_work_footprint::known(std::move(reads_),std::move(writes_));
   }
};
}

// Direct adapter for primary_overlay_result's const accessors. It intentionally
// uses a template so this small component does not pull Chainbase/FC/Boost into
// every budget client. The real source types are in parallel_state_overlay.hpp;
// no altered overlay or mutable database access is required. A sealed overlay
// is NOT coverage: without the separately established owner contract, return an
// empty-key global barrier immediately, before reading the frozen base.
//
// All DB reads (point/range/negative) and all primary/secondary writes share a
// whole logical-table guard. This deliberately creates false conflicts between
// distinct rows/index families of one table; it never hashes/truncates 256-bit
// secondary values and cannot miss range phantoms or table recreation.
//
// Missing capture/resource/interruption/OOM exceptions propagate. Callers must
// not catch arbitrary exceptions and retry a partially executed canonical TX.
template<typename Overlay>
canonical_work_footprint make_overlay_work_footprint(const Overlay& overlay,
      canonical_work_coverage coverage={},std::span<const owner_work_observation> owner={},
      work_footprint_limits limits={}) {
   using namespace work_footprint_detail;
   limits.validate();
   if(!coverage.complete()) return canonical_work_footprint::barrier(canonical_work_footprint::owner_coverage_missing);
   size_t remaining=limits.max_observations;
   const auto consume=[&](size_t size) {
      need(size<=remaining,canonical_work_footprint::observation_limit);remaining-=size;
   };
   try {
      // Check every length before traversing attacker-sized spans.
      consume(owner.size());consume(overlay.read_tables().size());consume(overlay.read_queries().size());
      consume(overlay.secondary_read_queries().size());consume(overlay.read_accounts().size());consume(overlay.writes().size());
      need(bool(overlay.base()));
      collector out(limits);
      for(const auto& access:owner) out.owner(access);
      for(const auto& account:overlay.read_accounts()) out.account(account.to_uint64_t());
      std::map<logical_table,table_state> tables;
      for(const auto& key:overlay.read_tables()) {
         const auto t=table_identity(key);out.table(t,work_access_mode::read);
         const auto found=overlay.base()->read_table(key);
         table_state state;
         if(found) {
            need(table_identity(found->key)==t && found->object_id>=0 && found->payer.to_uint64_t()!=0);
            state={true,found->object_id,found->payer.to_uint64_t(),found->count};
         }
         need(tables.emplace(t,state).second);
      }
      std::map<row_identity,row_state> rows;
      const auto check_table=[&](logical_table t,const auto& found) {
         const auto& expected=tables.find(t)->second;
         need(bool(found)==expected.present);
         if(found) need(table_identity(found->key)==t && found->object_id==expected.generation &&
                        found->payer.to_uint64_t()==expected.payer && found->count==expected.count);
      };
      const auto observe_row=[&](row_identity id,row_state state) {
         auto [it,inserted]=rows.emplace(id,state);need(inserted || it->second==state);
      };
      for(const auto& query:overlay.read_queries()) {
         const auto t=table_identity(query.table);need(tables.contains(t));out.table(t,work_access_mode::read);
         using kind=decltype(query.kind);
         need(query.kind==kind::find || query.kind==kind::lower_bound || query.kind==kind::upper_bound ||
              query.kind==kind::last || query.kind==kind::previous,canonical_work_footprint::invalid_observation);
         if(query.kind!=decltype(query.kind)::find) continue;
         const auto found=overlay.base()->locate_primary(query);row_state state;
         check_table(t,found.table);
         if(found.row) {
            need(found.table && found.row->primary_key==query.primary_key && found.row->object_id>=0 &&
                 found.row->table_object_id==found.table->object_id && found.row->payer.to_uint64_t()!=0);
            state={true,found.row->object_id,found.row->table_object_id,found.row->payer.to_uint64_t(),found.row->value_size};
         }
         observe_row({t,0,query.primary_key},state);
      }
      for(const auto& query:overlay.secondary_read_queries()) {
         const auto t=table_identity(query.table);need(tables.contains(t));out.table(t,work_access_mode::read);
         using kind=decltype(query.kind);
         need(query.kind==kind::find_primary || query.kind==kind::find_secondary || query.kind==kind::lower_bound ||
              query.kind==kind::upper_bound || query.kind==kind::first || query.kind==kind::last ||
              query.kind==kind::next || query.kind==kind::previous,canonical_work_footprint::invalid_observation);
         const auto family=query.key.index();(void)secondary_bytes(family);
         if(query.kind!=decltype(query.kind)::find_primary) continue;
         const auto found=overlay.base()->read_secondary(query);row_state state;
         check_table(t,found.table);
         if(found.row) {
            need(found.table && found.row->primary_key==query.primary_key && found.row->object_id>=0 &&
                 found.row->table_object_id==found.table->object_id && found.row->payer.to_uint64_t()!=0 &&
                 found.row->key.index()==family);
            state={true,found.row->object_id,found.row->table_object_id,found.row->payer.to_uint64_t(),secondary_bytes(family)};
         }
         observe_row({t,uint8_t(family+1),query.primary_key},state);
      }
      std::set<int64_t> temporary_ids;
      for(const auto& mutation:overlay.writes()) std::visit([&](const auto& write) {
         const auto t=table_identity(write.table);auto table=tables.find(t);need(table!=tables.end());
         out.table(t,work_access_mode::write);
         uint8_t family=0;size_t bytes=0;
         if constexpr(requires {write.key.index();}) {family=uint8_t(write.key.index()+1);bytes=secondary_bytes(write.key.index());}
         else bytes=write.value.size();
         const row_identity id{t,family,write.primary_key};auto row=rows.find(id);need(row!=rows.end());
         auto current=row->second;
         if(!table->second.present || current.generation!=table->second.generation) current={};
         const auto payer=write.payer.to_uint64_t();need(payer!=0);
         const bool store=write.operation==decltype(write.operation)::store;
         const bool update=write.operation==decltype(write.operation)::update;
         const bool remove=write.operation==decltype(write.operation)::remove;
         need(store || update || remove,canonical_work_footprint::invalid_observation);
         if(store) {
            need(!current.present && write.object_id<0 && temporary_ids.insert(write.object_id).second);
            if(!table->second.present) {
               need(write.table_object_id<0 && temporary_ids.insert(write.table_object_id).second);
               table->second={true,write.table_object_id,payer,0};out.ram(payer);
            }
            need(write.table_object_id==table->second.generation && table->second.count<UINT32_MAX);
            ++table->second.count;out.ram(payer);
            row->second={true,write.object_id,write.table_object_id,payer,bytes};
         } else {
            need(table->second.present && current.present && write.object_id==current.object &&
                 write.table_object_id==current.generation);
            if(update) {
               if(payer!=current.payer || bytes!=current.bytes) {out.ram(current.payer);out.ram(payer);}
               row->second={true,current.object,current.generation,payer,bytes};
            } else {
               need(payer==current.payer && table->second.count>0);out.ram(current.payer);row->second={};
               if(--table->second.count==0) {out.ram(table->second.payer);table->second={};}
            }
         }
      },mutation);
      return out.finish();
   } catch(const unavailable& missing) {return canonical_work_footprint::barrier(missing.reason);}
}

} // namespace eosio::chain
