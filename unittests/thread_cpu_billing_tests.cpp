#include <eosio/chain/thread_cpu_billing.hpp>
#include <atomic>
#include <functional>
#include <iostream>
#include <vector>

using namespace eosio::chain;
using namespace std::chrono_literals;

namespace {
void require(bool value, const char* message) {
   if (!value) throw std::runtime_error(message);
}
template<typename F> void logic_error_required(F&& operation) {
   try { operation(); } catch (const std::logic_error&) { return; }
   throw std::runtime_error("expected CPU ownership/counter rejection");
}
struct fake_cpu_clock {
   using duration = std::chrono::nanoseconds;
   using time_point = std::chrono::time_point<fake_cpu_clock>;
   static inline duration value{};
   static time_point now() { return time_point(value); }
};
using fake_billing = basic_thread_cpu_billing<fake_cpu_clock>;
}

int main() {
   const std::vector<std::pair<const char*, std::function<void()>>> cases{
      {"paused_work_and_idempotent_resume", [] {
         fake_cpu_clock::value = 0ns;
         fake_billing bill; bill.reset();
         fake_cpu_clock::value = 10us; bill.pause();
         fake_cpu_clock::value = 5000us;
         require(bill.elapsed() == 10us, "paused work became a CPU charge");
         bill.resume(); fake_cpu_clock::value = 5003us; bill.resume();
         fake_cpu_clock::value = 5007us;
         require(bill.elapsed() == 17us, "resume lost or duplicated CPU");
      }},
      {"short_segments_are_not_rounded_away", [] {
         fake_cpu_clock::value = 0ns;
         fake_billing bill; bill.reset();
         fake_cpu_clock::value = 600ns; bill.pause();
         fake_cpu_clock::value = 50us; bill.resume();
         fake_cpu_clock::value += 600ns;
         require(bill.elapsed() == 1us, "per-pause rounding lost CPU");
      }},
      {"reset_discards_aborted_attempt", [] {
         fake_cpu_clock::value = 0ns;
         fake_billing bill; bill.reset();
         fake_cpu_clock::value = 900us; bill.reset();
         fake_cpu_clock::value = 904us;
         require(bill.elapsed() == 4us, "aborted work was charged twice");
      }},
      {"regression_between_reads_and_across_a_pause", [] {
         fake_cpu_clock::value = 0ns;
         fake_billing bill; bill.reset();
         fake_cpu_clock::value = 10us;
         require(bill.elapsed() == 10us, "initial observation lost");
         fake_cpu_clock::value = 9us;
         logic_error_required([&] { (void)bill.elapsed(); });
         fake_cpu_clock::value = 11us; bill.pause();
         fake_cpu_clock::value = 10us;
         logic_error_required([&] { bill.resume(); });
         fake_cpu_clock::value = 12us; bill.resume();
         fake_cpu_clock::value = 13us;
         require(bill.elapsed() == 12us, "bad resume changed accounting");
      }},
      {"counter_regression_and_cross_thread_access", [] {
         fake_cpu_clock::value = 100us;
         fake_billing bill; bill.reset();
         fake_cpu_clock::value = 99us;
         logic_error_required([&] { (void)bill.elapsed(); });
         fake_cpu_clock::value = 101us;
         bool rejected = false;
         std::thread other([&] {
            try { (void)bill.elapsed(); } catch (const std::logic_error&) { rejected = true; }
         });
         other.join();
         require(rejected, "a different thread supplied CPU");
         require(bill.elapsed() == 1us, "wrong owner altered accounting");
      }},
      {"sleep_and_other_threads_are_not_account_cpu", [] {
         thread_cpu_billing bill; bill.reset();
         const auto start = std::chrono::steady_clock::now();
         std::atomic<uint64_t> result{};
         std::thread other([&] {
            const auto until = thread_cpu_clock::now() + 10ms;
            uint64_t work = 1;
            while (thread_cpu_clock::now() < until) work = work * 6364136223846793005ULL + 1;
            result.store(work);
         });
         std::this_thread::sleep_for(40ms); other.join();
         require(std::chrono::steady_clock::now() - start >= 40ms, "sleep control missing");
         require(bill.elapsed() < 10ms, "scheduler wait or another thread was charged");
         require(result.load() != 0, "other-thread computation missing");
      }},

      {"external_fractional_cpu_is_rounded_only_after_aggregation", [] {
         fake_cpu_clock::value=0ns;fake_billing bill;bill.reset();
         fake_cpu_clock::value=500ns;bill.add_external_cpu(900ns);
         require(bill.elapsed()==1us,"external fractional CPU lost or duplicated");
         bill.pause();fake_cpu_clock::value=1ms;bill.add_external_cpu(700ns);
         require(bill.elapsed()==2us,"paused external CPU lost");
         bill.resume();fake_cpu_clock::value+=900ns;
         require(bill.elapsed()==3us,"resume external remainder duplicated");
      }},
      {"external_cpu_rejects_negative_overflow_and_wrong_owner", [] {
         fake_cpu_clock::value=0ns;fake_billing bill;bill.reset();bill.pause();
         logic_error_required([&]{bill.add_external_cpu(-1ns);});
         require(bill.elapsed()==0us,"negative external charge mutated state");
         bool rejected=false;std::thread other([&]{try{bill.add_external_cpu(1000ns);}catch(const std::logic_error&){rejected=true;}});other.join();
         require(rejected && bill.elapsed()==0us,"external CPU accepted a foreign owner");
         bill.add_external_cpu(fake_cpu_clock::duration::max());
         bool overflow=false;try{bill.add_external_cpu(1ns);}catch(const std::overflow_error&){overflow=true;}
         require(overflow,"external CPU overflow accepted");
         require(bill.elapsed()==std::chrono::duration_cast<std::chrono::microseconds>(fake_cpu_clock::duration::max()),"overflow changed state");
      }},
      {"real_computation_is_charged", [] {
         thread_cpu_billing bill; bill.reset();
         const auto until = thread_cpu_clock::now() + 5ms;
         while (thread_cpu_clock::now() < until) std::atomic_signal_fence(std::memory_order_seq_cst);
         require(bill.elapsed() >= 5ms, "actual CPU execution went uncharged");
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
