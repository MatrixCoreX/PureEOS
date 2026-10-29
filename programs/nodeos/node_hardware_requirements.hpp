#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace eosio::nodeos {
inline constexpr unsigned minimum_physical_cores = 8;
inline constexpr unsigned minimum_logical_threads = 16;
// Decimal GB. Read MemTotal, never MemFree/MemAvailable or the chain RAM market.
inline constexpr std::uint64_t minimum_memory_bytes = 64'000'000'000ULL;
struct hardware_info {
   unsigned physical_cores = 0;
   unsigned logical_threads = 0;
   std::uint64_t memory_bytes = 0;
   bool topology_known = false;
   std::vector<std::string> detection_errors;
};
struct hardware_probe {
   std::filesystem::path cpu_root = "/sys/devices/system/cpu";
   std::filesystem::path proc_root = "/proc";
};
inline bool read_unsigned(const std::filesystem::path& path, std::uint64_t& value) {
   std::ifstream in(path);std::string token,extra;
   if (!(in>>token) || (in>>extra) || token.empty() ||
       !std::all_of(token.begin(),token.end(),[](unsigned char c){return c>='0'&&c<='9';})) return false;
   try {std::size_t count=0;value=std::stoull(token,&count);return count==token.size();}
   catch (...) {return false;}
}
inline std::string trim(std::string value) {
   const auto a=value.find_first_not_of(" \t\r\n");
   if(a==value.npos)return {};
   return value.substr(a,value.find_last_not_of(" \t\r\n")-a+1);
}
inline bool parse_cpu_list(const std::string& text,std::vector<unsigned>& cpus) {
   std::istringstream in(trim(text));std::string part;std::set<unsigned> unique;
   while(std::getline(in,part,',')) {
      const auto dash=part.find('-');
      auto number=[](const std::string& v,unsigned& out) {
         if(v.empty()||!std::all_of(v.begin(),v.end(),[](unsigned char c){return c>='0'&&c<='9';}))return false;
         try {auto n=std::stoull(v);if(n>=1048576)return false;out=n;return true;}catch(...){return false;}
      };
      unsigned first=0,last=0;
      if(!number(part.substr(0,dash),first))return false;
      if(dash==part.npos)last=first;
      else if(!number(part.substr(dash+1),last)||last<first)return false;
      for(unsigned n=first;n<=last;++n)unique.insert(n);
   }
   if(unique.empty())return false;
   cpus.assign(unique.begin(),unique.end());return true;
}
inline hardware_info detect_hardware(const hardware_probe& probe = {}) {
   hardware_info result;
   std::ifstream online(probe.cpu_root/"online");std::string list;std::vector<unsigned> cpus;
   if(std::getline(online,list)&&parse_cpu_list(list,cpus)) {
      std::set<std::tuple<std::uint64_t,std::uint64_t,std::uint64_t>> cores;
      bool complete=true;
      for(auto cpu:cpus) {
         auto base=probe.cpu_root/("cpu"+std::to_string(cpu))/"topology";
         std::uint64_t package=0,core=0,die=0;
         if(!read_unsigned(base/"physical_package_id",package)||!read_unsigned(base/"core_id",core)) {
            complete=false;break;
         }
         // Package/core identifies a core on common Linux topologies; include
         // die ID when exposed to distinguish repeated core IDs across dies.
         read_unsigned(base/"die_id",die);cores.emplace(package,die,core);
      }
      result.logical_threads=cpus.size();
      if(complete){result.physical_cores=cores.size();result.topology_known=true;}
   }
   if(!result.topology_known) {
      std::ifstream in(probe.proc_root/"cpuinfo");std::string line;
      std::map<std::string,std::string> fields;
      std::set<std::string> processors;
      std::set<std::pair<std::string,std::string>> cores;
      bool complete=true;
      const auto flush=[&] {
         if(fields.empty())return;
         const auto processor=fields.find("processor"),package=fields.find("physical id"),core=fields.find("core id");
         if(processor==fields.end()||package==fields.end()||core==fields.end()||
            processor->second.empty()||package->second.empty()||core->second.empty()||
            !processors.insert(processor->second).second) complete=false;
         else cores.emplace(package->second,core->second);
         fields.clear();
      };
      while(std::getline(in,line)) {
         if(trim(line).empty()){flush();continue;}
         const auto colon=line.find(':');
         if(colon!=line.npos)fields[trim(line.substr(0,colon))]=trim(line.substr(colon+1));
      }
      flush();
      if(complete&&!processors.empty()&&(!result.logical_threads||result.logical_threads==processors.size())) {
         result.physical_cores=cores.size();result.logical_threads=processors.size();result.topology_known=true;
      }
   }
   if(!result.topology_known)result.detection_errors.push_back("physical CPU topology could not be verified");
   std::ifstream mem(probe.proc_root/"meminfo");std::string line;
   while(std::getline(mem,line)) {
      std::istringstream row(line);std::string key,unit,extra;std::uint64_t kib=0;
      if(row>>key;key=="MemTotal:") {
         if((row>>kib>>unit)&&!(row>>extra)&&unit=="kB"&&kib&&kib<=std::numeric_limits<std::uint64_t>::max()/1024)
            result.memory_bytes=kib*1024;
         break;
      }
   }
   if(!result.memory_bytes)result.detection_errors.push_back("physical memory total could not be verified");
   return result;
}
inline std::string minimum_hardware_description() {
   return "Minimum configuration: 8 physical CPU cores, 16 logical CPU threads, 64 GB RAM "
          "(64,000,000,000 bytes of system-visible physical memory). Parallel mode is mandatory.";
}
inline std::string hardware_description(const hardware_info& info) {
   std::ostringstream out;out.imbue(std::locale::classic());
   out<<"Detected hardware: "<<info.physical_cores<<" physical CPU cores, "<<info.logical_threads
      <<" logical CPU threads, "<<info.memory_bytes<<" bytes RAM ("<<std::fixed<<std::setprecision(2)
      <<double(info.memory_bytes)/1e9<<" GB / "<<double(info.memory_bytes)/(1024.0*1024*1024)<<" GiB).";
   return out.str();
}
inline std::string hardware_requirement_error(const hardware_info& info) {
   if(info.topology_known&&info.detection_errors.empty()&&
      info.physical_cores>=minimum_physical_cores&&info.logical_threads>=minimum_logical_threads&&
      info.memory_bytes>=minimum_memory_bytes)return {};
   std::string message="nodeos startup rejected. "+minimum_hardware_description()+" "+hardware_description(info);
   for(const auto& error:info.detection_errors)message+=" Detection error: "+error+".";
   return message;
}
} // namespace eosio::nodeos
