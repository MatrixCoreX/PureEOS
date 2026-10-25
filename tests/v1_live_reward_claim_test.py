#!/usr/bin/env python3
"""Unchanged real 24h claim/blacklist history on a disposable native chain.

Three nodeos ledgers on ONE host: disjoint signing groups (19+2+1 for the real-day
test, 1+2+1 for the fault preflight). Stop ONLY the singleton fault process.
The secondary also signs; NOT 22 independent nodes or a TPS test.
No clock preload, time acceleration, setprods or production-constant changes.
"""
import argparse
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import tempfile
import time

from pureeos_large_multinode_local_test import LargeMultiNodeTest

FIXTURE_WITNESSES=('nodea','nodeb')


def signer_partition(names, victim=None):
    assert len(names)>=4 and len(names)==len(set(names)) and 'eosio' in names
    assert set(FIXTURE_WITNESSES)<=set(names)
    if victim is None:victim='nodeq' if 'nodeq' in names else next(n for n in names if n not in ('eosio',*FIXTURE_WITNESSES))
    assert victim in names and victim not in ('eosio',*FIXTURE_WITNESSES)
    primary=tuple(n for n in names if n not in FIXTURE_WITNESSES and n!=victim)
    assert set(primary).isdisjoint(FIXTURE_WITNESSES)
    assert set(primary)|set(FIXTURE_WITNESSES)|{victim}==set(names)
    return primary,FIXTURE_WITNESSES,(victim,)


def ready_offline_identity(names,members,rotation,observed):
    """Fault a historically producing active singleton with at least five seats of tenure left.

    Waiting for that account to leave and return can consume a full 22-account
    cycle. The payout/blacklist test needs three complete missed windows, not
    an additional re-entry; the independent 40-process test covers re-entry.
    """
    active=rotation.get('active',[]);victim=signer_partition(names)[2][0]
    if (rotation.get('stage')!=0 or len(members)!=21 or len(active)!=21
            or len(set(active))!=21 or set(active)!=set(members)
            or victim not in observed or victim not in active or active.index(victim)<4):
        return False
    return victim


def returning_identity_timeout(candidate_count):
    if type(candidate_count) is not int or not 4<=candidate_count<=210:
        raise ValueError('Use the explicitly bounded live-test candidate count')
    return max(1800,candidate_count*600)


def signer_lines(names, accounts, finalizers):
    result=[];public_keys=set()
    for name in names:
        result.append('producer-name = '+name)
        for key in (accounts[name],finalizers[name]):
            assert key['Public key'] not in public_keys, 'Fixture identities must not share signing keys'
            public_keys.add(key['Public key'])
            result.append('signature-provider = '+key['Public key']+'=KEY:'+key['Private key'])
    return result


def partition_complete(report, names):
    proof=report.get('signer_partition',{})
    primary,secondary,fault=signer_partition(names)
    return (proof.get('primary')==list(primary) and proof.get('secondary')==list(secondary)
        and proof.get('fault')==list(fault)
        and proof.get('shared_keys') is False and proof.get('production_pause_guard_unchanged') is True)


def isolated_fault_complete(report):
    proof=report.get('fault_isolation',{})
    return (proof.get('singleton_stopped') is True and proof.get('healthy_pids_unchanged') is True
        and len(proof.get('healthy_pids',[]))==2 and len(set(proof['healthy_pids']))==2
        and all(type(p) is int and p>0 for p in [*proof['healthy_pids'],proof.get('fault_pid')])
        and proof.get('fault_pid') not in proof['healthy_pids'])


def fault_preflight_complete(report):
    proof=report.get('fault_preflight',{})
    return (report.get('status')=='passed' and report.get('fault_preflight_only') is True
        and report.get('real_time_24h') is False and report.get('physical_hosts')==1
        and report.get('processes_stopped') is True and report.get('validator_stopped') is True
        and report.get('fault_signer_stopped') is True and isolated_fault_complete(report)
        and not report.get('shutdown_errors') and partition_complete(report,('eosio','nodea','nodeb','nodec'))
        and proof.get('omitted')=='nodec' and bool(proof.get('agreement'))
        and proof.get('unpaid_before',0)>0 and proof.get('unpaid_before')==proof.get('unpaid_while_offline')
        and proof.get('unpaid_after_recovery',0)>proof.get('unpaid_before',0)
        and proof.get('after',{}).get('last_irreversible_block_num',0)>
            proof.get('before',{}).get('head_block_num',0)+48)


def due(chain_time, last_claim, registered_monotonic, now_monotonic):
    def utc(value):
        result = datetime.fromisoformat(value)
        return result.replace(tzinfo=timezone.utc) if result.tzinfo is None else result.astimezone(timezone.utc)
    return (utc(chain_time) > utc(last_claim)+timedelta(seconds=86400)
        and now_monotonic-registered_monotonic > 86400)


def sha(path):
    with Path(path).open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()


def complete(report):
    claims = report.get('reward_claims', [])
    return (report.get('status') == 'passed' and report.get('processes_stopped') is True
        and report.get('validator_stopped') is True and not report.get('shutdown_errors')
        and report.get('fault_signer_stopped') is True and isolated_fault_complete(report)
        and report.get('real_time_24h') is True and report.get('physical_hosts') == 1
        and not report.get('fault_preflight_only',False) and partition_complete(report,LargeMultiNodeTest.names)
        and len(claims) == 2 and [r.get('historical') for r in claims] == [True, False]
        and len({r.get('transaction_id') for r in claims}) == 2
        and all(r.get('block_paid', 0) > 0 and r.get('agreement') and
            re.fullmatch('[0-9a-f]{64}', r.get('transaction_id', '')) for r in claims)
        and len(report.get('latest_live_wait', {}).get('elapsed_seconds', {})) == 2
        and all(v > 86400 for v in report['latest_live_wait']['elapsed_seconds'].values()))


class LiveClaimTest(LargeMultiNodeTest):
    wallet_unlock_seconds = 36*3600  # Only this disposable, network-isolated wallet.

    def __init__(self, build, contracts, state_parent, *, fault_preflight=False):
        if fault_preflight:self.names=('eosio','nodea','nodeb','nodec')
        super().__init__(build, contracts, state_parent)
        self.validator = None
        self.fault_node = None
        self.fault_expected_running=False
        self.registered_monotonic = {}
        self.fault_preflight_only=fault_preflight
        self.report.update(scope=__doc__, nodeos_process_target=3, fixture_signing_identities=len(self.names),
            process_target=3, physical_hosts=1, real_time_24h=not fault_preflight,
            fault_preflight_only=fault_preflight,elapsed_claim_wait=[],reward_claims=[],
            secondary_role='independent-ledger-and-disjoint-producer-finalizer-identities')

    def process_evidence(self):
        result = super().process_evidence()
        if self.validator is not None: result['independent-validator'] = dict(pid=self.validator.pid, url=self.validator_url)
        if self.fault_node is not None: result['fault-signer'] = dict(pid=self.fault_node.pid, url=self.fault_url)
        return result

    def observe(self):
        if self.validator is not None and self.validator.poll() is not None:
            raise RuntimeError('Independent witness signer exited')
        if self.fault_expected_running and (self.fault_node is None or self.fault_node.poll() is not None):
            raise RuntimeError('Singleton signer exited outside the intended fault')
        return super().observe()

    def spawn(self, command, log_name):
        if command[0] == str(self.nodeos):
            command = [*command, '--wasm-runtime', 'eos-vm-jit', '--experimental-parallel-engine', 'jit',
                '--experimental-parallel-validation-threads', '4', '--experimental-parallel-validation-batch-size', '8']
            command += ['--experimental-parallel-production']
            for name in ('eosio.token', 'eosio.system'):
                command += ['--experimental-parallel-code-hash', sha(self.contracts/name/(name+'.wasm'))]
        return super().spawn(command, log_name)

    def configure_fixture_signers(self):
        primary,secondary,fault=signer_partition(self.names)
        key_sets=[{group[n]['Public key'] for n in owners for group in (self.accounts,self.bls)}
                  for owners in (primary,secondary,fault)]
        assert all(a.isdisjoint(b) for i,a in enumerate(key_sets) for b in key_sets[i+1:]), 'Never copy a signing identity across ledgers'
        self.stop(self.node)
        config = self.root/'config/config.ini'
        lines = [line for line in config.read_text().splitlines()
            if not line.startswith(('producer-name', 'signature-provider'))]
        lines+=signer_lines(primary,self.accounts,self.bls)
        assert not any('production-pause-vote-timeout-ms' in line for line in lines)
        config.write_text('\n'.join(lines)+'\n')
        self.report['signer_partition']=dict(primary=list(primary),secondary=list(secondary),fault=list(fault),
            shared_keys=False,production_pause_guard_unchanged=True)
        self.start_node('fixture-signers-initial')
        self.wait_lib(12, timeout=180)

    def start_validator(self):
        root = self.root/'validator'; (root/'config').mkdir(parents=True)
        self.validator_url = 'http://127.0.0.1:'+str(self.port())
        config = ['plugin = eosio::producer_plugin', 'plugin = eosio::chain_api_plugin',
            'plugin = eosio::net_plugin', 'chain-state-db-size-mb = 128', 'chain-state-db-guard-size-mb = 4',
            'resource-monitor-space-absolute-gb = 1', 'http-server-address = '+self.validator_url.removeprefix('http://'),
            'p2p-listen-endpoint = 127.0.0.1:'+str(self.port()), 'p2p-peer-address = 127.0.0.1:'+str(self.p2p_port),
            'enable-stale-production = true']
        _,secondary,_=signer_partition(self.names)
        config+=signer_lines(secondary,self.accounts,self.bls)
        assert not any('production-pause-vote-timeout-ms' in line for line in config)
        (root/'config/config.ini').write_text('\n'.join(config)+'\n')
        self.validator = self.spawn([str(self.nodeos), '--config-dir', str(root/'config'),
            '--data-dir', str(root/'data'), '--genesis-json', str(self.root/'genesis.json')], 'validator.log')
        self.wait_for('Independent secondary ledger with disjoint signing identities joins', self.validator_agreement, 240)

    def start_fault_signer(self, restart=False):
        root=self.root/'fault-signer'
        if not restart:
            (root/'config').mkdir(parents=True)
            self.fault_url='http://127.0.0.1:'+str(self.port())
            _,_,fault=signer_partition(self.names)
            config=['plugin = eosio::producer_plugin','plugin = eosio::chain_api_plugin',
                'plugin = eosio::net_plugin','chain-state-db-size-mb = 128','chain-state-db-guard-size-mb = 4',
                'resource-monitor-space-absolute-gb = 1','http-server-address = '+self.fault_url.removeprefix('http://'),
                'p2p-listen-endpoint = 127.0.0.1:'+str(self.port()),'p2p-peer-address = 127.0.0.1:'+str(self.p2p_port),
                'enable-stale-production = true',*signer_lines(fault,self.accounts,self.bls)]
            (root/'config/config.ini').write_text('\n'.join(config)+'\n')
        self.fault_node=self.spawn([str(self.nodeos),'--config-dir',str(root/'config'),
            '--data-dir',str(root/'data'),'--genesis-json',str(self.root/'genesis.json')],
            'fault-signer-'+('restart' if restart else 'initial')+'.log')
        self.fault_expected_running=True
        self.wait_for('Singleton fault signer joins the independent ledgers',self.validator_agreement,240)

    def stop_fault_signer(self):
        healthy=(self.node,self.validator)
        before=[p.pid for p in healthy]
        assert self.fault_node is not None and self.fault_node.poll() is None
        assert self.fault_node.pid not in before and all(p.poll() is None for p in healthy)
        self.stop(self.fault_node)
        assert self.fault_node.poll() is not None
        self.fault_expected_running=False
        assert before==[p.pid for p in healthy] and all(p.poll() is None for p in healthy)
        self.report['fault_isolation']=dict(singleton_stopped=True,healthy_pids_unchanged=True,
            healthy_pids=before,fault_pid=self.fault_node.pid)
        self.checkpoint()

    def fault_preflight(self):
        self.wait_observing('Four identities on three ledgers produce under default vote-pause safety',
            lambda _i,m,r: len(m)==4 and r['stage']==0 and self.observed==set(self.names),1800)
        victim='nodec';before=self.rpc('get_info');initial_ledger=int(self.producer(victim)['unpaid_blocks'])
        first_slot=self.slot(before['head_block_time'])
        self.stop_fault_signer()
        self.wait_lib(before['head_block_num']+4,timeout=180)
        ledger=int(self.producer(victim)['unpaid_blocks'])
        def down_progress(info,_members,_rotation):
            return (info if self.slot(info['head_block_time'])-first_slot>=3*4*12
                    and info['last_irreversible_block_num']>before['head_block_num']+48 else False)
        after=self.wait_observing('Three real schedule rounds with one identity offline retain finality',down_progress,300)
        candidate=self.candidates()[victim]
        self.check(not candidate['banned'] and candidate['zero_windows']==0,
            'Below 21 identities, real missed windows never trigger a blacklist')
        self.check(int(self.producer(victim)['unpaid_blocks'])==ledger,
            'Offline fixture identity receives no new block reward units')
        agreement=self.wait_for('Two ledgers agree during the real missing-identity fault',self.validator_agreement,120)
        self.start_fault_signer(restart=True)
        self.wait_observing('Recovered fixture identity produces again without a safety override',
            lambda _i,_m,_r: int(self.producer(victim)['unpaid_blocks'])>ledger,240)
        self.report['fault_preflight']=dict(omitted=victim,before=before,after=after,agreement=agreement,
            unpaid_before_shutdown=initial_ledger,
            unpaid_before=ledger,unpaid_while_offline=ledger,
            unpaid_after_recovery=int(self.producer(victim)['unpaid_blocks']))
        self.checkpoint()

    def validator_agreement(self):
        if self.validator is None or self.validator.poll() is not None:
            raise RuntimeError('Independent validator exited')
        left = self.rpc('get_info')
        urls=[self.validator_url]
        if self.fault_node is not None and self.fault_node.poll() is None:urls.append(self.fault_url)
        others=[self.read_rpc(url,'get_info') for url in urls]
        if any(left['chain_id']!=v['chain_id'] for v in others):raise AssertionError('Independent validator chain differs')
        heights=[left['last_irreversible_block_num'],*(v['last_irreversible_block_num'] for v in others)]
        number=min(heights)
        if number < 17 or max(heights)-number>4:return False
        block = self.rpc('get_block', {'block_num_or_id': number})
        if any(self.read_rpc(url,'get_block',{'block_num_or_id':number})['id']!=block['id'] for url in urls):
            raise AssertionError('Independent irreversible block differs')
        return dict(common_lib=number, common_id=block['id'],ledgers=1+len(urls))

    def balance(self, name, url=None):
        rows = (self.read_rpc(url, 'get_table_rows', dict(json=True, code='eosio.token', scope=name, table='accounts', limit=10))['rows']
            if url else self.table('accounts', code='eosio.token', scope=name))
        return self.asset_units(rows[0]['balance']) if rows else 0

    def unlock_private_wallet(self):
        self.run(['wallet', 'lock_all'])
        password = (self.root/'wallet/password.txt').read_text().strip(); self.secrets.append(password)
        self.run(['wallet', 'unlock', '-n', 'localtest'], input_text=password+'\n')

    def claim(self, owner, historical=False):
        before = dict(balance=self.balance(owner), producer=self.producer(owner),
            supply=self.table('stat', code='eosio.token', scope='SYS')[0]['supply'])
        result = self.action('claimrewards', {'owner': owner}, owner)
        confirmed = next(r for r in reversed(self.report['confirmed_transactions']) if r['transaction_id'] == result['transaction_id'])
        transfers, issued = [], 0
        # Current node format is flat. Only the actual token receiver is an
        # execution; token notifications must not be counted twice.
        for trace in result['processed']['action_traces']:
            action = trace['act']
            if trace['receiver'] != 'eosio.token' or action['account'] != 'eosio.token': continue
            if action['name'] == 'transfer': transfers.append(action['data'])
            if action['name'] == 'issue': issued += self.asset_units(action['data']['quantity'])
        amount = lambda a, b: sum(self.asset_units(t['quantity']) for t in transfers if t['from'] == a and t['to'] == b)
        paid = amount('eosio.bpay', owner)
        self.check(paid > 0 and self.balance(owner) == before['balance']+paid, 'Real finalized token balance received block pay: '+owner)
        self.check(amount('eosio', 'eosio.bpay') == issued and not any(t['to'] in ('eosio.vpay', 'eosio.saving') for t in transfers),
            'New issuance funds only block pay; no saving or vote-pay transfer')
        self.check(self.asset_units(self.table('stat', code='eosio.token', scope='SYS')[0]['supply'])-
            self.asset_units(before['supply']) == issued, 'Actual token supply delta agrees with issue trace')
        self.check(self.balance('eosio.saving') == self.balance('eosio.vpay') == 0 and not self.table('producers2'),
            'Saving/vote-pay balances and legacy vote-pay share rows remain empty')
        if historical:
            self.check(int(self.producer(owner)['unpaid_blocks']) == 0 and self.candidates()[owner]['banned'],
                'Historical unpaid blocks paid once; permanent ban remains')
            self.action('claimrewards', {'owner': owner}, owner, failure='standby rewards are disabled')
        else:
            self.wait_observing('Healthy owner has new payable blocks after first payment',
                lambda _i, _m, _r: int(self.producer(owner)['unpaid_blocks']) > 0, 300)
            balance = self.balance(owner)
            self.action('claimrewards', {'owner': owner}, owner, failure='within past day')
            self.check(self.balance(owner) == balance, 'Ordinary owner cannot receive a second payment inside 24h')
        def finalized():
            value = self.validator_agreement()
            return value if value and value['common_lib'] >= confirmed['block_num'] else False
        agreement = self.wait_for('Independent validator finalizes the actual claim block', finalized, 240)
        self.check(self.balance(owner, self.validator_url) == self.balance(owner), 'Independent token ledger agrees after real claim')
        self.report['reward_claims'].append(dict(owner=owner, historical=historical, before=before,
            block_paid=paid, issued=issued, transaction_id=result['transaction_id'], agreement=agreement))
        self.checkpoint()

    def tests(self):
        if any(os.environ.get(name) for name in ('LD_PRELOAD', 'FAKETIME', 'FAKETIME_TIMESTAMP_FILE')):
            raise RuntimeError('Real-time claim test forbids process-clock overrides')
        self.configure_fixture_signers()
        self.start_validator()
        self.start_fault_signer()
        self.tx(['system', 'voteproducer', 'approve', 'alice', 'eosio'])
        self.action('deposit', {'owner': 'alice', 'amount': '100.0000 SYS'}, 'alice')
        self.action('buyrex', {'from': 'alice', 'amount': '100.0000 SYS'}, 'alice')
        self.observe()
        for index, name in enumerate(self.names[1:], 1):
            self.register(name); self.registered_monotonic[name] = time.monotonic()
            self.stake(name, 100+index, 0)
            self.tx(['system', 'voteproducer', 'approve', name, name]); self.observe()
        if self.fault_preflight_only:
            self.fault_preflight();return
        self.wait_observing('All 22 fixture identities actually produce, at most 21 active seats',
            lambda _i, members, _r: len(members) == 21 and self.observed == set(self.names), 10800)
        victim = self.wait_observing('Historically producing active singleton with enough remaining tenure for three zero-output windows',
            lambda _i, members, rotation: ready_offline_identity(self.names,members,rotation,self.observed),
            returning_identity_timeout(len(self.names)))
        self.report['fault_window_start']=dict(active=list(self.latest_rotation['active']),
            victim_position=self.latest_rotation['active'].index(victim),stable_policy=True,
            actual_historical_production=victim in self.observed)
        self.report['fault'] = dict(node=victim, scope='Only the singleton signer process stops; healthy signer processes retain their PIDs')
        self.report['restart_head'] = self.rpc('get_info')['head_block_num']
        self.stop_fault_signer()
        banned = self.wait_observing('Three complete zero-output windows become irreversible',
            lambda _i, _m, _r: self.candidates()[victim] if self.candidates()[victim]['banned'] else False, 1800)
        self.check(banned['zero_windows'] == 3 and len(set(banned['evidence_slots'])) == 3,
            'Three distinct complete windows recorded on the real chain')
        self.wait_observing('Banned account exits through finalized safe policy handoff',
            lambda i, m, r: victim not in m and len(m) == 21 and r['stage'] == 0 and
                int(banned['ban_final_block']) <= i['last_irreversible_block_num'], 900)
        old = self.producer(victim); ledger = int(old['unpaid_blocks'])
        self.check(ledger > 0, 'Banned account has actual historical unpaid block units')
        self.action('unregprod', {'producer': victim}, victim)
        self.action('regproducer', {'producer': victim, 'producer_key': self.accounts[victim]['Public key'], 'url': '', 'location': 0}, victim)
        self.check(self.producer(victim)['last_claim_time'] == old['last_claim_time'] and
            int(self.producer(victim)['unpaid_blocks']) == ledger and self.candidates()[victim]['banned'],
            'Re-registration preserves ban, original cooldown and historical reward ledger')
        self.start_fault_signer(restart=True)
        healthy = next(n for n in self.names[1:] if n != victim)
        for owner in (victim, healthy):
            self.action('claimrewards', {'owner': owner}, owner, failure='within past day')
        self.report['fault'].update(candidate=banned, preserved_unpaid_blocks=ledger)
        last_progress, high = time.monotonic(), 0
        deadline = time.monotonic()+27*3600
        while True:
            if time.monotonic() > deadline: raise TimeoutError('Bounded real 24h observation exhausted')
            if self.node.poll() is not None or self.wallet.poll() is not None: raise RuntimeError('Private live chain child exited')
            info, members, rotation = self.observe()
            if int(info['head_block_num']) > high: high = int(info['head_block_num']); last_progress = time.monotonic()
            if time.monotonic()-last_progress > 300: raise RuntimeError('Real-time claim chain stopped progressing')
            self.check_unchanged_ban(victim, ledger, members)
            agreement = self.validator_agreement()
            if not agreement: raise RuntimeError('Live independent validator lost irreversible agreement')
            self.report['latest_live_wait'] = dict(utc=datetime.now(timezone.utc).isoformat(),
                info=info, agreement=agreement, elapsed_seconds={n: time.monotonic()-self.registered_monotonic[n] for n in (victim, healthy)})
            self.checkpoint()
            if all(due(info['head_block_time'], self.producer(n)['last_claim_time'],
                self.registered_monotonic[n], time.monotonic()) for n in (victim, healthy)): break
            time.sleep(30)
        self.unlock_private_wallet()
        self.claim(victim, True); self.claim(healthy)
        self.check_resource_split('After genuine 24h ordinary and permanent-blacklist historical payouts')

    def check_unchanged_ban(self, victim, ledger, members):
        if victim in members or not self.candidates()[victim]['banned'] or int(self.producer(victim)['unpaid_blocks']) != ledger:
            raise AssertionError('Banned online identity re-entered or historical unpaid units changed')

    def execute(self):
        try: return super().execute()
        finally:
            try: self.stop(self.fault_node)
            except Exception as error:
                self.report.update(status='failed'); self.report.setdefault('shutdown_errors', []).append(str(error))
            self.report['fault_signer_stopped']=self.fault_node is None or self.fault_node.poll() is not None
            try: self.stop(self.validator)
            except Exception as error:
                self.report.update(status='failed'); self.report.setdefault('shutdown_errors', []).append(str(error))
            self.report['validator_stopped'] = self.validator is None or self.validator.poll() is not None
            (self.root/'report.json').write_text(json.dumps(self.report, indent=2))


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('programs', 'contracts', 'output'): parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--fault-preflight-only',action='store_true',help='Real four-identity fault test, NOT a 24h result')
    args = parser.parse_args(); args.output.mkdir(exist_ok=False)
    build = args.output/'build'; (build/'programs').mkdir(parents=True)
    alias = Path(tempfile.mkdtemp(prefix='pv1-day-'))/'b'; alias.symlink_to(build.resolve())
    for name in ('nodeos', 'cleos', 'keosd', 'helios-util'):
        target = args.programs.resolve()/(name+'.elf'); assert target.is_file()
        directory = build/'programs'/name; directory.mkdir(); (directory/name).symlink_to(target)
    state = args.output/'state'; state.mkdir()
    test = LiveClaimTest(alias, args.contracts.resolve(), state,fault_preflight=args.fault_preflight_only)
    def stop(_sig, _frame): raise KeyboardInterrupt('Stop only private real-time claim test')
    signal.signal(signal.SIGTERM, stop)
    test.execute()
    result = dict(status=test.report['status'], report=str((test.root/'report.json').resolve()), release_ready=False,
        real_time_24h=not args.fault_preflight_only,fault_preflight_only=args.fault_preflight_only,
        scope=__doc__,elapsed_claim_wait=test.report.get('latest_live_wait'))
    (args.output/'report.json').write_text(json.dumps(result, indent=2))
    return 0 if (fault_preflight_complete(test.report) if args.fault_preflight_only else complete(test.report)) else 1


if __name__ == '__main__': raise SystemExit(main())
