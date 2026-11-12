"""Local same-candidate CLI and modified-SDK WASM integration; no cloud/TPS work."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import time
import traceback

from v1_candidate_deployment_test import check_candidate


def sha(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def main():
    os.umask(0o077)
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    args.output.mkdir(exist_ok=False)
    source=Path(__file__).resolve().parents[1];base=args.output.parent
    sdk=base/'wasm-attempt-6';report=dict(status='waiting-for-passed-candidate',scope=__doc__,
        release_ready=False,jobs=[],driver_sha256=sha(Path(__file__)))
    def save():report['updated_utc']=datetime.now(timezone.utc).isoformat();(args.output/'report.json').write_text(json.dumps(report,indent=2))
    try:
        save();until=time.monotonic()+7200
        while not (args.candidate/'download-verified.json').exists():
            assert time.monotonic()<until,'Bounded local wait expired';time.sleep(30)
        proof=json.loads((args.candidate/'download-verified.json').read_text())
        core=json.loads((args.candidate/'report.json').read_text());check_candidate(proof,core)
        assert sha(proof['local'])==proof['sha256']
        compiled=json.loads((sdk/'report.json').read_text());assert compiled['status']=='passed'
        # Only reuse unchanged modified SDK inputs/products, not the old node's
        # test conclusion. Execute the new candidate's node, cleos AND exporter.
        relevant={p:d for p,d in compiled['inputs_sha256'].items() if str(source) in p or '/cdt-v1-hardening/' in p}
        assert relevant and all(sha(p)==d for p,d in relevant.items())
        assert all(sha(p)==d for p,d in compiled['artifacts_sha256'].items())
        programs={}
        with tarfile.open(proof['local']) as archive:
            for name,member_name in (('nodeos','programs/nodeos/nodeos'),('cleos','programs/cleos/cleos'),
                                     ('exporter','unittests/parallel_wasm_worker_test'),
                                     ('controller','unittests/parallel_validation_test')):
                member=archive.getmember('build/'+member_name);assert member.isfile()
                path=args.output/(name+'.elf');data=archive.extractfile(member).read()
                assert hashlib.sha256(data).hexdigest()==core['binaries_sha256'][member_name]
                path.write_bytes(data);path.chmod(0o700);programs[name]=path
        support=[source/'tests'/n for n in ('ubuntu_cdt_runtime_test.py','parallel_runtime_cli_test.py',
                 'parallel_execution_test.py','parallel_release_benchmark.py','parallel_loopback_wave.py')]
        frozen={str(p):sha(p) for p in [*programs.values(),*support]}
        report.update(status='running',core_archive_sha256=proof['sha256'],inputs_sha256=frozen,
                      sdk_artifacts_sha256=compiled['artifacts_sha256'],sdk_source_sha256=relevant);save()
        for n in range(1,4):
            commands=[('cli',[sys.executable,source/'tests/parallel_runtime_cli_test.py','--nodeos',programs['nodeos'],
                              '--work-dir',args.output/f'cli-{n}']),
                ('sdk',[sys.executable,source/'tests/ubuntu_cdt_runtime_test.py','--nodeos',programs['nodeos'],
                        '--cleos',programs['cleos'],'--fixture-exporter',programs['controller'],
                        '--complex-wasm',sdk/'complexbench/complexbench.wasm','--cdt-wasm',sdk/'cdtcompat/cdtcompat.wasm',
                        '--sdk-hardening','--work-dir',args.output/f'sdk-{n}'])]
            for kind,command in commands:
                with (args.output/f'{kind}-{n}.log').open('x') as log:
                    result=subprocess.run(list(map(str,command)),stdout=log,stderr=subprocess.STDOUT,timeout=600)
                actual=json.loads((args.output/f'{kind}-{n}/report.json').read_text())
                report['jobs'].append(dict(name=f'{kind}-{n}',exit_code=result.returncode,status=actual['status']));save()
                assert result.returncode==0 and actual['status']=='passed',kind+' failed; preserve evidence'
                if kind=='sdk':
                    assert [r['workers'] for r in actual['cases']]==[0,4] and all(r['passed'] for r in actual['cases'])
        # Do not compete with the real SDK node processes. After all three
        # stopped, also cover the explicit matching interpreter core profile.
        temporary=args.output/'native-tmp';temporary.mkdir(mode=0o700)
        for n in range(1,4):
            for kind,program in (('controller-interpreter',programs['controller']),('worker-interpreter',programs['exporter'])):
                with (args.output/f'{kind}-{n}.log').open('x') as log:
                    result=subprocess.run([str(program),'--run_test=!*/export_*','--report_level=detailed',
                        '--color_output=no','--','--eos-vm'],stdout=log,stderr=subprocess.STDOUT,timeout=1800,
                        env={**os.environ,'TMPDIR':str(temporary)})
                report['jobs'].append(dict(name=f'{kind}-{n}',exit_code=result.returncode));save()
                assert result.returncode==0,kind+' failed; retain actual failure'
        assert all(sha(p)==d for p,d in frozen.items())
        assert all(sha(p)==d for p,d in compiled['artifacts_sha256'].items())
        report['status']='passed'
    except Exception as error:report.update(status='failed-or-not-started',error=str(error),traceback=traceback.format_exc())
    save();print(json.dumps({k:report[k] for k in ('status','jobs')}))
    return 0 if report['status']=='passed' else 1


if __name__=='__main__':raise SystemExit(main())
