"""Actual resource manager before/after probe against an owned closed build.

Uses read-only cached dependencies, not the running candidate. This is neither
an integrated candidate pass nor a performance/consensus-budget change.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

from v1_local_release_workflows import IMAGE, owned_container, read_progress, save, sha

BASE = Path('/run/media/guagua/Windows/PureEOS-v1-hardening-20261022')
PARENT = BASE/'local-native-profile-retest-attempt-2'
FAILURES = {'resource_limits_test/pending_large_weights_remain_finite_and_enforce_block_caps',
    'resource_limits_test/pending_large_weight_query_undo_restores_finite_limits'}


def result_cases(log):
    return (set(re.findall(r'Test case "([^"]+)" (?:has failed|was aborted)', log)),
        set(re.findall(r'Test case "([^"]+)" has passed', log)))


def compile_command(entry, source, output):
    original = shlex.split(entry['command'])
    command, skip = [], False
    for value in original:
        if skip:
            skip = False
            continue
        if value in ('-o', '-MF', '-MT', '-MQ'):
            skip = True
            continue
        if value in ('-c', '-MD', '-MMD', '-MP', '-fmodules-ts') or value == entry['file']:
            continue
        if value.startswith(('-fmodule-mapper=', '-fdeps-')):
            continue
        command.append(value)
    return command+['-c', str(source), '-o', str(output)]


def link_command(text, output, objects):
    found = [line for line in text.splitlines() if '-o unittests/native_savanna_test ' in line]
    assert len(found) == 1, 'Exact existing native target link command required'
    tokens = shlex.split(found[0])
    compiler = next(i for i, value in enumerate(tokens) if Path(value).name in ('c++', 'g++'))
    tokens = tokens[compiler:]
    if '&&' in tokens:
        tokens = tokens[:tokens.index('&&')]
    command, skip = [], False
    for value in tokens:
        if skip:
            skip = False
            continue
        if value == '-o':
            skip = True
            continue
        if value.endswith('.o'):
            continue
        if value.startswith(('-Wl,--dependency-file=', '-Wl,--dependency-file,')):
            continue
        command.append(value)
    first_library = next(i for i, value in enumerate(command) if value.endswith('.a'))
    return command[:first_library]+list(map(str, objects))+command[first_library:]+['-o', str(output)]


def guest():
    root = Path('/probe')
    os.chdir('/work/build')
    plan = read_progress(root/'input.json')
    report = dict(status='running', scope=__doc__, release_ready=False, mode=plan['mode'], jobs=[],
        parent_archive_sha256=plan['parent_archive_sha256'], started_utc=datetime.now(timezone.utc).isoformat())
    def run(name, command, timeout=300):
        with (root/(name+'.log')).open('x') as stream:
            code = subprocess.run(command, cwd='/work/build', stdout=stream,
                stderr=subprocess.STDOUT, timeout=timeout).returncode
        report['jobs'].append(dict(name=name, exit_code=code))
        save(root/'report.json', report)
        return code
    try:
        assert all(sha(root/'inputs'/p) == d for p, d in plan['inputs_sha256'].items())
        entries = read_progress(Path('/work/build/compile_commands.json'))
        entry = next(e for e in entries if e['file'].endswith('/tests/native_policy_boundary_fixture.cpp'))
        test_object = root/'resource_limits_test.o'
        assert run('compile-resource-tests', compile_command(entry, root/'inputs/resource_limits_test.cpp', test_object)) == 0
        objects = [Path('/work/build/unittests/CMakeFiles/native_savanna_test.dir/main.cpp.o'), test_object]
        if plan['mode'] == 'after':
            entry = next(e for e in entries if e['file'].endswith('/libraries/chain/resource_limits.cpp'))
            changed_object = root/'resource_limits.o'
            assert run('compile-actual-resource-manager', compile_command(entry, root/'inputs/resource_limits.cpp', changed_object)) == 0
            objects.append(changed_object)
        text = subprocess.check_output(['ninja', '-C', '/work/build', '-t', 'commands', 'unittests/native_savanna_test'], text=True)
        program = root/'resource_limits_boundary_test'
        command = link_command(text, program, objects)
        dependencies = {str(p): sha(p) for value in command if (p := Path(value)).suffix == '.a' and p.is_file()}
        assert dependencies, 'No cached static library identity'
        report['cached_libraries_sha256'] = dependencies
        assert run('link', command) == 0, 'Probe link failed; see link.log, not a resource test result'
        for n in range(1, 2 if plan['mode'] == 'before' else 4):
            code = run('round-'+str(n), [str(program), '--run_test=resource_limits_test',
                '--report_level=detailed', '--log_level=test_suite', '--color_output=no'], 180)
            log = (root/('round-'+str(n)+'.log')).read_text(errors='replace')
            failed, passed = result_cases(log)
            report.setdefault('rounds', []).append(dict(round=n, exit_code=code,
                failed=sorted(failed), passed=sorted(passed)))
            if plan['mode'] == 'before':
                assert code == 201 and failed == FAILURES and len(passed) == 13, 'Original failures differ from diagnosed overflow'
                assert 'Casting a higher bit integer value' in log and 'cannot contain the value' in log
            else:
                assert code == 0 and not failed and FAILURES <= passed and len(passed) == 15, 'Fixed manager failed; preserve, do not retry until green'
            assert 'resource_limits_test/representable_pending_limits_keep_original_exact_formula' in passed
        assert all(sha(p) == d for p, d in dependencies.items()), 'Cached libraries changed'
        assert all(sha(root/'inputs'/p) == d for p, d in plan['inputs_sha256'].items())
        report.update(status='expected-original-failures-confirmed' if plan['mode'] == 'before' else 'passed-scoped-resource-manager',
            program_sha256=sha(program), inputs_unchanged=True)
    except Exception as error:
        report.update(status='failed', error=type(error).__name__+': '+str(error))
    report['finished_utc'] = datetime.now(timezone.utc).isoformat()
    save(root/'report.json', report)
    return 0 if report['status'] in ('expected-original-failures-confirmed', 'passed-scoped-resource-manager') else 1


def host(mode, attempt=1):
    source = Path(__file__).resolve().parents[1]
    assert isinstance(attempt, int) and 1 <= attempt <= 9
    root = BASE/('resource-limits-boundary-'+mode+'-attempt-'+str(attempt))
    name = 'pureeos-v1-resource-limits-'+mode+'-attempt-'+str(attempt)
    proof, monitor = (read_progress(PARENT/p) for p in ('download-verified.json', 'monitor.json'))
    assert proof['status'] == 'passed' and proof['candidate_test_status'] == 'failed'
    assert monitor['status'] == 'finished-and-archived' and monitor['candidate_status'] == 'failed'
    assert sha(PARENT/'candidate-evidence.tar.gz') == proof['sha256']
    assert not owned_container('pureeos-v1-local-native-profile-retest-attempt-2', IMAGE)
    assert not owned_container(name, IMAGE)
    root.mkdir(exist_ok=False)
    inputs = root/'inputs'
    inputs.mkdir()
    for file in ('unittests/resource_limits_test.cpp', 'libraries/chain/resource_limits.cpp'):
        origin = PARENT/'source'/file if mode == 'before' and file.endswith('/resource_limits.cpp') else source/file
        shutil.copyfile(origin, inputs/Path(file).name)
    baseline_sha = read_progress(PARENT/'source-identity-final.json')['files_sha256']['libraries/chain/resource_limits.cpp']
    assert sha(PARENT/'source/libraries/chain/resource_limits.cpp') == baseline_sha
    assert (sha(inputs/'resource_limits.cpp') == baseline_sha) == (mode == 'before')
    shutil.copyfile(Path(__file__), inputs/'runner.py')
    plan = dict(mode=mode, image=IMAGE, parent_archive_sha256=proof['sha256'], baseline_resource_sha256=baseline_sha,
        inputs_sha256={p.name: sha(p) for p in inputs.iterdir()}, release_ready=False)
    save(root/'input.json', plan)
    (root/'tmp').mkdir()
    command = ['sudo', '-n', 'docker', 'run', '--rm', '--name='+name, '--label=org.pureeos.task='+name,
        '--pull=never', '--network=none', '--read-only', '--log-driver=none', '--user=1000:1000',
        '--cpus=2', '--memory=4096m', '--memory-swap=5120m', '--pids-limit=128',
        '--env', 'PYTHONDONTWRITEBYTECODE=1', '--env', 'TMPDIR=/probe/tmp',
        '--tmpfs', '/tmp:rw,size=64m,uid=1000,gid=1000', '--mount', f'type=bind,src={root},dst=/probe',
        '--mount', f'type=bind,src={PARENT}/source,dst=/work/source,readonly',
        '--mount', f'type=bind,src={PARENT}/build,dst=/work/build,readonly', '--workdir=/probe', IMAGE,
        'python3', '/probe/inputs/runner.py', '--guest']
    # Keep the common pure helpers available without importing a cloud transport.
    shutil.copyfile(source/'tests/v1_local_release_workflows.py', inputs/'v1_local_release_workflows.py')
    plan['inputs_sha256']['v1_local_release_workflows.py'] = sha(inputs/'v1_local_release_workflows.py')
    save(root/'input.json', plan)
    try:
        with (root/'driver.log').open('x') as stream:
            code = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=1200).returncode
    finally:
        if owned_container(name, IMAGE):
            subprocess.run(['sudo', '-n', 'docker', 'stop', '--time=30', name], check=True, timeout=60)
    print(json.dumps(dict(scope=__doc__, exit_code=code, report=str(root/'report.json'))))
    return code


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--guest', action='store_true')
    parser.add_argument('--mode', choices=('before', 'after'))
    parser.add_argument('--attempt', type=int, default=1)
    args = parser.parse_args()
    if args.guest:
        sys.exit(guest())
    if args.mode is None:
        parser.error('--mode required')
    sys.exit(host(args.mode, args.attempt))
