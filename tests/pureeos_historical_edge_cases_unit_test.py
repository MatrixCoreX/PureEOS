import copy
import unittest
from unittest.mock import Mock, patch
from pathlib import Path
from pureeos_historical_edge_cases_test import HistoricalEdgeTest, quantity, token_units, verify_case_set, adoption_totals


class HistoricalCaseControlTests(unittest.TestCase):
    def test_independent_wave_preconnects_and_closes_every_client(self):
        owner=HistoricalEdgeTest.__new__(HistoricalEdgeTest);owner.url='http://127.0.0.1:8123'
        clients=[]
        for index in range(4):
            client=Mock();response=client.getresponse.return_value
            response.status=202;response.read.return_value=('{"transaction_id":"'+str(index)+'"}').encode()
            clients.append(client)
        with patch('pureeos_historical_edge_cases_test.http.client.HTTPConnection',side_effect=clients):
            result=owner.submit_independent_wave([{'packed_trx':str(i)} for i in range(4)])
        self.assertEqual([r['transaction_id'] for r in result],list('0123'))
        for client in clients:
            client.connect.assert_called_once();client.close.assert_called_once()
            self.assertEqual(client.request.call_args.args[:2],('POST','/v1/chain/push_transaction'))

    def test_independent_wave_refuses_external_targets_and_partial_waves(self):
        owner=HistoricalEdgeTest.__new__(HistoricalEdgeTest)
        for url in ('https://127.0.0.1:8123','http://example.com:8123','http://127.0.0.1:8123/path',
                    'http://user@127.0.0.1:8123','http://127.0.0.1:8123?override=yes'):
            owner.url=url
            with patch('pureeos_historical_edge_cases_test.http.client.HTTPConnection') as client:
                with self.assertRaises(ValueError):owner.submit_independent_wave([{}]*4)
                client.assert_not_called()
        owner.url='http://127.0.0.1:8123'
        with self.assertRaises(ValueError):owner.submit_independent_wave([{}]*3)

    def test_wave_connection_failure_closes_already_created_clients(self):
        owner=HistoricalEdgeTest.__new__(HistoricalEdgeTest);owner.url='http://127.0.0.1:8123'
        first,second=Mock(),Mock();second.connect.side_effect=OSError('private connection failure')
        with patch('pureeos_historical_edge_cases_test.http.client.HTTPConnection',side_effect=[first,second]):
            with self.assertRaises(OSError):owner.submit_independent_wave([{}]*4)
        first.close.assert_called_once();second.close.assert_called_once()

    def test_enabled_threads_without_actual_adoption_are_not_parallel_evidence(self):
        text='Experimental parallel production totals: batches=2, attempts=8, succeeded=8, adopted=4, serial_fallbacks=0; local diagnostic'
        self.assertEqual(adoption_totals(text,'production')['adopted'],4)
        self.assertIsNone(adoption_totals('no diagnostic','production'))
        for bad in (text+text,text.replace('adopted=4','adopted=-1'),text.replace('attempts=8, ','')):
            with self.assertRaises(RuntimeError):adoption_totals(bad,'production')

    def test_parallel_flags_only_enable_production_for_actual_producer(self):
        owner=HistoricalEdgeTest.__new__(HistoricalEdgeTest);owner.nodeos=Path('/private/nodeos')
        owner.root=Path('/private/chain');owner.parallel_workers=4;owner.parallel_token_hash='0'*64
        for config,producer in (('/private/chain/config',True),('/private/chain/validator/config',False)):
            with patch('pureeos_historical_edge_cases_test.LocalTest.spawn',return_value='fake') as spawn:
                owner.spawn(['/private/nodeos','--config-dir',config],'node.log')
            command=spawn.call_args.args[0]
            self.assertEqual('--experimental-parallel-production' in command,producer)
            self.assertEqual(command[command.index('--experimental-parallel-validation-threads')+1],'4')
            self.assertIn('--wasm-runtime',command)
    def test_loopback_snapshot_peers_do_not_hit_production_one_ip_limit(self):
        owner=HistoricalEdgeTest.__new__(HistoricalEdgeTest);owner.nodeos=Path('/private/nodeos')
        with patch('pureeos_historical_edge_cases_test.LocalTest.spawn',return_value='fake') as spawn:
            self.assertEqual(owner.spawn(['/private/nodeos','--data-dir','/private/state'],'node.log'),'fake')
        command=spawn.call_args.args[0]
        self.assertEqual(command[command.index('--p2p-max-nodes-per-host')+1],'8')
        self.assertEqual(command[command.index('--max-clients')+1],'16')

    def test_transfer_encoding_uses_current_cli_not_removed_rpc(self):
        owner=HistoricalEdgeTest.__new__(HistoricalEdgeTest)
        owner.run=Mock(return_value='00aabb\n');owner.rpc=Mock(side_effect=AssertionError('Removed RPC must not be used'))
        action=owner.transfer_action('1.0000 SYS','private test')
        self.assertEqual(action['data'],'00aabb');owner.rpc.assert_not_called()
        self.assertEqual(owner.run.call_args.args[0][:4],['convert','pack_action_data','eosio.token','transfer'])

    def test_wallet_signing_accepts_actual_eosio_http_201(self):
        owner=HistoricalEdgeTest.__new__(HistoricalEdgeTest)
        owner.root=Path('/private/test');owner.accounts={'alice':{'Public key':'public-test-key'}}
        owner.report={'chain_id':'0'*64}
        response=Mock(status=201);response.read.return_value=b'{"signatures":["test-signature"]}'
        with patch('pureeos_historical_edge_cases_test.http.client.HTTPConnection') as client, \
             patch('pureeos_historical_edge_cases_test.socket.socket'):
            client.return_value.getresponse.return_value=response
            self.assertEqual(owner.signed({'actions':[]})['signatures'],['test-signature'])

    def test_amounts_use_minimum_integer_units_not_float(self):
        for amount in (0,1,10001,2**60+1):
            self.assertEqual(token_units(quantity(amount)),amount)

    def test_other_symbols_precision_and_negative_balances_reject(self):
        for text in ('1.0000 EOS','1.00001 SYS','-0.0001 SYS'):
            with self.assertRaises(ValueError):token_units(text)

    def test_summary_needs_every_case_and_clean_closure(self):
        names=['duplicate-once-only','atomic-failure-and-identical-retry',
            'expired-tapos-signature-rejections','invalid-asset-rollback','rex-selection-rollback',
            'bounded-malformed-rpc','expired-dedup-snapshot-agreement','truncated-snapshot-refused',
            'snapshot-restore-and-signed-tail','dirty-refusal-and-checked-replay','same-symbol-different-contract']
        valid=dict(status='passed',processes_stopped=True,edge_cases=[dict(name=n) for n in names])
        self.assertTrue(verify_case_set(valid))
        for key,value in (('status','running'),('processes_stopped',False),('shutdown_errors',['failure'])):
            bad=copy.deepcopy(valid);bad[key]=value;self.assertFalse(verify_case_set(bad))
        bad=copy.deepcopy(valid);bad['edge_cases'].pop();self.assertFalse(verify_case_set(bad))
        parallel=copy.deepcopy(valid);parallel['parallel_workers']=4
        self.assertFalse(verify_case_set(parallel))
        parallel['edge_cases'].append({'name':'parallel-independent-wave'})
        parallel['parallel_evidence']={'production':{'adopted':1},'validation':{'adopted':1}}
        self.assertTrue(verify_case_set(parallel))
        parallel['parallel_evidence']['validation']['adopted']=0;self.assertFalse(verify_case_set(parallel))


if __name__=='__main__':unittest.main()
