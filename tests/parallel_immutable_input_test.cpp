#include <eosio/chain/parallel_immutable_input.hpp>
#include <atomic>
#include <future>
#include <iostream>
#include <type_traits>
#include <vector>

using namespace eosio::chain::experimental;
namespace {
size_t checks = 0;
void require(bool ok) { ++checks; if (!ok) throw std::runtime_error("immutable input check failed"); }
struct payload {
   static inline std::atomic<size_t> copies{0};
   static inline std::atomic<bool> fail_copy{false};
   std::vector<char> code, data;
   explicit payload(size_t bytes = 128 * 1024) : code(bytes, 'a'), data(1024, 'b') {}
   payload(const payload& other) : code(other.code), data(other.data) {
      ++copies;
      if (fail_copy) throw std::bad_alloc();
   }
   payload(payload&&) noexcept = default;
};
static_assert(std::is_same_v<decltype(*std::declval<immutable_worker_input<payload>>()), const payload&>);
static_assert(std::is_same_v<decltype(std::declval<immutable_worker_input<payload>>().operator->()), const payload*>);
}

int main() {
   try {
      payload builder;
      joined_input_cache<payload> cache;
      auto first = cache.get(builder);
      require(payload::copies == 1);
      for (unsigned wave = 0; wave < 64; ++wave) {
         const auto next = cache.get(builder);
         require(&*first == &*next);
         require(next->code.front() == 'a');
      }
      require(payload::copies == 1); // 64 copies previously; one owned version now
      cache.invalidate(); builder.code.front() = 'z';
      const auto second = cache.get(builder);
      require(payload::copies == 2);
      require(&*first != &*second && first->code.front() == 'a' && second->code.front() == 'z');
      first.reset(); require(!first);

      cache.invalidate(); payload::fail_copy = true;
      try { (void)cache.get(builder); require(false); } catch (const std::bad_alloc&) {}
      payload::fail_copy = false;
      const auto recovered = cache.get(builder);
      require(recovered->code.front() == 'z' && second->code.front() == 'z');
      auto wrong_owner = std::async(std::launch::async, [&] {
         try { (void)cache.get(builder); return false; } catch (const std::logic_error&) {}
         try { cache.invalidate(); return false; } catch (const std::logic_error&) {}
         return true;
      });
      require(wrong_owner.get());

      std::vector<std::future<bool>> readers;
      for (unsigned i = 0; i < 16; ++i) readers.push_back(std::async(std::launch::async, [second] {
         for (unsigned k = 0; k < 10000; ++k)
            if (second->code.front() != 'z' || second->data.back() != 'b') return false;
         return true;
      }));
      for (unsigned version = 0; version < 64; ++version) {
         cache.invalidate(); builder.code.front() = char(version);
         const auto current = cache.get(builder);
         require(current->code.front() == char(version));
         require(second->code.front() == 'z');
      }
      for (auto& reader : readers) require(reader.get());
      const auto begin = payload::copies.load();
      std::vector<joined_input_cache<payload>> batch(64);
      for (unsigned wave = 0; wave < 64; ++wave)
         for (auto& entry : batch) require(entry.get(builder)->data.size() == 1024);
      require(payload::copies - begin == 64); // not 64 jobs * 64 waves
      std::cout << "checks=" << checks << " versions_copied=64 old_wave_copies=4096 bytes_per_payload=132096\n";
      return 0;
   } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
