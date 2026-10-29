#include "../programs/nodeos/node_hardware_requirements.hpp"
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace eosio::nodeos;
void require(bool condition,const char* label){if(!condition)throw std::runtime_error(label);}
int main(){try{
 auto root=std::filesystem::temp_directory_path()/("aic-node-hardware-"+std::to_string(::getpid()));
 require(std::filesystem::create_directory(root),"unique fixture");
 struct clean {std::filesystem::path p;~clean(){std::error_code e;std::filesystem::remove_all(p,e);}} cleanup{root};
 auto put=[&](std::filesystem::path p,const std::string& text){std::filesystem::create_directories(p.parent_path());std::ofstream(p)<<text;};
 hardware_probe probe{root/"cpu",root/"proc"};
 put(probe.cpu_root/"online","0-15\n");
 for(unsigned i=0;i<16;++i){
   auto p=probe.cpu_root/("cpu"+std::to_string(i))/"topology";
   put(p/"physical_package_id","0\n");put(p/"core_id",std::to_string(i%8)+"\n");
 }
 put(probe.proc_root/"meminfo","MemTotal: 62500000 kB\nMemAvailable: 1 kB\n");
 auto good=detect_hardware(probe);require(good.physical_cores==8&&good.logical_threads==16,"8 cores /16 SMT threads");
 require(good.memory_bytes==64000000000ULL&&hardware_requirement_error(good).empty(),"exact 64 GB; free memory not a gate");
 auto low=good;low.physical_cores=7;require(!hardware_requirement_error(low).empty(),"physical core floor");
 low=good;low.logical_threads=15;require(!hardware_requirement_error(low).empty(),"logical thread floor");
 low=good;low.memory_bytes=minimum_memory_bytes-1;require(!hardware_requirement_error(low).empty(),"RAM exact byte floor");
 low=good;low.topology_known=false;require(!hardware_requirement_error(low).empty(),"unknown topology is rejected");
 require(hardware_requirement_error(low).find("Minimum configuration:")!=std::string::npos,"error contains minimum configuration");
 require(hardware_requirement_error(low).find("Detected hardware:")!=std::string::npos,"error contains detected hardware");
 // The selected 64 GiB bare-metal host reserves memory for the platform/kernel.
 put(probe.proc_root/"meminfo","MemTotal: 63360244 kB\nMemFree: 1 kB\n");
 require(hardware_requirement_error(detect_hardware(probe)).empty(),"actual selected host memory passes without using free RAM");
 // Multi-socket IDs are independent; SMT threads are not physical cores.
 for(unsigned i=0;i<16;++i){
   auto p=probe.cpu_root/("cpu"+std::to_string(i))/"topology";
   put(p/"physical_package_id",std::to_string(i/8)+"\n");put(p/"core_id",std::to_string((i%8)/2)+"\n");
 }
 require(detect_hardware(probe).physical_cores==8,"two packages and SMT counted correctly");
 std::filesystem::remove_all(probe.cpu_root);
 std::string cpuinfo;
 for(unsigned i=0;i<16;++i)cpuinfo+="processor : "+std::to_string(i)+"\nphysical id : 0\ncore id : "+std::to_string(i%8)+"\n\n";
 put(probe.proc_root/"cpuinfo",cpuinfo);
 require(hardware_requirement_error(detect_hardware(probe)).empty(),"proc topology fallback");
 put(probe.proc_root/"cpuinfo","processor : 0\n\n");
 require(!hardware_requirement_error(detect_hardware(probe)).empty(),"cannot infer physical cores from logical count");
 put(probe.proc_root/"meminfo","MemTotal: invalid kB\n");
 require(!hardware_requirement_error(detect_hardware(probe)).empty(),"malformed memory fails closed");
 std::vector<unsigned> cpus;
 require(parse_cpu_list("0-7,16-23\n",cpus)&&cpus.size()==16,"sparse CPU IDs");
 require(!parse_cpu_list("7-0",cpus)&&!parse_cpu_list("0,,2",cpus),"malformed CPU range rejected");
 std::cout<<"nodeos hardware minimum: exact boundaries, SMT, sockets, sparse CPU IDs, real-host RAM, fallback and unknown-data rejection passed\n";
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
