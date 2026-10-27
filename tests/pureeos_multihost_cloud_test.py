#!/usr/bin/env python3
"""Two disposable hosts, independently keyed real nodeos processes, no TPS claim.

Run only on the temporary coordinator. The peer must contain the SAME verified
nodeos ELF and allow key-only SSH from this host. RPC always remains loopback.
Imports the real contract/bootstrap/rotation/fault assertions of the local test.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import shlex
import signal
import socket
import subprocess
import time

from pureeos_large_multinode_local_test import LargeMultiNodeTest

COORDINATOR_P2P_PORT = 29000
PEER_HTTP_BASE = 29201
PEER_P2P_BASE = 29111


def check_listener_binding(address,port):
    """Match nodeos SO_REUSEADDR while still rejecting an actual active listener."""
    with socket.socket() as probe:
        probe.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
        probe.bind((address,port))
        probe.listen()


def validate_listener_ports(ports, ephemeral_range):
    first, last = ephemeral_range
    if not 1 <= first <= last <= 65535:
        raise ValueError('Invalid host ephemeral port range')
    if len(set(ports)) != len(ports):
        raise ValueError('Duplicate test listener port')
    for port in ports:
        if not 1024 <= port <= 65535 or first <= port <= last:
            raise ValueError('Test listener overlaps host ephemeral port allocation')


def balanced_candidates(names, count):
    if count not in (2, 22, 30, 40) or len(names) < count:
        raise ValueError('Use an explicitly supported cohort with enough independent account names')
    selected = tuple(names[:count])
    return selected, tuple(name for index, name in enumerate(selected) if index % 2 == 1)


class PeerCommandError(RuntimeError):
    def __init__(self, returncode, diagnostic):
        self.returncode = returncode
        super().__init__('Task-peer command failed: ' + diagnostic)


class RemoteProcess:
    def __init__(self, owner, unit):
        self.owner, self.unit, self.returncode = owner, unit, None
        self.next_poll = 0
        pid = owner.ssh(['systemctl', 'show', unit, '-p', 'MainPID', '--value']).strip()
        if not pid.isdigit() or int(pid) <= 0:
            raise RuntimeError('Peer unit has no running process: ' + unit)
        self.pid = 'peer:' + pid

    def poll(self):
        if self.returncode is not None or time.monotonic() < self.next_poll:
            return self.returncode
        fields = self.owner.ssh(['systemctl', 'show', self.unit, '-p', 'ActiveState', '-p', 'Result'])
        self.next_poll = time.monotonic() + 2
        if 'ActiveState=active\n' not in fields:
            self.returncode = 0 if 'Result=success\n' in fields else 1
        return self.returncode

    def terminate(self):
        self.owner.ssh(['systemctl', 'stop', self.unit], timeout=125)
        self.next_poll = 0

    def kill(self):
        self.owner.ssh(['systemctl', 'kill', '--signal=KILL', self.unit])
        self.next_poll = 0

    def wait(self, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            result = self.poll()
            if result is not None:
                return result
            time.sleep(.2)
        raise subprocess.TimeoutExpired(self.unit, timeout)


def peer_service_limits(runtime, memory_mib=450, shutdown_seconds=15):
    if type(runtime) is not int or not 60 <= runtime <= 129600:
        raise ValueError('Disposable peer lifetime must be between one minute and 36 hours')
    if type(memory_mib) is not int or not 256 <= memory_mib <= 4096:
        raise ValueError('Disposable peer memory must be between 256 and 4096 MiB')
    if type(shutdown_seconds) is not int or not 15 <= shutdown_seconds <= 90:
        raise ValueError('Disposable peer shutdown budget must be between 15 and 90 seconds')
    return ['--property=RuntimeMaxSec='+str(runtime), '--property=TimeoutStopSec='+str(shutdown_seconds),
        '--property=MemoryMax='+str(memory_mib)+'M', '--property=MemorySwapMax=0']


class CloudMultiNodeTest(LargeMultiNodeTest):
    remote_names = ('nodea', 'nodeb')

    def __init__(self, args):
        self.args = args
        self.names, self.remote_names = balanced_candidates(type(self).names, args.node_count)
        self.peer_ip = str(ipaddress.IPv4Address(args.peer_ip))
        self.coordinator_ip = str(ipaddress.IPv4Address(args.coordinator_ip))
        if not all(ipaddress.IPv4Address(v).is_global for v in (self.peer_ip, self.coordinator_ip)):
            raise ValueError('Two explicit task VM public IPv4 addresses required')
        super().__init__(args.build_dir.resolve(), args.contracts_dir.resolve(), args.state_parent.resolve())
        self._network_checked = False
        self.p2p_port = COORDINATOR_P2P_PORT
        self.ports.add(self.p2p_port)
        validate_listener_ports([self.p2p_port], tuple(map(int,
            Path('/proc/sys/net/ipv4/ip_local_port_range').read_text().split())))
        check_listener_binding('0.0.0.0',self.p2p_port)
        peer_ports = [base + index for base in (PEER_HTTP_BASE, PEER_P2P_BASE)
                      for index in range(len(self.remote_names))]
        probe_code = '''import json,pathlib,socket,sys
ports=json.load(sys.stdin)
sockets=[]
for port in ports:
    s=socket.socket()
    s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
    s.bind(('0.0.0.0',port))
    s.listen()
    sockets.append(s)
print(json.dumps(list(map(int,pathlib.Path('/proc/sys/net/ipv4/ip_local_port_range').read_text().split()))))
'''
        peer_range = json.loads(self.ssh(['python3', '-c', probe_code], input_text=json.dumps(peer_ports)))
        validate_listener_ports(peer_ports, peer_range)
        self.peer_root = Path('/opt/pureeos-peer') / self.root.name
        self.report.update(scope=__doc__, vm_hosts=2, remote_nodes=list(self.remote_names),
            node_limit=len(self.names), coordinator_ip=self.coordinator_ip, peer_ip=self.peer_ip,
            multihost_harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            resource_samples=[], listener_port_preflight='bound successfully outside both host ephemeral ranges',
            scope_limit=f'{len(self.names)} logical processes on two correlated hosts, not independent machines')

    def ssh(self, arguments, timeout=20, input_text=None):
        command = ['ssh', '-i', str(self.args.peer_key), '-o', 'IdentitiesOnly=yes',
            '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', '-o', 'StrictHostKeyChecking=yes',
            '-o', 'UserKnownHostsFile=' + str(self.args.peer_known_hosts),
            '-o', 'ControlMaster=auto', '-o', 'ControlPersist=60',
            '-o', 'ControlPath=/opt/pureeos-modern/peer-ssh-%C',
            'root@' + self.peer_ip, shlex.join(map(str, arguments))]
        result = subprocess.run(command, input=input_text, capture_output=True, text=True, timeout=timeout)
        if result.returncode:
            raise PeerCommandError(result.returncode, self.redact(result.stderr[-2000:]))
        return result.stdout

    def start_node(self, phase, extra=()):
        config = self.root / 'config/config.ini'
        value = config.read_text().replace('p2p-listen-endpoint = 127.0.0.1:' + str(self.p2p_port),
                                          'p2p-listen-endpoint = 0.0.0.0:' + str(self.p2p_port))
        config.write_text(value)
        super().start_node(phase, extra)

    def wait_lib(self, target, timeout=50):
        result = super().wait_lib(target, timeout)
        if not self._network_checked:
            probe = self.ssh(['python3', '-c',
                'import socket; s=socket.create_connection((' + repr(self.coordinator_ip) +
                ',' + str(self.p2p_port) + '),5); s.close(); print("ok")'])
            if probe.strip() != 'ok':
                raise RuntimeError('Cross-host P2P TCP preflight failed')
            self._network_checked = True
            self.report['peer_to_coordinator_p2p_tcp_preflight'] = 'passed'
        return result

    def start_peer(self, name, restart=False, *, replay=False, hard_replay=False):
        if (type(replay) is not bool or type(hard_replay) is not bool or (replay and hard_replay)
                or ((replay or hard_replay) and (not restart or name not in self.remote_names))):
            raise ValueError('Replay is only valid for an existing remote peer restart')
        if name not in self.remote_names:
            return super().start_peer(name, restart)
        if not restart:
            root = self.peer_root / name
            index = self.remote_names.index(name)
            http, p2p = PEER_HTTP_BASE + index, PEER_P2P_BASE + index
            config = ['plugin = eosio::producer_plugin', 'plugin = eosio::chain_api_plugin',
                'producer-name = ' + name, 'enable-stale-production = true',
                'chain-state-db-size-mb = 128', 'chain-state-db-guard-size-mb = 4',
                'resource-monitor-space-absolute-gb = 8', 'p2p-max-nodes-per-host = 64', 'max-clients = 128',
                'chain-threads = 1', 'http-threads = 1', 'net-threads = 1', 'vote-threads = 1',
                'http-max-response-time-ms = 200', 'abi-serializer-max-time-ms = 200',
                f'http-server-address = 127.0.0.1:{http}', f'p2p-listen-endpoint = 0.0.0.0:{p2p}',
                f'p2p-peer-address = {self.coordinator_ip}:{self.p2p_port}', 'verbose-http-errors = true']
            for key in (self.accounts[name], self.bls[name]):
                config.append('signature-provider = ' + key['Public key'] + '=KEY:' + key['Private key'])
            document = dict(root=str(root), config='\n'.join(config) + '\n',
                            genesis=(self.root / 'genesis.json').read_text())
            self.ssh(['python3', '-c',
                'import json,pathlib,sys,os;os.umask(0o077);v=json.load(sys.stdin);r=pathlib.Path(v["root"]);'
                '(r/"config").mkdir(parents=True);(r/"config/config.ini").write_text(v["config"]);'
                '(r/"genesis.json").write_text(v["genesis"])'], input_text=json.dumps(document))
            self.peers[name] = dict(root=root, url=f'ssh+peer://127.0.0.1:{http}', p2p_port=p2p, remote=True)
        peer = self.peers[name]
        peer['starts'] = peer.get('starts', 0) + 1
        unit = f'pureeos-peer-{self.root.name}-{name}-{peer["starts"]}'
        root = peer['root']
        runtime = getattr(self, 'peer_runtime_seconds', 10800)
        limits = peer_service_limits(runtime, getattr(self, 'peer_memory_mib', 450),
                                     getattr(self, 'peer_shutdown_seconds', 15))
        command = ['systemd-run', '--quiet', '--unit=' + unit, '--service-type=exec',
            '--property=KillMode=control-group', *limits,
            '--property=StandardOutput=append:' + str(root / f'node-{peer["starts"]}.log'),
            '--property=StandardError=inherit', '/opt/pureeos-peer/nodeos.elf',
            '--data-dir', str(root / 'data'), '--config-dir', str(root / 'config')]
        workers = getattr(self, 'parallel_workers', 0)
        if workers:
            if type(workers) is not int or not 1 <= workers <= 128:
                raise ValueError('Use one through 128 parallel workers')
            command += ['--wasm-runtime', 'eos-vm-jit', '--experimental-parallel-engine', 'jit',
                '--experimental-parallel-validation-threads', str(workers),
                '--experimental-parallel-validation-batch-size', '8', '--experimental-parallel-production']
            for contract in ('eosio.token', 'eosio.system'):
                digest = hashlib.sha256((self.contracts/contract/(contract+'.wasm')).read_bytes()).hexdigest()
                command += ['--experimental-parallel-code-hash', digest]
        if not restart:
            command += ['--genesis-json', str(root / 'genesis.json')]
        if replay:
            command += ['--replay-blockchain']
        if hard_replay:
            command += ['--hard-replay-blockchain']
        self.ssh(command)
        peer['process'] = RemoteProcess(self, unit)
        print(f'START {name}: {peer["process"].pid} on second host', flush=True)

    def read_rpc(self, url, method, data=None, *, namespace='chain'):
        if not url.startswith('ssh+peer://'):
            return super().read_rpc(url, method, data, namespace=namespace)
        endpoint = url.replace('ssh+peer://', 'http://', 1) + '/v1/' + namespace + '/' + method
        try:
            response = self.ssh(['curl', '--silent', '--show-error', '--fail-with-body', '--max-time', '5',
                '-H', 'Content-Type: application/json', '--data-binary', json.dumps(data or {}), endpoint])
        except PeerCommandError as error:
            # The existing bounded wait_for already retries OSError/URLError.
            # Never retry HTTP business errors (22) or SSH identity failures (255).
            if error.returncode in (7, 28):
                raise OSError('Peer loopback RPC not ready or timed out') from error
            raise
        return json.loads(response)

    def resources(self, label):
        local = subprocess.run(['bash', '-c', 'free -b; ps -C nodeos -C nodeos.elf -o pid,pcpu,rss,nlwp,etimes,comm'],
                               capture_output=True, text=True, check=True).stdout
        remote = self.ssh(['bash', '-c', 'free -b; ps -C nodeos.elf -o pid,pcpu,rss,nlwp,etimes,comm'])
        self.report['resource_samples'].append(dict(label=label, utc=datetime.now(timezone.utc).isoformat(),
                                                   coordinator=local, peer=remote))
        self.checkpoint()

    def peer_group_outage(self, names):
        """With seven active nodes, stop two peer processes, keeping quorum five."""
        down = [name for name in self.remote_names if name in names][:2]
        self.check(len(down) == 2, 'Seven-node fault uses exactly two currently active peer processes')
        for name in down:
            self.stop(self.peers[name]['process'])
            self.peers[name]['stopped'] = True
        time.sleep(10)
        before = self.rpc('get_info')
        ledger = {name: self.producer(name)['unpaid_blocks'] for name in down}
        time.sleep(50)
        after = self.rpc('get_info')
        self.check(after['last_irreversible_block_num'] > before['head_block_num'] + 12,
                   '7 nodes: five surviving nodes finalize with both peer-host node processes stopped')
        self.check(all(self.producer(name)['unpaid_blocks'] == ledger[name] for name in down),
                   'Both stopped peer processes accrue no new block-reward units')
        self.check(all(not c['banned'] and c['zero_windows'] == 0 for c in self.candidates().values()),
                   'Under-21 correlated process outage creates no blacklist strikes')
        self.report['outages'].append(dict(scope='Two peer processes stopped, VM/control SSH and third peer process remained running',
            nodes=list(names), stopped=list(down), before=before, after=after, unpaid_blocks=ledger))
        for name in down:
            self.start_peer(name, restart=True)
            self.peers[name]['stopped'] = False
        self.wait_for('Both peer-host nodes catch up after group outage', lambda: self.synced(names), 180)

    def finish_small_cluster(self):
        self.report['final_synchronization'] = self.wait_for('Two real VMs agree after actual node outage/restart',
            lambda: self.synced(self.names), 180)
        self.check_resource_split('After two actual-node test')
        self.resources('Two actual nodes, one on each equal VM')

    def tests(self):
        self.tx(['system', 'voteproducer', 'approve', 'alice', 'eosio'])
        self.action('deposit', {'owner': 'alice', 'amount': '100.0000 SYS'}, 'alice')
        self.action('buyrex', {'from': 'alice', 'amount': '100.0000 SYS'}, 'alice')
        self.observe()
        first = self.rpc('get_info')['last_irreversible_block_num'] + 1
        expected = {'eosio': 100}
        for index, name in enumerate(self.names[1:], 1):
            self.start_peer(name)
            self.wait_for(name + ' cross-host/common LIB', lambda n=name: self.synced(['eosio', n]))
            self.register(name)
            weight = 100 + index
            self.stake(name, weight, 0)
            self.tx(['system', 'voteproducer', 'approve', name, name])
            expected[name] = weight
            self.observe()
            count = index + 1
            if count in (2, 4, 7):
                names = self.names[:count]
                self.wait_observing(f'{count} genuinely active nodes',
                    lambda _i, members, _r, n=names: set(members) == set(n), 900)
                self.wait_for(f'{count} settled producer/finalizer policies', lambda n=names: self.policies(n), 180)
                self.sample_blocks(names, f'{count} nodes across two hosts', rounds=1)
                self.resources(f'{count} active nodes')
                self.report['stages'].append(dict(count=count, agreement=self.synced(names)))
                if count in (2, 4):
                    self.outage(names)
                    self.check(all(not c['banned'] and c['zero_windows'] == 0 for c in self.candidates().values()),
                               f'No under-21 blacklist after {count}-node fault')
                if count == 7:
                    self.peer_group_outage(names)
        if len(self.names) == 2:
            self.ranking(expected, 'Two actual nodes, exact votes and ranking')
            self.check(len(self.process_evidence()) == 2 and len(self.remote_names) == 1,
                       'Exactly two independently keyed nodes, one on each VM')
            self.action('claimrewards', {'owner': 'eosio'}, 'eosio',
                        failure='do not satisfy declared authorizations')
            self.check(True, 'Sealed eosio authority cannot be bypassed by its old genesis management key')
            name = 'nodea'
            before = self.table('accounts', code='eosio.token', scope=name)[0]['balance']
            producer_before = self.producer(name)
            self.action('claimrewards', {'owner': name}, name, failure='within past day')
            after = self.table('accounts', code='eosio.token', scope=name)[0]['balance']
            producer_after = self.producer(name)
            self.check(before == after and producer_before['last_claim_time'] == producer_after['last_claim_time'] and
                       int(producer_after['unpaid_blocks']) >= int(producer_before['unpaid_blocks']) > 0,
                       'Ordinary two-node producer accrues block rewards; unchanged 24-hour cooldown rejects early claim atomically')
            self.check(all(int(self.producer(n)['unpaid_blocks']) > 0 for n in self.names),
                       'Both actually producing accounts have positive unpaid block-reward units')
            self.report['reward_checks'] = dict(ordinary_owner=name, before=before, after=after,
                prior_unpaid_blocks=producer_before['unpaid_blocks'], unpaid_blocks=producer_after['unpaid_blocks'],
                genesis_management_key_rejected=True, early_claim_rejected=True,
                live_24_hour_payout_not_tested=True)
            self.finish_small_cluster()
            return
        self.check(len({p['pid'] for p in self.process_evidence().values()}) == 22,
                   '22 independently keyed processes on two actual VMs started')
        self.ranking(expected, '22 real registered/voted producers across two hosts')
        self.resources('22 processes, committee still growing')
        self.wait_observing('all 22 actually produced; full 21-seat committee',
            lambda _i, members, _r: len(members) == 21 and self.observed == set(self.names), 7200)
        self.check(set(len(e['schedule']['producers']) for e in self.report['committee_events']) == set(range(1, 22)),
                   'Every actual committee size 1 through 21 observed without bulk admission')
        self.check(all(not c['banned'] for c in self.candidates().values()), 'No false blacklist of healthy processes')
        self.wait_lib(self.rpc('get_info')['head_block_num'] + 12, timeout=150)
        produced = self.scan_finalized(first, self.rpc('get_info')['last_irreversible_block_num'], 'All 22 real finalized producers')
        self.check(set(produced['counts']) == set(self.names), 'All 22 produced irreversible blocks, no permanent standby')
        self.resources('21 active / 22 have produced')
        self.fault_test()
        self.report['final_synchronization'] = self.wait_for('All 22 agree after failure/restart', lambda: self.synced(self.names), 180)
        self.resources('Final post-failure agreement')
        self.check_resource_split('After cross-host admission and actual outage')


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('build-dir', 'contracts-dir', 'state-parent', 'peer-key', 'peer-known-hosts'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--peer-ip', required=True)
    parser.add_argument('--coordinator-ip', required=True)
    parser.add_argument('--node-count', type=int, choices=(2, 22), default=22,
                        help='First run 2 (one real node per VM); only then 22 balanced processes')
    args = parser.parse_args()
    def stop(_signum, _frame):
        raise KeyboardInterrupt('Stopping only disposable cross-host test processes')
    signal.signal(signal.SIGTERM, stop)
    raise SystemExit(CloudMultiNodeTest(args).execute())


if __name__ == '__main__':
    main()
