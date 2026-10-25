"""Actual owner-local access hint component; normal/ASan/UBSan/TSan, 3 rounds."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    args.output.mkdir(exist_ok=False)
    inputs = [Path(__file__), source / 'tests/parallel_access_hints_test.cpp',
        source / 'libraries/chain/include/eosio/chain/parallel_access_hints.hpp']
    def hashes(): return {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    report = dict(status='running', inputs_sha256=hashes(), jobs=[],
        started_utc=datetime.now(timezone.utc).isoformat(), scope='Actual access hints component, not controller integration or TPS')
    env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1',
        'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1', 'TSAN_OPTIONS': 'halt_on_error=1'}
    def run(name, command):
        with (args.output / (name + '.log')).open('x') as log:
            result = subprocess.run(list(map(str, command)), env=env, stdout=log, stderr=subprocess.STDOUT, timeout=90)
        report['jobs'].append(dict(name=name, exit_code=result.returncode, command=list(map(str, command))))
        (args.output / 'report.json').write_text(json.dumps(report, indent=2))
        print(name + ': exit=' + str(result.returncode), flush=True)
        if result.returncode: raise RuntimeError(name + ' failed; later rounds not run')
    try:
        for mode, flags in [('normal', ['-O2']), ('asan-ubsan', ['-O1', '-g', '-fno-omit-frame-pointer',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all']), ('tsan', ['-O1', '-g', '-fsanitize=thread'])]:
            program = args.output / (mode + '.elf')
            run('compile-' + mode, ['g++', '-std=c++20', '-pthread', *flags, '-I' + str(source / 'libraries/chain/include'),
                source / 'tests/parallel_access_hints_test.cpp', '-o', program])
            for n in range(1, 4): run(f'{mode}-round-{n}', [program])
        assert hashes() == report['inputs_sha256'], 'Source changed during regression'
        report['status'] = 'passed'
    except Exception as error: report.update(status='failed', error=str(error))
    report['finished_utc'] = datetime.now(timezone.utc).isoformat()
    (args.output / 'report.json').write_text(json.dumps(report, indent=2))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__': raise SystemExit(main())
