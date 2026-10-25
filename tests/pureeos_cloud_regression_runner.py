#!/usr/bin/env python3
"""Task-scoped cloud runner; never has cloud API credentials or creates VMs.

Records terminal failures even without a child report. Never restarts a test
on service restart. Archives on the cloud independently of the local observer.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import socket
import subprocess
import sys
import time
import traceback

NODE_SHA='7837882506bc626958e6b6668427b0006fd46cb777756c203d541571a8ff4d93'


def now():return datetime.now(timezone.utc).isoformat()


def atomic(path,value):
    path=Path(path);temporary=path.with_suffix('.pending')
    temporary.write_text(json.dumps(value,indent=2));temporary.replace(path)


def sha(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def verified_root(root):
    root=root.resolve()
    if root.parent!=Path('/opt') or not re.fullmatch('pureeos-history-[A-Za-z0-9]+',root.name):
        raise ValueError('Refuse a non-task root')
    return root


class Runner:
    def __init__(self,root):
        self.root=verified_root(root);self.input=self.root/'input'
        self.pair=json.loads((self.input/'pair.json').read_text())
        roles=[r for r,v in self.pair.items() if v['label']==socket.gethostname()]
        if len(roles)!=1:raise ValueError('Not an exact task host')
        self.role=roles[0]
        self.runtime=Path('/opt/pureeos-modern/reusable-runtime')

    def ssh(self,role,arguments,timeout=120,input_text=None):
        if role not in 'bcd' or len(role)!=1:raise ValueError('Invalid peer role')
        item=self.pair[role]
        args=['ssh','-i',item['key_path'],'-o','BatchMode=yes','-o','IdentitiesOnly=yes',
              '-o','ConnectTimeout=8','-o','ServerAliveInterval=15','-o','ServerAliveCountMax=2',
              '-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+item['known_hosts'],
              'root@'+item['main_ip'],shlex.join(map(str,arguments))]
        result=subprocess.run(args,input=input_text,text=True,capture_output=True,timeout=timeout)
        if result.returncode:raise RuntimeError('Task peer '+role+' failed: '+result.stderr[-1200:])
        return result.stdout

    def verify(self):
        manifest=json.loads((self.runtime/'manifest.json').read_text())
        for name,value in manifest.items():
            path=self.runtime/name
            if path.stat().st_size!=value['bytes'] or sha(path)!=value['sha256']:raise RuntimeError('Runtime mismatch: '+name)
        if manifest['programs/nodeos.elf']['sha256']!=NODE_SHA:raise RuntimeError('Wrong node binary')
        scripts=json.loads((self.input/'manifest.json').read_text())
        for name,digest in scripts.items():
            if sha(self.input/name)!=digest:raise RuntimeError('Test script changed: '+name)
        return len(manifest)

    def run(self,kind):
        expected='a' if kind=='cohort' else 'b'
        if self.role!=expected:raise ValueError('Wrong host for suite')
        self.verify()
        marker=self.root/(kind+'-launch.json')
        if marker.exists():
            atomic(self.root/(kind+'-restart-refused.json'),dict(status='interrupted-restart-refused',utc=now(),
                reason='Existing private test never reruns or overwrites its evidence on service restart'))
            return 1
        with marker.open('x') as output:json.dump(dict(started_utc=now(),pid=os.getpid()),output)
        report=dict(status='running',started_utc=now(),kind=kind,instance_id=self.pair[self.role]['id'])
        atomic(self.root/(kind+'-runner.json'),report)
        child=None
        try:
            if kind=='cohort':
                script=self.input/'pureeos_fourhost_rotation_cloud_test.py'
                args=['--build-dir',self.root/'build','--contracts-dir',self.runtime/'contracts',
                      '--state-parent',self.root/'state','--hosts-json',self.input/'pair.json']
            else:
                script=self.input/'pureeos_historical_edge_cases_test.py'
                args=['--build-dir',self.root/'build','--contracts-dir',self.runtime/'contracts',
                      '--report',self.root/'history-report.json','--rounds','3']
            with (self.root/(kind+'-chain.log')).open('x') as log:
                child=subprocess.Popen([sys.executable,str(script),*map(str,args)],stdout=log,stderr=subprocess.STDOUT,
                    env={**os.environ,'PYTHONPATH':str(self.input)})
                deadline=datetime.fromisoformat(json.loads((self.input/'limits.json').read_text())['deadline_utc'])
                remaining=(deadline-datetime.now(timezone.utc)).total_seconds()
                timeout=min(39000 if kind=='cohort' else 2400,max(1,remaining-1200))
                code=child.wait(timeout=timeout)
            report.update(exit_code=code,status='passed' if code==0 else 'failed')
        except (Exception,KeyboardInterrupt) as error:
            report.update(status='failed',error=type(error).__name__+': '+str(error),traceback=traceback.format_exc())
            if child and child.poll() is None:
                child.terminate()
                try:child.wait(timeout=120)
                except subprocess.TimeoutExpired:child.kill();child.wait(timeout=15)
        finally:
            report['finished_utc']=now();atomic(self.root/(kind+'-runner.json'),report)
        if kind=='cohort':
            # Archive regardless of pass/failure; never label an archived failed
            # chain as a passing test. Local verification decides final status.
            self.collect_all()
        return 0 if report['status']=='passed' else 1

    def close(self):
        # Only this fresh run's remote unit prefix; no old/user processes.
        if self.role!='a':
            units=subprocess.check_output(['systemctl','list-units','--all','--type=service','--no-legend',
                '--plain','pureeos-peer-integer-v2-*'],text=True)
            old=json.loads((self.input/'old-peer-roots.json').read_text())
            for line in units.splitlines():
                unit=line.split()[0]
                # tempfile's random suffix alphabet includes underscores.
                # Keep the exact task prefix/node shape, not a broad service glob.
                if not re.fullmatch(r'pureeos-peer-integer-v2-[a-z0-9_]+-node[a-z]+-\d+\.service',unit):
                    raise RuntimeError('Unexpected task peer unit name')
                if any(Path(p).name in unit for p in old):continue
                subprocess.run(['systemctl','stop',unit],check=True,timeout=45)
        for name in ('nodeos','nodeos.elf','keosd','keosd.elf'):
            if subprocess.run(['pgrep','-x',name],stdout=subprocess.DEVNULL).returncode!=1:
                raise RuntimeError('A private process is still running; defer archive: '+name)
        count=self.verify();meta=self.root/'archive-meta.json'
        if meta.exists():
            prior=json.loads(meta.read_text());path=Path(prior['archive'])
            if prior['instance_id']!=self.pair[self.role]['id'] or sha(path)!=prior['sha256']:
                raise RuntimeError('Existing archive proof no longer matches')
            return prior
        closure=dict(status='closed',role=self.role,instance_id=self.pair[self.role]['id'],
                     runtime_files_verified=count,nodeos_sha256=NODE_SHA,utc=now())
        atomic(self.root/'closure.json',closure)
        paths=[self.root.name,'pureeos-modern/reusable-runtime']
        # Do not include the earlier interrupted run twice. Its independent,
        # already verified four archives are retained locally.
        old=json.loads((self.input/'old-peer-roots.json').read_text())
        for peer in Path('/opt/pureeos-peer').glob('integer-v2-*'):
            if str(peer) not in old:paths.append(str(peer.relative_to('/opt')))
        archive=Path('/opt')/(self.root.name+'-'+self.role+'-closed.tar.zst')
        if archive.exists():archive.rename(archive.with_name(archive.name+'.incomplete-'+str(time.time_ns())))
        subprocess.run(['tar','--use-compress-program=zstd -T1 -3','-cf',str(archive),
                        '--exclude=*/ssh_key','-C','/opt',*paths],check=True,timeout=780)
        result={**closure,'status':'closed-and-archived','archive':str(archive),'bytes':archive.stat().st_size,'sha256':sha(archive)}
        atomic(meta,result);return result

    def collect_all(self):
        if self.role!='a':raise ValueError('Only coordinator can collect all roles')
        report=dict(status='running',started_utc=now(),archives={})
        for role in 'dcba':
            try:
                if role=='a':item=self.close()
                else:
                    command=[sys.executable,str(self.input/Path(__file__).name),'close','--root',str(self.root)]
                    item=json.loads(self.ssh(role,command,timeout=900).splitlines()[-1])
                if item['instance_id']!=self.pair[role]['id']:raise RuntimeError('Archive instance identity mismatch')
                report['archives'][role]=item
            except Exception as error:report.setdefault('errors',[]).append(dict(role=role,error=str(error)))
            atomic(self.root/'archives.json',report)
        report.update(status='passed' if set(report['archives'])==set('abcd') and not report.get('errors') else 'incomplete',finished_utc=now())
        atomic(self.root/'archives.json',report);return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=('cohort','history','close','collect','verify'))
    parser.add_argument('--root',type=Path,required=True)
    args=parser.parse_args();os.umask(0o077);runner=Runner(args.root)
    if args.action in ('cohort','history'):return runner.run(args.action)
    if args.action=='verify':print(json.dumps({'runtime_files_verified':runner.verify()}));return 0
    result=runner.close() if args.action=='close' else runner.collect_all()
    print(json.dumps(result));return 0 if result['status'] in ('passed','closed-and-archived') else 1


if __name__=='__main__':raise SystemExit(main())
