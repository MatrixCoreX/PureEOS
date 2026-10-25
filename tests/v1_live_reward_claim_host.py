#!/usr/bin/env python3
"""Bounded local real-day reward task, only after the exact core candidate passed."""
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

from v1_local_release_workflows import CORE_NAME, IMAGE, PROGRAMS, core_passed, owned_container, public_report, read_progress, save, sha

NAME = 'pureeos-v1-live-reward-claims-attempt-8'
SUPPORT = ('v1_live_reward_claim_host.py', 'v1_live_reward_claim_test.py', 'v1_local_release_workflows.py',
    'pureeos_large_multinode_local_test.py', 'pureeos_multinode_local_test.py',
    'pureeos_single_choice_local_test.py', 'native_savanna_genesis_test.py', 'pureeos_committed_policy_observer.py')


def guest():
    from v1_live_reward_claim_test import complete,fault_preflight_complete
    root = Path('/work'); inputs = Path('/inputs'); plan = read_progress(root/'input.json')
    preflight=plan.get('fault_preflight_only',False)
    report = dict(status='running', release_ready=False, real_time_24h=not preflight,
        fault_preflight_only=preflight,scope=__doc__,
        started_utc=datetime.now(timezone.utc).isoformat(), core_archive_sha256=plan['core_archive_sha256'])
    def stop(_sig, _frame): raise InterruptedError('Private real-day task stopped')
    signal.signal(signal.SIGTERM, stop)
    child = None
    try:
        save(root/'report.json', report)
        assert all(sha(inputs/p) == d for p, d in plan['inputs_sha256'].items())
        assert all(sha(Path('/contracts')/p) == d for p, d in plan['contracts_sha256'].items())
        with (root/'real-day.log').open('x') as stream:
            child = subprocess.Popen(['python3', '/inputs/tests/v1_live_reward_claim_test.py',
                '--programs', '/inputs/programs', '--contracts', '/contracts', '--output', '/work/chain',
                *(['--fault-preflight-only'] if preflight else [])],
                stdout=stream, stderr=subprocess.STDOUT)
            code = child.wait(timeout=32*3600)
        outer = read_progress(root/'chain/report.json')
        detail_path = Path(outer['report']).resolve(strict=True)
        assert detail_path.is_relative_to((root/'chain').resolve())
        detail = read_progress(detail_path)
        assert code == 0 and outer['status'] == 'passed', 'Live fixture failed: '+str(detail.get('error',outer.get('error','unknown')))
        assert (fault_preflight_complete(detail) if preflight else complete(detail)), 'Live fixture evidence incomplete'
        assert all(sha(inputs/p) == d for p, d in plan['inputs_sha256'].items())
        assert all(sha(Path('/contracts')/p) == d for p, d in plan['contracts_sha256'].items())
        report.update(status='passed',reward_claims=detail['reward_claims'],elapsed=detail.get('latest_live_wait'),
            signer_partition=detail['signer_partition'],fault_preflight=detail.get('fault_preflight'))
    except (Exception, KeyboardInterrupt) as error:
        report.update(status='failed', error=str(error), traceback=traceback.format_exc())
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            try: child.wait(timeout=90)
            except subprocess.TimeoutExpired: child.kill(); child.wait(timeout=15)
    report['finished_utc'] = datetime.now(timezone.utc).isoformat(); save(root/'report.json', report)
    return 0 if report['status'] == 'passed' else 1


def host(args):
    os.umask(0o077)
    root, core = args.output.resolve(), args.core.resolve(strict=True)
    assert root.name == 'live-reward-claims-attempt-8' and root.parent == core.parent
    assert core.name == 'local-native-profile-retest-attempt-7'
    assert shutil.disk_usage(root.parent).free > 15*1024**3
    root.mkdir(exist_ok=False); inputs = root/'inputs'; (inputs/'tests').mkdir(parents=True); (inputs/'programs').mkdir()
    for name in SUPPORT: shutil.copyfile(Path(__file__).with_name(name), inputs/'tests'/name)
    frozen = {str(p.relative_to(inputs)): sha(p) for p in (inputs/'tests').iterdir()}
    pinned_core_input = sha(core/'input.json') if (core/'input.json').is_file() else None
    pinned_core_runner = sha(core/'runner-identity.json')
    report = dict(status='waiting-for-core', scope=__doc__, release_ready=False, real_time_24h=True,
        no_cloud_purchase=True, started_utc=datetime.now(timezone.utc).isoformat(), workflow_sha256=frozen,
        core_input_sha256=pinned_core_input, core_runner_identity_sha256=pinned_core_runner)
    def stop(_sig, _frame): raise InterruptedError('Private real-day host service stopped')
    signal.signal(signal.SIGTERM, stop); signal.signal(signal.SIGINT, stop)
    process = None
    try:
        deadline = time.monotonic()+9*3600
        while True:
            report['updated_utc'] = datetime.now(timezone.utc).isoformat(); save(root/'monitor.json', report)
            monitor = read_progress(core/'monitor.json') if (core/'monitor.json').is_file() else {}
            if monitor.get('status') == 'finished-and-archived': break
            if time.monotonic() > deadline: raise TimeoutError('Bounded core wait expired')
            state = subprocess.check_output(['systemctl', '--user', 'show',
                CORE_NAME+'.service', '-p', 'ActiveState', '--value'], text=True, timeout=15).strip()
            if state in ('inactive', 'failed'):
                monitor = read_progress(core/'monitor.json') if (core/'monitor.json').is_file() else {}
                if monitor.get('status') == 'finished-and-archived': break
                raise RuntimeError('Core stopped without a verified closed archive; real-day task NOT started')
            time.sleep(30)
        actual, proof = (read_progress(core/p) for p in ('report.json', 'download-verified.json'))
        assert core_passed(actual, proof, monitor), 'Core failed/incomplete: real-day task NOT started'
        assert sha(core/'runner-identity.json') == pinned_core_runner
        assert pinned_core_input is None or sha(core/'input.json') == pinned_core_input
        report['core_input_sha256'] = sha(core/'input.json')
        assert sha(core/'candidate-evidence.tar.gz') == proof['sha256']
        assert (core/'candidate-evidence.tar.gz').stat().st_size == proof['bytes']
        assert not owned_container(CORE_NAME, IMAGE)
        assert not owned_container(NAME, IMAGE)
        for name in PROGRAMS:
            p = core/'build/programs'/name/name; digest = actual['binaries_sha256'][f'programs/{name}/{name}']
            assert sha(p) == digest
            target = inputs/'programs'/(name+'.elf'); shutil.copyfile(p, target); target.chmod(0o700)
            assert sha(target) == digest; frozen['programs/'+target.name] = digest
        contracts = core.parent/'contracts-safe-eviction-attempt-2/contracts'
        plan = dict(image=IMAGE, core_archive_sha256=proof['sha256'], inputs_sha256=frozen,
            contracts_sha256=read_progress(core/'input.json')['contracts_sha256'])
        save(root/'input.json', plan); (root/'tmp').mkdir()
        command = ['sudo', '-n', 'docker', 'run', '--rm', '--name='+NAME,
            '--label=org.pureeos.task='+NAME, '--pull=never', '--network=none', '--read-only', '--log-driver=none',
            '--user=1000:1000', '--cpus=2', '--memory=1536m', '--memory-swap=2048m', '--pids-limit=512',
            '--tmpfs', '/tmp:rw,size=64m,uid=1000,gid=1000', '--env', 'TMPDIR=/work/tmp', '--env', 'PYTHONDONTWRITEBYTECODE=1',
            '--mount', f'type=bind,src={root},dst=/work', '--mount', f'type=bind,src={inputs},dst=/inputs,readonly',
            '--mount', f'type=bind,src={contracts},dst=/contracts,readonly', '--workdir=/work', IMAGE,
            'python3', '/inputs/tests/v1_live_reward_claim_host.py', '--guest']
        report.update(status='running', core_archive_sha256=proof['sha256']); save(root/'monitor.json', report)
        with (root/'container-driver.log').open('x') as stream:
            process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
            deadline = time.monotonic()+33*3600
            while process.poll() is None:
                if time.monotonic() > deadline: raise TimeoutError('Private real-day container deadline')
                if (root/'report.json').is_file(): report['candidate_status'] = read_progress(root/'report.json').get('status')
                progress = list((root/'chain/build/local-tests').glob('integer-v2-*/progress.json'))
                assert len(progress) <= 1, 'Unexpected chain selection'
                if progress:
                    value = read_progress(progress[0]); report['latest_live_wait'] = value.get('latest_live_wait')
                    report['checks_completed'] = len(value.get('checks', [])); report['fault'] = value.get('fault', {})
                report['updated_utc'] = datetime.now(timezone.utc).isoformat(); save(root/'monitor.json', report)
                time.sleep(30)
        assert process.returncode == 0 and read_progress(root/'report.json')['status'] == 'passed'
        report['status'] = 'passed'
    except (Exception, KeyboardInterrupt) as error:
        report.update(status='failed-or-not-started', error=str(error), traceback=traceback.format_exc())
    finally:
        if owned_container(NAME, IMAGE):
            subprocess.run(['sudo', '-n', 'docker', 'stop', '--time=120', NAME], check=True, timeout=150)
        if process is not None:
            try: process.wait(timeout=30)
            except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=15)
    report['inputs_unchanged'] = all(sha(inputs/p) == d for p, d in frozen.items())
    if not report['inputs_unchanged']: report.update(status='failed', error='Frozen live claim inputs changed')
    report['finished_utc'] = datetime.now(timezone.utc).isoformat(); save(root/'monitor.json', report)
    files = {p for p in root.iterdir() if p.is_file() and p.suffix in ('.json', '.log')}
    files.update(p for p in root.rglob('report.json') if public_report(p, root))
    files.update(inputs/'tests'/n for n in SUPPORT)
    save(root/'public-manifest.json', {str(p.relative_to(root)): sha(p) for p in sorted(files)})
    files.add(root/'public-manifest.json'); archive = root/'public-evidence.tar.gz'
    with tarfile.open(archive, 'x:gz') as stream:
        for p in sorted(files): stream.add(p, arcname=str(p.relative_to(root)), recursive=False)
    manifest = read_progress(root/'public-manifest.json')
    with tarfile.open(archive) as stream:
        for name, digest in manifest.items():
            member = stream.getmember(name); assert member.isfile()
            assert hashlib.file_digest(stream.extractfile(member), 'sha256').hexdigest() == digest
    save(root/'archive.json', dict(status='archived-not-release', test_status=report['status'],
        sha256=sha(archive), bytes=archive.stat().st_size, release_ready=False))
    return 0 if report['status'] == 'passed' else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--guest', action='store_true'); parser.add_argument('--core', type=Path); parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.guest: return guest()
    if args.core is None or args.output is None: parser.error('--core and --output required')
    return host(args)


if __name__ == '__main__': raise SystemExit(main())
