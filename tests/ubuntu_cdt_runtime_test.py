#!/usr/bin/env python3
"""Fresh-chain CDT ABI/inline/index checks. TEST keys only; never a TPS test."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import threading
import time

from parallel_execution_test import Node, checked_manifest, require, sha256
from parallel_release_benchmark import pin_paused_head, wait_for, producer_health_errors, parallel_diagnostics
from parallel_loopback_wave import push_packed_wave


def fixture_key(account):
    """Matches base_tester::get_private_key(account, 'active'); NOT real key management."""
    payload = b'\x80' + hashlib.sha256((account + 'active').encode()).digest()
    encoded = payload + hashlib.sha256(hashlib.sha256(payload).digest()).digest()[:4]
    alphabet = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
    number, result = int.from_bytes(encoded, 'big'), ''
    while number:
        number, digit = divmod(number, 58); result = alphabet[digit] + result
    return '1' * (len(encoded) - len(encoded.lstrip(b'\0'))) + result


def require_inline_bool_trace(response):
    """Check the current flat trace format and its actual parent/child linkage."""
    traces = response['processed']['action_traces']
    require(len(traces) == 2, 'Expected exactly one root and one inline bool action')
    root, child = traces
    require(root['action_ordinal'] == 1 and root['creator_action_ordinal'] == 0 and
            child['action_ordinal'] == 2 and child['creator_action_ordinal'] == 1 and
            child['closest_unnotified_ancestor_action_ordinal'] == 1,
            'Missing real inline bool parent/child linkage')
    for trace, name in ((root, 'sendbool'), (child, 'boolfirst')):
        require(trace['receiver'] == 'test' and trace['act']['account'] == 'test' and
                trace['act']['name'] == name and trace.get('except') is None and
                trace['receipt']['receiver'] == 'test', 'Invalid executed inline bool trace')
    require(child['act']['data'] == {'enabled': True, 'actor': 'alice'} and
            child['act']['hex_data'] == '010000000000855c34', 'Inline bool arguments differ')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('nodeos', 'cleos', 'fixture-exporter', 'complex-wasm', 'cdt-wasm', 'work-dir'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--sdk-hardening', action='store_true', help='Require modified SDK allocator and parser checks')
    args = parser.parse_args()
    os.umask(0o077)
    root = args.work_dir.resolve(); root.mkdir()
    for name in ('nodeos', 'cleos', 'fixture_exporter', 'complex_wasm', 'cdt_wasm'):
        setattr(args, name, getattr(args, name).resolve(strict=True))
    require(args.cdt_wasm.with_suffix('.abi').is_file(), 'Missing CDT ABI')
    files = [args.nodeos, args.cleos, args.fixture_exporter, args.complex_wasm, args.cdt_wasm,
             args.cdt_wasm.with_suffix('.abi'), Path(__file__), Path(__file__).with_name('parallel_execution_test.py'),
             Path(__file__).with_name('parallel_loopback_wave.py')]
    frozen = {str(path): sha256(path) for path in files}
    report = dict(status='running', inputs=frozen, cases=[],
                  scope='Actual deployed CDT bool/inline/index correctness on fresh loopback chains; not TPS or economic-model evidence')
    nodes = []
    try:
        corpus = root / 'corpus'
        env = {key: value for key, value in os.environ.items() if not key.startswith('PUREEOS_PARALLEL_')}
        env.update(PUREEOS_PARALLEL_RELEASE_EXPORT_DIR=str(corpus), PUREEOS_PARALLEL_RELEASE_TRANSACTIONS='64',
            PUREEOS_PARALLEL_RELEASE_COMPUTE='1', PUREEOS_PARALLEL_RELEASE_WORK='0',
            PUREEOS_PARALLEL_RELEASE_INLINE_ACTIONS='100', PUREEOS_PARALLEL_RELEASE_CDT_WASM=str(args.complex_wasm))
        with (root / 'export.log').open('x') as output:
            exported = subprocess.run([str(args.fixture_exporter),
                '--run_test=parallel_release_fixtures/export_fresh_jit_throughput_fixture',
                '--report_level=detailed', '--color_output=no'], env=env, stdout=output, stderr=subprocess.STDOUT, timeout=300)
        require(exported.returncode == 0, 'Fresh fixture export failed')
        manifest = checked_manifest(corpus)
        manifest['code_hashes'] = [*manifest['code_hashes'], sha256(args.cdt_wasm)]
        for workers in (0, 4):
            case = root / ('workers-' + str(workers)); case.mkdir()
            row = dict(workers=workers, passed=False); report['cases'].append(row)
            validator = Node(args.nodeos, case / 'validator', corpus, manifest, workers,
                             peer='listen', engine='jit', batch_size=8)
            producer = Node(args.nodeos, case / 'producer', corpus, manifest, workers,
                            peer=validator.endpoint, engine='jit', batch_size=8)
            nodes.extend((producer, validator))
            lines = ['producer-name = eosio', 'enable-stale-production = true', 'pause-on-startup = true',
                     'max-transaction-time = 100', 'read-mode = head']
            if workers: lines.append('experimental-parallel-production = true')
            lines += ['signature-provider = ' + value for value in json.loads((corpus / 'TEST-ONLY-keys.json').read_text())]
            with (producer.root / 'config/config.ini').open('a') as output: output.write('\n'.join(lines) + '\n')
            for node in (validator, producer): node.start('live', ['--genesis-json', str(corpus / 'genesis.json')])
            for point in manifest['checkpoints']:
                validator.feed(point); producer.check(point)
            producer.rpc('resume', namespace='producer')
            base = [str(args.cleos), '--no-auto-keosd', '-u', f'http://127.0.0.1:{producer.port}']
            accepted = set()
            accepted_lock = threading.Lock()

            def cli(label, arguments, key_input=None, expected=True):
                # Never log private input or a command containing keys.
                result = subprocess.run([*base, *arguments], input=key_input, text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
                with (case / (label + '.log')).open('x') as output: output.write(result.stdout + result.stderr)
                require((result.returncode == 0) == expected, label + ': unexpected CLI status')
                return result.stdout, result.stderr

            def sign(label, unsigned, actor):
                output, _ = cli(label + '-sign', ['sign', '--chain-id', manifest['chain_id'],
                    '--private-key', '--', json.dumps(unsigned)], key_input=fixture_key(actor) + '\n')
                begin = output.find('{')
                require(begin >= 0, 'Missing signed transaction JSON')
                return json.loads(output[begin:])

            def push_signed(label, signed, expected=True, message=None):
                output, error = cli(label + '-push', ['push', 'transaction', json.dumps(signed),
                    '--skip-sign', '--json'], expected=expected)
                if not expected:
                    require(message in output + error, 'Rejected for an unrelated reason')
                    return None
                response = json.loads(output)
                require(response['processed']['receipt']['status'] == 'executed', 'Contract action failed')
                with accepted_lock:
                    accepted.add(response['transaction_id'])
                return response

            def action(label, name, data, expected=True, message=None, prepare_only=False):
                output, _ = cli(label + '-prepare', ['push', 'action', 'test', name, json.dumps(data),
                    '-p', 'alice@active', '--skip-sign', '--dont-broadcast', '--json'])
                signed = sign(label, json.loads(output), 'alice')
                return signed if prepare_only else push_signed(label, signed, expected, message)

            output, _ = cli('deploy-prepare', ['set', 'contract', 'test', str(args.cdt_wasm.parent),
                args.cdt_wasm.name, args.cdt_wasm.with_suffix('.abi').name,
                '-p', 'test@active', '--skip-sign', '--dont-broadcast', '--json'])
            push_signed('deploy', sign('deploy', json.loads(output), 'test'))
            action('bool-true', 'boolfirst', dict(enabled=True, actor='alice'))
            action('bool-false', 'boolfirst', dict(enabled=False, actor='alice'), False, 'expected enabled')
            response = action('inline-bool', 'sendbool', dict(actor='alice'))
            require_inline_bool_trace(response)
            if args.sdk_hardening:
                action('sdk-memory', 'checkmem', dict(actor='alice'))
                for name, message in (('badalloc', 'calloc size overflow'), ('badvar', 'varuint32 overflow'),
                                      ('badstream', 'datastream attempted to skip past the end'),
                                      ('badvector', 'vector length exceeds datastream')):
                    action('sdk-' + name, name, dict(actor='alice'), False, message)
            signed = [action('row-' + str(key), 'putrow', dict(actor='alice', key=key, value=100 + key),
                             prepare_only=True) for key in range(1, 9)]
            # Real simultaneous submissions, not a claim of throughput or that
            # every insert is conflict-free (they share table metadata).
            with ThreadPoolExecutor(max_workers=4) as pool:
                responses = list(pool.map(lambda item: push_signed('burst-' + str(item[0]), item[1]), enumerate(signed)))
            require(len(responses) == 8, 'Lost submitted row action')
            action('update-row', 'putrow', dict(actor='alice', key=3, value=7777))
            action('duplicate-secondary', 'putrow', dict(actor='alice', key=9, value=7777))
            action('missing-auth', 'putrow', dict(actor='bob', key=10, value=1), False, 'missing authority')
            # Eight predeclared waves, not a retry-until-adopted loop. Prepare
            # and pack first: CLI process startup must not drip-feed the burst.
            # Existing distinct rows avoid the shared new-table creation path.
            wave_rows={};waves=[]
            for wave in range(8):
                packed=[];updates={}
                for index,key in enumerate((1,2,4,5)):
                    label=f'fixed-wave-{wave}-{index}';value=1000+wave*4+index
                    transaction=action(label,'putrow',dict(actor='alice',key=key,value=value),prepare_only=True)
                    output,_=cli(label+'-pack',['convert','pack_transaction',json.dumps(transaction)])
                    packed.append(json.loads(output));updates[key]=value
                responses=push_packed_wave(f'http://127.0.0.1:{producer.port}',packed)
                identifiers=[]
                for response in responses:
                    require(response['processed']['receipt']['status']=='executed','Fixed wave transaction failed')
                    identifiers.append(response['transaction_id'])
                require(len(set(identifiers))==4 and not accepted.intersection(identifiers),'Duplicate wave business ID')
                accepted.update(identifiers);wave_rows.update(updates)
                waves.append(dict(wave=wave,transaction_ids=identifiers,updates=updates))
            row['fixed_correctness_waves']=waves
            table_query = dict(code='test', scope='alice', table='rows', json=True, limit=20)
            values = producer.rpc('get_table_rows', table_query)['rows']
            expected = {key: 7777 if key in (3, 9) else 100 + key for key in range(1, 10)}
            expected.update(wave_rows)
            require({int(value['id']): int(value['value']) for value in values} == expected, 'Primary ledger mismatch')
            if args.sdk_hardening:
                require(all(int(value['id']) != 9999 for value in values), 'Rejected SDK action left a partial write')
                row['sdk_rejection_partial_writes_rolled_back'] = True
            secondary = producer.rpc('get_table_rows', {**table_query, 'index_position': '2', 'key_type': 'i64',
                                                       'lower_bound': '7777'})['rows']
            require([int(value['id']) for value in secondary] == [3, 9], 'Secondary duplicate order mismatch')
            finalized, number = set(), manifest['checkpoints'][-1]['block_num'] + 1
            def finalized_all():
                nonlocal number
                lib = validator.rpc('get_info')['last_irreversible_block_num']
                while number <= lib:
                    block = validator.rpc('get_block', {'block_num_or_id': number})
                    finalized.update(value['trx'] if isinstance(value['trx'], str) else value['trx']['id']
                                     for value in block['transactions'] if value['status'] == 'executed')
                    number += 1
                return accepted <= finalized
            wait_for(finalized_all, 'all accepted CDT transactions finalized', timeout=90)
            producer.rpc('pause', namespace='producer')
            info, state = pin_paused_head(producer)
            wait_for(lambda: validator.rpc('get_info')['head_block_id'] == info['head_block_id'], 'CDT verifier catches head')
            require(state == validator.rpc('get_integrity_hash', namespace='producer'), 'CDT replicated state mismatch')
            producer.stop(); validator.stop()
            producer_log = producer.log.read_text()
            require(not producer_health_errors(producer_log), 'Producer lifecycle invariant failure')
            totals = re.findall(r'Experimental parallel production totals: batches=(\d+), attempts=(\d+), '
                                r'succeeded=(\d+), adopted=(\d+), serial_fallbacks=(\d+)', producer_log)
            row['production_attempts_including_aborted_blocks'] = [dict(zip(
                ('batches', 'attempts', 'succeeded', 'adopted', 'serial_fallbacks'), map(int, value))) for value in totals]
            row['production_diagnostics_including_aborted_blocks'] = parallel_diagnostics(producer_log)
            row['validator_attempts_including_aborted_blocks'] = validator.totals('live', require_adoption=bool(workers))
            if workers:
                require(len(totals) == 1 and int(totals[0][3]) > 0,
                        'No actual CDT production adoption; serial fallback alone is not parallel coverage')
            row.update(passed=True, finalized_transactions=len(accepted), primary_rows=expected,
                       secondary_duplicate_ids=[3, 9], replicated_state=state)
            print(json.dumps(dict(workers=workers, passed=True, finalized_transactions=len(accepted))), flush=True)
        require(all(sha256(Path(path)) == value for path, value in frozen.items()), 'CDT inputs changed')
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error_type=type(error).__name__, error=str(error))
    finally:
        for node in nodes: node.stop(check=False)
        with (root / 'report.json').open('x') as output: json.dump(report, output, indent=2)
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
