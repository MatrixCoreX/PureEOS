"""Fixed three-round native checks for the actual per-worker signal helper."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    inputs = [Path(__file__), source / 'tests/parallel_worker_signals_test.cpp',
              source / 'libraries/chain/include/eosio/chain/parallel_worker_signals.hpp',
              source / 'libraries/chain/parallel_worker_pool.cpp']
    def hashes():
        return {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    report = dict(status='running', scope='Actual signal helper, not complete VM pool/controller/node integration or TPS',
                  started_utc=datetime.now(timezone.utc).isoformat(), inputs_sha256=hashes(), jobs=[])
    env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1',
           'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1', 'TSAN_OPTIONS': 'halt_on_error=1'}
    def run(name, command):
        log = output / (name + '.log')
        with log.open('x') as stream:
            result = subprocess.run(list(map(str, command)), env=env, stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=60)
        report['jobs'].append(dict(name=name, exit_code=result.returncode, log=str(log), command=list(map(str, command))))
        print(f'{name}: exit={result.returncode}', flush=True)
        (output / 'report.json').write_text(json.dumps(report, indent=2))
        if result.returncode: raise RuntimeError(name + ' failed; later rounds not run')
    try:
        for mode, flags in [('normal', ['-O2']), ('asan-ubsan', ['-O1', '-g', '-fno-omit-frame-pointer',
                                                                 '-fsanitize=address,undefined', '-fno-sanitize-recover=all']),
                            ('tsan', ['-O1', '-g', '-fsanitize=thread'])]:
            program = output / (mode + '.elf')
            run('compile-' + mode, ['g++', '-std=c++20', '-pthread', *flags,
                '-I' + str(source / 'libraries/chain/include'), source / 'tests/parallel_worker_signals_test.cpp', '-o', program])
            for round_number in range(1, 4):
                run(f'{mode}-round-{round_number}', [program])
        assert hashes() == report['inputs_sha256'], 'Source changed during checks'
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error))
    finally:
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        (output / 'report.json').write_text(json.dumps(report, indent=2))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
