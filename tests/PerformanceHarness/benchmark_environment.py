"""Read-only, bounded Linux evidence. Never dumps config files, keys or argv."""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time


def sha256(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def command(args):
    return subprocess.check_output(args, text=True, timeout=30).strip()


def filesystem(path):
    result = json.loads(command(['findmnt', '--json', '--target', str(path),
                                 '--output', 'SOURCE,FSTYPE,TARGET,OPTIONS']))
    return result['filesystems'][0]


def preflight(path, reserve_gib=8, working_gib=1):
    """Refuse RAM/overlay/network storage and preserve the existing 8 GiB guard."""
    path = Path(path).resolve(strict=True)
    fs = filesystem(path)
    if fs['fstype'] not in ('ext4', 'xfs', 'btrfs') or not fs['source'].startswith('/dev/'):
        raise ValueError('Real-disk benchmark requires a local block filesystem, not tmpfs/overlay/network storage')
    free = shutil.disk_usage(path).free
    if reserve_gib < 8 or working_gib < 1 or free < (reserve_gib + working_gib) * 2**30:
        raise ValueError('Insufficient real-disk free space: keep >=8 GiB reserve plus >=1 GiB working space')
    return {'filesystem': fs, 'free_bytes': free, 'reserve_bytes': reserve_gib * 2**30,
            'working_bytes': working_gib * 2**30,
            'note': 'Local block filesystem verified; SSD device identity still requires review'}


def wait_for_quiet_gate(gate, directory, attempts=6):
    """Wait only on the gate's explicit BUSY exit, before starting a sample.

    Every preflight is retained. Never retries a failed running benchmark,
    changes the gate threshold, or signals unrelated processes.
    """
    if not 1 <= attempts <= 6:
        raise ValueError('Quiet gate retry bound must be 1..6')
    directory = Path(directory)
    directory.mkdir()
    for attempt in range(1, attempts + 1):
        with (directory / f'attempt-{attempt}.jsonl').open('x') as output:
            ready = subprocess.run([sys.executable, '-B', str(gate)], stdout=output,
                                   stderr=subprocess.STDOUT, timeout=30)
        if ready.returncode == 0:
            return {'attempts': attempt, 'prior_busy_checks': attempt - 1}
        if ready.returncode != 75:
            raise RuntimeError('Quiet gate failed, not a BUSY condition')
        if attempt < attempts:
            time.sleep(5)
    raise RuntimeError('Quiet gate remained busy; no measurement started')


def read_numbers(path):
    result = {}
    for line in Path(path).read_text().splitlines():
        words = line.split()
        if len(words) >= 2 and words[1].isdigit():
            result[words[0].rstrip(':')] = int(words[1])
    return result


def process_cpu(path):
    """Read only counters/comm, never command lines, environment or keys."""
    text = (Path(path) / 'stat').read_text()
    prefix, separator, rest = text.rpartition(') ')
    if not separator or '(' not in prefix:
        raise ValueError('Malformed proc stat')
    fields = rest.split()
    return {'name': prefix.split('(', 1)[1], 'user_ticks': int(fields[11]),
            'system_ticks': int(fields[12]), 'start_ticks': int(fields[19])}


def scheduler_sample(pids, proc_root=Path('/proc')):
    """Optional diagnostic instrumentation; not a zero-overhead TPS sample.

    PID/TID start times identify reuse. Disappearing/inaccessible tasks are
    omitted, so totals are observed lower bounds, not exact host attribution.
    Kernel schedstats may be disabled; zero waits alone prove nothing.
    """
    root = Path(proc_root)
    result = {'process_cpu': {}, 'threads': {}, 'missed_processes': 0,
              'missed_threads': 0}
    for entry in root.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            result['process_cpu'][entry.name] = process_cpu(entry)
        except (OSError, ValueError, IndexError):
            result['missed_processes'] += 1
    for pid in pids:
        threads = result['threads'][str(pid)] = {}
        try:
            for entry in (root / str(pid) / 'task').iterdir():
                try:
                    counters = process_cpu(entry)
                    counters['schedstat'] = [int(v) for v in (entry / 'schedstat').read_text().split()]
                    status = read_numbers(entry / 'status')
                    counters['context_switches'] = {k: status.get(k) for k in
                        ('voluntary_ctxt_switches', 'nonvoluntary_ctxt_switches')}
                    threads[entry.name] = counters
                except (OSError, ValueError, IndexError):
                    result['missed_threads'] += 1
        except OSError:
            result['missed_threads'] += 1
    try:
        result['kernel_schedstats'] = int((root / 'sys/kernel/sched_schedstats').read_text())
    except (OSError, ValueError):
        result['kernel_schedstats'] = None
    return result


def sample(pids=(), include_scheduler=False):
    pids = tuple(pids)
    started = time.monotonic_ns()
    result = {'at_ns': time.monotonic_ns(), 'loadavg': os.getloadavg(),
              'memory_kib': read_numbers('/proc/meminfo'), 'processes': {},
              'clock_ticks_per_second': os.sysconf('SC_CLK_TCK')}
    result['cpu_ticks'] = [int(v) for v in Path('/proc/stat').read_text().splitlines()[0].split()[1:]]
    # Keep host and process CPU counters adjacent. Sysfs sensors and diskstats
    # can take tens of milliseconds; placing them between these reads makes
    # busy multi-worker processes appear to exceed the host's CPU delta.
    for pid in pids:
        try:
            fields = Path(f'/proc/{int(pid)}/stat').read_text().rpartition(') ')[2].split()
            result['processes'][str(pid)] = {'user_ticks': int(fields[11]), 'system_ticks': int(fields[12]),
                'start_ticks': int(fields[19]), 'rss_pages': int(fields[21]),
                'minor_faults': int(fields[7]), 'major_faults': int(fields[9]),
                'io': read_numbers(f'/proc/{int(pid)}/io')}
            status = read_numbers(f'/proc/{int(pid)}/status')
            result['processes'][str(pid)]['context_switches'] = {k: status.get(k) for k in
                ('voluntary_ctxt_switches', 'nonvoluntary_ctxt_switches')}
        except (OSError, ValueError, IndexError):
            result['processes'][str(pid)] = {'unavailable': True}
    result['cpu_snapshot_collection_ns'] = time.monotonic_ns() - started
    result['diskstats'] = Path('/proc/diskstats').read_text().splitlines()
    result['temperature_millidegrees'] = {}
    result['frequency_khz'] = {}
    for pattern, field in (('/sys/class/hwmon/hwmon*/temp*_input', 'temperature_millidegrees'),
                           ('/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq', 'frequency_khz')):
        import glob
        for name in glob.glob(pattern):
            try:
                result[field][name] = int(Path(name).read_text())
            except (OSError, ValueError):
                pass  # Missing sensors are visible as empty, not invented zeroes.
    if include_scheduler:
        result['scheduler'] = scheduler_sample(pids)
        result['clock_ticks_per_second'] = os.sysconf('SC_CLK_TCK')
    result['collection_ns'] = time.monotonic_ns() - started
    return result


def runtime_cpu_evidence(samples):
    """Conservative performance-quality flag, independent of chain correctness.

    Predeclared limits: mean non-node work <=0.5 logical cores, every sampled
    interval <=1 core, no internal evidence gaps >3 seconds. Includes OS,
    sender and monitor work; it cannot identify a particular background app.
    Observation includes startup/warmup/drain, NOT an exact transaction window.
    Never drops slow samples or calls noisy-but-correct execution a chain bug.
    """
    samples = list(samples)
    result = {'status': 'inconclusive', 'mean_limit_cores': .5, 'interval_limit_cores': 1.0,
              'max_interval_seconds': 3.0, 'intervals': 0, 'missing_intervals': 0,
              'scope': 'sampled host busy minus observed node CPU; includes OS/sender/monitor; '
                       'whole monitoring interval, not exact replay or finality window'}
    usable = [i for i, s in enumerate(samples) if s.get('processes') and
              all(not p.get('unavailable') for p in s['processes'].values())]
    if len(usable) < 2:
        result['reason'] = 'insufficient node-alive samples'
        return result
    # Ignore only startup/shutdown edges without a live monitored node. Missing
    # intervals INSIDE the live span make performance evidence inconclusive.
    samples = samples[usable[0]:usable[-1] + 1]
    wall = other = peak = 0.0
    for before, after in zip(samples, samples[1:]):
        try:
            seconds = (after['at_ns'] - before['at_ns']) / 1e9
            hz = before['clock_ticks_per_second']
            if not 0 < seconds <= 3 or hz <= 0 or hz != after['clock_ticks_per_second']:
                raise ValueError('invalid interval/clock')
            a, b = before['cpu_ticks'], after['cpu_ticks']
            if len(a) < 8 or len(b) < 8:
                raise ValueError('missing host counters')
            ticks = [b[i] - a[i] for i in range(8)]  # guest already included in user/nice
            if min(ticks) < 0:
                raise ValueError('host counters reset')
            host_busy = sum(ticks) - ticks[3] - ticks[4]  # exclude idle and IO wait
            if not before['processes'] or before['processes'].keys() != after['processes'].keys():
                raise ValueError('monitored process set changed')
            node = 0
            for pid, p in before['processes'].items():
                q = after['processes'][pid]
                if p.get('unavailable') or q.get('unavailable') or p['start_ticks'] != q['start_ticks']:
                    raise ValueError('missing or reused PID')
                delta = [q[k] - p[k] for k in ('user_ticks', 'system_ticks')]
                if min(delta) < 0:
                    raise ValueError('node counters reset')
                node += sum(delta)
            # /proc reads are not atomic. Tolerate at most two scheduler ticks
            # per node, not arbitrarily inconsistent attribution.
            if host_busy - node < -2 * len(before['processes']):
                raise ValueError('inconsistent host/node accounting')
            non_node = max(0, host_busy - node) / hz
            wall += seconds
            other += non_node
            peak = max(peak, non_node / seconds)
            result['intervals'] += 1
        except (KeyError, TypeError, ValueError):
            result['missing_intervals'] += 1
    if not wall:
        result['reason'] = 'no complete CPU intervals'
        return result
    mean = other / wall
    result.update(observed_seconds=wall, non_node_cpu_seconds=other,
                  mean_non_node_cores=mean, max_interval_non_node_cores=peak)
    if result['missing_intervals']:
        result['reason'] = 'incomplete runtime CPU evidence'
    elif mean > .5 or peak > 1:
        result['reason'] = 'runtime background/OS/instrumentation load exceeds predeclared limit'
    else:
        result['status'] = 'eligible'
        result['reason'] = 'runtime CPU noise gate passed; not proof of a dedicated host'
    return result


def active_runtime_cpu_evidence(samples, start_ns, end_ns):
    """Predeclared ready-protocol active span, with COMPLETE bracketing intervals.

    Includes warmup, sending and the entire drain/last observation. Never trims
    an interior slow/noisy interval. Keep whole-lifecycle evidence separately;
    this policy does not upgrade any historical unphased measurement.
    """
    samples = list(samples)
    unavailable = {'status': 'inconclusive', 'scope': 'ready-v1 active span plus complete boundary intervals',
                   'mean_limit_cores': .5, 'interval_limit_cores': 1.0, 'max_interval_seconds': 3.0,
                   'active_start_ns': start_ns, 'active_end_ns': end_ns}
    if type(start_ns) is not int or type(end_ns) is not int or not 0 < start_ns < end_ns:
        return {**unavailable, 'reason': 'invalid declared active boundaries'}
    times = [row.get('at_ns') for row in samples]
    if (not times or any(type(value) is not int for value in times) or
            any(a >= b for a, b in zip(times, times[1:]))):
        return {**unavailable, 'reason': 'missing, unordered or duplicate host samples'}
    before = [i for i, value in enumerate(times) if value <= start_ns]
    after = [i for i, value in enumerate(times) if value >= end_ns]
    if not before or not after:
        return {**unavailable, 'reason': 'host samples do not bracket the complete active span'}
    first, last = before[-1], after[0]
    selected = samples[first:last + 1]
    # runtime_cpu_evidence may trim dead process edges of a generic lifecycle.
    # That must NEVER trim part of this explicitly declared active span.
    if any(not row.get('processes') or any(p.get('unavailable') for p in row['processes'].values())
           for row in selected):
        return {**unavailable, 'reason': 'node unavailable inside active-span brackets'}
    return {**runtime_cpu_evidence(selected), 'scope': unavailable['scope'],
            'active_start_ns': start_ns, 'active_end_ns': end_ns,
            'bracket_start_ns': times[first], 'bracket_end_ns': times[last],
            'source_samples': len(samples), 'selected_samples': len(selected)}


def check_host_health(data, temperature_limit=95000):
    temperatures = data['temperature_millidegrees'].values()
    if temperatures and max(temperatures) >= temperature_limit:
        raise ValueError('Host temperature exceeds predeclared benchmark limit; run is invalid')
    if data['memory_kib'].get('MemAvailable', 0) < 1024 * 1024:
        raise ValueError('Less than 1 GiB available memory; run is invalid')


def manifest(repo, build, artifacts, public_profile):
    repo, build = Path(repo), Path(build)
    sources = {}
    paths = command(['git', '-C', str(repo), 'ls-files', '-c', '-o', '--exclude-standard']).splitlines()
    # Record submodule commits as well as their current source bytes, including
    # local edits; a parent gitlink alone does not identify a dirty dependency.
    submodules = command(['git', '-C', str(repo), 'submodule', 'status', '--recursive'])
    for line in submodules.splitlines():
        words = line.split()
        if len(words) >= 2 and not line.startswith('-'):
            subdir = words[1]
            paths += [str(Path(subdir) / p) for p in command([
                'git', '-C', str(repo / subdir), 'ls-files', '-c', '-o', '--exclude-standard']).splitlines()]
    roots = {'libraries', 'plugins', 'programs', 'tests', 'unittests', 'scripts', 'genesis', 'cmake', 'CMakeModules'}
    for name in sorted(set(paths)):
        path = repo / name
        if (len(path.relative_to(repo).parts) == 1 or name.split('/')[0] in roots) and path.is_file():
            if path.suffix in ('.cpp', '.cc', '.c', '.S', '.s', '.asm', '.hpp', '.h', '.inc', '.in', '.def',
                               '.py', '.sh', '.cmake', '.txt', '.md', '.json'):
                sources[name] = sha256(path)
    cache = {}
    for line in (build / 'CMakeCache.txt').read_text().splitlines():
        if '=' not in line or ':' not in line or line.startswith(('#', '//')):
            continue
        key, value = line.split('=', 1)
        name = key.split(':')[0]
        if name in {'CMAKE_BUILD_TYPE', 'CMAKE_CXX_COMPILER', 'CMAKE_CXX_FLAGS', 'CMAKE_CXX_FLAGS_RELEASE',
                    'ENABLE_OC', 'BUILD_TESTING', 'CMAKE_INTERPROCEDURAL_OPTIMIZATION'}:
            cache[name] = value
    return {'schema': 'pureeos-benchmark-environment-v1', 'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
            'platform': platform.platform(), 'cpu': command(['lscpu', '--json']),
            'compiler': command([cache.get('CMAKE_CXX_COMPILER', 'c++'), '--version']),
            'git_head': command(['git', '-C', str(repo), 'rev-parse', 'HEAD']),
            'submodules': submodules, 'source_scope': 'Source checkout at measurement time; verify against compiler dependency/build evidence before attributing a prebuilt binary',
            'source_sha256': sources, 'build': cache,
            'artifacts': {name: {'path': str(path), 'sha256': sha256(path)} for name, path in artifacts.items()},
            'profile': public_profile, 'initial': sample(),
            'secrets': 'No config contents, signing keys, environment variables or process argv collected'}


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--artifact', action='append', default=[], help='NAME=/absolute/path; repeatable')
    parser.add_argument('--output', type=Path, required=True, help='NEW public evidence JSON')
    args = parser.parse_args()
    artifacts = {}
    for pair in args.artifact:
        name, separator, path = pair.partition('=')
        if not separator or not name or name in artifacts:
            parser.error('Artifacts must have unique NAME=PATH entries')
        artifacts[name] = Path(path).resolve(strict=True)
    document = manifest(args.repo.resolve(strict=True), args.build.resolve(strict=True), artifacts,
                        {'scope': 'Build/regression evidence only; no live-chain capacity measurement'})
    with args.output.open('x') as output:
        json.dump(document, output, indent=2)
