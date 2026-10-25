#include <eosio/chain/execution_work_meter.hpp>
#include <eosio/vm/execution_work_budget.hpp>
#include <array>
#include <functional>
#include <iostream>
#include <type_traits>
#include <vector>

using namespace eosio::chain;

namespace {
void require(bool value, const char* message) {
   if (!value) throw std::runtime_error(message);
}
template<typename Exception, typename F> void rejects(F&& operation) {
   try { operation(); } catch (const Exception&) { return; }
   throw std::runtime_error("expected execution work rejection");
}
uint64_t rng(uint64_t& state) {
   state ^= state << 13; state ^= state >> 7; state ^= state << 17;
   return state;
}
}

int main() {
   static_assert(!std::is_copy_constructible_v<execution_work_meter> &&
                 !std::is_move_constructible_v<execution_work_meter>);
   const auto max = std::numeric_limits<uint64_t>::max();
   const std::vector<std::pair<const char*, std::function<void()>>> cases{
      {"serial_work_is_a_subset_not_an_extra_invoice", [] {
         execution_work_meter meter(140);
         meter.consume({30, 40, 50, 20, 45});
         require(meter.usage().total() == 140 && meter.remaining() == 0, "serial work counted twice");
         require(meter.usage().billable_units(100, 5000) == 115, "signature policy changed raw capacity");
         require(!meter.exhausted(), "exact budget boundary rejected");
      }},
      {"minimum_price_does_not_consume_execution_capacity", [] {
         const execution_work_usage first{.wasm=10}, second{.host=10, .serial=5};
         execution_work_meter block(20);
         block.consume(first); block.consume(second);
         require(block.usage().total() == 20 && block.remaining() == 0, "pricing floor leaked into capacity");
         require(first.billable_units(100, 5000) + second.billable_units(100, 5000) == 200, "invoice floor disappeared");
      }},
      {"signature_discount_is_safe_at_uint64_max", [max] {
         for (uint32_t pct : {0u, 1u, 3333u, 5000u, 9999u, 10000u}) {
            const execution_work_usage work{.signature=max};
            const auto oracle = uint64_t((static_cast<unsigned __int128>(max) * pct) / 10000);
            require(work.billable_units(0, pct) == oracle, "signature percentage overflow/rounding error");
            require(work.total() == max, "discount reduced execution work");
         }
         const execution_work_usage work{.signature=max-10, .native=10, .serial=10};
         require(work.billable_units(100, 0) == 100, "invoice floor overflowed");
         rejects<std::invalid_argument>([&] { (void)work.billable_units(0, 10001); });
      }},
      {"failed_merge_is_atomic_and_cannot_revive", [max] {
         execution_work_meter meter(max);
         meter.consume({.wasm=max-10}); meter.consume({.signature=10});
         const auto before = meter.usage();
         rejects<execution_work_exhausted>([&] { meter.consume({.host=1}); });
         require(meter.usage() == before && meter.exhausted() && meter.remaining() == 0, "failed charge changed committed counters");
         rejects<execution_work_exhausted>([&] { meter.consume({}); });
      }},
      {"malformed_or_overflowing_usage_fails_closed", [max] {
         rejects<std::invalid_argument>([] { execution_work_meter zero(0); });
         for (const auto bad : {execution_work_usage{.native=1},
               execution_work_usage{.host=1, .serial=2},
               execution_work_usage{.wasm=max, .host=1}}) {
            execution_work_meter meter(max); meter.consume({.wasm=1});
            const auto before = meter.usage();
            rejects<std::exception>([&] { meter.consume(bad); });
            require(meter.usage() == before && meter.exhausted(), "invalid usage partially charged");
            rejects<execution_work_exhausted>([&] { meter.consume({.wasm=1}); });
         }
      }},
      {"discarded_trial_does_not_bill_the_canonical_meter", [] {
         execution_work_meter canonical(100);
         { execution_work_meter trial(100); trial.consume({.wasm=70, .host=20, .serial=10}); }
         require(canonical.usage().total() == 0, "discarded execution affected canonical work");
         execution_work_meter adopted(100); adopted.consume({.wasm=15}); adopted.consume({.host=25, .serial=10});
         canonical.consume(adopted.usage()); canonical.consume({.native=10, .serial=10});
         require(canonical.usage().total() == 50 && canonical.usage().serial == 20, "nested work merge lost or duplicated work");
      }},
      {"VM_and_native_nested_calls_share_one_remaining_budget", [] {
         execution_work_meter meter(100);
         auto native = meter.wasm_budget();
         eosio::vm::execution_work_budget_view parent{native.remaining, native.exhausted};
         parent.charge(7);
         meter.consume({.host=20, .serial=5});
         auto child = parent; child.charge(10);
         require(meter.remaining() == 63 && *parent.remaining == 63, "host/native work did not reduce the VM budget");
         require(meter.usage() == execution_work_usage{17,20,0,0,5}, "VM work was lost or charged twice");
         const auto before=meter.usage();
         rejects<eosio::vm::execution_work_exhausted>([&] { child.charge(64); });
         require(meter.usage()==before && meter.exhausted() && meter.remaining()==0, "failed VM debit changed committed usage");
         rejects<execution_work_exhausted>([&] {meter.consume({.wasm=1});});
         rejects<execution_work_exhausted>([&] {(void)meter.wasm_budget();});
      }},
      {"unbound_and_failed_VM_budget_fail_closed", [] {
         eosio::vm::execution_work_budget_view missing;
         rejects<std::logic_error>([&] {missing.charge(0);});
         execution_work_meter meter(10); auto native=meter.wasm_budget();
         eosio::vm::execution_work_budget_view view{native.remaining,native.exhausted};
         view.charge(10);
         require(meter.usage().wasm==10 && !meter.exhausted(), "exact VM budget boundary rejected");
         rejects<eosio::vm::execution_work_exhausted>([&] {view.charge(1);});
         rejects<eosio::vm::execution_work_exhausted>([&] {view.charge(0);});
         require(meter.usage().wasm==10, "VM underflow wrapped the counter");
      }},
      {"partitioning_1_4_8_12_16_matches_wide_integer_oracle", [] {
         uint64_t state = 0xa1c20261024;
         for (unsigned round = 0; round < 1000; ++round) {
            std::vector<execution_work_usage> parts;
            std::array<unsigned __int128, 5> sums{};
            for (unsigned i = 0; i < 32; ++i) {
               execution_work_usage p{rng(state)%65536, rng(state)%65536, rng(state)%65536, rng(state)%65536, 0};
               p.serial = p.native + rng(state)%(p.host+1); parts.push_back(p);
               sums[0]+=p.wasm; sums[1]+=p.host; sums[2]+=p.signature; sums[3]+=p.native; sums[4]+=p.serial;
            }
            const auto total = uint64_t(sums[0]+sums[1]+sums[2]+sums[3]);
            for (const unsigned batch : {1u, 4u, 8u, 12u, 16u}) {
               execution_work_meter meter(total);
               for (unsigned start = 0; start < parts.size(); start += batch) {
                  execution_work_usage grouped;
                  for (unsigned i=start; i<parts.size() && i<start+batch; ++i) {
                     grouped.wasm+=parts[i].wasm; grouped.host+=parts[i].host;
                     grouped.signature+=parts[i].signature; grouped.native+=parts[i].native; grouped.serial+=parts[i].serial;
                  }
                  meter.consume(grouped);
               }
               const auto& u = meter.usage();
               require(u.total()==total && u.serial==uint64_t(sums[4]) && meter.remaining()==0, "partition changed capacity accounting");
               for (uint32_t pct : {0u, 3333u, 5000u, 10000u}) {
                  const auto invoice=uint64_t(sums[0]+sums[1]+sums[3]+sums[2]*pct/10000);
                  require(u.billable_units(100, pct)==invoice, "partition changed deterministic invoice");
               }
            }
         }
      }}
   };
   size_t failed = 0;
   for (const auto& [name, run] : cases) {
      try { run(); std::cerr << name << ": passed\n"; }
      catch (const std::exception& error) { ++failed; std::cerr << name << ": FAILED: " << error.what() << '\n'; }
   }
   std::cout << "{\"status\":\"" << (failed ? "failed" : "passed") << "\",\"cases\":" << cases.size()
             << ",\"passed\":" << cases.size()-failed << ",\"failed\":" << failed << "}\n";
   return failed ? 1 : 0;
}
