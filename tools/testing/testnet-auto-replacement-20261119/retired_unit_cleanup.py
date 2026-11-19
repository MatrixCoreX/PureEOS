"""Delete only disabled unit configs of canonically retired owned ordinary BPs."""
from pathlib import Path
import hashlib
import re
import subprocess
from replacement_store import save, load


def cleanup(g, nodes, root, unit_root=Path('/etc/systemd/system')):
    rows = g.rows()
    removed = []
    reload_needed = False
    for name, node in nodes.items():
        receipt = load(g.R / 'auto-retired' / (name + '.json'))
        if name == 'gptgenesisbp' or not rows[name]['banned'] or receipt is None:
            continue
        if receipt['status'] not in ['PASS_PERMANENT_BAN_STOPPED_BLOCKS_CONFIG_DELETED', 'PASS_RECONCILED_COMPLETED_RETIREMENT']:
            continue
        unit = node['unit']
        if not re.fullmatch(r'pureeos-[a-z0-9-]+\.service', unit):
            raise RuntimeError('Unit outside owned TEST namespace')
        if g.state(unit)['MainPID'] != '0':
            raise RuntimeError('Retired unit still has a process')
        if (Path(node['config_dir']) / 'config.ini').exists():
            raise RuntimeError('Runtime configuration not retired')
        for port in [8888, 8890]:
            info = g.rpc('get_info', port=port)
            block_num = rows[name]['ban_final_block']
            if info['last_irreversible_block_num'] < block_num or g.rpc('get_block', {'block_num_or_id': block_num}, port=port)['id'] != receipt['ban_block_id']:
                raise RuntimeError('Canonical retirement proof differs')
        # Fresh automatic nodes have bootstrap metadata/checkpoint links beside
        # data/. Remove those own links too; the shared founder snapshot and
        # independent private key/safety escrow remain protected.
        owned = Path(root) / 'nodes' / name
        if node.get('data_dir') == str(owned / 'data'):
            if owned.is_symlink() or owned.resolve() != owned:
                raise RuntimeError('Unexpected automatic node root')
            extras = []
            for filename in ['bootstrap-snapshot.bin', 'bootstrap.json', 'sync-proof.json', 'bootstrap-attempted', 'bootstrap-complete']:
                path = owned / filename
                if path.is_symlink() or (path.exists() and not path.is_file()):
                    raise RuntimeError('Unexpected bootstrap file type')
                if path.exists():
                    extras.append({'name': filename, 'bytes': path.stat().st_size})
                    path.unlink()
            if extras:
                save(Path(root) / 'retired-unit-configs' / (name + '-bootstrap.json'),
                     {'status': 'COMPLETE_OWN_BOOTSTRAP_FILES_REMOVED', 'owner': name, 'files': extras,
                      'shared_founder_snapshot_and_finalizer_safety_retained': True})
        journal = Path(root) / 'retired-unit-configs' / (name + '.json')
        record = load(journal)
        if record and record['status'] == 'COMPLETE':
            continue
        paths = [unit_root / unit]
        drop = unit_root / (unit + '.d')
        if drop.is_symlink():
            raise RuntimeError('Symlink unit dropin directory')
        if drop.exists():
            paths.extend(sorted(drop.iterdir()))
        if record is None:
            files = {}
            for path in paths:
                if path.is_symlink() or (path.exists() and not path.is_file()):
                    raise RuntimeError('Unexpected unit file type')
                if path.exists():
                    files[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
            record = {'status': 'STARTED', 'owner': name, 'ban_block_id': receipt['ban_block_id'], 'files_SHA256': files}
            save(journal, record)
        if record['ban_block_id'] != receipt['ban_block_id']:
            raise RuntimeError('Existing deletion journal differs')
        reload_needed = reload_needed or bool(record['files_SHA256'])
        for value, sha in record['files_SHA256'].items():
            path = Path(value)
            if path != unit_root / unit and path.parent != drop:
                raise RuntimeError('Unexpected deletion journal path')
            if path.is_symlink():
                raise RuntimeError('Symlink unit configuration')
            if path.exists():
                if hashlib.sha256(path.read_bytes()).hexdigest() != sha:
                    raise RuntimeError('Unit config changed since retirement plan')
                path.unlink()
                removed.append(str(path))
        if drop.exists():
            drop.rmdir()
        record.update(status='COMPLETE', utc=g.now())
        save(journal, record)
    if reload_needed:
        subprocess.run(['systemctl', 'daemon-reload'], check=True, capture_output=True, timeout=15)
    return removed
