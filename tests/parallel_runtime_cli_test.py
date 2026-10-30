#!/usr/bin/env python3
"""Check the new binary's local parallel limits without starting a chain.

Each command uses a new, explicit data/config directory. No wallet, running
node, old binary, migration or public endpoint is involved. Invalid shapes
must fail at configuration validation, not merely fail for some other reason.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nodeos', type=Path, required=True)
    parser.add_argument('--work-dir', type=Path, required=True, help='Must not already exist')
    args = parser.parse_args()
    program = args.nodeos.resolve(strict=True)
    work = args.work_dir.resolve()
    work.mkdir()  # Never reuse a manual chain or overwrite a previous run.
    with program.open('rb') as binary:
        program_hash = hashlib.file_digest(binary, 'sha256').hexdigest()
    report = {'schema': 'pureeos-parallel-runtime-cli-v1', 'binary_sha256': program_hash,
              'scope': 'configuration only; no chain execution or TPS evidence', 'cases': [], 'status': 'running'}

    def run(label, options, expected=None):
        directory = work / label
        directory.mkdir()
        command = [str(program), '--data-dir', str(directory / 'data'),
                   '--config-dir', str(directory / 'config'),
                   '--http-server-address', '127.0.0.1:0', '--p2p-listen-endpoint', '127.0.0.1:0', *options]
        entry = {'label': label, 'options': options, 'started_ns': time.monotonic_ns()}
        report['cases'].append(entry)
        with (directory / 'output.log').open('x') as output:
            try:
                process = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, timeout=30)
            except subprocess.TimeoutExpired:
                entry['status'] = 'timeout'
                raise
            finally:
                entry['finished_ns'] = time.monotonic_ns()
        entry['returncode'] = process.returncode
        text = ' '.join((directory / 'output.log').read_text().split())
        if expected is None:
            valid = process.returncode == 0
        else:
            valid = process.returncode != 0 and expected in text
        entry['status'] = 'passed' if valid else 'failed'
        if not valid:
            raise RuntimeError(f'{label}: unexpected exit or configuration diagnostic; see output.log')
        return text

    try:
        help_text = run('help', ['--help'])
        workers_match = re.search(r'Restricted parallel block validation workers \(0 disables, maximum (\d+)\)', help_text)
        batch_match = re.search(r'Maximum transactions per restricted validation batch \(1-(\d+)\)', help_text)
        if not workers_match or not batch_match:
            raise RuntimeError('parallel limits missing from CLI help')
        workers, batch = int(workers_match[1]), int(batch_match[1])
        if workers != 128 or batch != 256:
            raise RuntimeError('unexpected release limits')
        report['reported_limits'] = {'workers': workers, 'batch_transactions': batch}
        bounds_error = f'experimental parallel validation requires threads 0-{workers} and batch size 1-{batch}'
        run('too-many-workers', ['--experimental-parallel-validation-threads', str(workers + 1)], bounds_error)
        run('empty-batch', ['--experimental-parallel-validation-batch-size', '0'], bounds_error)
        run('too-large-batch', ['--experimental-parallel-validation-batch-size', str(batch + 1)], bounds_error)
        for value in ['-1', 'Auto', '32x', '18446744073709551616']:
            run('invalid-workers-' + value, ['--experimental-parallel-validation-threads', value],
                'parallel workers must be auto or an unsigned decimal integer')
            run('invalid-batch-' + value, ['--experimental-parallel-validation-batch-size', value],
                'parallel batch must be auto or an unsigned decimal integer')
        # Stop at the unchanged allowlist guard after resolving valid settings.
        # This proves parser integration without creating or executing a chain.
        guard = 'experimental parallel validation requires full validation, a reviewed code hash'
        common = ['--wasm-runtime', 'eos-vm', '--experimental-parallel-engine', 'interpreter']
        for count in [32, 64, workers]:
            run(f'manual-workers-{count}', common + ['--experimental-parallel-validation-threads', str(count),
                '--experimental-parallel-validation-batch-size', str(batch)], guard)
        for label, extra, expected_batch in [('auto-default-batch', [], None),
                ('auto-explicit-batch', ['--experimental-parallel-validation-batch-size', '17'], 17),
                ('auto-both', ['--experimental-parallel-validation-batch-size', 'auto'], None),
                ('manual-auto-batch', ['--experimental-parallel-validation-batch-size', 'auto'], batch)]:
            count = str(workers) if label == 'manual-auto-batch' else 'auto'
            text = run(label, common + ['--experimental-parallel-validation-threads', count, *extra], guard)
            resolved = re.search(r'usable cores=(\d+), quota known=\w+; workers=(\d+), batch=(\d+)', text)
            if not resolved:
                raise RuntimeError('automatic sizing diagnostic missing')
            usable, chosen, actual_batch = map(int, resolved.groups())
            expected_workers = workers if count != 'auto' else min(usable, workers)
            if chosen != expected_workers or actual_batch != (expected_batch or min(3 * chosen, batch)):
                raise RuntimeError('automatic sizing integration disagrees with policy')
        run('production-without-workers', ['--experimental-parallel-production', '--experimental-parallel-validation-threads', '0'],
            'experimental parallel production requires nonzero parallel validation threads')
        run('invalid-engine', ['--experimental-parallel-engine', 'automatic'],
            'experimental parallel engine must be interpreter or jit')
        for engine, canonical in [('interpreter', 'eos-vm-jit'), ('jit', 'eos-vm')]:
            run(f'mismatched-{engine}', ['--experimental-parallel-validation-threads', '1',
                '--experimental-parallel-engine', engine, '--wasm-runtime', canonical,
                '--experimental-parallel-code-hash', '0' * 64],
                'parallel engine must match wasm-runtime: interpreter/eos-vm or jit/eos-vm-jit')
        report['status'] = 'passed'
    except Exception as error:
        report['status'] = 'failed'
        report['error_type'] = type(error).__name__
    with (work / 'report.json').open('x') as output:
        json.dump(report, output, indent=2)
        output.write('\n')
    print(json.dumps({'status': report['status'], 'cases': len(report['cases']), 'report': str(work / 'report.json')}))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
