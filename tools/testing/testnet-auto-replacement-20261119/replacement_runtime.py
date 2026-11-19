"""Create only a fresh replacement's own runtime; never reset an initialized DB."""
from pathlib import Path
import hashlib
import json
import os
import pwd
import shlex
import subprocess
import time
import urllib.request
from replacement_store import load, save
from replacement_chain import guard, G, C


def start_and_sync(adapter, job):
    node_path = adapter.root / 'jobs' / job['id'] / 'NODE.json'
    node = load(node_path)
    root = Path(node['data_dir']).parent
    owner = pwd.getpwnam('pureeostest')
    receipt_path = adapter.root / 'jobs' / job['id'] / 'RUNTIME_PREPARED.json'
    if not receipt_path.exists():
        if root.exists():
            raise RuntimeError('Interrupted runtime preparation: preserve files for reconciliation')
        snapshot_started = adapter.root / 'jobs' / job['id'] / 'SNAPSHOT_REQUESTED.json'
        snapshot_receipt = adapter.root / 'jobs' / job['id'] / 'SNAPSHOT_RESULT.json'
        if not snapshot_receipt.exists():
            if snapshot_started.exists():
                raise RuntimeError('Reconcile original snapshot request; no duplicate request')
            save(snapshot_started, {'utc': guard.now(), 'job': job['id']})
            request = urllib.request.Request('http://127.0.0.1:8888/v1/producer/create_snapshot',
                                             data=b'{}', headers={'Content-Type': 'application/json'})
            with urllib.request.urlopen(request, timeout=60) as response:
                snapshot = json.load(response)
            source = Path(snapshot['snapshot_name'])
            allowed = Path(guard.load('GUARD_NODE_MAP.json')['gptgenesisbp']['data_dir']) / 'snapshots'
            if source.resolve().parent != allowed.resolve() or source.is_symlink() or not source.is_file():
                raise RuntimeError('Snapshot outside exact founder snapshot directory')
            info = guard.rpc('get_info')
            block = guard.rpc('get_block', {'block_num_or_id': snapshot['head_block_num']})
            if info['last_irreversible_block_num'] < snapshot['head_block_num'] or block['id'] != snapshot['head_block_id']:
                raise RuntimeError('Snapshot not canonically irreversible')
            snapshot['SHA256'] = hashlib.sha256(source.read_bytes()).hexdigest()
            save(snapshot_receipt, snapshot)
        snapshot = load(snapshot_receipt)
        source = Path(snapshot['snapshot_name'])
        if hashlib.sha256(source.read_bytes()).hexdigest() != snapshot['SHA256']:
            raise RuntimeError('Snapshot bytes changed')
        parent = adapter.root / 'nodes'
        parent.mkdir(mode=0o750, exist_ok=True)
        os.chown(parent, 0, owner.pw_gid)
        parent.chmod(0o750)
        root.mkdir(mode=0o750)
        (root / 'data').mkdir()
        (root / 'config').mkdir()
        # All secret role material remains inside the server-owned node directory.
        keys = load(adapter.root / 'private' / job['id'] / 'keys.json')
        template = load(adapter.root / 'AUTO_RUNTIME_TEMPLATE.json')
        public = template['config']
        if 'validation-mode = full' not in public or 'vote-threads = 1' not in public:
            raise RuntimeError('Missing qualified full-validation/one-vote-thread profile')
        nodes = guard.load('GUARD_NODE_MAP.json')
        rows = guard.rows()
        seeds = []
        for n in ['gptgenesisbp', *sorted(nodes)]:
            if n in nodes and not rows[n]['banned'] and n != job['name']:
                value = nodes[n].get('p2p')
                if value and value not in seeds:
                    seeds.append(value)
            if len(seeds) == 4:
                break
        if len(seeds) < 3:
            raise RuntimeError('At least three distinct healthy seed peers required')
        extra = ['http-server-address = 127.0.0.1:' + str(node['rpc_port']),
                 'p2p-listen-endpoint = 127.0.0.1:' + str(node['p2p_port']),
                 'producer-name = ' + job['name'],
                 'p2p-bp-gossip-endpoint = ' + job['name'] + ',127.0.0.1:' + str(node['p2p_port']) + ',127.0.0.1',
                 'peer-private-key = ' + json.dumps([keys['peer']['Public key'], keys['peer']['Private key']]),
                 'pause-on-startup = false', 'enable-stale-production = false', 'max-reversible-blocks = 3600']
        extra.extend('p2p-peer-address = ' + seed for seed in seeds)
        extra.extend('signature-provider = ' + keys[role]['Public key'] + '=KEY:' + keys[role]['Private key'] for role in ['production', 'bls'])
        config = root / 'config/config.ini'
        config.write_text(public + '\n' + '\n'.join(extra) + '\n')
        config.chmod(0o600)
        (root / 'config/logging.json').write_text(template['logging'])
        target = root / 'bootstrap-snapshot.bin'
        os.link(source, target)
        os.chown(target, 0, owner.pw_gid)
        target.chmod(0o640)
        bootstrap = root / 'bootstrap.json'
        bootstrap.write_text(json.dumps({'name': job['name'], 'snapshot': str(target), 'SHA256': snapshot['SHA256']}))
        os.chown(bootstrap, 0, owner.pw_gid)
        bootstrap.chmod(0o640)
        for path in [root, root / 'data', root / 'config', config, root / 'config/logging.json']:
            os.chown(path, owner.pw_uid, owner.pw_gid)
        unit_path = Path('/etc/systemd/system') / node['unit']
        if unit_path.exists():
            raise RuntimeError('Do not overwrite an existing unit')
        command = ['/usr/bin/python3', '-B', str(adapter.root / 'auto_node_entry.py'), job['name']]
        unit_path.write_text('[Unit]\nDescription=Owned automatic TEST replacement ' + job['name'] + '\nAfter=network-online.target\n'
                            '[Service]\nType=simple\nUser=pureeostest\nGroup=pureeostest\nWorkingDirectory=' + str(root) + '\nExecStart=' + shlex.join(command) + '\n'
                            'Slice=pureeos-consensus.slice\nEnvironment=MALLOC_ARENA_MAX=2\nEnvironment=MALLOC_TRIM_THRESHOLD_=131072\n'
                            'MemoryHigh=384M\nMemoryLow=64M\nMemoryMax=infinity\nMemorySwapMax=infinity\nCPUWeight=500\nIOWeight=500\n'
                            'OOMScoreAdjust=-400\nRestart=on-failure\nRestartPreventExitStatus=78\nRestartSec=30\nTimeoutStopSec=infinity\nLimitCORE=0\nLimitNOFILE=65536\nNoNewPrivileges=true\n'
                            'StandardOutput=journal\nStandardError=journal\nLogRateLimitIntervalSec=30s\nLogRateLimitBurst=100\n[Install]\nWantedBy=multi-user.target\n')
        node['config_sha256'] = guard.sha(config)
        save(node_path, node)
        save(receipt_path, {'status': 'PREPARED_NOT_STARTED', 'config_SHA256': node['config_sha256'], 'snapshot_SHA256': snapshot['SHA256']})
        subprocess.run(['systemctl', 'daemon-reload'], check=True, capture_output=True, timeout=15)
        subprocess.run(['systemctl', 'enable', '--now', node['unit']], check=True, capture_output=True, timeout=20)
    if guard.state(node['unit'])['MainPID'] == '0':
        raise RuntimeError('Existing replacement stopped; preserve state and reconcile, no automatic reset')
    target = guard.rpc('get_info', port=node['rpc_port'])
    main = guard.rpc('get_info')
    if target['chain_id'] != guard.CHAIN or guard.age(target['head_block_time']) > 5 or guard.age(target['last_irreversible_block_time']) > 20:
        return False
    if target['last_irreversible_block_num'] < main['last_irreversible_block_num'] - 4:
        return False
    block = guard.rpc('get_block', {'block_num_or_id': target['last_irreversible_block_num']})
    if block['id'] != target['last_irreversible_block_id']:
        raise RuntimeError('Replacement canonical block mismatch')
    proof = load(root / 'sync-proof.json', {'samples': []})
    proof['samples'] = (proof['samples'] + [{'utc': guard.now(), 'LIB': target['last_irreversible_block_num'], 'id': target['last_irreversible_block_id']}])[-3:]
    save(root / 'sync-proof.json', proof)
    if len(proof['samples']) < 3:
        return False
    (root / 'bootstrap-complete').touch(mode=0o600, exist_ok=True)
    return True
