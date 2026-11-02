#include "programs/nodeos/node_test_requirements.hpp"
#include <iostream>
using namespace eosio::nodeos;
int main(int argc,char** argv) {
   unsigned checks=0;
   auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("profile assertion "+std::to_string(checks));};
   eosio::chain::experimental::parallel_cpu_info cpu;cpu.affinity_known=true;cpu.allowed_logical=4;cpu.allowed_physical=2;
   check(test_build::requirement_error(cpu,8ULL<<30).empty());
   cpu.allowed_logical=3;check(!test_build::requirement_error(cpu,8ULL<<30).empty());cpu.allowed_logical=4;
   cpu.quota_cores=3;check(!test_build::requirement_error(cpu,8ULL<<30).empty());cpu.quota_cores=4;
   check(test_build::requirement_error(cpu,7500000000ULL).empty());
   check(!test_build::requirement_error(cpu,7499999999ULL).empty());
   cpu.affinity_known=false;check(!test_build::requirement_error(cpu,8ULL<<30).empty());cpu.affinity_known=true;
   hardware_info stable;stable.topology_known=true;stable.physical_cores=4;stable.logical_threads=4;stable.memory_bytes=8ULL<<30;
   check(!hardware_requirement_error(stable).empty());
   stable.physical_cores=8;stable.logical_threads=16;stable.memory_bytes=64000000000ULL;
   check(hardware_requirement_error(stable).empty());
   stable.memory_bytes--;check(!hardware_requirement_error(stable).empty());
   namespace fs=std::filesystem;const fs::path base=argv[1],proc=base/"proc",mount=base/"cg";
   fs::create_directories(proc/"self");fs::create_directories(mount/"parent/child");
   auto write=[&](fs::path p,std::string text){std::ofstream(p)<<text;};
   auto configure=[&](std::string group,std::string root="/",bool v2=true) {
      write(proc/"self/cgroup",v2?"0::"+group+"\n":"4:memory:"+group+"\n");
      write(proc/"self/mountinfo","1 0 0:1 "+root+" "+mount.string()+" rw - "+(v2?"cgroup2":"cgroup")+" none "+(v2?"rw":"rw,memory")+"\n");
   };
   auto throws=[&]{try{test_build::effective_memory(64000000000ULL,proc);return false;}catch(...){return true;}};
   configure("/parent/child");write(mount/"memory.max","max\n");write(mount/"parent/memory.max","8589934592\n");write(mount/"parent/child/memory.max","max\n");
   check(test_build::effective_memory(64000000000ULL,proc)==8589934592ULL);
   check(test_build::effective_memory(8000000000ULL,proc)==8000000000ULL);
   write(mount/"parent/child/memory.max","6442450944\n");check(test_build::effective_memory(64000000000ULL,proc)==6442450944ULL);
   write(mount/"parent/child/memory.max","wrong\n");check(throws());
   write(mount/"parent/child/memory.max","0\n");check(throws());
   write(mount/"parent/child/memory.max","18446744073709551616\n");check(throws());
   configure("/../escape");check(throws());
   configure("/wrong","/delegated");check(throws());
   configure("/","/delegated");write(mount/"memory.max","8000000000\n");check(test_build::effective_memory(64000000000ULL,proc)==8000000000ULL);
   configure("/parent/child","/",false);write(mount/"memory.limit_in_bytes","9223372036854771712\n");write(mount/"parent/memory.limit_in_bytes","8589934592\n");
   check(test_build::effective_memory(64000000000ULL,proc)==8589934592ULL);
   configure("/","/",true);write(mount/"memory.max","max\n");check(test_build::effective_memory(64000000000ULL,proc)==64000000000ULL);
   fs::remove(proc/"self/cgroup");check(throws());
   std::cout<<"PASS "<<checks<<" TEST hardware/cgroup and stable-floor assertions\n";
}
