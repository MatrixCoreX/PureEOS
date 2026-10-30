#include <eosio/chain/parallel_auto_config.hpp>
#include <eosio/chain/parallel_runtime_limits.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>

using namespace eosio::chain::experimental;
namespace fs=std::filesystem;
static size_t assertions=0;
static void check(bool value,const char* label) {
   ++assertions;if (!value) throw std::runtime_error(label);
}
template<class F> void invalid(F&& f) {
   bool rejected=false;try {f();}catch(const std::invalid_argument&) {rejected=true;}
   check(rejected,"invalid configuration accepted");
}
struct fixture {
   fs::path root=fs::temp_directory_path()/("parallel-auto-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
   parallel_cpu_probe probe;
   fixture() {
      check(fs::create_directory(root),"create isolated fixture");probe.proc_root=root/"proc";probe.cpu_root=root/"cpu";probe.hardware_fallback=64;
      probe.allowed_cpus=std::vector<unsigned>(64);std::iota(probe.allowed_cpus->begin(),probe.allowed_cpus->end(),0);
      for (unsigned i=0;i<64;++i) {
         write(probe.cpu_root/("cpu"+std::to_string(i))/"topology/physical_package_id","0\n");
         write(probe.cpu_root/("cpu"+std::to_string(i))/"topology/core_id",std::to_string(i%32)+"\n");
      }
   }
   ~fixture() {std::error_code error;fs::remove_all(root,error);}
   static void write(const fs::path& p,const std::string& text) {
      fs::create_directories(p.parent_path());std::ofstream out(p);out<<text;check(bool(out),"write fixture");
   }
   std::string escaped(const fs::path& p) {
      auto s=p.string();size_t pos=0;while ((pos=s.find(' ',pos))!=s.npos) {s.replace(pos,1,"\\040");pos+=4;}return s;
   }
   void unified(std::string group="/slice/service",std::string mount_root="/") {
      write(probe.proc_root/"self/cgroup","0::"+group+"\n");
      write(probe.proc_root/"self/mountinfo","20 1 0:1 "+mount_root+" "+escaped(root/"cgroup space")+" rw - cgroup2 cgroup rw\n");
   }
   fs::path cgroup() const {return root/"cgroup space";}
};

int main() {
 try {
   parallel_cpu_info cpu;cpu.allowed_logical=64;cpu.allowed_physical=32;
   auto selected=resolve_parallel_config("auto","8",false,cpu);
   check(selected.workers==32 && selected.batch==96 && selected.workers_auto && selected.batch_auto,"32core auto sizing");
   selected=resolve_parallel_config("auto","24",true,cpu);
   check(selected.workers==32 && selected.batch==24 && !selected.batch_auto,"explicit batch override");
   selected=resolve_parallel_config("16","8",false,cpu);
   check(selected.workers==16 && selected.batch==8 && !selected.workers_auto,"legacy explicit config");
   selected=resolve_parallel_config("0","8",false,cpu);check(!selected.workers && selected.batch==8,"legacy disabled default");
   selected=resolve_parallel_config("0","auto",true,cpu);check(!selected.workers && selected.batch==8,"disabled auto batch");
   selected=resolve_parallel_config("128","auto",true,cpu);check(selected.workers==128 && selected.batch==256,"manual maximum");
   cpu.allowed_logical=256;cpu.allowed_physical=256;
   selected=resolve_parallel_config("auto","auto",true,cpu);check(selected.workers==128 && selected.batch==256,"auto capped to reviewed maximum");
   for (size_t cores:{1u,2u,8u,16u,32u,64u,96u,128u,192u}) {
      cpu.allowed_logical=2*cores;cpu.allowed_physical=cores;cpu.quota_cores.reset();
      selected=resolve_parallel_config("auto","8",false,cpu);
      check(selected.workers==std::min(cores,parallel_runtime_limits::max_workers),"auto worker topology table");
      check(selected.batch==std::min<size_t>(3*selected.workers,parallel_runtime_limits::max_batch_transactions),"auto batch topology table");
      for (size_t quota:{1u,4u,32u}) {
         cpu.quota_cores=quota;selected=resolve_parallel_config("auto","auto",true,cpu);
         check(selected.workers==std::min({cores,quota,parallel_runtime_limits::max_workers}),"quota overrides host cores");
      }
   }
   for (const auto* bad:{"","-1","+1","AUTO","16x","18446744073709551616"}) {
      invalid([&]{resolve_parallel_config(bad,"8",true,cpu);});
      invalid([&]{resolve_parallel_config("8",bad,true,cpu);});
   }
   for (const auto* bad:{"129","65536","4294967296","18446744073709551615"}) invalid([&]{resolve_parallel_config(bad,"auto",true,cpu);});
   for (const auto* bad:{"0","257","18446744073709551615"}) invalid([&]{resolve_parallel_config("auto",bad,true,cpu);});
   {
      fixture f;f.unified();
      fixture::write(f.cgroup()/"cpu.max","1600000 100000\n");
      fixture::write(f.cgroup()/"slice/cpu.max","450000 100000\n");
      fixture::write(f.cgroup()/"slice/service/cpu.max","max 100000\n");
      auto info=detect_parallel_cpu_info(f.probe);
      check(info.affinity_known && info.topology_known && info.quota_known,"discovery provenance");
      check(info.allowed_logical==64 && info.allowed_physical==32 && info.usable_cores()==4,"ancestor quota and SMT deduplication");
      check(resolve_parallel_config("auto","8",false,info).batch==12,"quota batch sizing");
      fixture::write(f.cgroup()/"slice/service/cpu.max","250000 100000\n");info=detect_parallel_cpu_info(f.probe);check(info.usable_cores()==2,"fractional quota floor");
      fixture::write(f.cgroup()/"slice/service/cpu.max","50000 100000\n");info=detect_parallel_cpu_info(f.probe);check(info.usable_cores()==1,"subcore quota minimum one worker");
      for (const auto* bad:{"","max 0","0 100000","bogus","100000 100000 extra","18446744073709551616 100000"}) {
         fixture::write(f.cgroup()/"slice/service/cpu.max",bad);info=detect_parallel_cpu_info(f.probe);
         check(info.usable_cores()==1 && !info.warnings.empty(),"invalid observed quota caps concurrency");
      }
      fixture::write(f.cgroup()/"slice/service/cpu.max","max 100000\n");
      fixture::write(f.cgroup()/"slice/cpu.max","max 100000\n");fixture::write(f.cgroup()/"cpu.max","max 100000\n");
      info=detect_parallel_cpu_info(f.probe);check(info.usable_cores()==32 && !info.quota_cores,"unlimited quota uses physical cores");
      f.probe.allowed_cpus=std::vector<unsigned>{0,32,1,33,33};info=detect_parallel_cpu_info(f.probe);
      check(info.allowed_logical==4 && info.allowed_physical==2 && info.usable_cores()==2,"restricted affinity and duplicate CPU IDs");
      fs::remove(f.probe.cpu_root/"cpu0/topology/core_id");info=detect_parallel_cpu_info(f.probe);
      check(!info.topology_known && info.allowed_physical==4,"missing topology fallback");
   }
   {
      fixture f;f.unified("/delegated/slice/service","/delegated");
      fixture::write(f.cgroup()/"slice/service/cpu.max","300000 100000");
      check(detect_parallel_cpu_info(f.probe).usable_cores()==3,"delegated mount root mapping");
      f.unified("/","/delegated");fixture::write(f.cgroup()/"cpu.max","200000 100000");
      check(detect_parallel_cpu_info(f.probe).usable_cores()==2,"cgroup namespace root mapping");
      f.unified("/../../unreachable","/delegated");
      check(detect_parallel_cpu_info(f.probe).usable_cores()==1,"unreachable cgroup must not oversubscribe");
   }
   {
      fixture f;
      fixture::write(f.probe.proc_root/"self/cgroup","4:cpuset:/ignored\n3:cpu,cpuacct:/slice/service\n");
      fixture::write(f.probe.proc_root/"self/mountinfo","20 1 0:1 / "+f.escaped(f.cgroup())+" rw - cgroup cgroup rw,cpu,cpuacct\n");
      for (const auto& dir:{f.cgroup(),f.cgroup()/"slice",f.cgroup()/"slice/service"}) {
         fixture::write(dir/"cpu.cfs_quota_us","-1\n");fixture::write(dir/"cpu.cfs_period_us","100000\n");
      }
      fixture::write(f.cgroup()/"slice/cpu.cfs_quota_us","600000\n");
      fixture::write(f.cgroup()/"slice/service/cpu.cfs_quota_us","350000\n");
      auto info=detect_parallel_cpu_info(f.probe);check(info.quota_known && info.usable_cores()==3,"legacy CPU quota hierarchy");
      fixture::write(f.cgroup()/"slice/service/cpu.cfs_quota_us","-2\n");
      check(detect_parallel_cpu_info(f.probe).usable_cores()==6,"negative legacy quota remains unlimited");
      fs::remove(f.cgroup()/"slice/service/cpu.cfs_period_us");
      check(detect_parallel_cpu_info(f.probe).usable_cores()==1,"missing legacy quota period");
   }
   const auto live=detect_parallel_cpu_info();
   check(live.usable_cores()>=1 && live.usable_cores()<=live.allowed_logical,"real host discovery is bounded");
   check(parallel_runtime_limits::max_batch_write_operations==262144,"retain original aggregate journal budget");
   check(parallel_runtime_limits::production_timeout==std::chrono::milliseconds(50),"retain production time cap");
   std::cout<<"auto configuration assertions="<<assertions<<" passed; actual logical="<<live.allowed_logical
      <<" physical="<<live.allowed_physical<<" usable="<<live.usable_cores()<<'\n';
 }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
