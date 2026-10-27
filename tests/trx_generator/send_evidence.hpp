#pragma once
#include <cstdio>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <time.h>
#include <unistd.h>

namespace eosio::testing {
// Same Linux clock domain as Python time.monotonic_ns(). NOT wall-clock time or
// an assumed std::chrono epoch. One bounded, buffered writer, no per-line fsync.
inline uint64_t benchmark_monotonic_ns() {
   timespec t{};
   if (::clock_gettime(CLOCK_MONOTONIC, &t) != 0) throw std::runtime_error("monotonic clock failed");
   return uint64_t(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
}

class send_evidence {
   FILE* file = nullptr;
   std::mutex mutex;
public:
   explicit send_evidence(const std::string& path) {
      const auto started = benchmark_monotonic_ns();
      std::string boot;
      std::ifstream("/proc/sys/kernel/random/boot_id") >> boot;
      if (boot.empty() || boot.find_first_not_of("0123456789abcdef-") != std::string::npos)
         throw std::runtime_error("evidence requires a Linux boot clock identity");
      int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
      if (fd < 0) throw std::runtime_error("cannot exclusively create send evidence");
      file = ::fdopen(fd, "w");
      if (!file) { ::close(fd); throw std::runtime_error("cannot open send evidence stream"); }
      std::fprintf(file, "{\"type\":\"generator_start\",\"at_ns\":%llu,\"clock\":\"CLOCK_MONOTONIC\",\"boot_id\":\"%s\"}\n",
                   (unsigned long long)started, boot.c_str());
   }
   send_evidence(const send_evidence&) = delete;
   ~send_evidence() { if (file) std::fclose(file); }
   void sent(const std::string& id, uint64_t queued_at, unsigned actions) {
      std::lock_guard lock(mutex);
      std::fprintf(file, "{\"type\":\"sent\",\"at_ns\":%llu,\"id\":\"%s\",\"actions\":%u,\"queued_ns\":%llu}\n",
                   (unsigned long long)benchmark_monotonic_ns(), id.c_str(), actions, (unsigned long long)queued_at);
      if (std::ferror(file)) throw std::runtime_error("send evidence write failed");
   }
   void error() {
      std::lock_guard lock(mutex);
      std::fprintf(file, "{\"type\":\"error\",\"at_ns\":%llu,\"stage\":\"sender_capacity\",\"category\":\"network_or_backpressure\"}\n",
                   (unsigned long long)benchmark_monotonic_ns());
      if (std::ferror(file)) throw std::runtime_error("send evidence write failed");
   }
   void finish(uint64_t attempted, uint64_t completed, uint64_t errors) {
      std::lock_guard lock(mutex);
      std::fprintf(file, "{\"type\":\"generator_end\",\"at_ns\":%llu,\"attempted\":%llu,\"completed\":%llu,\"errors\":%llu}\n",
                   (unsigned long long)benchmark_monotonic_ns(), (unsigned long long)attempted,
                   (unsigned long long)completed, (unsigned long long)errors);
      if (std::fflush(file) != 0 || std::ferror(file)) throw std::runtime_error("send evidence flush failed");
   }
};
} // namespace eosio::testing
