#!/usr/bin/env python3
"""Bounded EOSIO/Antelope historical-risk regressions on NEW private chains.

Never connects to a public chain. Three independent fresh genesis rounds are
the default. This is correctness/recovery testing, not a TPS benchmark, and
does not re-enable removed deferred-transaction behavior.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import copy
from datetime import datetime, timedelta
import hashlib
import http.client
import json
import os
import re
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import threading
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request

from native_savanna_genesis_test import free_port
from pureeos_single_choice_local_test import LocalTest

SOURCES = {
    'expired-dedup-snapshot': 'https://github.com/AntelopeIO/leap/issues/290',
    'snapshot-existing-forkdb': 'https://github.com/AntelopeIO/leap/pull/1304',
    'legacy-deferred-accounting': 'https://github.com/EOSIO/eos/issues/6103',
    'token-contract-identity': 'https://github.com/smlu/EOS-fake.token',
    'bounded-deserialization': 'https://github.com/AntelopeIO/leap/pull/2405',
}


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def token_units(value):
    number, symbol = value.split()
    if symbol != 'SYS': raise ValueError('Unexpected private test symbol')
    whole, fraction = number.split('.')
    if len(fraction) != 4 or number.startswith('-'): raise ValueError('Expected nonnegative precision-4 balance')
    return int(whole) * 10000 + int(fraction)


def quantity(units):
    if not isinstance(units, int) or units < 0: raise ValueError('Expected nonnegative integer units')
    return f'{units // 10000}.{units % 10000:04d} SYS'


def adoption_totals(log, mode):
    if mode not in ('production','validation'):raise ValueError('Unknown worker mode')
    marker='Experimental parallel '+mode+' totals:'
    rows=re.findall(re.escape(marker)+r' batches=(\d+), attempts=(\d+), succeeded=(\d+), adopted=(\d+), serial_fallbacks=(\d+)',log)
    if marker not in log:return None
    if len(rows)!=1:raise RuntimeError('Missing/malformed/duplicate actual parallel adoption totals')
    return dict(zip(('batches','attempts','succeeded','adopted','serial_fallbacks'),map(int,rows[0])))


def verify_case_set(report):
    required = {'duplicate-once-only', 'atomic-failure-and-identical-retry',
                'expired-tapos-signature-rejections', 'invalid-asset-rollback',
                'rex-selection-rollback', 'bounded-malformed-rpc',
                'expired-dedup-snapshot-agreement', 'truncated-snapshot-refused',
                'snapshot-restore-and-signed-tail', 'dirty-refusal-and-checked-replay',
                'same-symbol-different-contract'}
    if report.get('parallel_workers',0):
        if report['parallel_workers']!=4:return False
        required.add('parallel-independent-wave')
        evidence=report.get('parallel_evidence',{})
        if not all((evidence.get(mode) or {}).get('adopted',0)>0 for mode in ('production','validation')):return False
    return (report.get('status') == 'passed' and report.get('processes_stopped') is True
            and required <= {c['name'] for c in report.get('edge_cases', [])}
            and not report.get('shutdown_errors'))


class HistoricalEdgeTest(LocalTest):
    local_signers = ('eosio',)

    def __init__(self, build, contracts, round_index, parallel_workers=0):
        if parallel_workers not in (0,4):raise ValueError('Only explicit zero/four-worker correctness profiles')
        self.parallel_workers=parallel_workers
        self.parallel_token_hash=digest(contracts/'eosio.token/eosio.token.wasm') if parallel_workers else None
        self.parallel_accounts=['edge'+chr(97+i)+'1'*7 for i in range(8)] if parallel_workers else []
        super().__init__(build, contracts, resource_split=True)
        self.followers = []
        self.report.update(round=round_index, scope=__doc__, edge_cases=[], historical_sources=SOURCES,
            parallel_workers=parallel_workers,parallel_token_hash=self.parallel_token_hash,
            legacy_deferred='Not re-enabled or claimed tested: the historical replacement bug requires obsolete behavior',
            test_script_sha256=digest(__file__), production_clock_unchanged=True)

    def spawn(self, command, log_name):
        if command[0] == str(self.nodeos):
            command = list(command)
            for flag, value in (('--chain-threads','1'),('--http-threads','1'),
                                ('--net-threads','1'),('--vote-threads','1'),
                                ('--p2p-max-nodes-per-host','8'),('--max-clients','16'),
                                ('--http-max-response-time-ms','200'),('--abi-serializer-max-time-ms','200')):
                if flag not in command: command += [flag,value]
            if getattr(self,'parallel_workers',0):
                command += ['--wasm-runtime','eos-vm-jit','--experimental-parallel-engine','jit',
                    '--experimental-parallel-validation-threads','4','--experimental-parallel-validation-batch-size','8',
                    '--experimental-parallel-code-hash',self.parallel_token_hash]
                config=Path(command[command.index('--config-dir')+1])
                if config==self.root/'config':command += ['--experimental-parallel-production']
        return super().spawn(command,log_name)

    def action(self,name,data,actor,*,contract='eosio',failure=None):
        if name=='govinit' and contract=='eosio' and failure is None and self.parallel_workers:
            # Normal genesis provisioning BEFORE governance sealing. No later
            # recovery of eosio active authority is introduced by the fixture.
            public=self.accounts['alice']['Public key']
            for account in self.parallel_accounts:
                self.tx(['system','newaccount','eosio',account,public,public,'--buy-ram','100.0000 SYS',
                    '--stake-net','10.0000 SYS','--stake-cpu','100.0000 SYS'])
                self.tx(['transfer','eosio',account,'2.0000 SYS','private independent wave funding'])
        return super().action(name,data,actor,contract=contract,failure=failure)

    def record(self, name, **evidence):
        self.report['edge_cases'].append(dict(name=name, **evidence))
        (self.root/'progress.json').write_text(json.dumps(self.report,indent=2))
        print('EDGE PASS: '+name,flush=True)

    def balance(self, account):
        rows=self.table('accounts',code='eosio.token',scope=account)
        return token_units(rows[0]['balance']) if rows else 0

    def prepare(self):
        self.nonce += 1
        return json.loads(self.run(['transfer','alice','bob','0.0001 SYS',
            f'private-edge-{self.report["round"]}-{self.nonce}', '-s','-d','-j','-x','90']))

    def signed(self, transaction, public_key=None):
        # The Unix wallet signs without private keys in process arguments,
        # reports, cloud credentials or terminal output.
        tx=copy.deepcopy(transaction);tx.pop('signatures',None);tx.pop('context_free_data',None)
        client=http.client.HTTPConnection('localhost',timeout=5)
        connection=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
        connection.settimeout(5);connection.connect(str(self.root/'wallet/keosd.sock'));client.sock=connection
        try:
            client.request('POST','/v1/wallet/sign_transaction',json.dumps(
                [tx,[public_key or self.accounts['alice']['Public key']],self.report['chain_id']]),
                {'Content-Type':'application/json'})
            response=client.getresponse();body=response.read()
            if response.status not in (200,201):
                raise RuntimeError('Private test wallet failed to sign: '+self.redact(body.decode(errors='replace')[:1500]))
            return json.loads(body)
        finally:client.close()

    def packed(self, transaction):
        return json.loads(self.run(['convert','pack_transaction',json.dumps(transaction)]))

    def submit(self, packed, expected=None, url=None):
        target=(url or self.url)+'/v1/chain/push_transaction'
        if not target.startswith('http://127.0.0.1:'):raise ValueError('Only private loopback chain RPC permitted')
        req=urllib.request.Request(target,data=json.dumps(packed).encode(),headers={'Content-Type':'application/json'})
        try:
            with urllib.request.urlopen(req,timeout=10) as response: result=json.load(response)
        except urllib.error.HTTPError as error:
            with error:result=json.loads(error.read())
            detail=result.get('error',{})
            if not expected or not any(t.lower() in json.dumps(detail).lower() for t in expected):
                raise RuntimeError('Unexpected chain rejection: '+self.redact(json.dumps(detail)[:2500]))
            return dict(rejected=True,error_name=detail.get('name'),error_code=detail.get('code'))
        if expected:raise AssertionError('Invalid transaction was accepted')
        return result

    def confirmed(self, packed):
        start=self.rpc('get_info')['last_irreversible_block_num']
        sent=self.submit(packed)
        return self.wait_transaction(sent['transaction_id'],start)

    def transfer_action(self, amount, memo):
        data={'from':'alice','to':'bob','quantity':amount,'memo':memo}
        # Modern Spring no longer exposes the old abi_json_to_bin RPC. Use
        # cleos's current local ABI serializer, backed by the real contract ABI.
        binary=self.run(['convert','pack_action_data','eosio.token','transfer',json.dumps(data)]).strip()
        if binary.startswith('"'):binary=json.loads(binary)
        bytes.fromhex(binary)  # Never accidentally submit a JSON object as bytes.
        return dict(account='eosio.token',name='transfer',authorization=[{'actor':'alice','permission':'active'}],data=binary)

    def new_follower(self, label, network=True):
        root=self.root/label;(root/'config').mkdir(parents=True)
        http,p2p=free_port(),free_port()
        while p2p==http:p2p=free_port()
        (root/'config/config.ini').write_text('\n'.join([
            'plugin = eosio::chain_api_plugin','plugin = eosio::producer_plugin',
            'plugin = eosio::producer_api_plugin','plugin = eosio::net_api_plugin','chain-state-db-size-mb = 128',
            'chain-state-db-guard-size-mb = 4','resource-monitor-space-absolute-gb = 8',
            f'http-server-address = 127.0.0.1:{http}',f'p2p-listen-endpoint = 127.0.0.1:{p2p}',
            *([f'p2p-peer-address = 127.0.0.1:{self.p2p_port}'] if network else []),
            'verbose-http-errors = true','']))
        peer=dict(root=root,url=f'http://127.0.0.1:{http}',process=None)
        self.followers.append(peer);return peer

    def follower_start(self, peer, label, extra=()):
        peer['process']=self.spawn([str(self.nodeos),'--data-dir',str(peer['root']/'data'),
            '--config-dir',str(peer['root']/'config'),*extra],label+'.log')

    def follower_wait(self, peer, block_id=None, timeout=90):
        until=time.monotonic()+timeout
        last=None
        while time.monotonic()<until:
            if peer['process'].poll() is not None:raise RuntimeError('Follower exited: '+peer['root'].name)
            try:
                info=self.read_rpc(peer['url'],'get_info')
                last={k:info.get(k) for k in ('head_block_num','head_block_id','last_irreversible_block_num','chain_id')}
                if block_id is None or info['head_block_id']==block_id:return info
            except (OSError,urllib.error.URLError):pass
            time.sleep(.2)
        raise RuntimeError('Follower did not reach required canonical head: '+json.dumps(
            dict(peer=peer['root'].name,wanted=block_id,last=last)))

    def assert_no_change(self, before):
        if [self.balance('alice'),self.balance('bob')]!=before:raise AssertionError('Rejected transaction changed balances')

    def parallel_wave(self):
        evidence=[]
        # A predeclared bounded workload, not a retry-until-adopted loop. The
        # first waves also warm compiled modules. None is a TPS sample.
        for wave in range(8):
            before={n:self.balance(n) for n in self.parallel_accounts};packed=[]
            for index in range(4):
                sender,receiver=self.parallel_accounts[index*2:index*2+2]
                data={'from':sender,'to':receiver,'quantity':'0.0001 SYS','memo':f'independent-wave-{wave}-{index}'}
                binary=self.run(['convert','pack_action_data','eosio.token','transfer',json.dumps(data)]).strip()
                tx=self.prepare();tx['actions']=[dict(account='eosio.token',name='transfer',
                    authorization=[dict(actor=sender,permission='active')],data=binary)]
                packed.append(self.packed(self.signed(tx)))
            first=self.rpc('get_info')['last_irreversible_block_num']
            results=self.submit_independent_wave(packed)
            ids=[r['transaction_id'] for r in results]
            if len(set(ids))!=4:raise AssertionError('Independent wave did not submit four distinct transactions')
            confirmations=[self.wait_transaction(identifier,first) for identifier in ids]
            after={n:self.balance(n) for n in self.parallel_accounts}
            expected={n:value+(-1 if self.parallel_accounts.index(n)%2==0 else 1) for n,value in before.items()}
            if after!=expected:raise AssertionError('Independent parallel transfer balances differ')
            evidence.append(dict(wave=wave,confirmations=confirmations,before=before,after=after))
        self.record('parallel-independent-wave',waves=evidence,workers=4,
            note='Thirty-two unique finalized business transactions in eight fixed waves; shutdown adoption counters are separate local diagnostics, not unique TPS')

    def submit_independent_wave(self, packed):
        """Preconnect four loopback clients so Python setup does not drip-feed a wave."""
        target=urllib.parse.urlsplit(self.url)
        if (len(packed)!=4 or target.scheme!='http' or target.hostname!='127.0.0.1'
                or target.port is None or target.username is not None or target.password is not None
                or target.path not in ('','/') or target.query or target.fragment):
            raise ValueError('Exactly four transactions to a private loopback chain are required')
        bodies=[json.dumps(tx).encode() for tx in packed]
        clients=[];ready=threading.Barrier(4,timeout=5)
        try:
            for _ in bodies:
                client=http.client.HTTPConnection(target.hostname,target.port,timeout=10)
                clients.append(client);client.connect()
            def send(item):
                client,body=item
                ready.wait()
                client.request('POST','/v1/chain/push_transaction',body,{'Content-Type':'application/json'})
                response=client.getresponse();payload=response.read()
                if response.status not in (200,202):
                    raise RuntimeError('Independent wave rejected: '+self.redact(payload.decode(errors='replace')[:2500]))
                return json.loads(payload)
            with ThreadPoolExecutor(max_workers=4) as workers:
                return list(workers.map(send,zip(clients,bodies)))
        finally:
            for client in clients:client.close()

    def transactions(self, follower):
        before=[self.balance('alice'),self.balance('bob')]
        packed=self.packed(self.signed(self.prepare()));confirmed=self.confirmed(packed)
        rejected=self.submit(packed,expected=['duplicate'])
        self.follower_wait(follower)
        # Wait for the canonical transaction before testing its duplicate at
        # another independently executing validator.
        target=self.rpc('get_info')['last_irreversible_block_num']
        deadline=time.monotonic()+40
        while self.read_rpc(follower['url'],'get_info')['last_irreversible_block_num']<target:
            if time.monotonic()>deadline:raise RuntimeError('Follower finality did not catch up')
            time.sleep(.2)
        other=self.submit(packed,expected=['duplicate'],url=follower['url'])
        if [self.balance('alice'),self.balance('bob')]!=[before[0]-1,before[1]+1]:raise AssertionError('Duplicate paid twice')
        self.record('duplicate-once-only',finalized=confirmed,producer_rejection=rejected,validator_rejection=other)

        before=[self.balance('alice'),self.balance('bob')]
        tx=self.prepare();tx['actions']=[self.transfer_action('1.0000 SYS','atomic first'),
            self.transfer_action(quantity(before[0]+10000),'atomic failing second')]
        packed=self.packed(self.signed(tx));rejected=self.submit(packed,expected=['overdrawn balance'])
        self.assert_no_change(before)
        # Governance sealing intentionally removes the old eosio active-key
        # spending path. A normal funded user, not the sealed system account,
        # supplies the retry's additional tokens.
        self.tx(['transfer','bob','alice','10.0000 SYS','private retry funding'])
        confirmation=self.confirmed(packed)
        after=[self.balance('alice'),self.balance('bob')];amount=before[0]+20000
        if after != [before[0]+100000-amount,before[1]-100000+amount]:raise AssertionError('Failed/retried transaction left partial effects')
        self.record('atomic-failure-and-identical-retry',rejection=rejected,finalized_retry=confirmation)

        before=[self.balance('alice'),self.balance('bob')];rejections=[]
        expired=self.prepare();expired['expiration']=(datetime.fromisoformat(self.rpc('get_info')['head_block_time'])-
            timedelta(seconds=10)).strftime('%Y-%m-%dT%H:%M:%S')
        rejections.append(self.submit(self.packed(self.signed(expired)),expected=['expired']))
        tapos=self.prepare();tapos['ref_block_prefix']^=0xffffffff
        rejections.append(self.submit(self.packed(self.signed(tapos)),expected=['reference block','tapos']))
        rejections.append(self.submit(self.packed(self.signed(self.prepare(),self.accounts['bob']['Public key'])),
            expected=['authority','authorization','signature']))
        self.assert_no_change(before);self.record('expired-tapos-signature-rejections',rejections=rejections)

        for amount in ('-0.0001 SYS','0.0000 SYS','0.00001 SYS'):
            tx=self.prepare();tx['actions']=[self.transfer_action(amount,'invalid amount')]
            self.submit(self.packed(self.signed(tx)),expected=['positive quantity','precision mismatch','symbol precision'])
            self.assert_no_change(before)
        self.record('invalid-asset-rollback',tested_quantities=['negative','zero','wrong precision'])

        self.action('deposit',{'owner':'alice','amount':'1.0000 SYS'},'alice')
        fund=self.table('rexfund');voters=self.table('votersv2');balances=[self.balance('alice'),self.balance('bob')]
        self.action('buyrex',{'from':'alice','amount':'1.0000 SYS'},'alice',failure='select one producer before buying REX')
        if fund!=self.table('rexfund') or voters!=self.table('votersv2'):raise AssertionError('Failed REX buy changed funds/votes')
        self.assert_no_change(balances);self.record('rex-selection-rollback')

        info=self.rpc('get_info');responses=[]
        for body in (b'{',b'[]',b'{"signatures":[],"compression":0,"packed_context_free_data":"","packed_trx":"ff"}',
                     b'{"signatures":[],"compression":"none","packed_context_free_data":"","packed_trx":"'+b'ff'*128+b'"}'):
            req=urllib.request.Request(self.url+'/v1/chain/push_transaction',data=body,headers={'Content-Type':'application/json'})
            try:
                with urllib.request.urlopen(req,timeout=5) as response:response.read()
                raise AssertionError('Malformed transaction accepted')
            except urllib.error.HTTPError as error:
                with error:detail=error.read()
                if not detail:raise AssertionError('Malformed input returned no structured error')
                responses.append(error.code)
        self.wait_lib(info['head_block_num']+12)
        self.record('bounded-malformed-rpc',http_codes=responses,limit='Four loopback requests, less than 1 KiB each; not a denial-of-service benchmark')

        # Same symbol/precision is NOT the same token: contract identity must
        # also match. Reuse the real token WASM on a new private test account.
        public=self.accounts['alice']['Public key']
        # Twelve-character ordinary account: no privileged eosio creator,
        # premium-name auction or unsealing of system permissions is required.
        fake_account='faketoken111'
        self.tx(['system','newaccount','bob',fake_account,public,public,'--buy-ram','100.0000 SYS',
                 '--stake-net','10.0000 SYS','--stake-cpu','100.0000 SYS'])
        self.tx(['set','contract',fake_account,str(self.contracts/'eosio.token')])
        self.action('create',{'issuer':fake_account,'maximum_supply':'10000.0000 SYS'},fake_account,contract=fake_account)
        self.action('issue',{'to':fake_account,'quantity':'1000.0000 SYS','memo':'private lookalike token'},fake_account,contract=fake_account)
        self.action('transfer',{'from':fake_account,'to':'alice','quantity':'1000.0000 SYS','memo':'private lookalike funding'},fake_account,contract=fake_account)
        before=dict(fund=self.table('rexfund'),pool=self.table('rexpool'),votes=self.table('votersv2'),
                    alice=self.balance('alice'),rex=self.balance('eosio.rex'))
        self.action('transfer',{'from':'alice','to':'eosio.rex','quantity':'10.0000 SYS','memo':'must not credit real REX'},'alice',contract=fake_account)
        after=dict(fund=self.table('rexfund'),pool=self.table('rexpool'),votes=self.table('votersv2'),
                   alice=self.balance('alice'),rex=self.balance('eosio.rex'))
        if after!=before:raise AssertionError('Lookalike token changed real REX/vote accounting')
        fake=self.table('accounts',code=fake_account,scope='eosio.rex')
        if not fake or token_units(fake[0]['balance'])!=100000:raise AssertionError('Lookalike transfer did not actually execute')
        self.record('same-symbol-different-contract',real_contract='eosio.token',lookalike_contract=fake_account,
                    symbol='4,SYS',lookalike_received='10.0000 SYS',real_rex_funds_unchanged=True)

    def snapshots_and_recovery(self, follower):
        # Let short-lived successful transactions expire before comparing
        # portable state from independently executed nodes (Leap issue #290).
        tx=self.prepare();tx['expiration']=(datetime.fromisoformat(self.rpc('get_info')['head_block_time'])+
            timedelta(seconds=10)).strftime('%Y-%m-%dT%H:%M:%S')
        self.confirmed(self.packed(self.signed(tx)))
        expiry=datetime.fromisoformat(tx['expiration'])
        until=time.monotonic()+30
        while datetime.fromisoformat(self.rpc('get_info')['head_block_time'])<=expiry+timedelta(seconds=2):
            if time.monotonic()>until:raise RuntimeError('Native clock did not move past transaction expiration')
            time.sleep(.3)
        self.rpc('pause',namespace='producer');time.sleep(.7)
        info=self.rpc('get_info');self.follower_wait(follower,info['head_block_id'])
        # create_snapshot in speculative mode waits for a FUTURE LIB; pausing
        # that producer would deadlock the test. Use two separately replayed,
        # irreversible-mode copies of the cleanly stopped validator instead.
        self.stop(follower['process'])
        pinned=[]
        for label in ('snapshot-source-a','snapshot-source-b'):
            source=self.new_follower(label,network=False)
            (source['root']/'data').mkdir()
            shutil.copytree(follower['root']/'data/blocks',source['root']/'data/blocks')
            self.follower_start(source,label,['--read-mode','irreversible','--force-all-checks','--disable-replay-opts'])
            self.follower_wait(source);pinned.append(source)
        baseline=self.read_rpc(pinned[0]['url'],'get_integrity_hash',namespace='producer')
        other=self.read_rpc(pinned[1]['url'],'get_integrity_hash',namespace='producer')
        if baseline!=other:raise AssertionError('Independent same-head state integrity hashes differ')
        a=self.read_rpc(pinned[0]['url'],'create_snapshot',namespace='producer')
        b=self.read_rpc(pinned[1]['url'],'create_snapshot',namespace='producer')
        left,right=Path(a['snapshot_name']),Path(b['snapshot_name'])
        if not left.is_relative_to(self.root) or not right.is_relative_to(self.root):raise RuntimeError('Snapshot escaped private chain directory')
        if digest(left)!=digest(right):raise AssertionError('Same-head portable snapshots differ')
        self.record('expired-dedup-snapshot-agreement',head_id=a['head_block_id'],integrity=baseline,
                    snapshot_sha256=digest(left),historical_source=SOURCES['expired-dedup-snapshot'])
        for source in pinned:self.stop(source['process'])
        self.follower_start(follower,'validator-clean-restart')
        self.follower_wait(follower,info['head_block_id'])

        bad=self.new_follower('truncated-snapshot');truncated=bad['root']/'truncated.bin'
        with left.open('rb') as source,truncated.open('xb') as output:
            output.write(source.read(left.stat().st_size-32))
        command=[str(self.nodeos),'--data-dir',str(bad['root']/'data'),'--config-dir',str(bad['root']/'config'),
                 '--snapshot',str(truncated)]
        result=subprocess.run(command,capture_output=True,text=True,timeout=30)
        (bad['root']/'refusal.log').write_text(self.redact(result.stdout+result.stderr))
        if result.returncode<=0:raise AssertionError('Truncated snapshot was accepted or crashed by signal')
        if digest(left)!=self.report['edge_cases'][-1]['snapshot_sha256']:raise AssertionError('Original snapshot changed')
        self.record('truncated-snapshot-refused',exit_code=result.returncode)

        restored=self.new_follower('snapshot-restored',network=False)
        self.follower_start(restored,'snapshot-restored',['--snapshot',str(left)])
        # The paused canonical producer may already send its signed reversible
        # tail. Compare the portable checkpoint with networking disabled first.
        self.follower_wait(restored,a['head_block_id'])
        if self.read_rpc(restored['url'],'get_integrity_hash',namespace='producer')!=baseline:
            raise AssertionError('Restored snapshot state differs')
        self.read_rpc(restored['url'],'connect',f'127.0.0.1:{self.p2p_port}',namespace='net')
        self.rpc('resume',namespace='producer');self.wait_lib(info['head_block_num']+16)
        self.rpc('pause',namespace='producer');time.sleep(.7);end=self.rpc('get_info')
        self.follower_wait(restored,end['head_block_id']);self.follower_wait(follower,end['head_block_id'])
        after=self.rpc('get_integrity_hash',namespace='producer')
        if self.read_rpc(restored['url'],'get_integrity_hash',namespace='producer')!=after:
            raise AssertionError('Signed P2P tail changed snapshot-restored state')
        self.record('snapshot-restore-and-signed-tail',restored_head=a['head_block_id'],tail_head=end['head_block_id'],
                    tail_blocks=end['head_block_num']-a['head_block_num'],transport='Original signed native-Savanna blocks over loopback P2P',
                    note='Related recovery scenario, not an exact reproduction of historical PR #1304')

        follower['process'].kill();follower['process'].wait(timeout=10)
        command=[str(self.nodeos),'--data-dir',str(follower['root']/'data'),'--config-dir',str(follower['root']/'config')]
        result=subprocess.run(command,capture_output=True,text=True,timeout=30)
        (follower['root']/'dirty-refusal.log').write_text(self.redact(result.stdout+result.stderr))
        if result.returncode!=2 or 'dirty flag' not in (result.stdout+result.stderr):raise AssertionError('Dirty state was not explicitly refused')
        self.follower_start(follower,'dirty-checked-replay',['--replay-blockchain','--force-all-checks','--disable-replay-opts'])
        self.follower_wait(follower,end['head_block_id'])
        if self.read_rpc(follower['url'],'get_integrity_hash',namespace='producer')!=after:
            raise AssertionError('Full-check dirty replay changed canonical state')
        self.record('dirty-refusal-and-checked-replay',refusal_exit_code=result.returncode,canonical_integrity=after)
        self.rpc('resume',namespace='producer');self.wait_lib(end['head_block_num']+12)

    def execute(self):
        try:
            self.setup(register_candidates=False)
            follower=self.new_follower('independent-validator')
            self.follower_start(follower,'independent-validator',['--genesis-json',str(self.root/'genesis.json')])
            self.follower_wait(follower)
            if self.parallel_workers:self.parallel_wave()
            self.transactions(follower);self.snapshots_and_recovery(follower)
            self.check_resource_split('Historical edge cases final resource invariant')
            self.report['status']='passed'
        except (Exception,KeyboardInterrupt) as error:
            self.report.update(status='failed',error=self.redact(str(error)),traceback=self.redact(traceback.format_exc()))
            print('EDGE FAIL: '+self.report['error'],flush=True)
        finally:
            processes=[p['process'] for p in self.followers]+[self.node,self.wallet]
            for process in processes:
                try:self.stop(process)
                except Exception as error:self.report.setdefault('shutdown_errors',[]).append(self.redact(str(error)))
            self.report['processes_stopped']=all(p is None or p.poll() is not None for p in processes)
            if self.report.get('shutdown_errors'):self.report['status']='failed'
            for log in self.logs:log.close()
            if self.parallel_workers and self.report['status']=='passed':
                try:
                    production=adoption_totals((self.root/'genesis.log').read_text(errors='replace'),'production')
                    validation=adoption_totals((self.root/'independent-validator.log').read_text(errors='replace'),'validation')
                    self.report['parallel_evidence']=dict(production=production,validation=validation)
                    if not all((s or {}).get('adopted',0)>0 for s in (production,validation)):
                        raise RuntimeError('Four workers were configured, but actual production/validation adoption was not both proved')
                except Exception as error:self.report.update(status='failed',error=str(error))
            (self.root/'report.json').write_text(json.dumps(self.report,indent=2))
        return self.report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path,required=True)
    parser.add_argument('--contracts-dir',type=Path,required=True)
    parser.add_argument('--report',type=Path,required=True)
    parser.add_argument('--rounds',type=int,default=3,choices=(1,2,3))
    parser.add_argument('--fail-fast',action='store_true',help='Retain a failed round and stop; never count a partial run as three passing rounds')
    parser.add_argument('--parallel-workers',type=int,choices=(0,4),default=0,help='Separate four-worker correctness run; no TPS comparison')
    args=parser.parse_args();os.umask(0o077)
    def stop(_sig,_frame):
        signal.signal(signal.SIGTERM,signal.SIG_IGN);raise KeyboardInterrupt('Private historical regression interrupted')
    signal.signal(signal.SIGTERM,stop)
    report=dict(status='running',scope=__doc__,rounds=[],source_sha256=digest(__file__),parallel_workers=args.parallel_workers)
    for index in range(1,args.rounds+1):
        result=HistoricalEdgeTest(args.build_dir.resolve(),args.contracts_dir.resolve(),index,args.parallel_workers).execute()
        report['rounds'].append(result)
        args.report.write_text(json.dumps(report,indent=2))
        if args.fail_fast and not verify_case_set(result):break
    report['status']='passed' if len(report['rounds'])==args.rounds and all(verify_case_set(r) for r in report['rounds']) else 'failed'
    args.report.write_text(json.dumps(report,indent=2))
    print(json.dumps({'status':report['status'],'rounds':len(report['rounds']),'report':str(args.report)}),flush=True)
    return 0 if report['status']=='passed' else 1


if __name__=='__main__':raise SystemExit(main())
