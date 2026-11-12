"""One-shot actual-WASM eviction regression and pre-fix negative control.

Reuses a hashed native test executable; does not compile/mutate native core,
change production periods, create sockets or purchase/extend cloud hosts.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import traceback


CASES = (
    'grows_one_at_a_time_then_fifo_across_vote_changes_and_restart',
    'offline_entrant_three_finalized_windows_ban_and_single_seat_replacement',
    'no_alternative_keeps_nonempty_committee_and_no_automatic_ban_under21',
    'key_change_during_handoff_waits_then_updates_without_changing_seats',
    'small_committees_quorum_loss_and_recovery_under_rotation_profile',
    'rotation_committee_six_offline_seven_offline_and_recovery',
    'producing_without_new_finalizer_quorum_cannot_complete_handoff',
    'ineligible_incumbent_replaced_one_at_a_time_before_growth',
    'two_empty_windows_recovery_resets_and_global_stalls_do_not_mass_ban',
    'twenty_members_many_missed_windows_never_create_penalties',
    'incumbent_fault_seed_17_one_offline',
    'incumbent_fault_seed_2027_two_offline',
    'incumbent_fault_seed_65537_three_offline',
    'under21_offline_entrant_is_replaced_without_blacklist_or_growth',
    'under21_global_pause_and_no_replacement_preserve_membership_until_late_candidate',
    'alternating_incumbent_misses_cannot_veto_all_rotations',
)


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--program', type=Path, required=True)
    parser.add_argument('--fixed-contracts', type=Path, required=True)
    parser.add_argument('--old-contracts', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--select', help='Explicit narrower diagnostic selection; recorded in report')
    args = parser.parse_args()
    args.output.mkdir(exist_ok=False)
    temporary = args.output / 'tmp'; temporary.mkdir()
    report = dict(status='running', started_utc=datetime.now(timezone.utc).isoformat(), jobs=[],
        scope='Real system WASM plus frozen native core; no new N1/N4 or independent P2P/TPS claim',
        release_ready=False)
    root = Path(__file__).resolve().parents[1]
    inputs = [Path(__file__), args.program, root/'tests/native_policy_boundary_fixture.cpp',
        root/'unittests/pureeos_governance_tests.cpp', *root.joinpath('unittests').glob('pureeos_*.inc')]
    for contracts in (args.old_contracts, args.fixed_contracts):
        inputs += list(contracts.rglob('*.wasm')) + list(contracts.rglob('*.abi'))
    report['inputs_sha256'] = {str(p.resolve()): sha(p) for p in inputs}
    def save(): (args.output/'report.json').write_text(json.dumps(report, indent=2))
    def run(name, selected, contracts, negative=False):
        log = args.output/(name+'.log')
        command = [str(args.program.resolve()), '--run_test='+selected,
                   '--report_level=detailed', '--color_output=no', '--log_level=test_suite']
        with log.open('x') as stream:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=3600,
                env={**os.environ, 'TMPDIR': str(temporary.resolve()),
                     'PUREEOS_TEST_ROTATION_DIR': str(contracts.resolve())})
        record = dict(name=name, selection=selected, contracts=str(contracts.resolve()),
                      exit_code=result.returncode, log=str(log), expected_failure=negative)
        report['jobs'].append(record); save()
        if negative:
            text = log.read_text()
            assert result.returncode > 0, 'Old contract incorrectly passed the new eviction rule'
            marker = 'rotation did not reach expected state: '
            rows = [json.JSONDecoder().raw_decode(line.split(marker, 1)[1].lstrip())[0]
                    for line in text.splitlines() if marker in line]
            assert len(rows) == 1, 'Negative control failed for a different reason; inspect log'
            state = rows[0]
            # blocked is a per-onblock diagnostic, reset while a fresh bounded
            # scan runs; it need not still be 2 at the final sample. Verify the
            # actual permanent-ban/unchanged-policy boundary instead.
            assert (state['stage'] == 0 and len(state['active']) == 21 and
                    state['total_banned'] == 1 and not state['target'] and
                    state['completed'] == 21 and state['proposer_version'] == 21), \
                'Expected exactly the old banned-seat/no-replacement state, not an unrelated failure'
            record['old_boundary_state'] = state
            record['expected_old_no_replacement_failure_verified'] = True
        else:
            assert result.returncode == 0, name + ' failed; preserved log must be diagnosed'
        print(name + ': ' + ('expected failure verified' if negative else 'passed'), flush=True)
        save()
    save()
    try:
        run('old-policy-negative-control', 'pureeos_blacklist_current_policy_tests', args.old_contracts, True)
        selected = args.select or 'pureeos_sequential_rotation_tests/' + ','.join(CASES)
        run('fixed-policy-regression', selected, args.fixed_contracts)
        assert all(sha(p)==expected for p,expected in report['inputs_sha256'].items()), 'Frozen inputs changed'
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error), traceback=traceback.format_exc())
    report['finished_utc'] = datetime.now(timezone.utc).isoformat(); save()
    print(json.dumps({k:v for k,v in report.items() if k!='inputs_sha256'}, indent=2))
    return 0 if report['status']=='passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
