"""Early syntax check of repaired fixtures in the exact owned build container."""
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import time

NAME='pureeos-v1-local-cold-warm-retest-attempt-1'


def guest(attempt):
    root=Path('/work');started=time.monotonic();jobs=[]
    def processes():
        rows={}
        for p in Path('/proc').iterdir():
            if not p.name.isdigit():continue
            try:
                fields=(p/'stat').read_text().split(')')[-1].split()
                argv=(p/'cmdline').read_bytes().split(b'\0')
                rows[int(p.name)]=(int(fields[1]),fields[0],argv)
            except (FileNotFoundError,ProcessLookupError):pass
        return rows
    rows=processes();selected=[pid for pid,(_,state,argv) in rows.items()
        if state!='Z' and argv and Path(os.fsdecode(argv[0])).name=='ninja']
    assert len(selected)==1,'Only pause the one active Ninja compile, never tests or another build'
    pid=selected[0];os.kill(pid,signal.SIGSTOP)
    try:
        # Let the current compiler finish before starting another one. A stopped
        # Ninja may keep a completed zombie until resume; zombies use no memory.
        until=time.monotonic()+180
        while True:
            rows=processes();descendants={pid}
            for _ in range(len(rows)):
                added={p for p,(parent,state,_) in rows.items() if parent in descendants and state!='Z'}
                previous=len(descendants);descendants|=added
                if len(descendants)==previous:break
            if descendants=={pid}:break
            if time.monotonic()>until:raise TimeoutError('Compiler did not drain; resume without forcing it')
            time.sleep(.25)
        entries=json.loads((root/'build/compile_commands.json').read_text())
        for name in ('parallel_console_host_tests.cpp','parallel_sync_call_tests.cpp'):
            found=[e for e in entries if Path(e['file']).name==name and '/parallel_wasm_worker_test.dir/' in e['command']]
            assert len(found)==1,'Exact worker fixture command required: '+name
            entry=found[0];original=shlex.split(entry['command']);command=[];skip=False
            for arg in original:
                if skip:skip=False;continue
                if arg in ('-o','-MF','-MT','-MQ'):skip=True;continue
                if arg in ('-c','-MD','-MMD','-MP','-fmodules-ts'):continue
                # The full Ninja build will generate the real module mapper.
                # This early non-module syntax probe must not read a mapper
                # which its prerequisite dyndep step has not produced yet.
                if arg.startswith(('-fmodule-mapper=','-fdeps-')):continue
                command.append(arg)
            command.append('-fsyntax-only')
            with (root/(f'syntax-{attempt}-'+name+'.log')).open('x') as log:
                result=subprocess.run(command,cwd=entry['directory'],stdout=log,stderr=subprocess.STDOUT,timeout=300)
            jobs.append(dict(file=name,exit_code=result.returncode))
            if result.returncode:break
    finally:os.kill(pid,signal.SIGCONT)
    value=dict(status='passed' if len(jobs)==2 and all(j['exit_code']==0 for j in jobs) else 'failed',
        jobs=jobs,compile_scheduler_pause_seconds=time.monotonic()-started,
        scope='Early non-module syntax/template checks only; no object replacement, runtime or full build pass')
    with (root/f'syntax-report-{attempt}.json').open('x') as f:json.dump(value,f,indent=2)
    print(json.dumps(value))
    return 0 if value['status']=='passed' else 1


def host(attempt):
    root=Path('/run/media/guagua/Windows/PureEOS-v1-hardening-20261022/local-cold-warm-retest-attempt-1')
    plan=json.loads((root/'input.json').read_text())
    detail=json.loads(subprocess.check_output(['sudo','-n','docker','inspect',NAME],text=True))[0]
    assert detail['Config']['Labels']['org.pureeos.task']==NAME and detail['Image']==plan['image']
    return subprocess.run(['sudo','-n','docker','exec','--user=1000:1000',NAME,'python3','-c',
        Path(__file__).read_text(),'--guest',str(attempt)],timeout=850).returncode


if __name__=='__main__':
    attempt=int(sys.argv[-1]);assert attempt>=2
    raise SystemExit(guest(attempt) if '--guest' in sys.argv else host(attempt))
