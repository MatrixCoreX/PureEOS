"""Latest two-VM, four-worker correctness profile; no TPS or independent-physical-host claim."""
import argparse
from datetime import datetime,timezone
import hashlib,json,os
from pathlib import Path
import signal,time

from pureeos_multihost_cloud_test import CloudMultiNodeTest
from pureeos_rotation_cohort_cloud_test import CohortCloudTest,cohort_names
from pureeos_single_choice_local_test import LocalTest
from pureeos_host_metrics import collect as collect_host_metrics


def runtime_flags(argv):
    valued={'--wasm-runtime','--experimental-parallel-engine','--experimental-parallel-validation-threads',
        '--experimental-parallel-validation-batch-size','--experimental-parallel-code-hash'}
    flags={};hashes=[]
    for index,value in enumerate(argv):
        if value=='--experimental-parallel-production':flags['production']=True
        if value in valued:
            if index+1>=len(argv):raise ValueError('Missing runtime option value')
            if value=='--experimental-parallel-code-hash':hashes.append(argv[index+1])
            elif value in flags:raise ValueError('Duplicate explicit runtime option')
            else:flags[value]=argv[index+1]
    flags['code_hashes']=sorted(hashes)
    return flags


def require_parallel_flags(flags,expected_hashes):
    if (flags.get('--wasm-runtime')!='eos-vm-jit' or flags.get('--experimental-parallel-engine')!='jit'
            or flags.get('--experimental-parallel-validation-threads')!='4'
            or flags.get('--experimental-parallel-validation-batch-size')!='8' or flags.get('production') is not True
            or flags.get('code_hashes')!=sorted(expected_hashes)):
        raise AssertionError('Actual node process lacks the declared four-worker JIT profile')


def require_host_resources(hosts):
    if set(hosts)!={'a','b'}:raise AssertionError('Both actual VM resource samples required')
    for host in hosts.values():
        if (host.get('status')!='complete' or host['probes']['oom'].get('oom_detected') is not False
                or host['probes']['memory']['memory_bytes']['available']<2*1024**3):
            raise AssertionError('Incomplete resource sample, OOM, or less than two GiB host reserve')


def abi_true(value):
    # Contract ABI bool fields may be rendered as JSON true or integer 1.
    # Do not accept floats, strings or unrelated truthy values as ban evidence.
    return value is True or (type(value) is int and value == 1)


def complete_cloud(report,phase):
    if (report.get('status')!='passed' or report.get('processes_stopped') is not True or report.get('shutdown_errors')
            or report.get('parallel_workers')!=4 or report.get('vm_hosts')!=2 or not report.get('final_synchronization')
            or report.get('actual_parallel_runtime_verified') is not True):return False
    if not report.get('resource_samples'):return False
    try:
        for sample in report['resource_samples']:require_host_resources(sample['hosts'])
    except (KeyError,TypeError,AssertionError):return False
    if phase=='smoke':return set(report.get('processes',{}))==set(cohort_names(2)) and bool(report.get('reward_checks'))
    if phase!='cohort':return False
    names=set(cohort_names(40));phases=report.get('cohort_phases',[]);fault=report.get('fault',{});recovery=report.get('recovered_fault',{})
    return bool([p.get('candidate_count') for p in phases]==[30,40]
        and all(p.get('status')=='passed' and p.get('agreement') and set(cohort_names(p['candidate_count']))<=set(p.get('queue_entries',[]))
            and set(cohort_names(p['candidate_count']))<=set(p.get('finalized_production',{}).get('counts',{}))
            and all(int(v)>0 for v in p.get('finalized_production',{}).get('counts',{}).values()) for p in phases)
        and {len(e['schedule']['producers']) for e in report.get('committee_events',[])}==set(range(1,22))
        and set(report.get('processes',{}))==names and len({p['pid'] for p in report['processes'].values()})==40
        and recovery.get('missing',{}).get('zero_windows') in (1,2) and not recovery.get('missing',{}).get('banned',True)
        and recovery.get('recovered',{}).get('zero_windows')==0 and not recovery.get('recovered',{}).get('banned',True)
        and abi_true(fault.get('candidate_after',{}).get('banned')) and fault.get('candidate_after',{}).get('zero_windows')==3
        and len(set(fault.get('candidate_after',{}).get('evidence_slots',[])))==3
        and len(fault.get('actual_post_ban_entries',[]))>=2 and fault.get('node') not in fault.get('actual_post_ban_entries',[])
        and int(fault.get('preserved_unpaid_blocks',0))>0 and report.get('actual_parallel_runtime_verified') is True)


class ParallelCloudProfile:
    parallel_workers=4
    peer_runtime_seconds=39600
    wallet_unlock_seconds=43200
    peer_memory_mib=1200
    peer_shutdown_seconds=90

    def __init__(self,args):
        super().__init__(args)
        self.metrics_since_utc=datetime.now(timezone.utc).isoformat();self._last_resources=time.monotonic()
        self.report.update(parallel_workers=4,engine='jit',tps_not_tested=True,resource_samples=[],
            profile_script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())

    @staticmethod
    def stop(process):return LocalTest.stop(process,shutdown_timeout=90)

    def spawn(self,command,log_name):
        if command[0]==str(self.nodeos):
            command=[*command,'--wasm-runtime','eos-vm-jit','--experimental-parallel-engine','jit',
                '--experimental-parallel-validation-threads','4','--experimental-parallel-validation-batch-size','8',
                '--experimental-parallel-production']
            for name in ('eosio.token','eosio.system'):
                command+=['--experimental-parallel-code-hash',hashlib.sha256((self.contracts/name/(name+'.wasm')).read_bytes()).hexdigest()]
        return super().spawn(command,log_name)

    def resources(self,label):
        collector=Path(__file__).with_name('pureeos_host_metrics.py').read_text()
        remote=json.loads(self.ssh(['python3','-c',collector,'--since',self.metrics_since_utc],timeout=60))
        sample=dict(label=label,utc=datetime.now(timezone.utc).isoformat(),
            hosts={'a':collect_host_metrics(since=self.metrics_since_utc),'b':remote},runtime_flags={})
        expected=[hashlib.sha256((self.contracts/n/(n+'.wasm')).read_bytes()).hexdigest() for n in ('eosio.token','eosio.system')]
        for name,item in self.process_evidence().items():
            if name!='eosio' and self.peers[name].get('stopped'):continue
            pid=item['pid']
            if isinstance(pid,str):
                number=int(pid.split(':')[-1])
                code='import json;from pathlib import Path;print(json.dumps([v.decode() for v in Path("/proc/'+str(number)+'/cmdline").read_bytes().rstrip(bytes([0])).split(bytes([0]))]))'
                argv=json.loads(self.ssh(['python3','-c',code]))
            else:argv=Path('/proc/'+str(pid)+'/cmdline').read_bytes().decode().rstrip('\0').split('\0')
            flags=runtime_flags(argv);require_parallel_flags(flags,expected);sample['runtime_flags'][name]=flags
        self.report['resource_samples'].append(sample)
        self._last_resources=time.monotonic();self.checkpoint()
        require_host_resources(sample['hosts'])
        self.report['actual_parallel_runtime_verified']=True;self.checkpoint()

    def observe(self):
        result=super().observe()
        if time.monotonic()-self._last_resources>=120:self.resources('Periodic two-host resource and actual runtime flags')
        return result


class TwoHostSmokeTest(ParallelCloudProfile,CloudMultiNodeTest):pass
class TwoHostCohortTest(ParallelCloudProfile,CohortCloudTest):pass


def main():
    os.umask(0o077);parser=argparse.ArgumentParser(description=__doc__)
    for name in ('build-dir','contracts-dir','state-parent','peer-key','peer-known-hosts'):
        parser.add_argument('--'+name,type=Path,required=True)
    for name in ('peer-ip','coordinator-ip'):parser.add_argument('--'+name,required=True)
    parser.add_argument('--phase',choices=('smoke','cohort'),required=True)
    args=parser.parse_args();args.node_count=2 if args.phase=='smoke' else 40
    def interrupted(_s,_f):raise KeyboardInterrupt('Stop only this two-host private test')
    signal.signal(signal.SIGTERM,interrupted)
    test=(TwoHostSmokeTest if args.phase=='smoke' else TwoHostCohortTest)(args)
    code=test.execute();assert code!=0 or complete_cloud(test.report,args.phase),'Completion proof incomplete'
    return code


if __name__=='__main__':raise SystemExit(main())
