#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <eosio/chain/parallel_auto_config.hpp>
#include <eosio/chain/parallel_runtime_limits.hpp>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#ifdef __linux__
#include <sched.h>
#endif

namespace eosio::chain::experimental {
namespace {
using path = std::filesystem::path;
std::optional<std::string> read_text(const path& p,size_t maximum=8192) {
   std::error_code error;
   if (!std::filesystem::exists(p,error) && !error) return {};
   std::ifstream in(p);
   if (!in) throw std::runtime_error("unreadable CPU control file");
   std::string text(maximum+1,'\0');
   in.read(text.data(),text.size());
   if (in.bad() || static_cast<size_t>(in.gcount())>maximum) throw std::runtime_error("invalid CPU control file size");
   text.resize(in.gcount());return text;
}
uint64_t number(std::string_view s) {
   uint64_t n=0;
   const auto [end,error]=std::from_chars(s.data(),s.data()+s.size(),n);
   if (s.empty() || error!=std::errc{} || end!=s.data()+s.size())
      throw std::invalid_argument("expected an unsigned decimal integer");
   return n;
}
std::string one_token(std::string_view text) {
   std::istringstream in{std::string(text)};std::string value,extra;
   if (!(in>>value) || (in>>extra)) throw std::invalid_argument("invalid CPU topology value");
   return value;
}
bool contains_controller(std::string_view text,std::string_view name) {
   size_t first=0;
   while (first<=text.size()) {
      const auto end=text.find(',',first);
      if (text.substr(first,end==text.npos ? text.size()-first : end-first)==name) return true;
      if (end==text.npos) break;
      first=end+1;
   }
   return false;
}
std::string unescape_mount(std::string_view text) {
   std::string result;
   for (size_t i=0;i<text.size();++i) {
      if (text[i]=='\\' && i+3<text.size() && text[i+1]>='0' && text[i+1]<='7' &&
          text[i+2]>='0' && text[i+2]<='7' && text[i+3]>='0' && text[i+3]<='7') {
         result.push_back(char((text[i+1]-'0')*64+(text[i+2]-'0')*8+text[i+3]-'0'));i+=3;
      } else result.push_back(text[i]);
   }
   return result;
}
bool parent_path(const path& parent,const path& child) {
   auto p=parent.begin(),c=child.begin();
   for (;p!=parent.end();++p,++c) if (c==child.end() || *p!=*c) return false;
   return true;
}
std::optional<path> locate_group(const path& group,const path& root,const path& mount) {
   if (!group.is_absolute() || !root.is_absolute() || !mount.is_absolute()) return {};
   for (const auto& item:group) if (item=="..") return {};
   if (parent_path(root,group)) return (mount/group.lexically_relative(root)).lexically_normal();
   // A namespaced process can report itself at/ although the mount root is
   // already a delegated subtree. Ancestors outside that mount are invisible.
   if (group=="/") return mount;
   return {};
}
void cap_quota(parallel_cpu_info& info,uint64_t quota,uint64_t period) {
   if (!quota || !period) throw std::invalid_argument("zero CPU quota/period");
   const auto count=std::max<uint64_t>(1,quota/period);
   const auto capped=static_cast<size_t>(std::min<uint64_t>(count,std::numeric_limits<size_t>::max()));
   info.quota_cores=std::min(info.quota_cores.value_or(capped),capped);
}
void inspect_quota(parallel_cpu_info& info,path current,const path& mount,bool v2) {
   if (!parent_path(mount,current)) return;
   while (true) {
      try {
         if (v2) {
            if (auto text=read_text(current/"cpu.max")) {
               std::istringstream in(*text);std::string quota,period,extra;
               if (!(in>>quota>>period) || (in>>extra) || !number(period))
                  throw std::invalid_argument("invalid cpu.max");
               if (quota!="max") cap_quota(info,number(quota),number(period));
               info.quota_known=true;
            }
         } else if (auto quota=read_text(current/"cpu.cfs_quota_us")) {
            auto period=read_text(current/"cpu.cfs_period_us");
            if (!period) throw std::invalid_argument("missing CPU period");
            const auto q=one_token(*quota);const auto p=number(one_token(*period));
            if (!p) throw std::invalid_argument("zero CPU period");
            if (!q.empty() && q.front()=='-') {
               if (!number(std::string_view(q).substr(1))) throw std::invalid_argument("zero CPU quota");
            } else cap_quota(info,number(q),p);
            info.quota_known=true;
         }
      } catch (const std::exception&) {
         // A detected but malformed/unreadable quota must not silently turn
         // into permission to launch the host's entire worker count.
         info.quota_cores=1;info.warnings.emplace_back("CPU quota unreadable or malformed; auto capped to 1 worker");
      }
      if (current==mount) break;
      const auto next=current.parent_path();if (next==current || !parent_path(mount,next)) break;current=next;
   }
}
void detect_quota(parallel_cpu_info& info,const path& proc) {
   const auto memberships=read_text(proc/"self/cgroup",64*1024);
   const auto mounts=read_text(proc/"self/mountinfo",1024*1024);
   if (!memberships || !mounts) return;
   std::optional<path> unified,legacy;
   std::istringstream groups(*memberships);std::string line;
   while (std::getline(groups,line)) {
      const auto a=line.find(':'),b=a==line.npos ? line.npos : line.find(':',a+1);
      if (b==line.npos) continue;
      const auto controllers=line.substr(a+1,b-a-1);
      if (controllers.empty()) unified=line.substr(b+1);
      else if (contains_controller(controllers,"cpu")) legacy=line.substr(b+1);
   }
   std::istringstream lines(*mounts);
   bool cpu_mount_seen=false,group_located=false;
   while (std::getline(lines,line)) {
      std::istringstream in(line);std::vector<std::string> fields;std::string token;
      while (in>>token) fields.push_back(token);
      const auto sep=std::find(fields.begin(),fields.end(),"-");
      if (fields.size()<7 || sep==fields.end() || fields.end()-sep<4) continue;
      const auto kind=*(sep+1);const bool v2=kind=="cgroup2";
      const auto& group=v2 ? unified : legacy;
      if (!group || (!v2 && (kind!="cgroup" || !contains_controller(*(sep+3),"cpu")))) continue;
      cpu_mount_seen=true;
      const auto mount=path(unescape_mount(fields[4])).lexically_normal();
      if (auto current=locate_group(*group,path(unescape_mount(fields[3])).lexically_normal(),mount)) {
         group_located=true;
         inspect_quota(info,*current,mount,v2);
      }
   }
   if (cpu_mount_seen && !group_located) {
      info.quota_cores=1;info.warnings.emplace_back("CPU cgroup path is outside visible mounts; auto capped to 1 worker");
   }
}
std::vector<unsigned> affinity_cpus() {
#ifdef __linux__
   for (size_t words=16;words<=2048;words*=2) {
      std::vector<unsigned long> mask(words);
      if (::sched_getaffinity(0,mask.size()*sizeof(mask[0]),reinterpret_cast<cpu_set_t*>(mask.data()))==0) {
         std::vector<unsigned> cpus;
         for (size_t i=0;i<words*sizeof(mask[0])*8;++i)
            if ((mask[i/(sizeof(mask[0])*8)]>>(i%(sizeof(mask[0])*8)))&1ul) cpus.push_back(i);
         return cpus;
      }
      if (errno!=EINVAL) break;
   }
#endif
   return {};
}
std::string bounds_message() {
   return "experimental parallel validation requires threads 0-"+std::to_string(parallel_runtime_limits::max_workers)+
      " and batch size 1-"+std::to_string(parallel_runtime_limits::max_batch_transactions);
}
} // namespace

size_t parallel_cpu_info::usable_cores() const noexcept {
   auto result=std::max<size_t>(1,std::min(allowed_logical,allowed_physical));
   if (quota_cores) result=std::min(result,std::max<size_t>(1,*quota_cores));
   return result;
}
parallel_cpu_info detect_parallel_cpu_info(const parallel_cpu_probe& probe) {
   parallel_cpu_info info;
   auto cpus=probe.allowed_cpus ? *probe.allowed_cpus : affinity_cpus();
   std::sort(cpus.begin(),cpus.end());cpus.erase(std::unique(cpus.begin(),cpus.end()),cpus.end());
   if (!cpus.empty()) {info.allowed_logical=cpus.size();info.affinity_known=true;}
   else {
      info.allowed_logical=std::max(1u,probe.hardware_fallback ? probe.hardware_fallback : std::thread::hardware_concurrency());
      info.warnings.emplace_back("CPU affinity unavailable; using hardware concurrency");
   }
   info.allowed_physical=info.allowed_logical;
   if (!cpus.empty()) {
      try {
         std::set<std::pair<uint64_t,uint64_t>> cores;
         for (auto cpu:cpus) {
            const auto base=probe.cpu_root/("cpu"+std::to_string(cpu))/"topology";
            const auto socket=read_text(base/"physical_package_id"),core=read_text(base/"core_id");
            if (!socket || !core) throw std::runtime_error("CPU topology unavailable");
            cores.emplace(number(one_token(*socket)),number(one_token(*core)));
         }
         info.allowed_physical=cores.size();info.topology_known=true;
      } catch (const std::exception&) {info.warnings.emplace_back("CPU topology unavailable; using allowed logical CPUs");}
   }
   try {detect_quota(info,probe.proc_root);}
   catch (const std::exception&) {
      info.quota_cores=1;info.warnings.emplace_back("CPU quota discovery failed; auto capped to 1 worker");
   }
   return info;
}
resolved_parallel_config resolve_parallel_config(std::string_view workers,std::string_view batch,
      bool batch_explicit,const parallel_cpu_info& cpu) {
   resolved_parallel_config result;result.workers_auto=workers=="auto";
   result.batch_auto=batch=="auto" || (result.workers_auto && !batch_explicit);
   uint64_t count=0,size=0;
   try {count=result.workers_auto ? std::min(cpu.usable_cores(),parallel_runtime_limits::max_workers) : number(workers);}
   catch (const std::invalid_argument&) {throw std::invalid_argument("parallel workers must be auto or an unsigned decimal integer");}
   if (count>parallel_runtime_limits::max_workers) throw std::invalid_argument(bounds_message());
   try {size=result.batch_auto ? (count ? std::min<uint64_t>(3*count,parallel_runtime_limits::max_batch_transactions) : 8) : number(batch);}
   catch (const std::invalid_argument&) {throw std::invalid_argument("parallel batch must be auto or an unsigned decimal integer");}
   if (!size || size>parallel_runtime_limits::max_batch_transactions) throw std::invalid_argument(bounds_message());
   result.workers=static_cast<uint16_t>(count);result.batch=static_cast<uint16_t>(size);return result;
}
} // namespace eosio::chain::experimental
