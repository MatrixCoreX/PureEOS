#include <eosio/chain/parallel_access_hints.hpp>
#include <cassert>
#include <iostream>
#include <thread>

using eosio::chain::experimental::parallel_access_hints;
int main() {
   size_t checks = 0;
   const auto check = [&](bool value) { if (!value) throw std::runtime_error("access hint regression"); ++checks; };
   parallel_access_hints<int, int, 4, 3> hints;
   const auto equal = [](int a, int b) { return a == b; };
   const auto read = [&](int key) {
      std::vector<int> result;
      hints.visit(key, [&](int request) { result.push_back(request); });
      return result;
   };
   check(read(1).empty());
   hints.learn(1, 10, equal); hints.learn(1, 10, equal);
   hints.learn(1, 11, equal); hints.learn(1, 12, equal);
   check(read(1) == std::vector<int>({10, 11, 12}));
   check(hints.size() == 1);
   hints.learn(1, 13, equal);
   check(read(1).empty()); // Dynamic/saturated key disables partial prefetch.
   hints.learn(1, 14, equal);
   check(read(1).empty());
   for (int i = 2; i <= 10000; ++i) {
      hints.learn(i, i * 2, equal);
      check(hints.size() <= 4);
      check(read(i) == std::vector<int>{i * 2});
      check(read(i - 4).empty());
   }
   hints.invalidate(10000); hints.learn(10000, 3, equal);
   check(read(10000) == std::vector<int>{3});
   hints.invalidate(-1); check(hints.size() == 4);
   bool refused = false;
   std::thread other([&] { try { hints.learn(1, 1, equal); } catch (const std::logic_error&) { refused = true; } });
   other.join(); check(refused);
   refused = false;
   std::thread reader([&] { try { hints.visit(1, [](int) {}); } catch (const std::logic_error&) { refused = true; } });
   reader.join(); check(refused);
   std::cout << checks << " actual access-hint assertions passed\n";
}
