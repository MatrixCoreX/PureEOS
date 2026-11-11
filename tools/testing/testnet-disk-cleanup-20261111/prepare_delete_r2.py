"""Latest user override: delete runtime logs of stopped old nodes, including archives."""
import datetime
import hashlib
import json
import os
import re
import stat
import subprocess
from pathlib import Path

BASE = Path('/opt/pureeos-testnet-disk-cleanup-20261111-r1')
R = BASE / 'delete-r2'
OPS = Path('/opt/pureeos-testnet-new-ban-20261111-r1')
OLD = Path('/opt/pureeos-testnet-resilience-20261110-r1')
NATIVE = Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/libexec/nodeos')
LEGACY = [Path('/opt') / name for name in [
    'pureeos-keynet-20261103-r2', 'pureeos-test45-20261102-r1',
    'pureeos-keynet-20261103-r1', 'pureeos-key-history-20261103-r1',
    'pureeos-test30-20261103-r1', 'pureeos-test30-20261103-r2',
]]
os.umask(0o077)

def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()

def state(unit):
    raw = subprocess.check_output(['systemctl', 'show', unit, '-p', 'MainPID,ActiveState,UnitFileState'], text=True)
    return dict(line.split('=', 1) for line in raw.splitlines() if '=' in line)

def stopped(unit):
    value = state(unit)
    return value['MainPID'] == '0' and value['ActiveState'] in ['inactive', 'failed'] and value.get('UnitFileState') == 'disabled'

def metadata(p):
    s = p.lstat()
    return {'path': str(p), 'dev': s.st_dev, 'ino': s.st_ino,
            'size': s.st_size, 'allocated': s.st_blocks * 512,
            'mtime_ns': s.st_mtime_ns, 'mode': s.st_mode, 'nlink': s.st_nlink}

def runtime_name(name):
    # Never match blocks.log, trace_history.log, state_history or audit JSON.
    return re.fullmatch(r'(?:nodeos|stdout|stderr)\.log(?:\.(?:\d+|gz|zst|xz|bz2|old|partial|archive-\d+))*', name) is not None

def protected(parents, excluded):
    for parent in sorted(parents):
        for base, dirs, files in os.walk(parent, followlinks=False):
            dirs[:] = sorted(name for name in dirs if name not in {'private', 'wallet', 'keystore'} and not (Path(base) / name).is_symlink())
            for name in sorted(files):
                p = Path(base) / name
                if str(p) not in excluded and p.is_file() and not p.is_symlink():
                    yield metadata(p)

assert state('pureeos-testnet-disk-cleanup-20261111.service')['MainPID'] == '0'
assert state('pureeos-testnet-disk-cleanup-20261111.service')['ActiveState'] == 'inactive'
R.mkdir(mode=0o700, exist_ok=True)
assert not (R / 'PLAN.json').exists(), 'Inspect the existing deletion plan before repeating preparation'

parents = set()
for root in LEGACY:
    for candidate in [root / 'founder', root / 'history', *root.glob('bps/*')]:
        if candidate.is_dir() and not candidate.is_symlink():
            parents.add(str(candidate))

# Currently retired identities, including effective maintenance. No active BP archive is selected.
retired = {}
for row in json.loads((OLD / 'BANNED_DATA_STARTED.json').read_text())['targets']:
    if stopped(row['unit']):
        retired[row['owner']] = row['unit']
        parents.add(str(Path(row['root']).parent))
spec = json.loads((OLD / 'RESTORE_NODE_PATHS.json').read_text())
for name in ['alibabacloud', 'deepmindlabs', 'openaiclouds', 'sydneyunswai']:
    row = spec[name]
    assert stopped(row['unit'])
    retired[name] = row['unit']
    parents.add(str(Path(row['data_dir']).parent))
unit = 'pureeos-bp-demo-montrealmila-20261105.service'
assert stopped(unit)
retired['montrealmila'] = unit
parents.add('/opt/pureeos-bp-demo-six-20261105-r1/nodes/montrealmila')

candidates = {}
for parent in parents:
    p = Path(parent)
    assert p.resolve() == p
    for child in p.iterdir():
        if runtime_name(child.name) and child.is_file() and not child.is_symlink():
            m = metadata(child)
            assert stat.S_ISREG(m['mode']) and m['nlink'] == 1
            candidates[str(child)] = m

parents = {str(Path(path).parent) for path in candidates}
initial_plan = json.loads((BASE / 'PLAN.json').read_text())
for row in initial_plan['logs']:
    for key in ['path', 'archive', 'partial']:
        if Path(row[key]).exists():
            assert row[key] in candidates, row[key]

archive_rows = json.loads((OLD / 'LOG_ARCHIVE_RESULT.json').read_text())['files']
kept_active_archives = []
for row in archive_rows:
    p = Path(row['archive_path'])
    if not p.exists():
        continue
    if stopped(row['unit']):
        assert p.parent == OLD / 'log-archives' and p.name.endswith('.nodeos.log.gz') and not p.is_symlink()
        candidates[str(p)] = metadata(p)
    else:
        kept_active_archives.append(str(p))
sydney = OPS / 'sydneyunswai-original-nodeos.log.gz'
if sydney.exists():
    assert stopped(retired['sydneyunswai']) and not sydney.is_symlink()
    candidates[str(sydney)] = metadata(sydney)

# Re-downloadable package cache only. Do not enter wallets, SDKs, AVDs or signing directories.
cache_candidates = []
for p in [*Path('/var/cache/apt/archives').glob('*.deb'), Path('/var/cache/apt/pkgcache.bin'), Path('/var/cache/apt/srcpkgcache.bin')]:
    if p.exists() and p.is_file() and not p.is_symlink():
        m = metadata(p)
        if m['nlink'] == 1:
            cache_candidates.append(m)

targets = set(candidates) | {m['path'] for m in cache_candidates}
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
        if any(cwd == Path(p) or Path(p) in cwd.parents for p in parents):
            references.append({'pid': int(proc.name), 'cwd_in_old_node': str(cwd)})
    except OSError:
        pass
assert not references, references

health = json.loads((OPS / 'HEALTH_LATEST.json').read_text())
assert health['running_BP_count'] == 25 and health['status'] == 'OK'
protected_rows = list(protected(parents, set(candidates)))
protected_raw = json.dumps(protected_rows, sort_keys=True, separators=(',', ':')).encode()
(R / 'PROTECTED_METADATA.json').write_bytes(protected_raw)
old_progress = json.loads((BASE / 'PROGRESS.json').read_text())
(R / 'ARCHIVAL_PROGRESS_AT_USER_OVERRIDE.json').write_text(json.dumps(old_progress, indent=2) + '\n')
v = os.statvfs('/')
plan = {'status': 'PREPARED_USER_AUTHORIZED_DIRECT_DELETE', 'utc': now(),
        'user_override': '已停用旧节点 日志就全部删除',
        'logs': sorted(candidates.values(), key=lambda m: m['path']),
        'caches': cache_candidates, 'log_allocated_bytes': sum(m['allocated'] for m in candidates.values()),
        'cache_allocated_bytes': sum(m['allocated'] for m in cache_candidates),
        'stopped_node_parent_paths': sorted(parents), 'retired_current_units': retired,
        'active_node_archives_kept': kept_active_archives,
        'protected_nonlog_metadata_count': len(protected_rows),
        'protected_metadata_sha256': hashlib.sha256(protected_raw).hexdigest(),
        'protected_BP_PIDs': {n: row['state']['MainPID'] for n, row in health['BP_processes'].items()},
        'history_PID': state('pureeos-devnet-history-20261105.service')['MainPID'],
        'native_sha256': hashlib.sha256(NATIVE.read_bytes()).hexdigest(),
        'disk_available_bytes': v.f_bavail * v.f_frsize,
        'initial_archival_stage_files_completed': len(old_progress['completed_logs']),
        'initial_archival_stage_savings_bytes': old_progress.get('log_savings_bytes', 0),
        'raw_block_state_history_keys_config_wallets_and_audit_receipts_are_not_logs_for_deletion': True}
assert plan['logs'] and plan['native_sha256'] == '946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397'
(R / 'PLAN.json').write_text(json.dumps(plan, ensure_ascii=False, indent=2) + '\n')
(BASE / 'CANCELLED_BY_USER.json').write_text(json.dumps({'utc': now(), 'status': 'ARCHIVAL_STOPPED_AND_SUPERSEDED_BY_DIRECT_LOG_DELETE', 'unit_inactive': True, 'new_phase': str(R), 'prior_completed_archives': len(old_progress['completed_logs'])}, indent=2) + '\n')
print(json.dumps({k: plan[k] for k in ['status', 'utc', 'log_allocated_bytes', 'cache_allocated_bytes', 'protected_nonlog_metadata_count', 'initial_archival_stage_files_completed']}))
