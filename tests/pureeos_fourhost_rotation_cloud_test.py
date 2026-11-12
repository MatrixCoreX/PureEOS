#!/usr/bin/env python3
"""Four matching VMs, 30 -> 40 independent nodes; real-clock rotation, not TPS."""
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
from urllib.parse import urlsplit

from pureeos_large_multinode_local_test import LargeMultiNodeTest
from pureeos_multihost_cloud_test import RemoteProcess, PeerCommandError, validate_listener_ports
from pureeos_single_choice_local_test import LocalTest
from pureeos_rotation_cohort_cloud_test import CohortCloudTest, cohort_names
from pureeos_host_metrics import collect as collect_host_metrics

ROLES = ('a', 'b', 'c', 'd')


def placement(names):
    if len(names) != 40 or len(set(names)) != 40 or names[0] != 'eosio':
        raise ValueError('Exactly 40 distinct candidate accounts required')
    return {name: ROLES[index % 4] for index, name in enumerate(names)}


def safe_fault_group(members, hosts):
    if len(members) != 21 or len(set(members)) != 21:
        return None
    threshold = len(members) * 2 // 3 + 1
    groups = {r: [n for n in members if hosts[n] == r] for r in ROLES[1:]}
    choices = [(len(ns), role, ns) for role, ns in groups.items() if 0 < len(ns) <= 21 - threshold]
    return max(choices) if choices else None


def peer_endpoint(url):
    parsed = urlsplit(url)
    role = parsed.scheme.removeprefix('ssh+peer-')
    if (parsed.scheme != 'ssh+peer-' + role or role not in ROLES[1:] or
            parsed.hostname != '127.0.0.1' or parsed.path or parsed.query or parsed.fragment or
            parsed.username or parsed.password or parsed.port not in range(29201, 29211)):
        raise ValueError('Only exact task-host loopback RPC endpoints allowed')
    return role, 'http://127.0.0.1:' + str(parsed.port)


class HostOwner:
    def __init__(self, owner, role): self.owner, self.role = owner, role
    def ssh(self, arguments, **kwargs): return self.owner.ssh_host(self.role, arguments, **kwargs)


class HostProcess(RemoteProcess):
    def __init__(self, owner, role, unit):
        super().__init__(HostOwner(owner, role), unit)
        self.pid = role + ':' + self.pid


class FourHostRotationTest(CohortCloudTest):
    def __init__(self, args):
        self.args = args
        self.names = cohort_names(40)
        self.placement = placement(self.names)
        self.hosts = json.loads(args.hosts_json.read_text())
        if set(self.hosts) != set(ROLES): raise ValueError('Four exact task hosts required')
        ips = [str(ipaddress.IPv4Address(self.hosts[r]['main_ip'])) for r in ROLES]
        if len(set(ips)) != 4 or not all(ipaddress.IPv4Address(ip).is_global for ip in ips):
            raise ValueError('Four distinct current task public IPv4 addresses required')
        if len({self.hosts[r]['id'] for r in ROLES}) != 4:
            raise ValueError('Four independent VM UUIDs required')
        self.remote_names = tuple(n for n in self.names if self.placement[n] != 'a')
        self.coordinator_ip, self.peer_ip = ips[:2]
        self._network_checked = False
        self._last_resources = 0
        self.metrics_since_utc = datetime.now(timezone.utc).isoformat()
        LargeMultiNodeTest.__init__(self, args.build_dir.resolve(), args.contracts_dir.resolve(), args.state_parent.resolve())
        self.p2p_port = 29000
        self.ports.add(self.p2p_port)
        with socket.socket() as probe: probe.bind(('0.0.0.0', self.p2p_port))
        validate_listener_ports([self.p2p_port], tuple(map(int,
            Path('/proc/sys/net/ipv4/ip_local_port_range').read_text().split())))
        self.peer_root = Path('/opt/pureeos-peer') / self.root.name
        for role in ROLES[1:]:
            ports = [*range(29111, 29121), *range(29201, 29211)]
            code = ('import json,socket,pathlib; ports=' + repr(ports) + '; sockets=[]\n'
                'for p in ports:\n s=socket.socket(); s.bind(("0.0.0.0",p)); sockets.append(s)\n'
                'print(json.dumps(list(map(int,pathlib.Path("/proc/sys/net/ipv4/ip_local_port_range").read_text().split()))))')
            ephemeral = json.loads(self.ssh_host(role, ['python3', '-c', code]))
            validate_listener_ports(ports, ephemeral)
        self.configure_cohort_state()
        self.report.update(scope=__doc__, vm_hosts=4, node_limit=40, resource_samples=[],
            host_assignment=self.placement, peer_roots={r: str(self.peer_root) for r in ROLES[1:]},
            scope_limit='40 logical nodes across four VMs; physical placement not proven independent; no TPS claim',
            fourhost_script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            metrics_since_utc=self.metrics_since_utc,
            max_clients_per_node=128, p2p_topology='Every node connects to representatives on the other three hosts')

    def ssh_host(self, role, arguments, timeout=30, input_text=None):
        if role not in ROLES[1:]: raise ValueError('Not a remote task host')
        item = self.hosts[role]
        command = ['ssh', '-i', item['key_path'], '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes',
            '-o', 'ConnectTimeout=5', '-o', 'StrictHostKeyChecking=yes',
            '-o', 'UserKnownHostsFile=' + item['known_hosts'], '-o', 'ControlMaster=auto',
            '-o', 'ControlPersist=60', '-o', 'ControlPath=/opt/pureeos-modern/mesh-ssh-%C',
            'root@' + item['main_ip'], shlex.join(map(str, arguments))]
        result = subprocess.run(command, input=input_text, capture_output=True, text=True, timeout=timeout)
        if result.returncode: raise PeerCommandError(result.returncode, self.redact(result.stderr[-1600:]))
        return result.stdout

    def backbone(self, role):
        return [self.hosts[r]['main_ip'] + ':' + ('29000' if r == 'a' else '29111')
                for r in ROLES if r != role]

    def start_node(self, phase, extra=()):
        path = self.root / 'config/config.ini'
        with path.open('a') as config:
            for endpoint in self.backbone('a'): config.write('p2p-peer-address = ' + endpoint + '\n')
        return super().start_node(phase, extra)

    def wait_lib(self, target, timeout=50):
        info = LargeMultiNodeTest.wait_lib(self, target, timeout)
        if not self._network_checked:
            for role in ROLES[1:]:
                output = self.ssh_host(role, ['python3', '-c',
                    'import socket;s=socket.create_connection((' + repr(self.coordinator_ip) + ',29000),5);s.close();print("ok")'])
                if output.strip() != 'ok': raise RuntimeError('Real cross-host P2P preflight failed: ' + role)
            self._network_checked = True
            self.report['three_peer_to_coordinator_tcp_preflights'] = 'passed'
        return info

    def start_peer(self, name, restart=False):
        role = self.placement[name]
        if role == 'a':
            if not restart:
                # The inherited helper writes its config just before spawn.
                self._starting_local_peer = True
            try: return LargeMultiNodeTest.start_peer(self, name, restart)
            finally: self._starting_local_peer = False
        if not restart:
            local_names = [n for n in self.names if self.placement[n] == role]
            index = local_names.index(name)
            root = self.peer_root / name
            config = ['plugin = eosio::producer_plugin', 'plugin = eosio::chain_api_plugin',
                'producer-name = ' + name, 'enable-stale-production = true', 'max-clients = 128',
                'chain-state-db-size-mb = 128', 'chain-state-db-guard-size-mb = 4',
                'resource-monitor-space-absolute-gb = 8', 'p2p-max-nodes-per-host = 64',
                'chain-threads = 1', 'http-threads = 1', 'net-threads = 1', 'vote-threads = 1',
                'http-max-response-time-ms = 200', 'abi-serializer-max-time-ms = 200',
                f'http-server-address = 127.0.0.1:{29201 + index}',
                f'p2p-listen-endpoint = 0.0.0.0:{29111 + index}', 'verbose-http-errors = true']
            config += ['p2p-peer-address = ' + ep for ep in self.backbone(role)]
            for key in (self.accounts[name], self.bls[name]):
                config.append('signature-provider = ' + key['Public key'] + '=KEY:' + key['Private key'])
            document = dict(root=str(root), config='\n'.join(config) + '\n',
                genesis=(self.root / 'genesis.json').read_text())
            copied = self.ssh_host(role, ['python3', '-c',
                'import json,pathlib,sys,os,hashlib;os.umask(0o077);v=json.load(sys.stdin);r=pathlib.Path(v["root"]);'
                '(r/"config").mkdir(parents=True);(r/"config/config.ini").write_text(v["config"]);'
                '(r/"genesis.json").write_text(v["genesis"]);'
                'print(hashlib.sha256((r/"genesis.json").read_bytes()).hexdigest())'], input_text=json.dumps(document))
            if copied.strip() != hashlib.sha256(document['genesis'].encode()).hexdigest():
                raise RuntimeError('Remote genesis copy/hash verification failed on ' + role)
            self.peers[name] = dict(root=root, role=role, url=f'ssh+peer-{role}://127.0.0.1:{29201 + index}', remote=True)
        peer = self.peers[name]
        peer['starts'] = peer.get('starts', 0) + 1
        unit = f'pureeos-peer-{self.root.name}-{name}-{peer["starts"]}'
        command = ['systemd-run', '--quiet', '--unit=' + unit, '--service-type=exec',
            '--property=KillMode=control-group', '--property=RuntimeMaxSec='+str(self.peer_runtime_seconds),
            '--property=TimeoutStopSec=30', '--property=MemoryMax=900M', '--property=MemorySwapMax=256M',
            '--property=StandardOutput=append:' + str(peer['root'] / f'node-{peer["starts"]}.log'),
            '--property=StandardError=inherit', '/opt/pureeos-peer/nodeos.elf',
            '--data-dir', str(peer['root'] / 'data'), '--config-dir', str(peer['root'] / 'config')]
        if not restart: command += ['--genesis-json', str(peer['root'] / 'genesis.json')]
        self.ssh_host(role, command)
        peer['process'] = HostProcess(self, role, unit)
        print(f'START {name}: {peer["process"].pid} on task host {role}', flush=True)

    def spawn(self, command, log_name):
        if getattr(self, '_starting_local_peer', False):
            path = Path(command[command.index('--config-dir') + 1]) / 'config.ini'
            with path.open('a') as config:
                for ep in self.backbone('a'): config.write('p2p-peer-address = ' + ep + '\n')
        return super().spawn(command, log_name)

    def read_rpc(self, url, method, data=None, *, namespace='chain'):
        if not url.startswith('ssh+'):
            return LocalTest.read_rpc(self, url, method, data, namespace=namespace)
        role, endpoint = peer_endpoint(url)
        try:
            output = self.ssh_host(role, ['curl', '--silent', '--show-error', '--fail-with-body', '--max-time', '5',
                '-H', 'Content-Type: application/json', '--data-binary', json.dumps(data or {}),
                endpoint + '/v1/' + namespace + '/' + method])
        except PeerCommandError as error:
            if error.returncode in (7, 28): raise OSError('Task peer RPC not ready or timed out') from error
            raise
        return json.loads(output)

    def resources(self, label):
        collector = Path(__file__).with_name('pureeos_host_metrics.py').read_text()
        since = getattr(self, 'metrics_since_utc', None)
        samples = {'a': collect_host_metrics(since=since)}
        for role in ROLES[1:]:
            args = ['python3', '-c', collector]
            if since: args += ['--since', since]
            samples[role] = json.loads(self.ssh_host(role, args))
        self.report['resource_samples'].append(dict(label=label, utc=datetime.now(timezone.utc).isoformat(), hosts=samples))
        for role, sample in samples.items():
            if sample['status'] != 'complete':
                self.report.setdefault('resource_collection_errors', []).append(dict(
                    role=role, utc=sample['utc'], label=label, probes=sample['probes']))
                print('RESOURCE EVIDENCE INCOMPLETE on ' + role + '; raw probe errors retained', flush=True)
        self.report['resource_evidence_status'] = 'incomplete' if self.report.get('resource_collection_errors') else 'complete'
        self._last_resources = time.monotonic()
        self.checkpoint()

    def observe(self):
        result = super().observe()
        if time.monotonic() - self._last_resources > 120: self.resources('Periodic in-flight memory/process/OOM evidence')
        return result

    def additional_topology_fault(self):
        # Stop every NODE PROCESS on one remote VM, not the OS or VM itself.
        choice = self.wait_observing('full committee with a quorum-safe peer-host fault group',
            lambda _i, members, r: safe_fault_group(members, self.placement) if r['stage'] == 0 else False, 1200)
        count, role, active_down = choice
        down = [n for n in self.names if self.placement[n] == role]
        before = self.rpc('get_info')
        prior_fault = self.report['fault']
        prior_banned = {n for n, row in self.candidates().items() if row['banned']}
        self.report['fault'] = dict(host_group_only=True, role=role)
        for n in down:
            self.stop(self.peers[n]['process']); self.peers[n]['stopped'] = True
        time.sleep(8)  # Drain blocks/votes already in flight before measuring new finality.
        begin = self.rpc('get_info')
        ledger = {n: self.producer(n)['unpaid_blocks'] for n in down}
        self.wait_lib(begin['head_block_num'] + 20, 180)
        self.check(all(self.producer(n)['unpaid_blocks'] == ledger[n] for n in down),
                   'All stopped peer-host processes accrue no new block-reward units')
        after = self.rpc('get_info')
        self.report['host_group_outage'] = dict(role=role, stopped=down, active_stopped=active_down,
            surviving_finalizers=21 - count, threshold=15, before=before, after=after,
            scope='Node-process group only, not a VM power-off')
        for n in down:
            self.start_peer(n, restart=True); self.peers[n]['stopped'] = False
        self.wait_for('All forty original ledgers recover after peer-host process-group fault',
                      lambda: self.synced(self.names), 300)
        self.check({n for n, row in self.candidates().items() if row['banned']} == prior_banned,
                   'Short host-group fault did not add a false permanent blacklist')
        self.report['fault'] = prior_fault
        self.report['final_synchronization'] = self.synced(self.names)
        self.resources('Final forty-node agreement after peer-host process-group recovery')
        self.check(True, 'Whole peer-host node-process group recovered without false permanent bans')


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('build-dir', 'contracts-dir', 'state-parent', 'hosts-json'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    def stop(_signal, _frame):
        # A systemd control-group SIGTERM can also arrive during subprocess
        # cleanup. Keep the first interruption, but never interrupt final report
        # persistence a second time.
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        raise KeyboardInterrupt('Stopping only four-host private test processes')
    signal.signal(signal.SIGTERM, stop)
    raise SystemExit(FourHostRotationTest(args).execute())


if __name__ == '__main__': main()
