"""Fresh Release/O3 build; frozen old code/cache and running tests stay read-only."""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import traceback

ROOT = Path(os.environ.get('PUREEOS_V1_BUILD_ROOT', '/opt/pureeos-v1-integrated-20261022'))
assert ROOT in tuple(Path('/opt/pureeos-v1-integrated-20261022'+suffix) for suffix in ('','-v2','-v3','-v4','-v5','-v6','-v7','-v8'))
BASE = Path('/opt/pureeos-unified-TBMyRV/clock-v3/source')


def sha(path):
    with Path(path).open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    os.umask(0o077)
    report = dict(status='running', scope='Fresh separate Release/O3 node and real controller tests, no TPS',
        started_utc=datetime.now(timezone.utc).isoformat(), jobs=[])
    def save(): (ROOT / 'report.json').write_text(json.dumps(report, indent=2))
    def run(name, command, timeout):
        remaining = int((datetime.fromisoformat(plan['deadline']) - datetime.now(timezone.utc)).total_seconds()) - 900
        assert remaining > 60, 'Original cloud deadline is too close; do not start another job'
        timeout = min(timeout, remaining)
        with (ROOT / (name + '.log')).open('x') as log:
            result = subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT,
                timeout=timeout, env={**os.environ, 'CCACHE_DISABLE': '1'})
        report['jobs'].append(dict(name=name, exit_code=result.returncode)); save()
        if result.returncode: raise RuntimeError(name + ' failed')
    try:
        assert not (ROOT / 'report.json').exists()
        plan = json.loads((ROOT / 'input.json').read_text())
        assert (datetime.fromisoformat(plan['deadline']) - datetime.now(timezone.utc)).total_seconds() > 3600
        assert shutil.disk_usage(ROOT).free > 20 * 1024**3
        for name, expected in plan['sha256'].items(): assert sha(ROOT / 'overlay' / name) == expected, name
        for name, expected in plan['contracts_sha256'].items(): assert sha(ROOT / 'contracts' / name) == expected, name
        # Baseline source equality is checked before overlay. Do not copy any
        # local worktree config-dir, keys, wallets or unrelated user files.
        for name, expected in plan['baseline_sha256'].items():
            assert sha(BASE / name) == expected, 'Frozen baseline differs: ' + name
        source, build = ROOT / 'source', ROOT / 'build'
        if plan.get('resume'):
            previous = Path(plan['resume']['root'])
            assert previous in tuple(Path('/opt/pureeos-v1-integrated-20261022'+s) for s in ('','-v2','-v3','-v4','-v5','-v6','-v7')) and previous != ROOT
            assert sha(previous / 'input.json') == plan['resume']['parent_input_sha256']
            assert sha(previous / 'candidate-evidence.tar.gz') == plan['resume']['parent_archive_sha256']
            parent_unit=plan['resume']['unit']
            assert parent_unit in tuple('pureeos-unified-TBMyRV-v1-integrated-build'+s for s in ('','-v2','-v3','-v4','-v5','-v6','-v7'))
            assert subprocess.check_output(['systemctl', 'show', parent_unit,
                '-p', 'ActiveState', '--value'], text=True).strip() in ('inactive','failed')
            cache=Path('/opt/pureeos-v1-integrated-20261022')
            source, build = cache / 'source', cache / 'build'
            old_plan = json.loads((previous / 'input.json').read_text())
            for name, expected in old_plan['sha256'].items(): assert sha(source / name) == expected, name
            report['reused_private_build_cache'] = str(build)
            report['parent_attempt'] = plan['resume']
        else:
            shutil.copytree(BASE, source, symlinks=True)
        for name in plan['sha256']:
            p = source / name; p.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / 'overlay' / name, p)
        fixture_root = Path(plan.get('compiled_contracts_candidate_root',str(ROOT))) / 'contracts'
        assert fixture_root in (ROOT/'contracts',Path('/opt/pureeos-v1-integrated-20261022-v5/contracts'))
        assert all(sha(fixture_root/name)==digest for name,digest in plan['contracts_sha256'].items())
        report['compiled_contracts_root'] = str(fixture_root)
        report['source_overlay_sha256'] = plan['sha256']; save()
        run('configure', ['cmake', '-S', source, '-B', build, '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG', '-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG',
            '-DENABLE_OC=OFF', '-DENABLE_TESTS=OFF', '-DENABLE_TOOLS=ON', '-DENABLE_CCACHE=OFF',
            '-DPUREEOS_GOVERNANCE_CONTRACTS_DIR=' + str(fixture_root),
            '-DPUREEOS_SINGLE_CHOICE_CONTRACTS_DIR=' + str(fixture_root),
            '-DPUREEOS_RESOURCE_SPLIT_CONTRACTS_DIR=' + str(fixture_root),
            '-DPUREEOS_ROTATION_CONTRACTS_DIR=' + str(fixture_root)], 180)
        run('build-node-controller', ['cmake', '--build', build, '--target', 'nodeos', 'cleos', 'keosd',
            'helios-util', 'parallel_validation_test', 'parallel_wasm_worker_test', 'native_savanna_test',
            'parallel_access_hints_test', 'parallel_worker_signals_test', 'parallel_immutable_input_test',
            '--parallel', str(plan.get('compilation_jobs',2))], 9000)
        report['binaries_sha256'] = {str(p.relative_to(build)): sha(p) for p in (
            build / 'programs/nodeos/nodeos', build / 'programs/cleos/cleos', build / 'programs/keosd/keosd',
            build / 'programs/helios-util/helios-util', build / 'unittests/parallel_validation_test',
            build / 'unittests/parallel_wasm_worker_test', build / 'unittests/native_savanna_test',
            build / 'unittests/parallel_access_hints_test', build / 'unittests/parallel_worker_signals_test',
            build / 'unittests/parallel_immutable_input_test')}; save()
        for n in range(1,4):
            run(f'immutable-input-integration-probe-{n}', [build/'unittests/parallel_wasm_worker_test',
                '--run_test=parallel_worker_pool_tests/shared_tree_catalog_versions_keep_old_inputs_and_original_budgets',
                '--report_level=detailed','--log_level=test_suite','--color_output=no'],180)
        for n in range(1, 11):
            run(f'leaf-overlap-stability-{n}', [build / 'unittests/parallel_wasm_worker_test',
                '--run_test=parallel_sync_pool_tests/multi_sync_pool_overlaps_leaf_execution_across_transactions_and_resets_each_root',
                '--report_level=detailed', '--log_level=test_suite', '--color_output=no', '--', '--eos-vm-jit'], 180)
        for n in range(1, 4):
            run(f'immutable-input-round-{n}', [build / 'unittests/parallel_immutable_input_test'], 60)
            run(f'access-hints-round-{n}', [build / 'unittests/parallel_access_hints_test'], 60)
            run(f'worker-signals-round-{n}', [build / 'unittests/parallel_worker_signals_test'], 60)
            run(f'controller-round-{n}', [build / 'unittests/parallel_validation_test',
                '--run_test=!*/export_*', '--report_level=detailed', '--color_output=no', '--', '--eos-vm-jit'], 1800)
            run(f'worker-round-{n}', [build / 'unittests/parallel_wasm_worker_test', '--run_test=!*/export_*',
                '--report_level=detailed', '--color_output=no', '--', '--eos-vm-jit'], 1800)
        # Functional-only, isolated processes and temp dirs, not TPS. Run these
        # three fresh native rounds concurrently AFTER the timing/overlap and
        # whole controller/worker matrices, so they cannot disturb those probes.
        native_jobs = []
        try:
            for n in range(1, 4):
                temporary = ROOT / f'native-tmp-{n}'; temporary.mkdir()
                log = (ROOT / f'blacklist-and-history-round-{n}.log').open('x')
                command = [build / 'unittests/native_savanna_test',
                    '--run_test=pureeos_blacklist_current_policy_tests:pureeos_reward_halving_tests/permanently_banned_producer_can_claim_old_unpaid_blocks_but_not_new_rewards:pureeos_sequential_rotation_tests/rotation_committee_six_offline_seven_offline_and_recovery',
                    '--report_level=detailed', '--color_output=no']
                process = subprocess.Popen(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT,
                    env={**os.environ, 'TMPDIR': str(temporary)})
                native_jobs.append((n, process, log))
            report['native_round_execution'] = 'Three isolated native functional processes after core matrices, shared 6.5GiB cgroup'
            save()
            for n, process, log in native_jobs:
                remaining = int((datetime.fromisoformat(plan['deadline']) - datetime.now(timezone.utc)).total_seconds()) - 900
                code = process.wait(timeout=max(1, min(1800, remaining)))
                log.close()
                report['jobs'].append(dict(name=f'blacklist-and-history-round-{n}', exit_code=code)); save()
            assert all(process.returncode == 0 for _, process, _ in native_jobs), 'Native critical round failed'
        finally:
            for _, process, log in native_jobs:
                if process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=15)
                    except subprocess.TimeoutExpired: process.kill(); process.wait()
                log.close()
        assert all(sha(source / p) == expected for p, expected in plan['sha256'].items())
        assert all(sha(BASE / p) == expected for p, expected in plan['baseline_sha256'].items())
        assert all(sha(ROOT / 'contracts' / p) == expected for p, expected in plan['contracts_sha256'].items())
        assert all(sha(fixture_root/p)==expected for p,expected in plan['contracts_sha256'].items())
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error), traceback=traceback.format_exc())
    finally:
        report['finished_utc'] = datetime.now(timezone.utc).isoformat(); save()
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__': raise SystemExit(main())
