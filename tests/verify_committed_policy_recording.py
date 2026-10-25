"""One-shot observer replay of preserved block-log evidence; no chain mutation."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import traceback

from pureeos_committed_policy_observer import CommittedPolicyObserver


def sha(path):
    with Path(path).open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--utility', type=Path, required=True)
    parser.add_argument('--blocks-dir', type=Path, required=True)
    parser.add_argument('--failed-report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(exist_ok=False)
    report = dict(status='running', scope='Replay the new observer against original failed-run ledger; NOT a new 40-node run or signature audit',
        started_utc=datetime.now(timezone.utc).isoformat(), production_code_changed=False, original_failure_reclassified=False)
    paths = [args.utility, args.failed_report, args.blocks_dir / 'blocks.log', args.blocks_dir / 'blocks.index',
             Path(__file__), Path(__file__).with_name('pureeos_committed_policy_observer.py')]
    report['inputs_sha256'] = {str(p): sha(p) for p in paths}
    try:
        failed = json.loads(args.failed_report.read_text())
        assert failed['status'] == 'failed'
        report['chain_id'] = failed['chain_id']
        base = [str(args.utility), 'block-log', '--blocks-dir', str(args.blocks_dir)]
        genesis = json.loads(subprocess.check_output([*base, 'genesis'], text=True, stderr=subprocess.PIPE))
        observer = CommittedPolicyObserver(genesis, failed['chain_id'])
        with (args.output / 'blocks.json').open('x') as output, (args.output / 'print-log.stderr').open('x') as errors:
            subprocess.run([*base, 'print-log', '--first', '1', '--as-json-array', '--no-pretty-print',
                            '--print-from=block_log'], stdout=output, stderr=errors, check=True, timeout=90)
        blocks = json.loads((args.output / 'blocks.json').read_text())
        events = []
        for block in blocks:
            event = observer.feed(block, blocks[-1]['block_num'])
            if event: events.append(event)
        assert [e['schedule']['version'] for e in events] == list(range(events[-1]['schedule']['version'] + 1))
        by_version = {e['schedule']['version']: e for e in events}
        for expected in failed['committee_events']:
            # Sampled RPC can expose the next pending block; compare the full
            # policy content by verified sequential version, not sampled head.
            assert by_version[expected['schedule']['version']]['schedule'] == expected['schedule']
        repaired = [by_version[v] for v in (51, 52, 53)]
        assert [e['head'] for e in repaired] == [33178, 33934, 34690]
        assert all(len(e['schedule']['producers']) == 21 and e['rotation']['incoming'] and e['rotation']['outgoing'] for e in repaired)
        assert all(sha(p) == expected for p, expected in report['inputs_sha256'].items())
        report.update(status='passed', source_blocks=len(blocks), verified_blocks=observer.verified_blocks,
            last_block=observer.number, last_block_id=observer.block_id,
            observed_versions=[e['schedule']['version'] for e in events], events=events,
            recovered_gap_versions=[e['schedule']['version'] for e in repaired],
            recorded_blocks_sha256=sha(args.output / 'blocks.json'))
    except Exception as error:
        report.update(status='failed', error=str(error), traceback=traceback.format_exc())
    report['finished_utc'] = datetime.now(timezone.utc).isoformat()
    with (args.output / 'report.json').open('x') as stream: json.dump(report, stream, indent=2)
    print(json.dumps({k: v for k, v in report.items() if k not in ('events', 'inputs_sha256')}, indent=2))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__': raise SystemExit(main())
