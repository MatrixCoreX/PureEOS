#include <eosio/chain/parallel_access_hints.hpp>
#include <cassert>
#include <iostream>
#include <thread>
#include <type_traits>

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
   // Stable map iterators must remain owned by their original cache.
   using bounded_cache = parallel_access_hints<int,int,256,3>;
   static_assert(!std::is_copy_constructible_v<bounded_cache>);
   static_assert(!std::is_move_constructible_v<bounded_cache>);
   bounded_cache wide;
   const auto wide_read = [&](int key) {
      std::vector<int> result;
      wide.visit(key,[&](int value){result.push_back(value);});
      return result;
   };
   for(int i=0;i<1024;++i)wide.learn(i,i,equal);
   for(int i=0;i<1024;++i)
      check(wide_read(i)==(i<768?std::vector<int>{}:std::vector<int>{i}));
   wide.invalidate(768);wide.invalidate(900);wide.invalidate(1023);
   check(wide.size()==253);
   for(int i=10000;i<10004;++i)wide.learn(i,i,equal);
   check(wide.size()==256 && wide_read(769).empty());
   check(wide_read(770)==std::vector<int>{770});
   // A hit or saturation must not change FIFO eviction order.
   wide.learn(770,770,equal);
   wide.learn(800,1,equal);wide.learn(800,2,equal);wide.learn(800,3,equal);
   check(wide_read(800).empty());
   wide.learn(10004,10004,equal);
   check(wide_read(770).empty());
   check(wide_read(771)==std::vector<int>{771});
   wide.invalidate(800);wide.learn(800,9,equal);
   check(wide_read(800)==std::vector<int>{9});
   // Repeated full queue churn crosses deque blocks, including head/middle/tail
   // erases. All remaining entries must still refer to the correct map node.
   for(int round=0;round<8;++round) {
      const int first=20000+round*1024;
      for(int i=first;i<first+1024;++i) {
         wide.learn(i,i,equal);
         if(i%19==0)wide.invalidate(i);
         check(wide.size()<=256);
         if(i%19)check(wide_read(i)==std::vector<int>{i});
      }
      wide.invalidate(first+1000);wide.invalidate(first+1023);
      for(int i=first+768;i<first+1024;++i)
         check(wide_read(i)==(i%19==0 || i==first+1000 || i==first+1023 ?
                             std::vector<int>{}:std::vector<int>{i}));
   }
   refused=false;
   std::thread invalidator([&]{try{wide.invalidate(800);}catch(const std::logic_error&){refused=true;}});
   invalidator.join();check(refused);
   std::cout << checks << " actual access-hint assertions passed\n";
}
