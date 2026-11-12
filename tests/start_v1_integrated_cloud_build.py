"""Compile a separate v1 tree on exact existing C; never extend cloud lifetime."""
from datetime import datetime
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile

SOURCE = Path(__file__).resolve().parents[1]
TASK = SOURCE.parent / 'PureEOS/build/local-tests/unified-cdt5-20261021-TBMyRV'
sys.path.insert(0, str(TASK))
import cloud_round as c


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--resume', action='store_true', help='Resume only the stopped, archived private first build')
    parser.add_argument('--attempt', type=int, choices=(1, 2, 3, 4, 5, 6, 7, 8))
    args = parser.parse_args()
    attempt = args.attempt or (2 if args.resume else 1)
    resumed = attempt > 1
    minimum = 3600 if attempt >= 8 else 5400 if attempt >= 4 else 14400
    assert (datetime.fromisoformat(c.DEADLINE) - c.now()).total_seconds() > minimum
    output = Path('/run/media/guagua/Windows/PureEOS-v1-hardening-20261022/integrated-build-cloud' + (f'-v{attempt}' if resumed else ''))
    resume = None
    if resumed:
        previous = output.with_name('integrated-build-cloud' + (f'-v{attempt-1}' if attempt>2 else ''))
        proof = c.read(previous / 'download-verified.json')
        assert proof['status'] == 'passed' and c.sha(proof['local']) == proof['sha256']
        resume = dict(parent_archive_sha256=proof['sha256'], parent_input_sha256=c.sha(previous / 'input.json'),
            root='/opt/pureeos-v1-integrated-20261022' + (f'-v{attempt-1}' if attempt>2 else ''),
            unit=c.PREFIX+'-v1-integrated-build'+(f'-v{attempt-1}' if attempt>2 else ''),
            reason=('Use a valid nonzero budget-test descriptor; bind positive probes to actual tree fingerprints; focused integration before whole matrices; production code unchanged'
                if attempt >= 8 else 'Construct malformed tree/sync/mixed inputs BEFORE freezing; retain all rejection and healthy-reuse assertions; production N1/N2/N4 unchanged'
                if attempt >= 7 else 'Add immutable joined input catalogs for tree/sync/mixed; preserve budgets and canonical proof; separate cold correctness from fixed warm leaf-overlap batches'
                if attempt >= 6 else 'Separate cold discovery-budget probes from warm path reuse; retain exact 64-attempt exhaustion and all serial-oracle checks; add fixture registration guard'
                if attempt >= 5 else 'Fix test expiration type; integrate verified safe-eviction WASM and current native boundary fixtures; preserve O3 and one compiler job'
                if attempt >= 4 else 'Prior build cgroup OOM at 6.5 GiB; use one compiler job, preserve O3 and original limits; broaden safe path hints beyond payload nonces'
                if attempt==3 else 'First compile explicitly stopped and archived; fix execution-owner lazy cache construction and add affected-path tests'))
    output.mkdir(exist_ok=False)
    root = '/opt/pureeos-v1-integrated-20261022' + (f'-v{attempt}' if resumed else '')
    files = ['libraries/chain/controller.cpp', 'libraries/chain/parallel_worker_pool.cpp',
        'libraries/chain/include/eosio/chain/parallel_worker_signals.hpp',
        'libraries/chain/include/eosio/chain/parallel_access_hints.hpp',
        'libraries/chain/include/eosio/chain/parallel_worker_pool.hpp',
        'libraries/chain/include/eosio/chain/parallel_immutable_input.hpp',
        'unittests/CMakeLists.txt', 'unittests/parallel_prefix_refresh_cases.hpp', 'unittests/parallel_validation_tests.cpp',
        'unittests/parallel_sync_call_tests.cpp', 'unittests/parallel_wasm_worker_tests.cpp',
        'unittests/parallel_action_tree_pool_tests.cpp', 'unittests/parallel_mixed_tree_tests.cpp',
        'tests/parallel_immutable_input_test.cpp',
        'unittests/pureeos_reward_halving_tests.inc',
        'unittests/pureeos_governance_tests.cpp', 'unittests/pureeos_sequential_rotation_tests.inc',
        'unittests/pureeos_blacklist_current_policy_tests.inc', 'tests/native_policy_boundary_fixture.cpp',
        'tests/parallel_worker_signals_test.cpp', 'tests/parallel_access_hints_test.cpp',
        'tests/v1_integrated_cloud_build_guest.py']
    def sha(path):
        with Path(path).open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()
    plan = dict(sha256={p: sha(SOURCE / p) for p in files}, deadline=c.DEADLINE,
        baseline_sha256={p: sha(SOURCE.parent / 'PureEOS' / p) for p in files if (SOURCE.parent / 'PureEOS' / p).is_file()},
        contracts_build_report_sha256=sha(output.parent / 'contracts-safe-eviction-attempt-2/report.json'),
        no_new_vm_or_extension=True, tps_cancelled=True, resume=resume, compilation_jobs=1 if attempt>=3 else 2)
    archive = output / 'payload.tar.gz'
    contracts = output.parent / 'contracts-safe-eviction-attempt-2/contracts'
    products = sorted(p for p in contracts.rglob('*') if p.is_file() and p.suffix in ('.wasm', '.abi'))
    assert len(products) == 25
    assert c.read(output.parent / 'contracts-safe-eviction-attempt-2/report.json')['status'] == 'passed'
    plan['contracts_sha256'] = {str(p.relative_to(contracts)): sha(p) for p in products}
    if attempt >= 7:
        # Identical current WASM/ABI, fixed absolute fixture path. Reuse only
        # unchanged fixture objects; relink all programs against current core.
        plan['compiled_contracts_candidate_root'] = '/opt/pureeos-v1-integrated-20261022-v5'
    (output / 'input.json').write_text(json.dumps(plan, indent=2))
    with tarfile.open(archive, 'x:gz') as stream:
        for name in files: stream.add(SOURCE / name, arcname='overlay/' + name)
        stream.add(output / 'input.json', arcname='input.json')
        for p in products: stream.add(p, arcname='contracts/' + str(p.relative_to(contracts)))
    c.remote('c', ['python3', '-c', 'import pathlib;pathlib.Path(' + repr(root) + ').mkdir(mode=0o700)'], 30)
    ssh = c.old.ssh_args('c')
    subprocess.run(['rsync', '-a', '--chmod=F600', '-e', shlex.join(ssh[:-1]), str(archive), ssh[-1] + ':' + root + '/'], check=True, timeout=90)
    extract = ('import pathlib,tarfile,hashlib;p=pathlib.Path(' + repr(root) + ');a=p/"payload.tar.gz";'
        'assert hashlib.sha256(a.read_bytes()).hexdigest()==' + repr(sha(archive)) + ';'
        't=tarfile.open(a);m=t.getmembers();assert all(x.isfile() and not pathlib.Path(x.name).is_absolute() and ".." not in pathlib.Path(x.name).parts for x in m);'
        't.extractall(p,members=m,filter="data")')
    c.remote('c', ['python3', '-c', extract], 30)
    unit = c.PREFIX + '-v1-integrated-build' + (f'-v{attempt}' if resumed else '')
    c.remote('c', ['systemd-run', '--quiet', '--unit=' + unit, '--service-type=exec',
        '--property=RuntimeMaxSec='+str(min(17100,int((datetime.fromisoformat(c.DEADLINE)-c.now()).total_seconds())-900)),
        '--property=MemoryMax=6500M', '--property=MemorySwapMax=512M',
        '--property=KillMode=control-group', '--property=StandardOutput=append:' + root + '/driver.log',
        '--property=StandardError=inherit', '/usr/bin/env', 'PUREEOS_V1_BUILD_ROOT=' + root,
        'python3', root + '/overlay/tests/v1_integrated_cloud_build_guest.py'], 30)
    (output / 'launch.json').write_text(json.dumps(dict(role='c', instance_id=c.IDS['c'], root=root,
        unit=unit, utc=c.now().isoformat(), deadline_unchanged=c.DEADLINE), indent=2))
    print(json.dumps(dict(status='started', root=root, unit=unit)))


if __name__ == '__main__': main()
