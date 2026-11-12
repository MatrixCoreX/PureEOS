#pragma once

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <time.h>

namespace eosio::chain {

// Resource billing uses CPU consumed by THIS thread. Deadlines continue to use
// the existing wall clock: descheduling must not become a user CPU charge, nor
// may excluding it allow an execution to ignore the block's deadline.
struct thread_cpu_clock {
   using rep = int64_t;
   using period = std::nano;
   using duration = std::chrono::nanoseconds;
   using time_point = std::chrono::time_point<thread_cpu_clock>;
   static constexpr bool is_steady = true;

   static time_point now() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
      timespec value{};
      if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0)
         throw std::system_error(errno, std::generic_category(), "thread CPU clock unavailable");
      constexpr int64_t scale = 1'000'000'000;
      if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= scale ||
          uint64_t(value.tv_sec) > uint64_t((std::numeric_limits<int64_t>::max() - value.tv_nsec) / scale))
         throw std::overflow_error("invalid thread CPU clock");
      return time_point(duration(int64_t(value.tv_sec) * scale + value.tv_nsec));
#else
      throw std::runtime_error("this platform does not support thread CPU billing");
#endif
   }
};

// Keep nanoseconds across billing pauses; rounding every segment would lose
// short execution intervals. The clock template supports deterministic tests.
template<typename Clock = thread_cpu_clock>
class basic_thread_cpu_billing {
   using duration = typename Clock::duration;
   using time_point = typename Clock::time_point;
   std::thread::id owner;
   time_point since{};
   mutable time_point last_seen{};
   duration consumed{};
   bool running = false;

   void check_owner() const {
      if (owner != std::this_thread::get_id())
         throw std::logic_error("CPU billing used by a different thread");
   }
   duration current() const {
      check_owner();
      if (!running) return consumed;
      const auto now = read();
      const auto delta = now - since;
      if (delta > duration::max() - consumed) throw std::overflow_error("CPU billing overflow");
      return consumed + delta;
   }
   time_point read() const {
      const auto now = Clock::now();
      if (now < last_seen) throw std::logic_error("thread CPU counter decreased");
      last_seen = now;
      return now;
   }

public:
   void reset() {
      const auto now = Clock::now();
      owner = std::this_thread::get_id();
      since = now;
      last_seen = now;
      consumed = duration::zero();
      running = true;
   }
   void pause() {
      consumed = current();
      running = false;
   }
   void resume() {
      check_owner();
      if (!running) {
         since = read();
         running = true;
      }
   }
   std::chrono::microseconds elapsed() const {
      return std::chrono::duration_cast<std::chrono::microseconds>(current());
   }
};

using thread_cpu_billing = basic_thread_cpu_billing<>;

} // namespace eosio::chain
