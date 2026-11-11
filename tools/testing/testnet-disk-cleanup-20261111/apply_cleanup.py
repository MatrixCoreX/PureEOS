"""Archive inactive logs with decoded SHA256 proof; remove only exact package cache files."""
import datetime
import fcntl
import hashlib
import json
import os
import stat
import subprocess
import time
from pathlib import Path

R = Path('/opt/pureeos-testnet-disk-cleanup-20261111-r1')
OPS = Path('/opt/pureeos-testnet-new-ban-20261111-r1')
NATIVE = Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/libexec/nodeos')
CHUNK = 2 * 1024**2
os.umask(0o077)

def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()

def save(name, value):
    p = R / name
    tmp = p.with_suffix('.tmp')
    p.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with tmp.open('w') as out:
        out.write(json.dumps(value, ensure_ascii=False, indent=2) + '\n')
        out.flush()
        os.fsync(out.fileno())
    os.replace(tmp, p)
    fd = os.open(p.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)

def state(unit):
    raw = subprocess.check_output(['systemctl', 'show', unit, '-p', 'MainPID,ActiveState,UnitFileState'], text=True)
    return dict(line.split('=', 1) for line in raw.splitlines() if '=' in line)

def snapshot(p):
    s = p.lstat()
    return s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_mode, s.st_nlink

def expected(m):
    return tuple(m[k] for k in ['dev', 'ino', 'size', 'mtime_ns', 'mode', 'nlink'])

last_guard = 0.0
def guard(force=False):
    global last_guard
    if not force and time.monotonic() - last_guard < 5:
        return
    deadline = time.monotonic() + 150
    while True:
        h = json.loads((OPS / 'HEALTH_LATEST.json').read_text())
        age = (datetime.datetime.now(datetime.timezone.utc) - datetime.datetime.fromisoformat(h['utc'])).total_seconds()
        safe = (age < 35 and h['running_BP_count'] == 25
                and h.get('head_age_seconds', 999) < 10
                and h.get('LIB_age_seconds', 999) < 45
                and h.get('recent10s_strong_votes', 0) >= 18
                and h['memory']['MemAvailable'] > 512 * 1024**2
                and h['memory']['SwapFree'] > 8 * 1024**3)
        if safe:
            last_guard = time.monotonic()
            return
        assert time.monotonic() < deadline, 'Resource guard timeout; no further raw logs removed'
        time.sleep(5)

def references(path):
    found = []
    target = str(path)
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit() or int(proc.name) == os.getpid():
            continue
        try:
            for fd in (proc / 'fd').iterdir():
                try:
                    if os.readlink(fd) == target:
                        found.append(int(proc.name))
                except OSError:
                    pass
        except OSError:
            pass
    return found

def protected_records(parents, excluded):
    for parent in sorted(parents):
        for base, dirs, files in os.walk(parent, followlinks=False):
            dirs[:] = sorted(n for n in dirs if n not in {'private', 'wallet', 'keystore'}
                             and not (Path(base) / n).is_symlink())
            for name in sorted(files):
                p = Path(base) / name
                if str(p) in excluded or p.is_symlink() or not p.is_file():
                    continue
                s = p.lstat()
                yield {'path': str(p), 'dev': s.st_dev, 'ino': s.st_ino,
                       'size': s.st_size, 'allocated': s.st_blocks * 512,
                       'mtime_ns': s.st_mtime_ns, 'mode': s.st_mode, 'nlink': s.st_nlink}

assert not (R / 'APPLY_STARTED.json').exists(), 'Inspect existing progress instead of replaying cleanup'
plan = json.loads((R / 'PLAN.json').read_text())
assert plan['status'] == 'PREPARED_NOT_APPLIED' and len(plan['logs']) == 72
assert hashlib.sha256(NATIVE.read_bytes()).hexdigest() == plan['native_sha256']
guard(force=True)
for m in plan['logs']:
    assert snapshot(Path(m['path'])) == expected(m)
    assert not references(Path(m['path']))
before = os.statvfs('/')
progress = {'status': 'RUNNING', 'utc': now(), 'completed_logs': [], 'removed_caches': [], 'skipped_caches': []}
save('APPLY_STARTED.json', {'utc': now(), 'logs': len(plan['logs']), 'native_sha256': plan['native_sha256']})
save('PROGRESS.json', progress)
children = []
try:
    # A small first file qualifies the exact compression/decode/hash/unlink path.
    for m in sorted(plan['logs'], key=lambda value: value['size']):
        guard(force=True)
        src, archive, partial = (Path(m[k]) for k in ['path', 'archive', 'partial'])
        assert src.name == 'nodeos.log' and src.resolve() == src
        assert snapshot(src) == expected(m) and not references(src)
        assert not archive.exists() and not partial.exists()
        progress.update(current=str(src), current_started_utc=now(), utc=now())
        save('PROGRESS.json', progress)
        original_hash = hashlib.sha256()
        original_bytes = 0
        with src.open('rb') as inp, partial.open('xb') as out:
            child = subprocess.Popen(['/usr/bin/zstd', '-1', '--single-thread', '--no-asyncio',
                                      '--no-progress', '-q', '-c', '--stream-size=' + str(m['size'])],
                                     stdin=subprocess.PIPE, stdout=out, stderr=subprocess.PIPE)
            children.append(child)
            for block in iter(lambda: inp.read(CHUNK), b''):
                original_hash.update(block)
                original_bytes += len(block)
                child.stdin.write(block)
                guard()
            child.stdin.close()
            assert child.wait(timeout=90) == 0, child.stderr.read().decode(errors='replace')
            child.stderr.close()
            children.remove(child)
            out.flush()
            os.fsync(out.fileno())
        assert original_bytes == m['size'] and snapshot(src) == expected(m)
        decoded_hash = hashlib.sha256()
        decoded_bytes = 0
        child = subprocess.Popen(['/usr/bin/zstd', '-d', '--single-thread', '--no-asyncio',
                                  '--no-progress', '-q', '-c', '--', str(partial)],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        children.append(child)
        for block in iter(lambda: child.stdout.read(CHUNK), b''):
            decoded_hash.update(block)
            decoded_bytes += len(block)
            guard()
        child.stdout.close()
        assert child.wait(timeout=90) == 0, child.stderr.read().decode(errors='replace')
        child.stderr.close()
        children.remove(child)
        assert decoded_bytes == original_bytes and decoded_hash.digest() == original_hash.digest()
        guard(force=True)
        assert snapshot(src) == expected(m) and not references(src)
        assert not archive.exists()
        proof_name = 'log-proofs/' + hashlib.sha256(str(src).encode()).hexdigest()[:20] + '.json'
        save(proof_name, {'status': 'FULL_DECODE_VERIFIED_BEFORE_UNLINK', 'utc': now(),
                          'source': str(src), 'archive': str(archive),
                          'uncompressed_bytes': original_bytes,
                          'uncompressed_sha256': original_hash.hexdigest(),
                          'original_metadata': m})
        os.replace(partial, archive)
        dirfd = os.open(archive.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(dirfd)
            src.unlink()
            os.fsync(dirfd)
        finally:
            os.close(dirfd)
        done = {'source': str(src), 'archive': str(archive), 'uncompressed_bytes': original_bytes,
                'uncompressed_sha256': original_hash.hexdigest(), 'old_allocated_bytes': m['allocated'],
                'archive_bytes': archive.stat().st_size, 'archive_allocated_bytes': archive.stat().st_blocks * 512,
                'sha256_after_full_decode_matches': True}
        progress['completed_logs'].append(done)
        progress['utc'] = now()
        progress['log_savings_bytes'] = sum(v['old_allocated_bytes'] - v['archive_allocated_bytes'] for v in progress['completed_logs'])
        save('PROGRESS.json', progress)

    # Respect APT's cache lock, and unlink only the previously inventoried re-downloadable files.
    with Path('/var/cache/apt/archives/lock').open('a+b') as lock:
        fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        for m in plan['caches']:
            p = Path(m['path'])
            allowed = (p.parent == Path('/var/cache/apt/archives') and p.suffix == '.deb') or p in [Path('/var/cache/apt/pkgcache.bin'), Path('/var/cache/apt/srcpkgcache.bin')]
            assert allowed
            if not p.exists() or snapshot(p) != expected(m) or references(p):
                progress['skipped_caches'].append({'path': str(p), 'reason': 'Changed, removed or currently in use'})
                continue
            assert stat.S_ISREG(p.lstat().st_mode) and not p.is_symlink()
            p.unlink()
            progress['removed_caches'].append(m)

    excluded = {m['path'] for m in plan['logs']}
    excluded.update(m['archive'] for m in plan['logs'])
    excluded.update(m['partial'] for m in plan['logs'])
    original = json.loads((R / 'PROTECTED_METADATA.json').read_text())
    current = list(protected_records(plan['protected_node_parent_paths'], excluded))
    assert current == original, 'A protected non-log file changed; reconcile without deleting any more files'
    assert hashlib.sha256(NATIVE.read_bytes()).hexdigest() == plan['native_sha256']
    h = json.loads((OPS / 'HEALTH_LATEST.json').read_text())
    assert h['running_BP_count'] == 25
    assert {n: v['state']['MainPID'] for n, v in h['BP_processes'].items()} == plan['protected_BP_PIDs']
    assert state('pureeos-devnet-history-20261105.service')['MainPID'] == plan['history_PID']
    maint = state('pureeos-bp-demo-montrealmila-20261105.service')
    assert maint['MainPID'] == '0' and maint['UnitFileState'] == 'disabled'
    after = os.statvfs('/')
    result = {**progress, 'status': 'PASS_VERIFIED_LOG_ARCHIVES_AND_EXACT_CACHE_CLEANUP', 'utc': now(),
              'cache_savings_bytes': sum(m['allocated'] for m in progress['removed_caches']),
              'disk_available_before_bytes': before.f_bavail * before.f_frsize,
              'disk_available_after_bytes': after.f_bavail * after.f_frsize,
              'filesystem_available_delta_bytes': (after.f_bavail - before.f_bavail) * after.f_frsize,
              'protected_nonlog_metadata_unchanged_count': len(original),
              'all25_BP_and_history_PIDs_unchanged': True,
              'maintenance_still_stopped_disabled': True,
              'native_sha256_unchanged': plan['native_sha256'],
              'no_service_stop_or_financial_actions': True,
              'chain_state_config_keys_wallets_signers_APKs_AVD_unchanged': True}
    save('RESULT.json', result)
    save('PROGRESS.json', result)
    print(json.dumps({k: result[k] for k in ['status', 'utc', 'log_savings_bytes', 'cache_savings_bytes', 'disk_available_after_bytes']}))
except BaseException as error:
    for child in children:
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill()
    progress.update(status='STOPPED_RECONCILE_NO_BLIND_REPLAY', utc=now(), error=repr(error))
    save('FAILURE.json', progress)
    save('PROGRESS.json', progress)
    raise
