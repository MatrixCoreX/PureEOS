"""Relink frozen native core read-only with current real-WASM economic fixtures."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tarfile
import traceback


def sha(p):
    with Path(p).open('rb') as stream: return hashlib.file_digest(stream,'sha256').hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--portable',type=Path,required=True)
    parser.add_argument('--contracts',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--fixture-root',type=Path,help='Isolated source snapshot with tests/ wrapper and unittests/; native core stays read-only')
    parser.add_argument('--select', default='pureeos_blacklist_current_policy_tests:pureeos_reward_halving_tests/permanently_banned_producer_can_claim_old_unpaid_blocks_but_not_new_rewards')
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--test-timeout', type=int, default=1800)
    args=parser.parse_args();args.output.mkdir(exist_ok=False)
    assert 1 <= args.rounds <= 10 and args.test_timeout > 0
    temporary=args.output/'tmp';temporary.mkdir()
    source=Path(__file__).resolve().parents[1]
    fixture_source=(args.fixture_root or source).resolve()
    report=dict(status='running',scope='Real system WASM on frozen baseline native core plus new test cases; not new N1/N4 node or P2P/TPS',
        started_utc=datetime.now(timezone.utc).isoformat(),jobs=[],fixture_root=str(fixture_source))
    def save(): (args.output/'report.json').write_text(json.dumps(report,indent=2))
    def run(name,command,timeout):
        with (args.output/(name+'.log')).open('x') as log:
            result=subprocess.run(list(map(str,command)),cwd=args.portable,stdout=log,stderr=subprocess.STDOUT,
                timeout=timeout,env={**os.environ,'CCACHE_DISABLE':'1','TMPDIR':str(temporary)})
        report['jobs'].append(dict(name=name,exit_code=result.returncode));save()
        if result.returncode:raise RuntimeError(name+' failed; see preserved log')
    try:
        proof=json.loads((args.portable/'download-verified.json').read_text())
        archive=args.portable/'native-policy-cache.tar.gz'
        assert sha(archive)==proof['sha256']
        # A verified read-only cache can serve multiple isolated fixture builds.
        # Never erase/re-extract an existing cache or mutate its link objects.
        if not (args.portable/'cache').exists():
            with tarfile.open(archive) as stream:
                members=stream.getmembers()
                assert all(m.isfile() and not Path(m.name).is_absolute() and '..' not in Path(m.name).parts for m in members)
                stream.extractall(args.portable,members=members,filter='data')
        plan=json.loads((args.portable/'input.json').read_text())
        assert all(sha(args.portable/'cache'/p)==expected for p,expected in plan['inputs_sha256'].items())
        root=source.parent/'PureEOS'
        old_source='/opt/pureeos-unified-TBMyRV/clock-v3/source'
        old_build='/opt/pureeos-unified-TBMyRV/clock-v3/native-build'
        def rewrite(value):
            return value.replace(old_source,str(root)).replace(old_build,str(args.portable/'cache')).replace(plan['cache'],str(args.portable/'cache'))
        inputs=[fixture_source/'tests/native_policy_boundary_fixture.cpp',fixture_source/'unittests/pureeos_governance_tests.cpp',
            *fixture_source.joinpath('unittests').glob('pureeos_*.inc'),Path(__file__),
            *args.contracts.rglob('*.wasm'),*args.contracts.rglob('*.abi')]
        report['inputs_sha256']={str(p):sha(p) for p in inputs}
        raw=shlex.split(plan['compile']['command']);command=[];i=0
        obj=args.output/'policy-fixture.o'
        while i<len(raw):
            value=raw[i];i+=1
            if value in ('-MD','-MMD','-MP','-fmodules-ts') or value.startswith(('-fmodule-mapper=','-fdeps-format=')):continue
            if value in ('-MT','-MQ','-MF'):i+=1;continue
            if value in ('-o','-c'):
                i+=1;command += [value,str(obj if value=='-o' else fixture_source/'tests/native_policy_boundary_fixture.cpp')];continue
            if value.startswith('-DPUREEOS_') and '_CONTRACTS_DIR=' in value:
                value=value.split('=')[0]+'="'+str(args.contracts)+'"'
            command.append(rewrite(value))
        command[1:1]=['-I'+str(source/'libraries/chain/include'),'-I'+str(fixture_source/'unittests'),'-I'+str(source/'unittests')]
        run('compile-policy-fixture',command,600)
        raw=shlex.split(plan['link']);assert raw[:2]==[':','&&'] and raw[-2:]==['&&',':']
        command=[rewrite(v) for v in raw[2:-2] if not v.startswith('-Wl,--dependency-file=')]
        command=[str(obj) if v.endswith('native_savanna_test.dir/pureeos_governance_tests.cpp.o') else v for v in command]
        # Relative cache paths in the native link command resolve explicitly.
        command=[str(args.portable/'cache'/v) if Path(v).suffix in ('.o','.a') and not Path(v).is_absolute() else v for v in command]
        program=args.output/'native-policy-check.elf';command[command.index('-o')+1]=str(program)
        run('link-policy-fixture',command,180)
        report['program_sha256']=sha(program)
        report['selection']=args.select
        report['rounds']=args.rounds
        for number in range(1,args.rounds+1):
            run(f'boundary-round-{number}',[program,'--run_test='+args.select,'--report_level=detailed','--color_output=no','--log_level=test_suite'],args.test_timeout)
        assert all(sha(p)==expected for p,expected in report['inputs_sha256'].items())
        assert all(sha(args.portable/'cache'/p)==expected for p,expected in plan['inputs_sha256'].items())
        report.update(status='passed',test_scope='Only explicitly selected real contract scenarios; other suites unselected')
    except Exception as error:report.update(status='failed',error=str(error),traceback=traceback.format_exc())
    report['finished_utc']=datetime.now(timezone.utc).isoformat();save()
    print(json.dumps({k:v for k,v in report.items() if k!='inputs_sha256'},indent=2))
    return 0 if report['status']=='passed' else 1


if __name__=='__main__':raise SystemExit(main())
