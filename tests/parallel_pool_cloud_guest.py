"""One-shot N4 component relink, using existing frozen build objects read-only."""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import traceback

ROOT = Path('/opt/pureeos-v1-hardening-20261022-n4')
CACHE = Path('/opt/pureeos-unified-TBMyRV-compiler-cache-after-sanitizers-v2')
OLD_BUILD = '/opt/pureeos-unified-TBMyRV/clock-v3/native-build'


def sha(path):
    with Path(path).open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    report = dict(status='running', scope='New worker-pool implementation and actual WASM cache regression; cached frozen core relink, not new full node or TPS',
                  started_utc=datetime.now(timezone.utc).isoformat(), jobs=[])
    def save(): (ROOT / 'report.json').write_text(json.dumps(report, indent=2))
    def run(name, command, timeout=360):
        log = ROOT / (name + '.log')
        with log.open('x') as stream:
            result = subprocess.run(list(map(str, command)), cwd=CACHE,
                env={**os.environ, 'CCACHE_DISABLE': '1'}, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
        report['jobs'].append(dict(name=name, exit_code=result.returncode, log=str(log), command=list(map(str, command))))
        save()
        if result.returncode: raise RuntimeError(name + ' failed; later rounds not run')
    try:
        assert not (ROOT / 'report.json').exists(), 'One shot; never replace previous evidence'
        plan = json.loads((ROOT / 'input.json').read_text())
        for name, expected in plan['sha256'].items(): assert sha(ROOT / name) == expected, name
        commands = json.loads((CACHE / 'compile_commands.json').read_text())
        inputs = {}
        def compile_one(label, new_source=None):
            row = next(v for v in commands if v['file'].endswith('/' + label))
            original = Path(row['file']); inputs[str(original)] = sha(original)
            if label in plan['baseline_sha256']:
                assert inputs[str(original)] == plan['baseline_sha256'][label], 'Frozen source is not the local baseline'
            args = shlex.split(row['command'])
            args = [arg.replace(OLD_BUILD, str(CACHE)) for arg in args
                    if arg != '-fmodules-ts' and arg != '-MD' and not arg.startswith(('-fmodule-mapper=', '-fdeps-format='))]
            target = ROOT / (label + '.o')
            args[args.index('-o') + 1] = str(target)
            if new_source: args[args.index('-c') + 1] = str(ROOT / new_source)
            args.insert(1, '-I' + str(ROOT / 'libraries/chain/include'))
            run('compile-' + label, args)
            return target
        pool = compile_one('parallel_worker_pool.cpp', 'libraries/chain/parallel_worker_pool.cpp')
        tests = compile_one('parallel_module_cache_tests.cpp')
        link = subprocess.check_output(['ninja', '-C', str(CACHE), '-t', 'commands', 'native_savanna_test'], text=True).splitlines()[-1]
        args = shlex.split(link)
        assert args[:2] == [':', '&&'] and args[-2:] == ['&&', ':']
        args = [arg for arg in args[2:-2] if not arg.startswith('-Wl,--dependency-file=')]
        args = [arg.replace(OLD_BUILD, str(CACHE)) for arg in args]
        program = ROOT / 'pool-cache-regression.elf'
        args[args.index('-o') + 1] = str(program)
        args[1:1] = [str(pool), str(tests)]
        # Reused object/archive bytes are part of the provenance. The linker
        # may read them, but no command writes the frozen cache or old ELF.
        for arg in args[3:]:
            p = Path(arg)
            if p.suffix in ('.o', '.a'):
                p = p if p.is_absolute() else CACHE / p
                inputs[str(p)] = sha(p)
        report['read_only_inputs_sha256'] = inputs
        run('link-new-pool-component', args)
        report['program_sha256'] = sha(program)
        for round_number in range(1, 4):
            run(f'cache-round-{round_number}', [program, '--run_test=parallel_module_cache_tests',
                '--report_level=detailed', '--color_output=no'], 180)
        assert all(sha(p) == expected for p, expected in inputs.items()), 'Read-only source/cache changed'
        assert all(sha(ROOT / p) == expected for p, expected in plan['sha256'].items()), 'N4 inputs changed'
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error), traceback=traceback.format_exc())
    finally:
        report['finished_utc'] = datetime.now(timezone.utc).isoformat(); save()
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
