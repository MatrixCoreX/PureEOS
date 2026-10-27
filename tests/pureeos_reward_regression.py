#!/usr/bin/env python3
"""Rerun the prior isolated acceptance matrix plus reward-halving coverage.

Evidence belongs to a new output directory. Resume is allowed only when every
pinned artifact is unchanged; interrupted/failed attempts are retained.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

PROJECT = Path(__file__).resolve().parents[1]
BUILD = PROJECT / 'build'
CONTRACTS = PROJECT.parent / 'reference-contracts'
RISK_CASES = [
    'pureeos_sequential_rotation_tests/large_vote_balances_cannot_affect_exact_block_reward_share',
    'pureeos_sequential_rotation_tests/under21_offline_entrant_is_replaced_without_blacklist_or_growth',
    'pureeos_sequential_rotation_tests/alternating_incumbent_misses_cannot_veto_all_rotations',
    'pureeos_single_choice_contract_tests/governance_scan_bounds_visited_rows_and_never_truncates_electorate',
    'pureeos_sequential_rotation_tests/governance_live_index_skips_incomplete_registrations_and_tracks_mutations',
    'pureeos_sequential_rotation_tests/previous_v2_state_survives_reviewed_upgrade_and_new_admission',
    'pureeos_sequential_rotation_tests/under21_global_pause_and_no_replacement_preserve_membership_until_late_candidate',
]


def hashes():
    paths = [BUILD / p for p in (
        'programs/nodeos/nodeos', 'programs/cleos/cleos', 'programs/keosd/keosd',
        'programs/helios-util/helios-util', 'unittests/unit_test', 'unittests/native_savanna_test',
        'plugins/net_plugin/tests/test_net_plugin', 'plugins/chain_plugin/test/test_chain_plugin',
        'plugins/resource_monitor_plugin/test/test_resmon_plugin')]
    paths += list((PROJECT / 'tests').glob('*pureeos*'))
    paths += list((PROJECT / 'tests').glob('native_savanna*.py'))
    paths += list((PROJECT / 'unittests').glob('pureeos*'))
    paths += [PROJECT / 'unittests/CMakeLists.txt']
    # Upgrade tests execute these historical WASMs too; pin them along with
    # current code, not merely the binary containing their configured paths.
    for line in (BUILD / 'CMakeCache.txt').read_text().splitlines():
        if line.startswith(('PUREEOS_PREVIOUS_ROTATION_CONTRACTS_DIR:PATH=',
                            'PUREEOS_PREVIOUS_REWARD_CONTRACTS_DIR:PATH=')):
            baseline = line.split('=', 1)[1]
            if baseline:
                paths += [p for p in Path(baseline).rglob('*') if p.suffix in ('.wasm', '.abi')]
    for profile in ('pureeos-governance', 'pureeos-single-choice', 'pureeos-resource-split', 'pureeos-rotation-v2'):
        paths += [p for p in (CONTRACTS / 'build' / profile).rglob('*') if p.suffix in ('.wasm', '.abi')]
    for directory in ('eosio.system', 'eosio.bpay'):
        paths += [p for p in (CONTRACTS / 'contracts' / directory).rglob('*') if p.is_file()]
    result = {}
    for path in sorted(set(paths)):
        if path.is_file():
            with path.open('rb') as stream:
                result[str(path)] = hashlib.file_digest(stream, 'sha256').hexdigest()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--resume', action='store_true')
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    summary = root / 'regression-summary.json'
    pinned = hashes()
    if args.resume:
        report = json.loads(summary.read_text())
        if report['artifacts'] != pinned:
            raise RuntimeError('Artifacts changed; refusing to reuse prior results')
        for entry in report['groups']:
            if entry.get('status') != 'passed':
                entry['status'] = 'superseded'
    else:
        if summary.exists():
            raise RuntimeError('Existing report; use --resume or a new directory')
        report = {'status': 'running', 'artifacts': pinned, 'groups': [], 'started': time.time(),
                  'scope': 'All previously selected 1420 CTest targets plus positive-vote activation and repeated supplements',
                  'limitations': ['Not all repository CTest targets', 'Not OC/macOS or cross-region',
                                  'This matrix uses up to four independent nodes; 22-process acceptance has a separate report; no 841-process deployment',
                                  'Four-year and 14-day WASM tests advance the controller clock; production periods unchanged',
                                  'Existing manual-test-chain is not modified']}
    report['status'] = 'running'

    def save():
        summary.write_text(json.dumps(report, indent=2) + '\n')

    def run(name, command, expected=None):
        command = [str(a) for a in command]
        previous = next((g for g in report['groups'] if g['name'] == name and g.get('status') == 'passed'), None)
        if previous:
            if previous['command'] != command:
                raise RuntimeError('Command changed for completed group ' + name)
            print('REUSE ' + name + ': unchanged artifacts and recorded pass', flush=True)
            return
        attempt = sum(g['name'] == name for g in report['groups']) + 1
        log_path = root / f'{name}-attempt{attempt}.log'
        entry = {'name': name, 'command': command, 'log': str(log_path), 'started': time.time(), 'status': 'running'}
        report['groups'].append(entry)
        save()
        print('START ' + name, flush=True)
        environment = os.environ.copy()
        environment.pop('PUREEOS_TEST_ROTATION_DIR', None)
        environment.pop('PUREEOS_TEST_PREACTIVATION_DIR', None)
        with log_path.open('w') as log:
            child = subprocess.Popen(command, cwd=BUILD, stdout=log, stderr=subprocess.STDOUT,
                                     start_new_session=True, env=environment)
            try:
                while child.poll() is None:
                    try:
                        child.wait(timeout=20)
                    except subprocess.TimeoutExpired:
                        print(f'RUNNING {name}: {time.time() - entry["started"]:.0f}s', flush=True)
            except BaseException:
                os.killpg(child.pid, signal.SIGINT)
                try:
                    child.wait(timeout=25)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait()
                raise
        entry.update(returncode=child.returncode, seconds=round(time.time() - entry['started'], 2),
                     status='passed' if child.returncode == 0 else 'failed')
        if expected is not None:
            xml = root / f'{name}-attempt{attempt}.xml'
            if xml.exists():
                cases = ET.parse(xml).getroot().findall('testcase')
                entry['targets'] = [c.get('name') for c in cases]
                entry['failed_targets'] = [c.get('name') for c in cases if c.get('status') != 'run' or
                                           c.find('failure') is not None or c.find('skipped') is not None]
                if len(cases) != expected or entry['failed_targets']:
                    entry['status'] = 'failed'
            else:
                entry.update(status='failed', error='JUnit evidence missing')
        save()
        print(f'END {name}: {entry["status"]} ({entry["seconds"]}s)', flush=True)
        if entry['status'] != 'passed':
            raise RuntimeError('Failed group: ' + name + '; inspect ' + str(log_path))

    def ctest(name, regex, count, jobs=1, repeat=1):
        # Attempt paths are fixed per execution, while complete groups retain
        # their original paths on a hash-verified resume.
        previous = next((g for g in report['groups'] if g['name'] == name and g.get('status') == 'passed'), None)
        if previous:
            run(name, previous['command'], count)
            return
        attempt = sum(g['name'] == name for g in report['groups']) + 1
        command = ['ctest', '--output-on-failure', '--no-tests=error', '-j', jobs, '-R', regex,
                   '--output-junit', root / f'{name}-attempt{attempt}.xml',
                   '--output-log', root / f'{name}-attempt{attempt}-detail.log',
                   '--test-output-size-passed', '2000000']
        if repeat > 1:
            command += ['--repeat', f'until-fail:{repeat}', '--verbose']
        run(name, command, count)

    def native(name, selection, runtime):
        run(name, [BUILD / 'unittests/native_savanna_test', '--run_test=' + selection,
                   '--report_level=detailed', '--log_level=message', '--color_output=no', '--', '--' + runtime])

    save()
    try:
        for runtime in ('eos-vm', 'eos-vm-jit'):
            for attempt in range(1, 3):
                native(f'halving-{runtime}-{attempt}', 'pureeos_reward_halving_tests', runtime)
                native(f'activation-{runtime}-{attempt}', 'pureeos_reward_activation_tests', runtime)
                for number, case in enumerate(RISK_CASES):
                    native(f'risk{number}-{runtime}-{attempt}', case, runtime)
            for attempt in range(1, 4):
                native(f'rex-fees-{runtime}-{attempt}', 'pureeos_resource_split_tests/rex_rental_fee_custody*', runtime)
        ctest('full-unit-and-native', r'(_unit_test_|^protocol_feature_digest_unit_test$|^native_(savanna_genesis|upgrade_governance(_contract)?|single_choice_contract|genesis_blocklog_compat|resource_split_contract|committee_boundary_contract|sequential_rotation_contract|reward_halving_contract|reward_activation_contract)$)', 1411, jobs=2)
        ctest('support-and-smoke', r'^(test_fc|test_producer_plugin|test_producer_plugin_integration|protocol_feature_digest_generator_test|pureeos_local_test_harness|reversible_recovery_test|native_savanna_nodeos_smoke)$', 7)
        run('upgrade-helper', ['python3', PROJECT / 'tests/pureeos_upgrade_helper_test.py'])
        run('reward-chain-helper', ['python3', PROJECT / 'tests/pureeos_reward_chain_test.py'])
        for profile in (1, 2):
            command = ['python3', PROJECT / 'tests/native_savanna_offline_process_test.py', '--build-dir', BUILD,
                       '--output-parent', root, '--profile', profile, '--kill-cycles', 3]
            if profile == 2:
                command += ['--initial-lib', 260, '--check-corruption']
            run(f'process-profile{profile}', command)
        for name, binary in (
            ('net', 'plugins/net_plugin/tests/test_net_plugin'),
            ('chain', 'plugins/chain_plugin/test/test_chain_plugin'),
            ('resource-monitor', 'plugins/resource_monitor_plugin/test/test_resmon_plugin')):
            run('supplemental-' + name, [BUILD / binary, '--report_level=detailed', '--color_output=no'])
        ctest('single-node-local', r'^native_(single_choice|resource_split)_nodeos_local$', 2, repeat=1)
        ctest('four-node-local', r'^native_multinode_nodeos_local$', 1, repeat=2)
        report['multinode_reports'] = []
        four = next(g for g in report['groups'] if g['name'] == 'four-node-local' and g['status'] == 'passed')
        for path in re.findall(r'Report: (.+report.json)', Path(four['log']).read_text()):
            evidence = json.loads(Path(path.strip()).read_text())
            checks = evidence.get('rental_fee_checks', [])
            confirmed = {t['transaction_id'] for t in evidence.get('confirmed_transactions', [])}
            valid = evidence['status'] == 'passed' and evidence['processes_stopped'] and len(checks) == 2
            valid = valid and all(c['transaction_id'] in confirmed for c in checks)
            report['multinode_reports'].append({'path': path.strip(), 'valid': valid, 'chain_id': evidence['chain_id']})
        if len(report['multinode_reports']) != 2 or not all(r['valid'] for r in report['multinode_reports']) or len({r['chain_id'] for r in report['multinode_reports']}) != 2:
            raise RuntimeError('Two independent complete four-node evidence reports required')
        report['artifacts_unchanged'] = hashes() == pinned
        if not report['artifacts_unchanged']:
            raise RuntimeError('Artifacts changed during acceptance')
        report['distinct_ctest_targets'] = len({target for g in report['groups'] if g['status'] == 'passed' for target in g.get('targets', [])})
        if report['distinct_ctest_targets'] != 1421:
            raise RuntimeError('Distinct CTest target count does not match acceptance manifest')
        report['status'] = 'passed'
    except BaseException as error:
        report.update(status='interrupted' if isinstance(error, KeyboardInterrupt) else 'failed', error=str(error))
        raise
    finally:
        report['finished'] = time.time()
        save()
        print('FINAL ' + report['status'] + ': ' + str(summary), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
