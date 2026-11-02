#pragma once
// Included only in the separately compiled PUREEOS_TEST_BUILD executable.
#include "node_hardware_requirements.hpp"
#include <eosio/chain/parallel_auto_config.hpp>
#include <optional>
#include <stdexcept>

namespace eosio::nodeos::test_build {
inline constexpr unsigned minimum_vcpus = 4;
// An advertised 8 GB VM loses some memory to its kernel. This is a startup
// floor, not a promise that every chain workload will fit into 8 GB.
inline constexpr uint64_t minimum_memory_bytes = 7'500'000'000ULL;
namespace detail {
using path = std::filesystem::path;
inline std::optional<std::string> read(const path& p) {
   std::error_code ec;
   if (!std::filesystem::exists(p,ec) && !ec) return {};
   std::ifstream in(p);
   if (!in) throw std::runtime_error("unreadable memory control file");
   std::string value(1024*1024+1,'\0');in.read(value.data(),value.size());
   if (in.bad() || size_t(in.gcount())>1024*1024) throw std::runtime_error("invalid memory control file size");
   value.resize(in.gcount());return value;
}
inline bool contains(const std::string& values,const std::string& wanted) {
   std::istringstream in(values);std::string item;
   while(std::getline(in,item,',')) if(item==wanted) return true;
   return false;
}
inline std::string unescape(const std::string& s) {
   std::string out;
   for(size_t i=0;i<s.size();++i) {
      if(s[i]=='\\' && i+3<s.size() && s[i+1]>='0' && s[i+1]<='7' &&
         s[i+2]>='0' && s[i+2]<='7' && s[i+3]>='0' && s[i+3]<='7') {
         out+=char((s[i+1]-'0')*64+(s[i+2]-'0')*8+s[i+3]-'0');i+=3;
      } else out+=s[i];
   }
   return out;
}
inline bool within(const path& parent,const path& child) {
   auto p=parent.begin(),c=child.begin();
   for(;p!=parent.end();++p,++c) if(c==child.end() || *p!=*c) return false;
   return true;
}
inline uint64_t number(const std::string& text) {
   const auto t=trim(text);
   if(t.empty() || !std::all_of(t.begin(),t.end(),[](unsigned char c){return c>='0'&&c<='9';}))
      throw std::runtime_error("invalid memory limit");
   size_t n=0;auto value=std::stoull(t,&n);
   if(n!=t.size() || !value) throw std::runtime_error("invalid memory limit");
   return value;
}
} // namespace detail

// Honor both v1/v2 ancestor limits and namespaced mount roots. There is no
// user-controlled bypass path; alternate proc roots are for filesystem tests.
inline uint64_t effective_memory(uint64_t physical,const std::filesystem::path& proc="/proc") {
   using namespace detail;
   auto memberships=read(proc/"self/cgroup"),mounts=read(proc/"self/mountinfo");
   if(!memberships || !mounts) throw std::runtime_error("cannot inspect memory cgroup membership");
   std::optional<path> unified,legacy;std::string line;
   std::istringstream groups(*memberships);
   while(std::getline(groups,line)) {
      auto a=line.find(':'),b=a==line.npos?line.npos:line.find(':',a+1);
      if(b==line.npos) continue;
      auto controllers=line.substr(a+1,b-a-1);
      if(controllers.empty()) unified=line.substr(b+1);
      else if(contains(controllers,"memory")) legacy=line.substr(b+1);
   }
   auto result=physical;bool seen=false,located=false;
   std::istringstream rows(*mounts);
   while(std::getline(rows,line)) {
      std::istringstream row(line);std::vector<std::string> f;std::string token;
      while(row>>token) f.push_back(token);
      auto sep=std::find(f.begin(),f.end(),"-");
      if(f.size()<7 || sep==f.end() || f.end()-sep<4) continue;
      auto kind=*(sep+1);bool v2=kind=="cgroup2";auto group=v2?unified:legacy;
      if(!group || (!v2 && (kind!="cgroup" || !contains(*(sep+3),"memory")))) continue;
      seen=true;path root=unescape(f[3]),mount=unescape(f[4]);
      if(!group->is_absolute() || !root.is_absolute() || !mount.is_absolute())
         throw std::runtime_error("invalid memory cgroup path");
      for(auto& item:*group) if(item=="..") throw std::runtime_error("invalid memory cgroup path");
      root=root.lexically_normal();mount=mount.lexically_normal();path current;
      if(within(root,*group)) current=(mount/group->lexically_relative(root)).lexically_normal();
      else if(*group=="/") current=mount;
      else continue;
      if(!within(mount,current)) throw std::runtime_error("invalid memory cgroup mount");
      located=true;
      while(true) {
         if(auto text=read(current/(v2?"memory.max":"memory.limit_in_bytes"))) {
            if(!(v2 && trim(*text)=="max")) result=std::min(result,number(*text));
         }
         if(current==mount) break;
         auto next=current.parent_path();
         if(next==current || !within(mount,next)) break;
         current=next;
      }
   }
   if(seen && !located) throw std::runtime_error("memory cgroup is outside visible mounts");
   return result;
}
inline std::string requirement_error(const chain::experimental::parallel_cpu_info& cpu,uint64_t memory) {
   const auto usable=cpu.quota_cores?std::min(cpu.allowed_logical,*cpu.quota_cores):cpu.allowed_logical;
   if(cpu.affinity_known && usable>=minimum_vcpus && memory>=minimum_memory_bytes) return {};
   return "TEST nodeos startup rejected. Requires 4 allowed vCPUs and an 8 GB-class machine "
      "(at least 7,500,000,000 effective bytes after host/cgroup limits). Detected "+
      std::to_string(cpu.allowed_logical)+" allowed logical CPUs, "+std::to_string(usable)+
      " quota-limited vCPUs, "+std::to_string(memory)+" effective bytes. Parallel execution remains mandatory.";
}
inline std::string check() {
   const auto cpu=chain::experimental::detect_parallel_cpu_info();
   const auto physical=detect_hardware().memory_bytes;
   const auto memory=effective_memory(physical);
   if(auto error=requirement_error(cpu,memory);!error.empty()) throw std::runtime_error(error);
   return "TEST ONLY: 4 vCPU / 8 GB-class profile; allowed logical CPUs="+std::to_string(cpu.allowed_logical)+
      ", effective memory bytes="+std::to_string(memory)+". Not production hardware qualification.";
}
} // namespace eosio::nodeos::test_build
