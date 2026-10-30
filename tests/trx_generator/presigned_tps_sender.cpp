// Loopback-only sender for an immutable, already signed release fixture.
// Uses the existing tested P2P provider and evidence writer; no new wire format.
#include "trx_provider.hpp"
#include <fc/io/json.hpp>
#include <charconv>
#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <thread>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
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
                      " --port N --rate N --count N (--origin-ns N | --start-stdin 1) [--hold-stdin 1]\n";
         return 0;
      }
      const std::set<std::string> keys{"--transactions", "--evidence", "--port", "--rate", "--count", "--origin-ns", "--start-stdin", "--hold-stdin"};
      std::map<std::string, std::string> options;
      if (argc != 13 && argc != 15) throw std::runtime_error("six explicit options and optional hold required");
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
      const auto count = number("--count", 1048576);
      const bool controlled_start = options.contains("--start-stdin");
      if (controlled_start && options.contains("--origin-ns")) throw std::runtime_error("ambiguous start protocol");
      uint64_t origin = controlled_start ? 0 : number("--origin-ns", UINT64_MAX - 1000000000000ULL);
      if (controlled_start) number("--start-stdin", 1);
      const bool controlled_hold = options.contains("--hold-stdin");
      if (controlled_hold) {
         number("--hold-stdin", 1);
         if (!controlled_start) throw std::runtime_error("hold requires controlled start");
      }
      if (count > rate * 600) throw std::runtime_error("duration exceeds fixture bound");
      phase = "load-fixture";
      if (std::filesystem::file_size(options.at("--transactions")) > 1024 * 1024 * 1024)
         throw std::runtime_error("fixture too large");
      const auto entries = fc::json::from_file(options.at("--transactions")).get_array();
      if (entries.size() < count || entries.size() > 1048576) throw std::runtime_error("invalid fixture length");
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
      // Eight client connections share the unchanged aggregate8192-send bound.
      // Node-side per-peer in-flight limits and incoming queue remain unchanged.
      constexpr size_t connection_count = 8;
      config._max_pending_sends = 1024;
      phase = "open-evidence";
      std::vector<std::unique_ptr<et::p2p_connection>> connections;
      std::vector<bool> connected(connection_count, false);
      for (size_t i = 0; i < connection_count; ++i) {
         config._evidence_log = options.at("--evidence") + ".connection-" + std::to_string(i);
         connections.emplace_back(std::make_unique<et::p2p_connection>(config));
      }
      uint64_t max_lag_ns = 0;
      try {
         phase = "connect";
         for (size_t i = 0; i < connection_count; ++i) {
            connected[i] = true; connections[i]->init_and_connect();
         }
         const auto ready = et::benchmark_monotonic_ns();
         if (controlled_start) {
            phase = "start-control";
            std::cout << "{\"status\":\"ready\",\"protocol\":1,\"count\":" << count
                      << ",\"ready_ns\":" << ready << ",\"connections\":8}\n" << std::flush;
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
         // One dispatcher per existing connection. Keep the same fixed origin,
         // per-TX due times, 250ms lag guard and aggregate8192-send bound.
         std::array<uint64_t, connection_count> dispatch_lag{};
         std::array<std::exception_ptr, connection_count> dispatch_errors{};
         std::atomic<bool> dispatch_failed{false};
         std::vector<std::jthread> dispatchers;
         dispatchers.reserve(connection_count);
         for (size_t connection = 0; connection < connection_count; ++connection) {
            dispatchers.emplace_back([&, connection](std::stop_token stop) {
               try {
                  for (uint64_t i = connection; i < count; i += connection_count) {
                     if (stop.stop_requested() || dispatch_failed.load(std::memory_order_relaxed)) return;
                     const auto due = origin + i * 1000000000ULL / rate;
                     if (et::benchmark_monotonic_ns() < due) {
                        const timespec when{static_cast<time_t>(due / 1000000000ULL), static_cast<long>(due % 1000000000ULL)};
                        int result;
                        do { result = ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &when, nullptr); } while (result == EINTR);
                        if (result) throw std::runtime_error("pacing clock failed");
                     }
                     const auto now = et::benchmark_monotonic_ns();
                     const auto lag = now > due ? now - due : 0;
                     dispatch_lag[connection] = std::max<uint64_t>(dispatch_lag[connection], lag);
                     if (lag > 250000000ULL) throw std::runtime_error("sender fell behind its fixed schedule");
                     connections[connection]->send_transaction(packets[i]);
                  }
               } catch (...) {
                  dispatch_errors[connection] = std::current_exception();
                  dispatch_failed.store(true, std::memory_order_relaxed);
               }
            });
         }
         for (auto& dispatcher : dispatchers) dispatcher.join();
         for (const auto& error : dispatch_errors) if (error) std::rethrow_exception(error);
         for (auto lag : dispatch_lag) max_lag_ns = std::max(max_lag_ns, lag);

         if (controlled_hold) {
            // Socket writes do not mean node acceptance. Keep every connection
            // alive through the controller's independent drain observation.
            // A bounded explicit release prevents an early close from discarding
            // unread P2P frames; it never changes due times or transaction counts.
            phase = "release-control";
            std::cout << "{\"status\":\"sent-awaiting-release\",\"sent\":" << count << "}\n" << std::flush;
            const auto release_deadline = et::benchmark_monotonic_ns() + 180000000000ULL;
            std::string release;
            while (true) {
               const auto now = et::benchmark_monotonic_ns();
               if (now >= release_deadline) throw std::runtime_error("release control timed out");
               pollfd input{STDIN_FILENO, POLLIN, 0};
               const auto timeout = static_cast<int>((release_deadline - now + 999999) / 1000000);
               const int polled = ::poll(&input, 1, timeout);
               if (polled < 0 && errno == EINTR) continue;
               if (polled <= 0) throw std::runtime_error("release control unavailable");
               char byte;
               if (::read(STDIN_FILENO, &byte, 1) != 1) throw std::runtime_error("release control closed");
               if (byte == '\n') break;
               if (release.size() >= 7) throw std::runtime_error("invalid release control");
               release.push_back(byte);
            }
            if (release != "release") throw std::runtime_error("invalid release control");
         }
         phase = "drain";
         for (size_t i = 0; i < connection_count; ++i) {
            connected[i] = false; // cleanup may throw after joining;never repeat it
            connections[i]->cleanup_and_disconnect();
         }
      } catch (...) {
         for (size_t i = 0; i < connection_count; ++i)
            if (connected[i]) { try { connections[i]->cleanup_and_disconnect(); } catch (...) {} }
         throw;
      }
      std::cout << "{\"status\":\"completed\",\"sent\":" << count << ",\"max_enqueue_lag_ns\":" << max_lag_ns
                << ",\"note\":\"socket writes are not acceptance or finality\"}\n";
      return 0;
   } catch (const std::runtime_error& e) {
      // Runtime-error reasons here are bounded static strings, never payloads.
      std::cerr << "Pre-signed sender failed in " << phase << ": " << ((std::string_view(e.what()) == "bounded P2P sender is saturated" || std::string_view(e.what()) == "sender fell behind its fixed schedule" || std::string_view(e.what()) == "P2P socket write deadline exceeded") ? e.what() : "runtime error (details redacted)") << "\n";
      return 2;
   } catch (...) {
      // Never print a parse exception containing signed payloads or test keys.
      std::cerr << "Pre-signed sender failed in " << phase
                << "; retained evidence is not a capacity result.\n";
      return 2;
   }
}
