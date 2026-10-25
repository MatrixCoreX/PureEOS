#!/usr/bin/env python3
"""Candidate-bound local P2P/recovery/30->40 workflow; no cloud or TPS calls."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import signal
import subprocess
import tarfile
import time
import traceback

NAME = 'pureeos-v1-local-release-workflows-attempt-10'
CORE_NAME = 'pureeos-v1-local-native-profile-retest-attempt-9'
IMAGE = 'sha256:5dd671bd834c70a74b076e4b0dc90e280958df1200d837d19e15df68fad4e83d'
PROGRAMS = ('nodeos', 'cleos', 'keosd', 'helios-util')
SUPPORT = ('v1_local_release_workflows.py', 'v1_local_cohort_release_test.py',
    'run_observer_admission_smoke.py', 'pureeos_large_multinode_local_test.py',
    'pureeos_multinode_local_test.py', 'pureeos_single_choice_local_test.py',
    'native_savanna_genesis_test.py', 'pureeos_committed_policy_observer.py',
    'pureeos_rotation_cohort_cloud_test.py', 'pureeos_multihost_cloud_test.py',
    'pureeos_historical_edge_cases_test.py')


def sha(path):
    with Path(path).open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()


def save(path, value):
    pending = path.with_suffix('.pending')
    pending.write_text(json.dumps(value, indent=2)); pending.replace(path)


def read_progress(path):
    # The older core host's final write predates the atomic monitor helper.
    # Retry only reading an incomplete JSON write, never a failed test.
    for attempt in range(3):
        try: return json.loads(path.read_text())
        except json.JSONDecodeError:
            if attempt == 2: raise
            time.sleep(0.2)


def public_report(path, root):
    forbidden = {'config', 'config-dir', 'wallet', 'wallets', 'data', 'state', '.ssh'}
    return (path.name == 'report.json' and path.is_file() and not path.is_symlink()
        and path.resolve().is_relative_to(root.resolve())
        and not forbidden.intersection(path.relative_to(root).parts))


def core_jobs():
    names = {'configure', 'build-node-controller', 'build-version-provenance', 'native-profile-preflight'}
    for n in range(1, 4):
        names.update(f'cold-warm-{engine}-round-{n}' for engine in ('eos-vm', 'eos-vm-jit'))
        names.add(f'immutable-input-integration-probe-{n}')
        names.add(f'resource-limits-round-{n}')
        names.update(f'{part}-round-{n}' for part in ('immutable-input', 'access-hints', 'worker-signals',
            'controller', 'worker', 'controller-interpreter', 'worker-interpreter', 'blacklist-and-history',
            'full-native-matrix', 'cli', 'sdk'))
    names.update(f'leaf-overlap-stability-{n}' for n in range(1, 11))
    return names


def core_passed(report, proof, monitor):
    jobs = report.get('jobs', [])
    return (report.get('status') == 'passed' and proof.get('status') == 'passed'
        and proof.get('candidate_test_status') == 'passed'
        and monitor.get('status') == 'finished-and-archived' and monitor.get('candidate_status') == 'passed'
        and len(jobs) == len(core_jobs()) and {j.get('name') for j in jobs} == core_jobs()
        and all(j.get('exit_code') == 0 for j in jobs) and len(report.get('binaries_sha256', {})) == 11
        and report.get('build_version_provenance', {}).get('verified') is True
        and re.fullmatch('[0-9a-f]{64}', report.get('source_identity_sha256', '')) is not None
        and report['build_version_provenance'].get('source_manifest_sha256') == report['source_identity_sha256']
        and report['build_version_provenance'].get('client') == 'v2.0.0'
        and report['build_version_provenance'].get('full') == 'v2.0.0-src-'+report['source_identity_sha256']
        and report.get('runtime_dependencies_verified',0)>0
        and report.get('runtime_dependencies_unchanged') is True
        and report.get('full_cdt_fixture_preflight',{}).get('status')=='passed'
        and report['full_cdt_fixture_preflight'].get('exit_code')==0)


def recovery_passed(report):
    from pureeos_historical_edge_cases_test import verify_case_set
    rounds = report.get('rounds', [])
    return (report.get('status') == 'passed' and report.get('parallel_workers') == 4
        and [r.get('round') for r in rounds] == [1, 2, 3]
        and all(r.get('parallel_workers') == 4 and verify_case_set(r) for r in rounds))


def guest():
    root, inputs = Path('/work'), Path('/inputs')
    plan = json.loads((root/'input.json').read_text())
    frozen = plan['inputs_sha256']
    report = dict(status='running', release_ready=False, scope=__doc__, jobs=[],
        core_archive_sha256=plan['core_archive_sha256'], started_utc=datetime.now(timezone.utc).isoformat())
    active = None
    def interrupted(_sig, _frame):
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        raise InterruptedError('Stop only this isolated workflow')
    signal.signal(signal.SIGTERM, interrupted)
    def run(name, command, timeout):
        nonlocal active
        started = datetime.now(timezone.utc).isoformat()
        with (root/(name+'.log')).open('x') as stream:
            active = subprocess.Popen(list(map(str, command)), stdout=stream, stderr=subprocess.STDOUT)
            try: code = active.wait(timeout=timeout)
            except BaseException:
                active.terminate()
                try: active.wait(timeout=90)
                except subprocess.TimeoutExpired: active.kill(); active.wait(timeout=15)
                raise
            finally: active = None
        report['jobs'].append(dict(name=name, exit_code=code, started_utc=started,
            finished_utc=datetime.now(timezone.utc).isoformat()))
        save(root/'report.json', report)
        if code: raise RuntimeError(name+' failed; preserve and do not retry until green')
    try:
        save(root/'report.json', report)
        assert all(sha(inputs/p) == d for p, d in frozen.items()), 'Frozen workflow input changed'
        assert all(sha(Path('/contracts')/p) == d for p, d in plan['contracts_sha256'].items())
        for workers in (0, 4):
            output = root/f'observer-{workers}'
            run(f'observer-full-{workers}', ['python3', inputs/'tests/run_observer_admission_smoke.py',
                '--programs', inputs/'programs', '--contracts', '/contracts', '--output', output,
                '--workers', workers, '--workflow', 'full'], 3600)
            outer = json.loads((output/'report.json').read_text())
            detail_path = Path(outer['report']).resolve(strict=True)
            assert detail_path.is_relative_to(output.resolve())
            detail = json.loads(detail_path.read_text())
            assert (outer['status'] == 'passed' and detail['status'] == 'passed'
                and detail.get('processes_stopped') is True and not detail.get('shutdown_errors')
                and detail.get('parallel_workers') == workers and detail.get('final_synchronization')
                and set(detail.get('processes', {})) == {'eosio', 'nodea', 'nodeb', 'nodec'}
                and {len(e['schedule']['producers']) for e in detail.get('committee_events', [])} == {1, 2, 3, 4})
        # A short build alias is intentional: keosd's Unix socket has a strict
        # pathname limit. No original/manual build tree or wallet is used.
        build = root/'recovery-build'; (build/'programs').mkdir(parents=True)
        for name in PROGRAMS:
            p = build/'programs'/name; p.mkdir(); (p/name).symlink_to(inputs/'programs'/(name+'.elf'))
        run('historical-recovery-3-rounds', ['python3', inputs/'tests/pureeos_historical_edge_cases_test.py',
            '--build-dir', build, '--contracts-dir', '/contracts', '--report', root/'recovery.json',
            '--rounds', '3', '--parallel-workers', '4', '--fail-fast'], 5400)
        assert recovery_passed(json.loads((root/'recovery.json').read_text()))
        run('real-clock-30-40-cohort', ['python3', inputs/'tests/v1_local_cohort_release_test.py',
            '--programs', inputs/'programs', '--contracts', '/contracts', '--output', root/'cohort'], 18*3600)
        from v1_local_cohort_release_test import complete
        outer = json.loads((root/'cohort/report.json').read_text())
        detail_path = Path(outer['report']).resolve(strict=True)
        assert detail_path.is_relative_to((root/'cohort').resolve())
        assert outer['status'] == 'passed' and complete(json.loads(detail_path.read_text()))
        assert all(sha(inputs/p) == d for p, d in frozen.items()), 'Workflow inputs changed during testing'
        assert all(sha(Path('/contracts')/p) == d for p, d in plan['contracts_sha256'].items())
        report['status'] = 'passed'
    except (Exception, KeyboardInterrupt) as error:
        report.update(status='failed', error=str(error), traceback=traceback.format_exc())
    report['finished_utc'] = datetime.now(timezone.utc).isoformat(); save(root/'report.json', report)
    return 0 if report['status'] == 'passed' else 1


def owned_container(name, required_image):
    result = subprocess.run(['sudo', '-n', 'docker', 'container', 'inspect', name], text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    if result.returncode:
        assert re.search(r'No such (?:container|object):\s*'+re.escape(name)+r'(?:\s|$)',
            result.stderr, re.IGNORECASE), 'Container absence unproved: '+result.stderr
        return False
    detail = json.loads(result.stdout)[0]
    assert detail['Config']['Labels'].get('org.pureeos.task') == name and detail['Image'] == required_image
    return True


def host(args):
    os.umask(0o077)
    root, core = args.output.resolve(), args.core.resolve(strict=True)
    assert root.name == 'local-release-workflows-attempt-10' and root.parent == core.parent
    assert core.name == 'local-native-profile-retest-attempt-9'
    assert shutil.disk_usage(root.parent).free > 25*1024**3
    root.mkdir(exist_ok=False)
    inputs = root/'inputs'; (inputs/'tests').mkdir(parents=True); (inputs/'programs').mkdir()
    for name in SUPPORT: shutil.copyfile(Path(__file__).with_name(name), inputs/'tests'/name)
    frozen = {str(p.relative_to(inputs)): sha(p) for p in (inputs/'tests').iterdir()}
    initial_core_input = sha(core/'input.json') if (core/'input.json').is_file() else None
    initial_core_runner = sha(core/'runner-identity.json')
    report = dict(status='waiting-for-core', release_ready=False, scope=__doc__, no_cloud_purchase=True,
        started_utc=datetime.now(timezone.utc).isoformat(), workflow_sha256=frozen,
        core_input_sha256=initial_core_input, core_runner_identity_sha256=initial_core_runner)
    def interrupted(_sig, _frame): raise InterruptedError('Workflow host service stop requested')
    signal.signal(signal.SIGTERM, interrupted); signal.signal(signal.SIGINT, interrupted)
    process = None
    try:
        deadline = time.monotonic()+9*3600
        while True:
            if time.monotonic() > deadline: raise TimeoutError('Bounded wait for core expired')
            report['updated_utc'] = datetime.now(timezone.utc).isoformat(); save(root/'monitor.json', report)
            path = core/'monitor.json'
            monitor = read_progress(path) if path.is_file() else {}
            if monitor.get('status') == 'finished-and-archived': break
            unit = subprocess.check_output(['systemctl', '--user', 'show', CORE_NAME+'.service',
                '-p', 'ActiveState', '--value'], text=True, timeout=15).strip()
            if unit in ('inactive', 'failed'):
                # Re-read once after unit state: a just-completed final write is
                # valid, but an archive failure must not wait silently for hours.
                monitor = read_progress(path) if path.is_file() else {}
                if monitor.get('status') == 'finished-and-archived': break
                raise RuntimeError('Core service ended without a closed archive; downstream NOT started')
            time.sleep(30)
        actual, proof = (json.loads((core/p).read_text()) for p in ('report.json', 'download-verified.json'))
        assert core_passed(actual, proof, monitor), 'Core failed or incomplete: downstream workflows NOT started'
        assert sha(core/'runner-identity.json') == initial_core_runner
        assert initial_core_input is None or sha(core/'input.json') == initial_core_input
        report['core_input_sha256'] = sha(core/'input.json')
        archive = core/'candidate-evidence.tar.gz'
        assert proof['bytes'] == archive.stat().st_size and sha(archive) == proof['sha256']
        assert not owned_container(CORE_NAME, IMAGE), 'Core container must have stopped before P2P tests'
        assert not owned_container(NAME, IMAGE), 'Never replace an existing workflow container'
        # Close the earlier archiver's explicit scope gap, without changing it
        # mid-run or discarding any original failure/attempt.
        extra = root/'core-evidence-extra'; extra.mkdir()
        for name in ('verified-runtime-overlay.tar.gz', 'runner-sources.tar.gz', 'runner-identity.json',
                'source-identity-final.json', 'input.json', 'report.json', 'download-verified.json'):
            shutil.copyfile(core/name, extra/name)
        for group in ('cli', 'sdk'):
            for n in range(1, 4):
                shutil.copyfile(core/f'{group}-{n}/report.json', extra/f'{group}-{n}-report.json')
        adjustment = core/'build-memory-adjustment.json'
        if adjustment.is_file(): shutil.copyfile(adjustment, extra/adjustment.name)
        for name in PROGRAMS:
            path = core/'build/programs'/name/name
            digest = actual['binaries_sha256'][f'programs/{name}/{name}']
            assert sha(path) == digest
            destination = inputs/'programs'/(name+'.elf')
            shutil.copyfile(path, destination); destination.chmod(0o700)
            assert sha(destination) == digest; frozen['programs/'+destination.name] = digest
        contracts = core.parent/'contracts-safe-eviction-attempt-2/contracts'
        plan = dict(image=IMAGE, core_archive_sha256=proof['sha256'], inputs_sha256=frozen,
            core_report_sha256=sha(core/'report.json'), contracts_sha256=json.loads((core/'input.json').read_text())['contracts_sha256'])
        save(root/'input.json', plan); (root/'tmp').mkdir()
        command = ['sudo', '-n', 'docker', 'run', '--rm', '--name='+NAME,
            '--label=org.pureeos.task='+NAME, '--pull=never', '--network=none', '--read-only', '--log-driver=none',
            '--user=1000:1000', '--cpus=6', '--memory=7168m', '--memory-swap=8192m', '--pids-limit=2048',
            '--tmpfs', '/tmp:rw,size=128m,uid=1000,gid=1000', '--env', 'TMPDIR=/work/tmp',
            '--env', 'PYTHONDONTWRITEBYTECODE=1', '--mount', f'type=bind,src={root},dst=/work',
            '--mount', f'type=bind,src={inputs},dst=/inputs,readonly',
            '--mount', f'type=bind,src={contracts},dst=/contracts,readonly', '--workdir=/work', IMAGE,
            'python3', '/inputs/tests/v1_local_release_workflows.py', '--guest']
        report.update(status='running', core_archive_sha256=proof['sha256']); save(root/'monitor.json', report)
        with (root/'container-driver.log').open('x') as stream:
            process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
            deadline = time.monotonic()+22*3600
            while process.poll() is None:
                if time.monotonic() > deadline: raise TimeoutError('Workflow runtime budget exhausted')
                path = root/'report.json'
                detail = read_progress(path) if path.is_file() else {}
                report.update(updated_utc=datetime.now(timezone.utc).isoformat(),
                    candidate_status=detail.get('status'), jobs=detail.get('jobs', []))
                save(root/'monitor.json', report); time.sleep(30)
        report['container_exit_code'] = process.returncode
        detail = json.loads((root/'report.json').read_text())
        assert process.returncode == 0 and detail['status'] == 'passed', 'Local workflow failed; inspect preserved detail'
        report['status'] = 'passed'
    except (Exception, KeyboardInterrupt) as error:
        report.update(status='failed-or-not-started', error=str(error), traceback=traceback.format_exc())
    finally:
        if owned_container(NAME, IMAGE):
            subprocess.run(['sudo', '-n', 'docker', 'stop', '--time=90', NAME], check=True, timeout=120)
        if process is not None:
            try: process.wait(timeout=30)
            except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=15)
    # No recursive directory add: config.ini, wallet files, signing keys and
    # state databases must never enter the public evidence archive.
    public = {p for p in root.iterdir() if p.is_file() and p.suffix in ('.json', '.log')}
    public.update(p for p in root.rglob('report.json') if public_report(p, root))
    public.update(inputs/'tests'/name for name in SUPPORT)
    extra = root/'core-evidence-extra'
    if extra.is_dir(): public.update(p for p in extra.iterdir() if p.is_file() and not p.is_symlink())
    report['inputs_unchanged'] = all(sha(inputs/p) == d for p, d in frozen.items())
    if not report['inputs_unchanged']: report.update(status='failed', error='Frozen workflow inputs changed')
    report['finished_utc'] = datetime.now(timezone.utc).isoformat(); save(root/'monitor.json', report)
    public.add(root/'monitor.json')
    manifest = {str(p.relative_to(root)): sha(p) for p in sorted(public)}
    save(root/'public-manifest.json', manifest); public.add(root/'public-manifest.json')
    archive = root/'public-evidence.tar.gz'
    with tarfile.open(archive, 'x:gz') as stream:
        for p in sorted(public): stream.add(p, arcname=str(p.relative_to(root)), recursive=False)
    with tarfile.open(archive) as stream:
        for name, digest in manifest.items():
            member = stream.getmember(name); assert member.isfile()
            assert hashlib.file_digest(stream.extractfile(member), 'sha256').hexdigest() == digest
    save(root/'archive-verified.json', dict(status='passed', test_status=report['status'], release_ready=False,
        bytes=archive.stat().st_size, sha256=sha(archive), local=str(archive)))
    return 0 if report['status'] == 'passed' else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--guest', action='store_true')
    parser.add_argument('--core', type=Path); parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.guest: return guest()
    if args.core is None or args.output is None: parser.error('--core and --output required for local host runner')
    return host(args)


if __name__ == '__main__': raise SystemExit(main())
