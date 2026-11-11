"""Prepare an exact inactive-log/cache whitelist, without deleting or changing services."""
import datetime
import hashlib
import json
import os
import stat
import subprocess
import time
from pathlib import Path

R = Path('/opt/pureeos-testnet-disk-cleanup-20261111-r1')
OPS = Path('/opt/pureeos-testnet-new-ban-20261111-r1')
ROOTS = [Path('/opt') / n for n in [
    'pureeos-keynet-20261103-r2', 'pureeos-test45-20261102-r1',
    'pureeos-keynet-20261103-r1', 'pureeos-key-history-20261103-r1',
    'pureeos-test30-20261103-r1',
]]
NATIVE = Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/libexec/nodeos')
os.umask(0o077)

def stamp():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()

def metadata(p):
    s = p.lstat()
    return {'path': str(p), 'dev': s.st_dev, 'ino': s.st_ino,
            'size': s.st_size, 'allocated': s.st_blocks * 512,
            'mtime_ns': s.st_mtime_ns, 'mode': s.st_mode, 'nlink': s.st_nlink}

def state(unit):
    raw = subprocess.check_output(['systemctl', 'show', unit, '-p',
                                   'MainPID,ActiveState,UnitFileState'], text=True)
    return dict(line.split('=', 1) for line in raw.splitlines() if '=' in line)

def protected_records(parents, excluded):
    for parent in sorted(parents):
        for base, dirs, files in os.walk(parent, followlinks=False):
            dirs[:] = sorted(n for n in dirs if n not in {'private', 'wallet', 'keystore'}
                             and not (Path(base) / n).is_symlink())
            for name in sorted(files):
                p = Path(base) / name
                if str(p) in excluded or p.is_symlink():
                    continue
                if p.is_file():
                    # Metadata only: no key/configuration/database content is read here.
                    yield metadata(p)

R.mkdir(mode=0o700, exist_ok=True)
assert not (R / 'PLAN.json').exists(), 'Reconcile an existing plan before repeating preparation'
health = json.loads((OPS / 'HEALTH_LATEST.json').read_text())
assert health['status'] == 'OK' and health['running_BP_count'] == 25
maint = state('pureeos-bp-demo-montrealmila-20261105.service')
assert maint['MainPID'] == '0' and maint['UnitFileState'] == 'disabled'
logs = []
for root in ROOTS:
    for pattern in ['founder/nodeos.log', 'history/nodeos.log', 'bps/*/nodeos.log']:
        for p in root.glob(pattern):
            m = metadata(p)
            if not stat.S_ISREG(m['mode']) or p.is_symlink() or m['size'] < 16 * 1024**2:
                continue
            assert p.resolve() == p and m['nlink'] == 1
            assert time.time_ns() - m['mtime_ns'] > 86400 * 10**9
            archive = p.with_name(p.name + '.archive-20261111.zst')
            partial = archive.with_name(archive.name + '.partial')
            assert not archive.exists() and not partial.exists()
            m.update(archive=str(archive), partial=str(partial))
            logs.append(m)
assert len(logs) == 72 and len({m['path'] for m in logs}) == len(logs)

caches = []
for p in [*Path('/var/cache/apt/archives').glob('*.deb'),
          Path('/var/cache/apt/pkgcache.bin'), Path('/var/cache/apt/srcpkgcache.bin')]:
    if p.exists() and not p.is_symlink():
        m = metadata(p)
        if stat.S_ISREG(m['mode']) and m['nlink'] == 1:
            caches.append(m)

targets = {m['path'] for m in logs + caches}
parents = {str(Path(m['path']).parent) for m in logs}
references = []
for proc in Path('/proc').iterdir():
    if not proc.name.isdigit():
        continue
    try:
        for fd in (proc / 'fd').iterdir():
            try:
                value = os.readlink(fd)
            except OSError:
                continue
            if value in targets:
                references.append({'pid': int(proc.name), 'target': value})
    except OSError:
        pass
    try:
        cwd = Path(os.readlink(proc / 'cwd'))
        if any(cwd == Path(parent) or Path(parent) in cwd.parents for parent in parents):
            references.append({'pid': int(proc.name), 'cwd_in_stopped_node': str(cwd)})
    except OSError:
        pass
assert not references, references

protected = list(protected_records(parents, targets))
protected_raw = json.dumps(protected, sort_keys=True, separators=(',', ':')).encode()
(R / 'PROTECTED_METADATA.json').write_bytes(protected_raw)
v = os.statvfs('/')
plan = {'status': 'PREPARED_NOT_APPLIED', 'utc': stamp(),
        'scope': 'Exact72 inactive historical node logs, byte-preserving zstd archive; exact apt cache files only',
        'compression': 'zstd -1 --single-thread --no-asyncio; decompressed bytes and SHA256 must match before unlink',
        'logs': logs, 'caches': caches,
        'log_allocated_bytes': sum(x['allocated'] for x in logs),
        'cache_allocated_bytes': sum(x['allocated'] for x in caches),
        'protected_metadata_count': len(protected),
        'protected_metadata_sha256': hashlib.sha256(protected_raw).hexdigest(),
        'protected_node_parent_paths': sorted(parents),
        'protected_BP_PIDs': {n: p['state']['MainPID'] for n, p in health['BP_processes'].items()},
        'history_PID': state('pureeos-devnet-history-20261105.service')['MainPID'],
        'maintenance_state': maint, 'disk_available_bytes': v.f_bavail * v.f_frsize,
        'native_sha256': hashlib.sha256(NATIVE.read_bytes()).hexdigest(),
        'raw_chain_state_source_config_keys_wallets_signers_APKs_AVD_not_cleanup_targets': True}
assert plan['native_sha256'] == '946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397'
(R / 'PLAN.json').write_text(json.dumps(plan, ensure_ascii=False, indent=2) + '\n')
print(json.dumps({k: plan[k] for k in ['status', 'utc', 'log_allocated_bytes',
                                      'cache_allocated_bytes', 'protected_metadata_count',
                                      'disk_available_bytes']}))
