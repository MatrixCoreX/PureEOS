"""Verify four native tools and four component tests; not chain or release acceptance."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import traceback
import xml.etree.ElementTree as ET


TESTS = {
    'pureeos_parallel_state_view': 'unittests/parallel_state_view_test',
    'pureeos_parallel_secondary_state': 'unittests/parallel_secondary_state_test',
    'test_incoming_transaction_batch': 'plugins/producer_plugin/test/test_incoming_transaction_batch',
    'test_production_timer_guard': 'plugins/producer_plugin/test/test_production_timer_guard',
}
PATTERN = '^(' + '|'.join(re.escape(name) for name in TESTS) + ')$'
TOOLS = {
    'nodeos': ('programs/nodeos/pureeos-ubuntu-nodeos', ['--full-version']),
    'cleos': ('programs/cleos/cleos', ['version', 'full']),
    'keosd': ('programs/keosd/keosd', ['--full-version']),
    'helios-util': ('programs/helios-util/helios-util', ['--help']),
}


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def validate_inventory(inventory, build):
    tests = inventory.get('tests', [])
    names = [test['name'] for test in tests]
    if len(names) != len(TESTS) or set(names) != set(TESTS):
        raise ValueError('CTest inventory is missing, duplicated or unexpected')
    for test in tests:
        command = test.get('command', [])
        if not command or Path(command[0]).resolve() != (build / TESTS[test['name']]).resolve():
            raise ValueError('CTest command does not point to the selected executable')


def validate_junit(path):
    root = ET.parse(path).getroot()
    cases = list(root.iter('testcase'))
    if len(cases) != len(TESTS) or {case.get('name') for case in cases} != set(TESTS):
        raise ValueError('Incomplete or duplicated CTest JUnit cases')
    if (any(case.get('status') != 'run' for case in cases)
            or any(case.find(tag) is not None for case in cases for tag in ('skipped', 'failure', 'error'))
            or any(int(suite.get(attribute, '0')) for suite in root.iter('testsuite')
                   for attribute in ('failures', 'errors', 'skipped', 'disabled'))):
        raise ValueError('Skipped or failed component test')
    return [case.get('name') for case in cases]


def validate_version(output, expected):
    lines = output.splitlines()
    if expected not in lines and expected + '-dirty' not in lines:
        raise ValueError('Tool version does not match the expected source identity')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expected-version', required=True)
    args = parser.parse_args()
    build = args.build_dir.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = dict(status='running', scope=__doc__, release_ready=False,
                  expected_version=args.expected_version, jobs=[],
                  started_utc=datetime.now(timezone.utc).isoformat())

    def save():
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(name, command, timeout):
        with (output / (name + '.log')).open('x') as log:
            result = subprocess.run([str(item) for item in command], stdout=log,
                                    stderr=subprocess.STDOUT, timeout=timeout)
        report['jobs'].append(dict(name=name, exit_code=result.returncode))
        save()
        if result.returncode:
            raise RuntimeError('Native check failed: ' + name)
        return (output / (name + '.log')).read_text()

    try:
        save()
        paths = {relative: build / relative for relative in
                 (*TESTS.values(), *(value[0] for value in TOOLS.values()))}
        for path in paths.values():
            if path.is_symlink() or not path.is_file():
                raise ValueError('Missing native executable: ' + str(path))
            with path.open('rb') as stream:
                if stream.read(4) != b'\x7fELF':
                    raise ValueError('Expected a real native ELF: ' + str(path))
        before = {name: sha(path) for name, path in paths.items()}
        report['executables_sha256'] = before
        for name, (relative, arguments) in TOOLS.items():
            text = run('probe-' + name, [build / relative, *arguments], 30)
            if name != 'helios-util':
                validate_version(text, args.expected_version)
            elif not text.strip():
                raise ValueError('Empty helios-util help output')
        listing = run('ctest-inventory', ['ctest', '--test-dir', build,
                      '--show-only=json-v1', '-R', PATTERN], 30)
        validate_inventory(json.loads(listing), build)
        junit = output / 'ctest.junit.xml'
        run('ctest-components', ['ctest', '--test-dir', build, '--output-on-failure',
            '--no-tests=error', '-j1', '--timeout', '300', '-R', PATTERN,
            '--output-junit', junit], 1250)
        report['passed_tests'] = validate_junit(junit)
        report['inputs_unchanged'] = all(sha(path) == before[name] for name, path in paths.items())
        if not report['inputs_unchanged']:
            raise RuntimeError('Native executable changed during checks')
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error), traceback=traceback.format_exc())
    report['finished_utc'] = datetime.now(timezone.utc).isoformat()
    save()
    print(json.dumps({key: report[key] for key in ('status', 'scope', 'release_ready')}))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
