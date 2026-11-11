"""Apply the exact latest-user-authorized stopped-node runtime-log deletion plan."""
import datetime
import fcntl
import hashlib
import json
import os
import stat
import subprocess
import time
from pathlib import Path

BASE = Path('/opt/pureeos-testnet-disk-cleanup-20261111-r1')
R = BASE / 'delete-r2'
OPS = Path('/opt/pureeos-testnet-new-ban-20261111-r1')
NATIVE = Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/libexec/nodeos')
os.umask(0o077)

def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()

def save(name, value):
    p = R / name
    tmp = p.with_suffix('.tmp')
    with tmp.open('w') as f:
        f.write(json.dumps(value, ensure_ascii=False, indent=2) + '\n')
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, p)

def state(unit):
    raw = subprocess.check_output(['systemctl', 'show', unit, '-p', 'MainPID,ActiveState,UnitFileState'], text=True)
    return dict(line.split('=', 1) for line in raw.splitlines() if '=' in line)

def matches(m):
    p = Path(m['path'])
    if p.is_symlink() or not p.exists():
        return False
    s = p.lstat()
    return (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_mode, s.st_nlink) == tuple(m[k] for k in ['dev', 'ino', 'size', 'mtime_ns', 'mode', 'nlink'])

def references(paths):
    found = []
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit() or int(proc.name) == os.getpid():
            continue
        try:
            for fd in (proc / 'fd').iterdir():
                try:
                    target = os.readlink(fd)
                except OSError:
                    continue
                if target in paths:
                    found.append({'pid': int(proc.name), 'path': target})
        except OSError:
            pass
    return found

def protected(parents, excluded):
    for parent in sorted(parents):
        for base, dirs, files in os.walk(parent, followlinks=False):
            dirs[:] = sorted(name for name in dirs if name not in {'private', 'wallet', 'keystore'} and not (Path(base) / name).is_symlink())
            for name in sorted(files):
                p = Path(base) / name
                if str(p) in excluded or p.is_symlink() or not p.is_file():
                    continue
                s = p.lstat()
                yield {'path': str(p), 'dev': s.st_dev, 'ino': s.st_ino, 'size': s.st_size,
                       'allocated': s.st_blocks * 512, 'mtime_ns': s.st_mtime_ns,
                       'mode': s.st_mode, 'nlink': s.st_nlink}

assert not (R / 'DELETE_STARTED.json').exists(), 'Never blindly repeat log deletion'
assert state('pureeos-testnet-disk-cleanup-20261111.service')['MainPID'] == '0'
plan = json.loads((R / 'PLAN.json').read_text())
assert plan['status'] == 'PREPARED_USER_AUTHORIZED_DIRECT_DELETE'
deadline = time.monotonic() + 120
while True:
    health = json.loads((OPS / 'HEALTH_LATEST.json').read_text())
    if health['status'] == 'OK' and health['running_BP_count'] == 25:
        break
    assert time.monotonic() < deadline, 'Wait for healthy native chain before cleanup'
    time.sleep(5)
for unit in plan['retired_current_units'].values():
    s = state(unit)
    assert s['MainPID'] == '0' and s['UnitFileState'] == 'disabled'
for m in plan['logs']:
    assert matches(m) and stat.S_ISREG(m['mode']) and m['nlink'] == 1
refs = references({m['path'] for m in plan['logs']})
assert not refs, refs
before = os.statvfs('/')
save('DELETE_STARTED.json', {'utc': now(), 'user_override': plan['user_override'], 'log_files': len(plan['logs']), 'log_allocated_bytes': plan['log_allocated_bytes']})
deleted = []
for m in plan['logs']:
    p = Path(m['path'])
    assert matches(m)
    p.unlink()
    deleted.append(m)
save('DELETED_LOGS.json', {'utc': now(), 'files': deleted})

removed_caches = []
skipped_caches = []
with Path('/var/cache/apt/archives/lock').open('a+b') as lock:
    fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    busy = {x['path'] for x in references({m['path'] for m in plan['caches']})}
    for m in plan['caches']:
        p = Path(m['path'])
        allowed = (p.parent == Path('/var/cache/apt/archives') and p.suffix == '.deb') or p in [Path('/var/cache/apt/pkgcache.bin'), Path('/var/cache/apt/srcpkgcache.bin')]
        assert allowed
        if not matches(m) or m['path'] in busy:
            skipped_caches.append({'path': m['path'], 'reason': 'Changed or in use'})
            continue
        p.unlink()
        removed_caches.append(m)

original = json.loads((R / 'PROTECTED_METADATA.json').read_text())
current = list(protected(plan['stopped_node_parent_paths'], {m['path'] for m in plan['logs']}))
assert current == original, 'Protected non-log metadata changed; inspect without further deletion'
assert all(not Path(m['path']).exists() for m in plan['logs'])
assert all(Path(p).is_file() for p in plan['active_node_archives_kept'])
assert hashlib.sha256(NATIVE.read_bytes()).hexdigest() == plan['native_sha256']
health = json.loads((OPS / 'HEALTH_LATEST.json').read_text())
assert {n: row['state']['MainPID'] for n, row in health['BP_processes'].items()} == plan['protected_BP_PIDs']
assert state('pureeos-devnet-history-20261105.service')['MainPID'] == plan['history_PID']
assert state('pureeos-bp-demo-montrealmila-20261105.service')['MainPID'] == '0'
after = os.statvfs('/')
first = json.loads((BASE / 'PLAN.json').read_text())
result = {'status': 'PASS_STOPPED_NODE_LOGS_DELETED_AND_DISK_CLEANED', 'utc': now(),
          'user_override': plan['user_override'], 'deleted_runtime_log_files': len(deleted),
          'deleted_log_allocated_bytes_this_phase': sum(m['allocated'] for m in deleted),
          'removed_cache_files': len(removed_caches), 'cache_allocated_bytes_removed': sum(m['allocated'] for m in removed_caches),
          'skipped_caches': skipped_caches, 'all_selected_raw_compressed_partial_logs_absent': True,
          'prior_archival_completed_then_deleted_files': plan['initial_archival_stage_files_completed'],
          'prior_archival_stage_savings_bytes': plan['initial_archival_stage_savings_bytes'],
          'disk_available_before_direct_delete_bytes': before.f_bavail * before.f_frsize,
          'disk_available_after_bytes': after.f_bavail * after.f_frsize,
          'filesystem_delta_direct_phase_bytes': (after.f_bavail - before.f_bavail) * after.f_frsize,
          'disk_available_before_whole_cleanup_bytes': first['disk_available_bytes'],
          'filesystem_delta_whole_cleanup_bytes': after.f_bavail * after.f_frsize - first['disk_available_bytes'],
          'protected_nonlog_metadata_unchanged_count': len(original),
          'active_node_archives_retained': len(plan['active_node_archives_kept']),
          'all25_BP_and_history_PIDs_unchanged': True, 'maintenance_still_stopped_disabled': True,
          'native_sha256_unchanged': plan['native_sha256'],
          'raw_block_state_history_source_config_keys_wallets_safety_reports_retained': True,
          'no_native_service_stop_or_chain_financial_action': True}
save('RESULT.json', result)
print(json.dumps(result, ensure_ascii=False))
