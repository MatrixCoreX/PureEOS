#!/usr/bin/env python3
"""Create an isolated, activated reward chain; retain it for manual testing.

Uses real transactions and the production 14-day governance policy. Never
changes another chain. Copied runtime and contracts make the resulting directory
independent of future builds. All generated keys are disposable local test keys.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time
import urllib.request

from pureeos_single_choice_local_test import LocalTest
from native_savanna_genesis_test import free_port


def identity(pid):
    proc = Path('/proc') / str(pid)
    try:
        stat = (proc / 'stat').read_text().split(') ', 1)[1].split()
        if stat[0] == 'Z':
            return None
        return {'pid': pid, 'start_ticks': stat[19], 'exe': str((proc / 'exe').resolve()),
                'args': (proc / 'cmdline').read_bytes().decode().split('\0')[:-1]}
    except (FileNotFoundError, ProcessLookupError):
        return None


def matching(record):
    return record is not None and identity(record['pid']) == record


def await_identity(process, command):
    expected = [str(a) for a in command]
    for _ in range(100):
        record = identity(process.pid)
        # /proc/exe can become visible before cmdline after execve. Recording
        # that transient empty argv would make safe status/stop reject our own
        # live node later. Never weaken the identity comparison to compensate.
        if record and record['exe'] == str(Path(command[0]).resolve()) and record['args'] == expected:
            return record
        if process.poll() is not None:
            raise RuntimeError('Child exited before its process identity could be recorded')
        time.sleep(.01)
    process.terminate()
    process.wait(timeout=15)
    raise RuntimeError('Could not record complete child process identity')


class RewardChain(LocalTest):
    local_signers = ("eosio",)  # This helper explicitly adds only nodea below.
    def __init__(self, build, contracts, directory, clock_library):
        if directory.exists():
            raise RuntimeError('Refusing to overwrite an existing chain directory')
        if not clock_library.is_file():
            raise RuntimeError('A locally built libfaketimeMT library is required for the isolated 25-hour branch test')
        super().__init__(build, contracts, True)
        self.root.rename(directory)
        self.root = directory
        self.report['directory'] = str(directory)
        self.report['scope'] = 'Activated native Savanna chain; real halving-policy issuance and producer claims'
        self.report['test_only_max_supply'] = '2000000000.0000 SYS'
        self.report['initial_supply'] = '1000000000.0000 SYS'
        runtime = directory / 'runtime'
        for name in ('nodeos', 'cleos', 'keosd', 'helios-util'):
            target = runtime / 'programs' / name / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(build / 'programs' / name / name, target)
        self.build = runtime
        self.nodeos = runtime / 'programs/nodeos/nodeos'
        self.cleos = runtime / 'programs/cleos/cleos'
        self.util = runtime / 'programs/helios-util/helios-util'
        self.contracts = directory / 'contracts'
        for source in contracts.rglob('*'):
            if source.suffix in ('.wasm', '.abi') and source.is_file():
                target = self.contracts / source.relative_to(contracts)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
        scripts = directory / 'tools'
        scripts.mkdir()
        for name in ('pureeos_reward_chain.py', 'pureeos_single_choice_local_test.py', 'native_savanna_genesis_test.py'):
            shutil.copy2(Path(__file__).parent / name, scripts / name)
        self.registry = {}
        self.prepared = False
        self.clock_library = runtime / 'test-clock/libfaketimeMT.so.1'
        self.clock_library.parent.mkdir()
        shutil.copy2(clock_library, self.clock_library)
        self.report['test_clock_library_sha256'] = hashlib.sha256(self.clock_library.read_bytes()).hexdigest()

    def spawn(self, command, log_name, env=None):
        log = (self.root / log_name).open('w', encoding='utf-8')
        self.logs.append(log)
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True, env=env)
        record = await_identity(process, command)
        self.registry['wallet' if log_name == 'wallet.log' else 'node'] = record
        (self.root / 'processes.json').write_text(json.dumps(self.registry, indent=2))
        return process

    def start_node(self, phase, extra=()):
        if not self.prepared:
            config = self.root / 'config/config.ini'
            with config.open('a') as output:
                output.write('producer-name = nodea\n')
                for key in (self.accounts['nodea'], self.bls['nodea']):
                    output.write('signature-provider = ' + key['Public key'] + '=KEY:' + key['Private key'] + '\n')
            (self.root / 'test-keys.json').write_text(json.dumps({'accounts': self.accounts, 'bls': self.bls}, indent=2))
            self.prepared = True
        super().start_node(phase, extra)

    def action(self, name, data, actor, *, contract='eosio', failure=None):
        if name == 'create' and contract == 'eosio.token':
            data = {**data, 'maximum_supply': '2000000000.0000 SYS'}
        if name == 'govinit' and failure is None:
            # Bootstrap authority is still valid here. No post-sealing bypass.
            self.tx(['set', 'contract', 'eosio.fees', str(self.contracts / 'eosio.fees')])
            for node in ('nodea', 'nodeb'):
                super().action('regproducer', {'producer': node, 'producer_key': self.accounts[node]['Public key'],
                                              'url': '', 'location': 0}, node)
                super().action('regfinkey', {'finalizer_name': node, 'finalizer_key': self.bls[node]['Public key'],
                                            'proof_of_possession': self.bls[node]['Proof of Possession']}, node)
            super().action('voteproducer', {'voter': 'alice', 'proxy': '', 'producers': ['nodea']}, 'alice')
            super().action('deposit', {'owner': 'alice', 'amount': '100.0000 SYS'}, 'alice')
            super().action('buyrex', {'from': 'alice', 'amount': '100.0000 SYS'}, 'alice')
        return super().action(name, data, actor, contract=contract, failure=failure)

    def balance(self, owner):
        rows = self.table('accounts', code='eosio.token', scope=owner)
        return self.asset_units(rows[0]['balance']) if rows else 0

    def snapshot(self):
        return {'info': self.rpc('get_info'), 'global': self.table('globalv2')[0],
                'policy': self.table('rewardpolicy')[0], 'parameters': self.table('global4')[0],
                'rotation': self.table('rotation')[0], 'producers': self.table('producersv2'),
                'stat': self.table('stat', code='eosio.token', scope='SYS')[0],
                'balances': {a: self.balance(a) for a in ('eosio', 'eosio.saving', 'eosio.bpay', 'eosio.vpay', 'nodea', 'nodeb')}}

    def reward_test(self):
        before = self.snapshot()
        producer = next(p for p in before['producers'] if p['owner'] == 'nodea')
        age = (datetime.datetime.fromisoformat(before['info']['head_block_time']) -
               datetime.datetime.fromisoformat(producer['last_claim_time'])).total_seconds()
        self.check(age > 86400 and int(producer['unpaid_blocks']) > 0,
                   'Isolated process branch exceeds real contract 24-hour first-claim cooldown')
        self.check(before['policy']['current_era'] == 0 and before['policy']['halving_seconds'] == 1461 * 86400,
                   'Activation starts fixed four-year halving clock at era zero')
        self.check(before['parameters']['inflation_pay_factor'] == 10000 and before['parameters']['votepay_factor'] == 10000,
                   'New issuance is entirely block funding, with vote pay disabled')
        self.action('claimrewards', {'owner': 'nodeb'}, 'nodeb', failure='standby rewards are disabled')
        self.check(True, 'Registered zero-output node cannot claim either reward component')
        claim = self.action('claimrewards', {'owner': 'nodea'}, 'nodea')
        transfers, issues = [], []
        def visit(traces):
            for trace in traces:
                action = trace['act']
                if trace['receiver'] == 'eosio.token' and action['account'] == 'eosio.token':
                    if action['name'] == 'transfer':
                        transfers.append(action['data'])
                    elif action['name'] == 'issue':
                        issues.append(action['data'])
                visit(trace.get('inline_traces', []))
        visit(claim['processed']['action_traces'])
        def amount(source, target):
            return sum(self.asset_units(t['quantity']) for t in transfers if t['from'] == source and t['to'] == target)
        block, vote = amount('eosio', 'eosio.bpay'), amount('eosio', 'eosio.vpay')
        issued = sum(self.asset_units(t['quantity']) for t in issues)
        after = self.snapshot()
        self.check(issued > 0 and issued == block and vote == 0,
                   'Real token issue equals block funding in full, with zero vote funding')
        self.check(self.asset_units(after['stat']['supply']) - self.asset_units(before['stat']['supply']) == issued,
                   'Token supply actually increases, not just treasury redistribution')
        self.check(not any(t['to'] == 'eosio.saving' for t in transfers) and
                   after['balances']['eosio.saving'] == before['balances']['eosio.saving'] == 0,
                   'Cancelled savings portion is neither issued nor transferred')
        bp, vp = amount('eosio.bpay', 'nodea'), amount('eosio.vpay', 'nodea')
        self.check(bp > 0 and vp == 0 and after['balances']['nodea'] - before['balances']['nodea'] == bp and
                   after['balances']['eosio.vpay'] == before['balances']['eosio.vpay'] == 0,
                   'Producing nodea receives only block pay; vote pool remains empty')
        self.check(not self.table('producers2'), 'Registration and voting create no legacy vote-pay share rows')
        self.action('claimrewards', {'owner': 'nodea'}, 'nodea', failure='already claimed rewards within past day')
        self.check(True, 'Second claim within 24 hours is rejected')
        self.report['reward_evidence'] = {'before': before, 'after': after, 'issued': issued,
                                          'block_funding': block, 'vote_funding': vote,
                                          'block_paid': bp, 'vote_paid': vp,
                                          'transaction_id': claim['transaction_id'], 'endpoint': self.url,
                                          'scope': 'isolated data-copy branch with +25h nodeos clock; not the manual branch'}
        (self.root / 'clock-check/claim-trace.json').write_text(json.dumps(claim, indent=2))
        self.check_resource_split('Activated reward chain')

    def clock_branch_test(self):
        # Real nodeos/cleos transactions, unchanged production cooldown, and no
        # host clock adjustment. Only a disposable copy runs with +25h time.
        # The user-facing manual chain is restarted on its original real-time
        # data; future-dated blocks never enter that branch.
        before = self.snapshot()
        branch = self.root / 'clock-check'
        branch.mkdir()
        self.stop(self.node)
        shutil.copytree(self.root / 'data', branch / 'data')
        shutil.copytree(self.root / 'config', branch / 'config')
        http, p2p = free_port(), free_port()
        while len({http, p2p, self.http_port, self.p2p_port}) != 4:
            http, p2p = free_port(), free_port()
        config = branch / 'config/config.ini'
        config.write_text(config.read_text().replace('127.0.0.1:' + str(self.http_port), '127.0.0.1:' + str(http))
                          .replace('127.0.0.1:' + str(self.p2p_port), '127.0.0.1:' + str(p2p)))
        original_url = self.url
        self.url = f'http://127.0.0.1:{http}'
        env = os.environ.copy()
        env.update(LD_PRELOAD=str(self.clock_library), FAKETIME='+25h',
                   FAKETIME_DONT_FAKE_MONOTONIC='1', NO_FAKE_STAT='1')
        first_transaction = len(self.report.get('confirmed_transactions', []))
        try:
            self.node = self.spawn([str(self.nodeos), '--data-dir', str(branch / 'data'),
                                   '--config-dir', str(branch / 'config'),
                                   '--resource-monitor-space-absolute-gb', '8'], 'clock-check/node.log', env=env)
            self.wait_lib(before['info']['head_block_num'] + 10, timeout=90)
            self.reward_test()
            for tx in self.report['confirmed_transactions'][first_transaction:]:
                tx['branch'] = 'clock-check (+25h process clock)'
            self.report['clock_branch'] = {'directory': str(branch), 'seconds_forward': 90000,
                                            'independent_chain_id': False, 'disposable_copy': True}
        finally:
            self.stop(self.node)
            self.url = original_url
            self.start_node('normal-clock-restored')
        self.wait_lib(before['info']['last_irreversible_block_num'] + 10)
        after = self.snapshot()
        self.check(after['stat'] == before['stat'] and after['balances'] == before['balances'],
                   'Manual real-time branch retains original supply and balances; simulated-branch issuance did not leak')
        self.check(after['policy'] == before['policy'], 'Manual branch activation clock survives clean restart unchanged')
        self.report['clock_branch']['process_stopped'] = True
        self.report['manual_branch_first_claim_not_yet_due'] = True

    def execute(self):
        try:
            self.setup(register_candidates=False)
            deadline = time.monotonic() + 180
            while time.monotonic() < deadline:
                producers = self.table('producersv2')
                if any(p['owner'] == 'nodea' and int(p['unpaid_blocks']) > 0 for p in producers):
                    break
                time.sleep(1)
            else:
                raise RuntimeError('Nodea did not start actual block production')
            self.action('claimrewards', {'owner': 'nodea'}, 'nodea', failure='already claimed rewards within past day')
            self.check(True, 'Normal-clock first claim before 24 hours rejects without shortening production rule')
            self.clock_branch_test()
            self.run(['wallet', 'lock_all'])
            self.report['status'] = 'passed'
            self.report['left_running'] = True
            self.report['wallet_locked'] = True
            self.report['final_info'] = self.rpc('get_info')
        except Exception as error:
            self.report.update(status='failed', error=self.redact(str(error)), left_running=False)
            for process in (self.node, self.wallet):
                try:
                    self.stop(process)
                except Exception as shutdown:
                    self.report.setdefault('shutdown_errors', []).append(str(shutdown))
            print('FAIL: ' + self.report['error'], flush=True)
        finally:
            (self.root / 'report.json').write_text(json.dumps(self.report, indent=2))
            print('Report: ' + str(self.root / 'report.json'), flush=True)
        return 0 if self.report['status'] == 'passed' else 1


def control(root, operation, cleos_args):
    report = json.loads((root / 'report.json').read_text())
    path = root / 'processes.json'
    records = json.loads(path.read_text())
    if operation == 'status':
        result = {'endpoint': report['endpoint'], 'processes': {k: matching(v) for k, v in records.items()}}
        try:
            with urllib.request.urlopen(report['endpoint'] + '/v1/chain/get_info', timeout=5) as response:
                result['chain'] = json.load(response)
        except OSError as error:
            result['rpc_error'] = str(error)
        print(json.dumps(result, indent=2))
        return 0
    if operation == 'stop':
        for name in ('node', 'wallet'):
            record = records.get(name)
            if matching(record):
                os.kill(record['pid'], signal.SIGTERM)
                deadline = time.monotonic() + 25
                while matching(record) and time.monotonic() < deadline:
                    time.sleep(.2)
                if matching(record):
                    raise RuntimeError(name + ' did not stop cleanly; not sending a forced kill')
        print('Only this directory\'s matching node and wallet processes were stopped.')
        return 0
    if operation == 'start':
        for name, binary, data, extra in (
            ('wallet', 'keosd', 'wallet', ['--http-server-address', '']),
            ('node', 'nodeos', 'data', ['--resource-monitor-space-absolute-gb', '8'])):
            if matching(records.get(name)):
                continue
            command = [str(root / f'runtime/programs/{binary}/{binary}'), '--data-dir', str(root / data),
                       '--config-dir', str(root / ('config' if name == 'node' else 'wallet')), *extra]
            with (root / (name + '-manual.log')).open('a') as log:
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            records[name] = await_identity(process, command)
            path.write_text(json.dumps(records, indent=2))
        print('Started this directory\'s node and wallet; use status to verify LIB advancement.')
        return 0
    command = [str(root / 'runtime/programs/cleos/cleos'), '--no-auto-keosd', '--url', report['endpoint'],
               '--wallet-url', f'unix://{root}/wallet/keosd.sock']
    if operation == 'unlock':
        result = subprocess.run(command + ['wallet', 'unlock', '-n', 'localtest'],
                                input=(root / 'wallet/password.txt').read_text().strip() + '\n', text=True,
                                capture_output=True)
        if result.returncode:
            raise RuntimeError('Could not unlock local test wallet (already unlocked or wallet unavailable)')
        print('Local test wallet unlocked. Use cleos wallet lock_all when finished.')
        return 0
    return subprocess.run(command + cleos_args).returncode


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=['create', 'status', 'start', 'stop', 'unlock', 'cleos'])
    parser.add_argument('--directory', type=Path, required=True)
    project = Path(__file__).resolve().parents[1]
    parser.add_argument('--build-dir', type=Path, default=project / 'build')
    parser.add_argument('--contracts-dir', type=Path, default=project.parent / 'reference-contracts/build/pureeos-rotation-v2')
    parser.add_argument('--clock-library', type=Path, help='Test-only libfaketimeMT.so.1, loaded only into a disposable data-copy node')
    args, remainder = parser.parse_known_args()
    root = args.directory.resolve()
    if args.operation == 'create':
        if remainder:
            parser.error('Unexpected create arguments')
        if args.clock_library is None:
            parser.error('create requires --clock-library for the isolated 25-hour branch test')
        return RewardChain(args.build_dir.resolve(), args.contracts_dir.resolve(), root, args.clock_library.resolve()).execute()
    return control(root, args.operation, remainder[1:] if remainder[:1] == ['--'] else remainder)


if __name__ == '__main__':
    sys.exit(main())
