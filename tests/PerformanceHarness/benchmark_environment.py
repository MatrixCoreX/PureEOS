"""Read-only, bounded Linux evidence. Never dumps config files, keys or argv."""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
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


def read_numbers(path):
    result = {}
    for line in Path(path).read_text().splitlines():
        words = line.split()
        if len(words) >= 2 and words[1].isdigit():
            result[words[0].rstrip(':')] = int(words[1])
    return result


def sample(pids=()):
    started = time.monotonic_ns()
    result = {'at_ns': time.monotonic_ns(), 'loadavg': os.getloadavg(),
              'memory_kib': read_numbers('/proc/meminfo'), 'processes': {}}
    result['cpu_ticks'] = [int(v) for v in Path('/proc/stat').read_text().splitlines()[0].split()[1:]]
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
    result['collection_ns'] = time.monotonic_ns() - started
    return result


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
