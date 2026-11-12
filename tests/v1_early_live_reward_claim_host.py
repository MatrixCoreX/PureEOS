"""Overlap real-day claims with full regression; full core gate still required at acceptance."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tarfile
import time
import traceback

from v1_fast_regression import CONTROLLER, INTERNAL_MATRIX, RESOURCE, WORKER, check_parent, native_cases
from v1_runtime_dependencies import FULL_CDT_CASES
from v1_live_reward_claim_host import SUPPORT as LIVE_SUPPORT
from v1_local_release_workflows import (CORE_NAME, IMAGE, PROGRAMS, core_passed,
    owned_container, public_report, read_progress, save, sha)

NAME = 'pureeos-v1-live-reward-claims-attempt-14'
SUPPORT = (*LIVE_SUPPORT, 'v1_early_live_reward_claim_host.py', 'v1_fast_regression.py', 'v1_runtime_dependencies.py')


def initial_report_ready(exists, unit_state):
    if exists:return True
    assert unit_state in ('active','activating'), 'Core stopped before creating its initial report'
    return False


def fixture_preflight_ready(core):
    preflight=core.get('full_cdt_fixture_preflight',{})
    return (core.get('runtime_dependencies_verified',0)>0 and preflight.get('status')=='passed'
        and preflight.get('exit_code')==0 and preflight.get('cases')==list(FULL_CDT_CASES)
        and core.get('build_version_provenance',{}).get('verified') is True)


def early_ready(core, smoke, smoke_input):
    identity = core.get('source_identity_sha256', '')
    check_parent(core, identity)
    assert fixture_preflight_ready(core), 'Restored runtime fixtures must pass the exact nine-case preflight'
    assert {'native-profile-preflight', 'full-native-matrix-round-1',
            'resource-limits-round-1', 'resource-limits-round-2', 'resource-limits-round-3'} <= {
                j['name'] for j in core['jobs']}, 'First complete business round required before early start'
    assert smoke.get('status') == 'smoke-passed-not-release' and smoke.get('inputs_unchanged') is True
    assert smoke.get('release_ready') is False and smoke.get('full_regression') is False
    assert smoke.get('source_identity_sha256') == smoke_input.get('source_identity_sha256') == identity
    assert smoke_input.get('binaries_sha256') == core['binaries_sha256']
    counts = {'resource-limits': len(RESOURCE), 'native-policy': len(native_cases()),
        'controller-internal-matrix': len(INTERNAL_MATRIX),
        **{'worker-' + e: len(WORKER) for e in ('eos-vm', 'eos-vm-jit')},
        **{'controller-' + e: len(CONTROLLER) for e in ('eos-vm', 'eos-vm-jit')}}
    expected = set(counts) | {'version-' + p for p in ('nodeos', 'cleos', 'keosd')} | {
        'parallel_access_hints_test', 'parallel_worker_signals_test', 'parallel_immutable_input_test'}
    jobs = smoke.get('jobs', [])
    assert len(jobs) == len(expected) and {j.get('name') for j in jobs} == expected
    assert all(j.get('exit_code') == 0 and (j['name'] not in counts or
        j.get('cases_passed') == counts[j['name']]) for j in jobs)
    assert smoke.get('boost_case_executions') == sum(counts.values())
    return True


def final_core_ready(actual, proof, monitor, identity, binaries):
    return (core_passed(actual, proof, monitor)
        and actual.get('source_identity_sha256') == identity
        and actual.get('binaries_sha256') == binaries)


def host(args):
    os.umask(0o077)
    root, core, smoke = args.output.absolute(), args.core.resolve(strict=True), args.smoke.resolve(strict=True)
    preflight=getattr(args,'fault_preflight_only',False)
    container_name='pureeos-v1-live-reward-fault-preflight-attempt-2' if preflight else NAME
    assert root.name == container_name.removeprefix('pureeos-v1-') and root.parent == core.parent == smoke.parent
    assert core.name == 'local-native-profile-retest-attempt-9'
    assert smoke.name == 'fast-regression-attempt-2'
    assert not root.exists() and not root.is_symlink()
    assert shutil.disk_usage(root.parent).free > 15 * 1024**3
    root.mkdir(); inputs = root / 'inputs'
    (inputs / 'tests').mkdir(parents=True); (inputs / 'programs').mkdir()
    for name in SUPPORT: shutil.copyfile(Path(__file__).with_name(name), inputs / 'tests' / name)
    frozen = {str(p.relative_to(inputs)): sha(p) for p in (inputs / 'tests').iterdir()}
    pinned = {n: sha(core / n) for n in ('input.json', 'runner-identity.json', 'source-identity-final.json')}
    report = dict(status='waiting-for-smoke', release_ready=False, real_time_24h=not preflight,
        fault_preflight_only=preflight,
        scope=__doc__, provisional_at_start=True, core_gate_status='pending', no_cloud_purchase=True,
        started_utc=datetime.now(timezone.utc).isoformat(), core_inputs_sha256=pinned)
    process = None
    def stop(_sig, _frame): raise InterruptedError('Stop only the early real-day task')
    signal.signal(signal.SIGTERM, stop); signal.signal(signal.SIGINT, stop)
    def current_core():
        assert all(sha(core / n) == digest for n, digest in pinned.items()), 'Pinned core inputs changed'
        actual = read_progress(core / 'report.json')
        assert actual.get('status') in ('running', 'passed'), 'Core failed: provisional claims invalidated'
        assert all(j.get('exit_code') == 0 for j in actual.get('jobs', [])), 'Core job failed'
        return actual
    def final_gate(actual):
        monitor = read_progress(core / 'monitor.json')
        if monitor.get('status') != 'finished-and-archived':
            state = subprocess.check_output(['systemctl', '--user', 'show', CORE_NAME + '.service',
                '-p', 'ActiveState', '--value'], text=True, timeout=15).strip()
            if state in ('inactive', 'failed'):
                monitor = read_progress(core / 'monitor.json')
                assert monitor.get('status') == 'finished-and-archived', 'Core stopped without closure'
            else: return False
        proof = read_progress(core / 'download-verified.json')
        assert final_core_ready(actual, proof, monitor, pinned['source-identity-final.json'],
                                plan['binaries_sha256']), 'Final full core gate failed'
        archive = core / 'candidate-evidence.tar.gz'
        assert archive.stat().st_size == proof['bytes'] and sha(archive) == proof['sha256']
        assert not owned_container(CORE_NAME, IMAGE), 'Core container not closed'
        report.update(core_gate_status='passed', core_archive_sha256=proof['sha256'])
        save(root / 'accepted-core-proof.json', dict(core_report_sha256=sha(core / 'report.json'),
            core_monitor_sha256=sha(core / 'monitor.json'), **proof))
        return True
    try:
        # A freshly launched core first validates 75k frozen source files.
        # Wait only for initial creation while its exact unit is active; a
        # later missing report remains an error, not an endless startup retry.
        startup_deadline=time.monotonic()+300
        while not initial_report_ready((core/'report.json').is_file(),
                subprocess.check_output(['systemctl','--user','show',CORE_NAME+'.service',
                    '-p','ActiveState','--value'],text=True,timeout=15).strip()):
            assert time.monotonic()<startup_deadline, 'Initial core report deadline expired'
            report['status']='waiting-for-initial-core-report';save(root/'monitor.json',report)
            time.sleep(2)
        deadline = time.monotonic() + 3600
        while True:
            actual = current_core()
            path = smoke / 'monitor.json'
            progress = read_progress(path) if path.is_file() else {}
            if (progress.get('status') == 'finished-smoke-not-release'
                    and (smoke / 'download-verified.json').is_file()
                    and fixture_preflight_ready(actual)): break
            if (smoke / 'report.json').is_file():
                assert read_progress(smoke / 'report.json').get('status') != 'failed', 'Smoke failed; no chain started'
            assert progress.get('status') not in ('failed', 'interrupted-or-failed'), 'Smoke failed'
            assert time.monotonic() < deadline, 'Bounded smoke wait expired'
            report['updated_utc'] = datetime.now(timezone.utc).isoformat(); save(root / 'monitor.json', report)
            time.sleep(5)
        actual = current_core()
        smoke_report, smoke_input, proof = (read_progress(smoke / n) for n in
                                           ('report.json', 'input.json', 'download-verified.json'))
        assert early_ready(actual, smoke_report, smoke_input)
        archive = smoke / 'smoke-evidence.tar.gz'
        assert proof.get('status') == 'passed' and proof.get('smoke_status') == 'smoke-passed-not-release'
        assert archive.stat().st_size == proof['bytes'] and sha(archive) == proof['sha256']
        assert smoke_input['contracts_sha256'] == read_progress(core / 'input.json')['contracts_sha256']
        assert not owned_container('pureeos-v1-' + smoke.name, IMAGE) and not owned_container(container_name, IMAGE)
        for name in PROGRAMS:
            source = core / 'build/programs' / name / name
            digest = actual['binaries_sha256'][f'programs/{name}/{name}']
            assert sha(source) == digest
            target = inputs / 'programs' / (name + '.elf')
            shutil.copyfile(source, target); target.chmod(0o700)
            assert sha(target) == digest; frozen['programs/' + target.name] = digest
        save(root / 'early-core-snapshot.json', actual)
        save(root / 'early-smoke-snapshot.json', smoke_report)
        contracts = core.parent / 'contracts-safe-eviction-attempt-2/contracts'
        plan = dict(image=IMAGE, core_archive_sha256=None, provisional_at_start=True,
            fault_preflight_only=preflight,
            source_identity_sha256=pinned['source-identity-final.json'],
            binaries_sha256=actual['binaries_sha256'], inputs_sha256=frozen,
            smoke_archive_sha256=proof['sha256'], contracts_sha256=smoke_input['contracts_sha256'])
        save(root / 'input.json', plan); (root / 'tmp').mkdir()
        command = ['sudo', '-n', 'docker', 'run', '--rm', '--name=' + container_name,
            '--label=org.pureeos.task=' + container_name, '--pull=never', '--network=none', '--read-only',
            '--log-driver=none', '--user=1000:1000', '--cpus=2', '--memory=1536m',
            '--memory-swap=2048m', '--pids-limit=512', '--tmpfs', '/tmp:rw,size=64m,uid=1000,gid=1000',
            '--env', 'TMPDIR=/work/tmp', '--env', 'PYTHONDONTWRITEBYTECODE=1',
            '--mount', f'type=bind,src={root},dst=/work',
            '--mount', f'type=bind,src={inputs},dst=/inputs,readonly',
            '--mount', f'type=bind,src={contracts},dst=/contracts,readonly', '--workdir=/work', IMAGE,
            'python3', '/inputs/tests/v1_live_reward_claim_host.py', '--guest']
        report.update(status='running-provisional', smoke_archive_sha256=proof['sha256'])
        save(root / 'monitor.json', report)
        with (root / 'container-driver.log').open('x') as stream:
            process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 33 * 3600
            while process.poll() is None:
                assert time.monotonic() < deadline, 'Real-day task deadline expired'
                actual = current_core()
                assert actual.get('binaries_sha256') == plan['binaries_sha256'], 'Core binaries changed'
                if report['core_gate_status'] != 'passed': final_gate(actual)
                progress = list((root / 'chain/build/local-tests').glob('integer-v2-*/progress.json'))
                assert len(progress) <= 1, 'Ambiguous live chain'
                if progress:
                    value = read_progress(progress[0])
                    report.update(latest_live_wait=value.get('latest_live_wait'),
                        checks_completed=len(value.get('checks', [])), fault=value.get('fault', {}))
                report['updated_utc'] = datetime.now(timezone.utc).isoformat(); save(root / 'monitor.json', report)
                time.sleep(15)
        inner=read_progress(root/'report.json')
        assert process.returncode == 0 and inner['status'] == 'passed', 'Live container failed: '+str(inner.get('error',process.returncode))
        if preflight:
            report.update(status='fault-preflight-passed-not-release',provisional_at_completion=True,
                note='Four-identity fault topology only, not a full-core or 24h acceptance')
        else:
            assert final_gate(current_core()), 'Recheck the full gate and archive at final acceptance'
            report.update(status='passed', provisional_at_completion=False)
    except (Exception, KeyboardInterrupt) as error:
        report.update(status='failed-or-not-started', error=str(error), traceback=traceback.format_exc())
    finally:
        if owned_container(container_name, IMAGE):
            subprocess.run(['sudo', '-n', 'docker', 'stop', '--time=120', container_name], check=True, timeout=150)
        if process is not None:
            try: process.wait(timeout=30)
            except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=15)
    report['inputs_unchanged'] = all(sha(inputs / p) == digest for p, digest in frozen.items())
    if not report['inputs_unchanged']: report.update(status='failed', error='Frozen inputs changed')
    report['finished_utc'] = datetime.now(timezone.utc).isoformat(); save(root / 'monitor.json', report)
    files = {p for p in root.iterdir() if p.is_file() and p.suffix in ('.json', '.log')}
    files.update(p for p in root.rglob('report.json') if public_report(p, root))
    files.update(inputs / 'tests' / n for n in SUPPORT)
    save(root / 'public-manifest.json', {str(p.relative_to(root)): sha(p) for p in sorted(files)})
    files.add(root / 'public-manifest.json'); archive = root / 'public-evidence.tar.gz'
    with tarfile.open(archive, 'x:gz') as stream:
        for p in sorted(files): stream.add(p, arcname=str(p.relative_to(root)), recursive=False)
    with tarfile.open(archive) as stream:
        for name, digest in read_progress(root / 'public-manifest.json').items():
            member = stream.getmember(name); assert member.isfile()
            with stream.extractfile(member) as content:
                assert hashlib.file_digest(content, 'sha256').hexdigest() == digest
    save(root / 'archive.json', dict(status='archived-not-release', test_status=report['status'],
        bytes=archive.stat().st_size, sha256=sha(archive), release_ready=False))
    return 0 if report['status'] in ('passed','fault-preflight-passed-not-release') else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('core', 'smoke', 'output'): parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--fault-preflight-only',action='store_true')
    return host(parser.parse_args())


if __name__ == '__main__': raise SystemExit(main())
