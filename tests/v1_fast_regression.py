"""No-build, exact-candidate smoke lane. Never a full-regression/release/TPS gate."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tarfile
import time
import traceback


IMAGE = 'sha256:5dd671bd834c70a74b076e4b0dc90e280958df1200d837d19e15df68fad4e83d'
BASE = Path('/run/media/guagua/Windows/PureEOS-v1-hardening-20261022')
PROGRAMS = ('nodeos', 'cleos', 'keosd', 'helios-util')
TESTS = ('parallel_validation_test', 'parallel_wasm_worker_test', 'native_savanna_test',
         'parallel_access_hints_test', 'parallel_worker_signals_test',
         'parallel_immutable_input_test', 'resource_limits_boundary_test')
RUNTIME_EXPORTER = 'parallel_validation_test'

# Explicit cases, not wildcards or a time-truncated full suite. Keep the costly
# complete FIFO/fault/841-candidate matrix in the independent full lane.
NATIVE = {
    'pureeos_genesis_tests': (
        'single_node_starts_native_and_finalizes_without_activation_transactions',
        'observer_validates_native_blocks_and_rejects_bad_signature',
        'native_snapshot_and_existing_state_restart'),
    'pureeos_governance_tests': (
        'review_period_is_fixed_at_fourteen_days',
        'developer_only_and_no_stale_electorate',
        'dedicated_upgrade_key_needs_no_active_key',
        'no_objections_allows_execution_only_after_fourteen_days',
        'one_two_three_four_nodes_veto_threshold',
        'two_hundred_ten_nodes_require_140_not_141',
        'exact_deadline_ends_veto_and_allows_execution',
        'execution_failure_rolls_back_proposal'),
    'pureeos_governance_contract_tests': (
        'production_bootstrap_and_electorate_ranking',
        'production_system_self_upgrade_and_resource_transfers_survive'),
    'pureeos_single_choice_contract_tests': (
        'multi_choice_and_proxy_actions_reject_and_roll_back',
        'rex_deposit_is_free_of_voting_but_buy_requires_choice',
        'queued_sale_blocks_clear_and_closed_sale_settles_once_before_switch',
        'integer_votes_do_not_grow_with_time_or_repeated_refresh',
        'integer_rpc_preserves_above_double_precision_and_pagination',
        'zero_block_candidate_cannot_claim_either_reward_bucket',
        'independent_bpay_cannot_distribute_by_rank_without_production'),
    'pureeos_resource_split_tests': (
        'fixed_ratio_and_manual_limits_cannot_bypass_policy',
        'rex_rent_renew_and_expiry_track_only_outstanding_resources',
        'rex_rental_fee_custody_with_forwarder',
        'rex_rental_fee_custody_without_forwarder',
        'deployed_fees_contract_routes_ram_and_powerup_income_after_sealing',
        'failed_payment_rolls_back_resources_orders_and_counters'),
    'pureeos_sequential_rotation_tests': (
        'no_alternative_keeps_nonempty_committee_and_no_automatic_ban_under21',
        'snapshot_during_handoff_restores_policy_queue_and_deterministic_context',
        'under21_offline_entrant_is_replaced_without_blacklist_or_growth',
        'large_vote_balances_cannot_affect_exact_block_reward_share'),
    'pureeos_reward_halving_tests': (
        'activation_and_real_issuance_fund_only_actual_blocks',
        'exact_four_year_boundary_and_later_eras_do_not_reprice_prior_accrual',
        'rate_changes_at_the_exact_boundary_not_one_block_early',
        'insufficient_treasury_rolls_back_claim_without_losing_reward_accrual'),
    'pureeos_reward_activation_tests': (
        'one_smallest_unit_vote_and_one_producer_can_issue_and_claim_with_small_supply',
        'failed_positive_vote_transaction_rolls_back_activation'),
    'pureeos_blacklist_current_policy_tests': (
        'six_finalized_bans_exit_sequentially_even_after_committee_drops_below21',),
}
WORKER = (
    'parallel_integer_oracle_tests/selected_integer_profile_configures_canonical_and_private_engines',
    'parallel_float_oracle_tests/selected_float_profile_configures_canonical_and_private_engines',
    'parallel_pure_hash_oracle_tests/selected_hash_profile_configures_canonical_and_private_engines',
    'parallel_secondary_wasm_oracle_tests/selected_secondary_profile_configures_canonical_and_private_engines',
    'parallel_context_free_adoption_tests/selected_context_free_profile_configures_canonical_and_private_engines',
    'parallel_sync_private_tests/sync_leaf_wasm_intervals_overlap_on_two_and_four_threads',
    'parallel_console_oracle_tests/selected_console_profile_matches_both_canonical_and_private_engines',
    'parallel_mixed_tree_adoption_tests/independent_mixed_batches_adopt_every_transaction',
    'parallel_mixed_tree_adoption_tests/original_inline_authority_and_ram_checks_undo_then_reuse_the_same_workers',
    'parallel_mixed_tree_adoption_tests/transaction_final_ram_limit_rejects_even_a_fully_adopted_mixed_tree',
)
CONTROLLER = (
    'parallel_validation_tests/same_table_disjoint_rows_adopt_and_shared_balances_fall_back_in_real_savanna_blocks',
    'parallel_validation_tests/real_token_transfers_preserve_notifications_conflicts_and_lifecycle_fallbacks',
)
# These cases explicitly iterate interpreter/JIT x 1/4/16 workers internally.
# Repeating them with an outer --eos-vm* switch adds cost, not engine coverage.
INTERNAL_MATRIX = (
    'parallel_prefix_refresh_tests/warm_access_hint_for_old_branch_cannot_hide_new_state_dependent_path',
    'parallel_prefix_refresh_tests/successful_private_refresh_does_not_bypass_canonical_authorization',
    'parallel_prefix_refresh_tests/prefix_retry_cannot_renew_the_production_private_deadline',
)
RESOURCE = tuple('resource_limits_test/' + name for name in (
    'elastic_cpu_relax_contract', 'elastic_net_relax_contract', 'weighted_capacity_cpu',
    'weighted_capacity_net', 'enforce_block_limits_cpu', 'enforce_block_limits_net',
    'enforce_account_ram_limit', 'enforce_account_ram_limit_underflow',
    'enforce_account_ram_limit_overflow', 'enforce_account_ram_commitment', 'sanity_check',
    'get_account_net_limit_ex_and_get_account_cpu_limit_ex',
    'pending_large_weights_remain_finite_and_enforce_block_caps',
    'pending_large_weight_query_undo_restores_finite_limits',
    'representable_pending_limits_keep_original_exact_formula'))


def native_cases():
    return tuple(suite + '/' + case for suite, cases in NATIVE.items() for case in cases)


def success_status(profile):
    assert profile in ('smoke', 'runtime')
    return 'smoke-passed-not-release' if profile == 'smoke' else 'runtime-preflight-passed-not-release'


def runtime_complete(cli, sdk):
    cases = sdk.get('cases', [])
    return (cli.get('status') == 'passed' and len(cli.get('cases', [])) == 8
        and all(c.get('status') == 'passed' for c in cli['cases'])
        and cli.get('reported_limits') == dict(workers=16, batch_transactions=64)
        and sdk.get('status') == 'passed' and [c.get('workers') for c in cases] == [0, 4]
        and all(c.get('passed') is True and c.get('sdk_rejection_partial_writes_rolled_back') is True
                and c.get('finalized_transactions', 0) > 0 for c in cases)
        and sum(r.get('adopted', 0) for r in cases[-1].get('production_attempts_including_aborted_blocks', [])) > 0
        and sum(r.get('adopted', 0) for r in cases[-1].get('validator_attempts_including_aborted_blocks', [])) > 0)


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def save(path, value):
    pending = path.with_suffix('.pending')
    pending.write_text(json.dumps(value, indent=2)); pending.replace(path)


def utc():
    return datetime.now(timezone.utc).isoformat()


def check_parent(report, identity):
    """A completed build is sufficient ONLY for smoke, never for release."""
    assert report.get('status') in ('running', 'passed'), 'Failed candidate cannot seed smoke'
    jobs = report.get('jobs', [])
    names = [j.get('name') for j in jobs]
    assert len(names) == len(set(names)) and all(j.get('exit_code') == 0 for j in jobs)
    assert {'configure', 'build-node-controller', 'build-version-provenance'} <= set(names)
    expected = {'programs/' + p + '/' + p for p in PROGRAMS} | {'unittests/' + p for p in TESTS}
    binaries = report.get('binaries_sha256', {})
    assert set(binaries) == expected and all(re.fullmatch('[0-9a-f]{64}', d) for d in binaries.values())
    assert re.fullmatch('[0-9a-f]{64}', identity) and report.get('source_identity_sha256') == identity
    version = report.get('build_version_provenance', {})
    assert version == dict(verified=True, client='v2.0.0', full='v2.0.0-src-' + identity,
                           source_manifest_sha256=identity), 'Unidentified/mismatched binary provenance'


def verify_boost(text, expected, code=0):
    actual = re.findall(r'Test case "([^"]+)" has passed', text)
    assert code == 0, 'Nonzero Boost exit'
    assert len(actual) == len(set(actual)) and set(actual) == set(expected), 'Missing/extra/duplicate cases'
    assert re.search(r'Test module "[^"]+" has passed with:', text), 'Missing final Boost report'
    assert not re.search(r'(?:test cases?|assertions?) out of \d+ (?:failed|aborted)', text)
    return len(actual)


def timing_cases(text):
    """Parse completed Boost case durations; never infer TPS or chain performance."""
    result, suite = [], []
    for line in text.splitlines():
        enter = re.search(r'Entering test suite "([^"]+)"', line)
        leave = re.search(r'Leaving test suite "([^"]+)"', line)
        case = re.search(r'Leaving test case "([^"]+)"; testing time: (\d+)us', line)
        if enter: suite.append(enter[1])
        if case:
            result.append(dict(case='/'.join((*suite, case[1])), seconds=int(case[2]) / 1e6))
        if leave:
            assert suite and suite.pop() == leave[1], 'Unbalanced Boost timing log'
    return result


def guest():
    root, build = Path('/work'), Path('/build')
    plan = json.loads((root / 'input.json').read_text())
    profile = plan.get('profile', 'smoke')
    passed_status = success_status(profile)
    report = dict(status='running', scope=__doc__, release_ready=False,
        full_regression=False, clock_mode='production-constants/virtual-block-time-in-unit-tests',
        source_identity_sha256=plan['source_identity_sha256'], profile=profile, jobs=[], started_utc=utc())
    started = time.monotonic()
    def verify_inputs():
        assert sha('/runner.py') == plan['runner_sha256']
        assert all(sha(build / name) == digest for name, digest in plan['binaries_sha256'].items())
        assert all(sha(Path('/contracts') / name) == digest for name, digest in plan['contracts_sha256'].items())
        assert all(sha(root / name) == digest for name, digest in plan['fixtures_sha256'].items())
    def run(name, command, expected=None, timeout=180):
        remaining = int(900 - (time.monotonic() - started))
        assert remaining > 0, 'Finite smoke deadline exhausted'
        before = time.monotonic()
        with (root / (name + '.log')).open('x') as log:
            result = subprocess.run(list(map(str, command)), cwd=root, stdout=log,
                stderr=subprocess.STDOUT, timeout=min(timeout, remaining))
        text = (root / (name + '.log')).read_text(errors='replace')
        job = dict(name=name, exit_code=result.returncode, elapsed_seconds=time.monotonic()-before)
        report['jobs'].append(job); save(root / 'report.json', report)
        assert result.returncode == 0, name + ' failed; no retry-until-green'
        if expected is not None:
            job['cases_passed'] = verify_boost(text, expected, result.returncode)
        save(root / 'report.json', report)
        return text
    def boost(name, program, cases, engine=None):
        command = [build / 'unittests' / program, '--run_test=' + ':'.join(cases),
                   '--report_level=detailed', '--log_level=test_suite', '--color_output=no']
        if engine: command += ['--', '--' + engine]
        run(name, command, cases, timeout=300)
    try:
        save(root / 'report.json', report); verify_inputs()
        expected_version = 'v2.0.0-src-' + plan['source_identity_sha256']
        for name, args in (('nodeos', ['--full-version']), ('cleos', ['version', 'full']),
                           ('keosd', ['--full-version'])):
            text = run('version-' + name, [build / 'programs' / name / name, *args], timeout=30)
            assert expected_version in text.splitlines(), 'Actual version differs from pinned source'
        if profile == 'runtime':
            scripts = root / 'source/tests'
            rounds = plan.get('runtime_rounds', 1)
            assert rounds in (1, 3)
            report['runtime_rounds'] = []
            for n in range(1, rounds + 1):
                cli_root, sdk_root = root / f'cli-{n}', root / f'sdk-{n}'
                run(f'cli-round-{n}', ['python3', scripts / 'parallel_runtime_cli_test.py',
                    '--nodeos', build / 'programs/nodeos/nodeos', '--work-dir', cli_root], timeout=300)
                run(f'sdk-round-{n}', ['python3', scripts / 'ubuntu_cdt_runtime_test.py',
                    '--nodeos', build / 'programs/nodeos/nodeos', '--cleos', build / 'programs/cleos/cleos',
                    '--fixture-exporter', build / 'unittests' / RUNTIME_EXPORTER, '--sdk-hardening',
                    '--complex-wasm', root / 'sdk/complexbench/complexbench.wasm',
                    '--cdt-wasm', root / 'sdk/cdtcompat/cdtcompat.wasm', '--work-dir', sdk_root], timeout=900)
                assert runtime_complete(json.loads((cli_root / 'report.json').read_text()),
                    json.loads((sdk_root / 'report.json').read_text())), 'Missing actual CLI or two-ledger SDK adoption'
                report['runtime_rounds'].append(n)
        else:
            boost('resource-limits', 'resource_limits_boundary_test', RESOURCE)
            # Cheap VM/conflict/authorization checks run before the longer policy case.
            for engine in ('eos-vm-jit', 'eos-vm'):
                boost('worker-' + engine, 'parallel_wasm_worker_test', WORKER, engine)
                boost('controller-' + engine, 'parallel_validation_test', CONTROLLER, engine)
            boost('controller-internal-matrix', 'parallel_validation_test', INTERNAL_MATRIX)
            for name in ('parallel_access_hints_test', 'parallel_worker_signals_test', 'parallel_immutable_input_test'):
                run(name, [build / 'unittests' / name], timeout=30)
            boost('native-policy', 'native_savanna_test', native_cases())
        verify_inputs()
        report.update(status=passed_status, inputs_unchanged=True,
            boost_case_executions=sum(j.get('cases_passed', 0) for j in report['jobs']))
    except Exception as error:
        report.update(status='failed', error=str(error), traceback=traceback.format_exc())
    report.update(finished_utc=utc(), elapsed_seconds=time.monotonic()-started)
    save(root / 'report.json', report)
    return 0 if report['status'] == passed_status else 1


def inspect_owned(name):
    result = subprocess.run(['sudo', '-n', 'docker', 'container', 'inspect', name],
                            text=True, capture_output=True, timeout=15)
    if result.returncode:
        assert re.search(r'No such (?:container|object):\s*' + re.escape(name) + r'(?:\s|$)',
                         result.stderr, re.I), 'Container absence unproved'
        return False
    detail = json.loads(result.stdout)[0]
    assert detail['Image'] == IMAGE and detail['Config'].get('Labels', {}).get('org.pureeos.task') == name
    return True


def host(args):
    os.umask(0o077)
    core = args.candidate.resolve(strict=True)
    output = args.output.absolute()
    prefix = 'fast-regression' if args.profile == 'smoke' else 'runtime-preflight'
    passed_status = success_status(args.profile)
    assert core.parent == BASE and re.fullmatch('local-native-profile-retest-attempt-[0-9]+', core.name)
    assert output.parent.resolve(strict=True) == BASE and re.fullmatch(prefix + '-attempt-[0-9]+', output.name)
    assert not output.exists() and not output.is_symlink(), 'Never overwrite a test attempt'
    assert shutil.disk_usage(BASE).free > 2 * 1024**3
    identity = sha(core / 'source-identity-final.json')
    report = json.loads((core / 'report.json').read_text()); check_parent(report, identity)
    inputs = json.loads((core / 'input.json').read_text()); assert inputs['image'] == IMAGE
    contracts = BASE / 'contracts-safe-eviction-attempt-2/contracts'
    for name, digest in report['binaries_sha256'].items(): assert sha(core / 'build' / name) == digest
    for name, digest in inputs['contracts_sha256'].items():
        assert not Path(name).is_absolute() and '..' not in Path(name).parts
        assert sha(contracts / name) == digest
    container_name = 'pureeos-v1-' + output.name
    assert not inspect_owned(container_name), 'Never replace an existing container'
    output.mkdir(); (output / 'tmp').mkdir()
    runner = output / 'runner.py'; shutil.copyfile(__file__, runner)
    save(output / 'parent-build-snapshot.json', report)
    fixtures = {}
    for filename in ('eosio.token.wasm', 'eosio.token.abi'):
        relative = 'source/unittests/contracts/eosio.token/' + filename
        path = core / relative
        fixtures[relative] = sha(path)
        target = output / relative; target.parent.mkdir(parents=True, exist_ok=True)
        # A private copy restores __FILE__-relative fixtures, without exposing
        # the candidate's writable source/build tree or modifying its artifacts.
        shutil.copyfile(path, target)
        assert sha(target) == fixtures[relative]
    if args.profile == 'runtime':
        # Preserve the exact candidate scripts plus their nested Python import
        # dependencies. Copy code only, never wallets/config/data/compiled caches.
        for path in sorted((core / 'source/tests').rglob('*.py')):
            assert path.is_file() and not path.is_symlink()
            relative = str(path.relative_to(core))
            fixtures[relative] = sha(path)
            target = output / relative; target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target); assert sha(target) == fixtures[relative]
        sdk = BASE / 'wasm-attempt-6'
        sdk_report = json.loads((sdk / 'report.json').read_text()); assert sdk_report['status'] == 'passed'
        for artifact_name in ('complexbench/complexbench.wasm', 'cdtcompat/cdtcompat.wasm', 'cdtcompat/cdtcompat.abi'):
            path = sdk / artifact_name; digest = sdk_report['artifacts_sha256'][str(path)]
            assert sha(path) == digest
            relative = 'sdk/' + artifact_name; fixtures[relative] = digest
            target = output / relative; target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target); assert sha(target) == digest
    plan = dict(image=IMAGE, candidate=str(core), source_identity_sha256=identity,
        binaries_sha256=report['binaries_sha256'], contracts_sha256=inputs['contracts_sha256'],
        fixtures_sha256=fixtures,
        runner_sha256=sha(runner), parent_report_sha256=sha(output / 'parent-build-snapshot.json'),
        release_ready=False, no_build=True, no_cloud_purchase=True, profile=args.profile, runtime_rounds=args.rounds)
    save(output / 'input.json', plan)
    # Only completed logs inform timing diagnostics. Missing/in-progress logs are
    # not classified as passed and are not opened by this lane.
    timings = []
    for job in report['jobs']:
        if job['name'].startswith(('full-native-matrix-', 'cold-warm-')):
            path = core / (job['name'] + '.log')
            timings += [dict(job=job['name'], **row) for row in timing_cases(path.read_text())]
    save(output / 'timing-analysis.json', dict(scope='Observed test durations only, NOT TPS',
        candidate=str(core), completed_jobs=len(report['jobs']),
        longest_completed_cases=sorted(timings, key=lambda row: row['seconds'], reverse=True)[:25]))
    command = ['sudo', '-n', 'docker', 'run', '--rm', '--name=' + container_name,
        '--label=org.pureeos.task=' + container_name, '--pull=never', '--network=none', '--read-only',
        '--log-driver=none', '--user=1000:1000', '--cpus=2', '--memory=2048m',
        '--memory-swap=2560m', '--pids-limit=256', '--tmpfs', '/tmp:rw,size=64m,uid=1000,gid=1000',
        '--env', 'TMPDIR=/work/tmp', '--env', 'PYTHONDONTWRITEBYTECODE=1',
        '--mount', f'type=bind,src={output},dst=/work',
        '--mount', f'type=bind,src={runner},dst=/runner.py,readonly',
        '--mount', f'type=bind,src={core}/build,dst=/build,readonly',
        '--mount', f'type=bind,src={contracts},dst=/contracts,readonly',
        '--workdir=/work', IMAGE, 'python3', '-I', '-B', '/runner.py', '--guest']
    launch = dict(status='running', container=container_name, started_utc=utc(), release_ready=False,
                  no_cloud_purchase=True, command=command)
    save(output / 'launch.json', launch)
    def interrupted(_sig, _frame): raise InterruptedError('Stop only this smoke lane')
    signal.signal(signal.SIGTERM, interrupted); signal.signal(signal.SIGINT, interrupted)
    process = None
    try:
        with (output / 'container-driver.log').open('x') as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            launch['container_exit_code'] = process.wait(timeout=1020)
    except Exception as error:
        launch.update(status='interrupted-or-failed', error=str(error))
    finally:
        if inspect_owned(container_name):
            subprocess.run(['sudo', '-n', 'docker', 'stop', '--time=15', container_name], check=True, timeout=40)
        if process is not None: process.wait(timeout=30)
    assert not inspect_owned(container_name), 'Smoke container must be absent before closure'
    result_path = output / 'report.json'
    if not result_path.is_file(): save(result_path, dict(status='failed', release_ready=False,
        error='Container ended before guest report'))
    result = json.loads(result_path.read_text())
    ok = launch.get('container_exit_code') == 0 and result.get('status') == passed_status
    assert not result.get('release_ready'), 'Smoke cannot certify a release'
    launch.update(status='finished-' + args.profile + '-not-release' if ok else 'failed', finished_utc=utc())
    save(output / 'monitor.json', launch)
    archive = output / 'smoke-evidence.tar.gz'
    with tarfile.open(archive, 'x:gz') as stream:
        for path in sorted(output.iterdir()):
            if path.is_file() and not path.is_symlink() and path.suffix in ('.json', '.log', '.py'):
                stream.add(path, arcname=path.name, recursive=False)
        for relative in fixtures:
            stream.add(output / relative, arcname=relative, recursive=False)
        for group in ('cli', 'sdk'):
            for n in range(1, args.rounds + 1):
                relative = f'{group}-{n}/report.json'
                if (output / relative).is_file(): stream.add(output / relative, arcname=relative, recursive=False)
    with tarfile.open(archive) as stream:
        for member in stream.getmembers():
            assert member.isfile() and not Path(member.name).is_absolute() and '..' not in Path(member.name).parts
            with stream.extractfile(member) as content:
                assert hashlib.file_digest(content, 'sha256').hexdigest() == sha(output / member.name)
    save(output / 'download-verified.json', dict(status='passed', smoke_status=result['status'],
        release_ready=False, bytes=archive.stat().st_size, sha256=sha(archive), finished_utc=utc()))
    print(json.dumps(dict(status=launch['status'], jobs=len(result.get('jobs', [])),
        elapsed_seconds=result.get('elapsed_seconds'), release_ready=False)), flush=True)
    return 0 if ok else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--guest', action='store_true')
    parser.add_argument('--profile', choices=('smoke', 'runtime'), default='smoke')
    parser.add_argument('--rounds', type=int, choices=(1, 3), default=1)
    parser.add_argument('--candidate', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.guest: return guest()
    assert args.candidate and args.output, '--candidate and --output required'
    return host(args)


if __name__ == '__main__': raise SystemExit(main())
