// Loopback-only sender for an immutable, already signed release fixture.
// Uses the existing tested P2P provider and evidence writer; no new wire format.
#include "trx_provider.hpp"
#include <fc/io/json.hpp>
#include <charconv>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string_view>
#include <poll.h>
#include <unistd.h>

namespace et = eosio::testing;
using eosio::chain::packed_transaction;

int main(int argc, char** argv) {
   const char* phase = "options";
   try {
      if (argc == 2 && std::string_view(argv[1]) == "--help") {
         std::cout << "Loopback pre-signed P2P sender: --transactions FILE --evidence NEW_FILE"
                      " --port N --rate N --count N (--origin-ns N | --start-stdin 1)\n";
         return 0;
      }
      const std::set<std::string> keys{"--transactions", "--evidence", "--port", "--rate", "--count", "--origin-ns", "--start-stdin"};
      std::map<std::string, std::string> options;
      if (argc != 13) throw std::runtime_error("six explicit options required");
      for (int i = 1; i < argc; i += 2)
         if (!keys.contains(argv[i]) || !options.emplace(argv[i], argv[i + 1]).second)
            throw std::runtime_error("unknown or repeated option");
      const auto number = [&](const std::string& key, uint64_t maximum) {
         const auto& value = options.at(key);
         uint64_t result = 0;
         const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
         if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !result || result > maximum)
            throw std::runtime_error("invalid bounded number");
         return result;
      };
      const auto port = number("--port", 65535), rate = number("--rate", 1000000);
      const auto count = number("--count", 262144);
      const bool controlled_start = options.contains("--start-stdin");
      if (controlled_start && options.contains("--origin-ns")) throw std::runtime_error("ambiguous start protocol");
      uint64_t origin = controlled_start ? 0 : number("--origin-ns", UINT64_MAX - 1000000000000ULL);
      if (controlled_start) number("--start-stdin", 1);
      if (count > rate * 600) throw std::runtime_error("duration exceeds fixture bound");
      phase = "load-fixture";
      if (std::filesystem::file_size(options.at("--transactions")) > 256 * 1024 * 1024)
         throw std::runtime_error("fixture too large");
      const auto entries = fc::json::from_file(options.at("--transactions")).get_array();
      if (entries.size() < count || entries.size() > 262144) throw std::runtime_error("invalid fixture length");
      std::vector<packed_transaction> packets;
      packets.reserve(count);
      std::set<eosio::chain::transaction_id_type> identities;
      size_t total_actions = 0;
      for (size_t i = 0; i < count; ++i) {
         auto packet = entries[i]["packed"].as<packed_transaction>();
         const auto& transaction = packet.get_signed_transaction();
         const auto actions = transaction.actions.size();
         if (packet.id().str() != entries[i]["id"].as_string() || !identities.insert(packet.id()).second ||
             actions == 0 || actions > 100 || entries[i]["actions"].as_uint64() != actions ||
             !transaction.context_free_actions.empty() || actions > 2000000 - total_actions ||
             transaction.expiration.to_time_point() <= fc::time_point::now())
            throw std::runtime_error("invalid signed business fixture");
         total_actions += actions;
         packets.push_back(std::move(packet));
      }
      et::provider_base_config config;
      config._peer_endpoint = "127.0.0.1";
      config._port = static_cast<unsigned short>(port);
      config._evidence_log = options.at("--evidence");
      config._max_pending_sends = 256;
      phase = "open-evidence";
      et::p2p_connection connection(config);
      bool connected = false;
      uint64_t max_lag_ns = 0;
      try {
         phase = "connect";
         connected = true; connection.init_and_connect();
         const auto ready = et::benchmark_monotonic_ns();
         if (controlled_start) {
            phase = "start-control";
            std::cout << "{\"status\":\"ready\",\"protocol\":1,\"count\":" << count
                      << ",\"ready_ns\":" << ready << "}\n" << std::flush;
            // The controller chooses ONE future origin only after all packet
            // decoding and the socket connect finish. Bound both bytes and
            // total waiting time; a partial line cannot block indefinitely.
            std::string line;
            const auto deadline = ready + 60000000000ULL;
            while (true) {
               const auto now = et::benchmark_monotonic_ns();
               if (now >= deadline) throw std::runtime_error("start control timed out");
               pollfd input{STDIN_FILENO, POLLIN, 0};
               const auto timeout = static_cast<int>((deadline - now + 999999) / 1000000);
               const int polled = ::poll(&input, 1, timeout);
               if (polled < 0 && errno == EINTR) continue;
               if (polled <= 0) throw std::runtime_error("start control unavailable");
               char byte;
               if (::read(STDIN_FILENO, &byte, 1) != 1) throw std::runtime_error("start control closed");
               if (byte == '\n') break;
               if (byte < '0' || byte > '9' || line.size() == 20) throw std::runtime_error("invalid start control");
               line.push_back(byte);
            }
            const auto parsed = std::from_chars(line.data(), line.data() + line.size(), origin);
            if (parsed.ec != std::errc{} || parsed.ptr != line.data() + line.size())
               throw std::runtime_error("invalid start origin");
         }
         phase = "fixed-origin";
         const auto before_send = et::benchmark_monotonic_ns();
         if (origin <= before_send || origin - before_send > 60000000000ULL ||
             origin > UINT64_MAX - 1000000000000ULL)
            throw std::runtime_error("sender not ready before the fixed origin");
         phase = "send";
         for (uint64_t i = 0; i < count; ++i) {
            const auto due = origin + i * 1000000000ULL / rate;
            const timespec when{static_cast<time_t>(due / 1000000000ULL), static_cast<long>(due % 1000000000ULL)};
            int result;
            do { result = ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &when, nullptr); } while (result == EINTR);
            if (result) throw std::runtime_error("pacing clock failed");
            const auto now = et::benchmark_monotonic_ns();
            const auto lag = now > due ? now - due : 0;
            max_lag_ns = std::max<uint64_t>(max_lag_ns, lag);
            if (lag > 250000000ULL) throw std::runtime_error("sender fell behind its fixed schedule");
            connection.send_transaction(packets[i]);
         }
         phase = "drain";
         connected = false; // cleanup may throw after joining; never do it twice
         connection.cleanup_and_disconnect();
      } catch (...) {
         if (connected) { try { connection.cleanup_and_disconnect(); } catch (...) {} }
         throw;
      }
      std::cout << "{\"status\":\"completed\",\"sent\":" << count << ",\"max_enqueue_lag_ns\":" << max_lag_ns
                << ",\"note\":\"socket writes are not acceptance or finality\"}\n";
      return 0;
   } catch (...) {
      // Never print a parse exception containing signed payloads or test keys.
      std::cerr << "Pre-signed sender failed in " << phase
                << "; retained evidence is not a capacity result.\n";
      return 2;
   }
}
