"""Freeze test-only repairs now; reuse an owned cache only after its full run closes."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tarfile
import time
import traceback

from v1_local_release_workflows import IMAGE, PROGRAMS, core_passed, read_progress, save, sha

PARENT = 'local-cold-warm-retest-attempt-1'
NAME = 'local-engine-profile-retest-attempt-1'
OVERLAY = tuple('unittests/'+n+'.cpp' for n in (
    'parallel_console_host_tests', 'parallel_sync_call_tests', 'parallel_context_free_tests',
    'parallel_integer_host_tests', 'parallel_float_host_tests', 'parallel_pure_host_tests',
    'parallel_secondary_wasm_tests', 'parallel_validation_tests', 'pureeos_genesis_tests')) + (
    'tests/ubuntu_cdt_runtime_test.py',)
RUNNERS = ('prepare_v1_engine_profile_retest.py', 'v1_engine_profile_retest_host.py',
    'v1_engine_profile_retest_guest.py', 'v1_local_release_workflows.py')
PROFILES = ('parallel_context_free_tests', 'parallel_integer_host_tests', 'parallel_float_host_tests',
    'parallel_pure_host_tests', 'parallel_secondary_wasm_tests', 'parallel_validation_tests')


def safe_relative(name):
    p = Path(name)
    return bool(p.parts) and not p.is_absolute() and '..' not in p.parts and '.' not in p.parts


def verify_tree(source, expected):
    assert expected and all(safe_relative(n) for n in expected), 'Unsafe or empty source manifest'
    actual = set()
    for path in source.rglob('*'):
        assert not path.is_symlink(), 'Frozen actual source must not contain symlinks'
        if path.is_file(): actual.add(str(path.relative_to(source)))
    assert actual == set(expected), 'Missing or extra source files'
    assert all(sha(source/n) == d for n, d in expected.items()), 'Source hash mismatch'


def require_absent(name):
    result = subprocess.run(['sudo', '-n', 'docker', 'container', 'inspect', name],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    if result.returncode == 0: raise RuntimeError('Parent/next container is still present: '+name)
    # A daemon/permission failure is NOT evidence that a container is absent.
    assert ('No such container:' in result.stderr or 'No such object:' in result.stderr), result.stderr


def profile_audit(overlay):
    import re
    result = {}
    for stem in PROFILES:
        text = (overlay/'unittests'/f'{stem}.cpp').read_text()
        assert not re.search(r'\bwasm_runtime\s*=\s*wasm_interface::vm_type::eos_vm\b', text)
        count = text.count('configure_parallel_test_runtime(')
        assert count >= (2 if stem == 'parallel_context_free_tests' else 28 if stem == 'parallel_validation_tests' else 1)
        result[stem] = count
    return dict(scope='Static routing check only, NOT runtime engine equivalence', calls=result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--parent', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    os.umask(0o077)
    parent = args.parent.resolve(strict=True); root = args.output.resolve()
    assert parent.name == PARENT and root.name == NAME and parent.parent == root.parent
    assert root.parent.name == 'PureEOS-v1-hardening-20261022'
    assert shutil.disk_usage(root.parent).free > 25*1024**3
    root.mkdir(exist_ok=False)
    source = Path(__file__).resolve().parents[1]
    overlay = root/'frozen-overlay'; runners = root/'frozen-runners'; runners.mkdir()
    for name in OVERLAY:
        dst = overlay/name; dst.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(source/name, dst)
    for name in RUNNERS: shutil.copyfile(source/'tests'/name, runners/name)
    overlay_hashes = {n: sha(overlay/n) for n in OVERLAY}
    runner_hashes = {n: sha(runners/n) for n in RUNNERS}
    parent_input = sha(parent/'input.json'); parent_identity = sha(parent/'source-identity-final.json')
    plan0 = read_progress(parent/'input.json'); assert plan0['image'] == IMAGE
    report = dict(status='waiting-for-parent', release_ready=False, no_cloud_purchase=True,
        scope=__doc__, started_utc=datetime.now(timezone.utc).isoformat(), overlay_sha256=overlay_hashes,
        runner_sha256=runner_hashes, parent_input_sha256=parent_input, parent_source_identity_sha256=parent_identity)
    save(root/'runner-identity.json', report)
    save(root/'static-profile-audit.json', profile_audit(overlay))
    for archive_name in ('repair-overlay.tar.gz', 'verified-runtime-overlay.tar.gz'):
        with tarfile.open(root/archive_name, 'x:gz') as stream:
            for name in OVERLAY: stream.add(overlay/name, arcname=name, recursive=False)
    with tarfile.open(root/'runner-sources.tar.gz', 'x:gz') as stream:
        for name in RUNNERS: stream.add(runners/name, arcname=name, recursive=False)
    def stop(_sig, _frame): raise InterruptedError('Preparation stopped; no implicit retry')
    signal.signal(signal.SIGTERM, stop); signal.signal(signal.SIGINT, stop)
    try:
        deadline = time.monotonic()+9*3600
        while True:
            report['updated_utc'] = datetime.now(timezone.utc).isoformat()
            save(root/'preparation.json', report)
            monitor = read_progress(parent/'monitor.json')
            if monitor.get('status') == 'finished-and-archived': break
            state = subprocess.check_output(['systemctl', '--user', 'show', 'pureeos-v1-'+PARENT+'.service',
                '-p', 'ActiveState', '--value'], text=True, timeout=15).strip()
            if state in ('failed', 'inactive'):
                monitor = read_progress(parent/'monitor.json')
                if monitor.get('status') != 'finished-and-archived':
                    raise RuntimeError('Parent ended without verified closed archive')
                break
            if time.monotonic() > deadline: raise TimeoutError('Parent wait exhausted')
            time.sleep(30)
        actual, proof = (read_progress(parent/n) for n in ('report.json', 'download-verified.json'))
        assert core_passed(actual, proof, monitor), 'Parent incomplete or failed; next candidate NOT started'
        assert sha(parent/'input.json') == parent_input and sha(parent/'source-identity-final.json') == parent_identity
        archive = parent/'candidate-evidence.tar.gz'
        assert archive.stat().st_size == proof['bytes'] and sha(archive) == proof['sha256']
        require_absent('pureeos-v1-'+PARENT); require_absent('pureeos-v1-'+NAME)
        report.update(status='verifying-owned-parent-cache', parent_archive_sha256=proof['sha256'])
        save(root/'preparation.json', report)
        identity = read_progress(parent/'source-identity-final.json')
        verify_tree(parent/'source', identity['files_sha256'])
        for name in PROGRAMS:
            assert sha(parent/'build/programs'/name/name) == actual['binaries_sha256'][f'programs/{name}/{name}']
        # Independent copies, never hardlinks: the old candidate stays immutable.
        for folder in ('source', 'build'):
            subprocess.run(['cp', '-a', '--reflink=auto', str(parent/folder), str(root/folder)], check=True, timeout=1800)
        expected = dict(identity['files_sha256'])
        for name, digest in overlay_hashes.items():
            assert name in expected
            shutil.copyfile(overlay/name, root/'source'/name); expected[name] = digest
        verify_tree(root/'source', expected)
        final_identity = dict(scope='Test-only engine-profile repair; same frozen production source and compiler',
            parent_identity_sha256=parent_identity, parent_archive_sha256=proof['sha256'], files_sha256=expected,
            relative_symlinks=0, changes=list(OVERLAY), toolchain=identity['toolchain'])
        save(root/'source-identity-final.json', final_identity)
        parent_programs = {n: actual['binaries_sha256'][f'programs/{n}/{n}'] for n in PROGRAMS}
        assert all(sha(root/'build/programs'/n/n) == d for n,d in parent_programs.items())
        assert all(sha(overlay/n) == d for n,d in overlay_hashes.items())
        assert all(sha(runners/n) == d for n,d in runner_hashes.items())
        for n in ('tmp',): (root/n).mkdir()
        plan = dict(status='frozen-test-repair-input-not-release', image=IMAGE,
            runner_sha256=runner_hashes['v1_engine_profile_retest_guest.py'], overlay_sha256=overlay_hashes,
            contracts_sha256=plan0['contracts_sha256'], sdk_report_sha256=plan0['sdk_report_sha256'],
            parent_programs_sha256=parent_programs, parent_core_archive_sha256=proof['sha256'],
            no_cloud_purchase=True, tps_cancelled=True, compilation_jobs=1, cache_reuse='Owned completed parent; independent copy; full tests rerun')
        save(root/'input.json', plan)
        report.update(status='prepared', finished_utc=datetime.now(timezone.utc).isoformat())
        save(root/'preparation.json', report)
        host = runners/'v1_engine_profile_retest_host.py'
        os.execv('/usr/bin/python3', ['/usr/bin/python3', str(host), '--root', str(root)])
    except (Exception, KeyboardInterrupt) as error:
        report.update(status='failed-or-not-started', error=str(error), traceback=traceback.format_exc(),
            finished_utc=datetime.now(timezone.utc).isoformat())
        save(root/'preparation.json', report)
        return 1


if __name__ == '__main__': raise SystemExit(main())
