#!/usr/bin/env python3
"""Read-only G8 capacity evidence; never changes parameters or estimates TPS.

The public APIs are separate observations, not an atomic historical snapshot.
Even a stable head does not prove that pending-state configuration was stable.
Receipt CPU, wall time, account usage and implicit work are deliberately kept
separate. A collector failure is retained, never silently retried away.
"""
import argparse
import hashlib
import ipaddress
import json
from pathlib import Path
import re
import time
import urllib.request
from urllib.parse import urlsplit


METHODS = frozenset(('get_info', 'get_consensus_parameters', 'get_block',
                     'get_account', 'get_code_hash'))
MAX_RESPONSE_BYTES = 16 * 1024 * 1024
MAX_SELECTED_ACCOUNTS = 16


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unsigned(value, bits, label):
    require(type(value) is int and 0 <= value < 2**bits, f'Invalid {label}')
    return value


def digest(value, label):
    require(isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value), f'Invalid {label}')
    return value


def object_value(value, label):
    require(isinstance(value, dict), f'Invalid {label}')
    return value


def account_names(values):
    values = list(values)
    require(len(values) <= MAX_SELECTED_ACCOUNTS and len(values) == len(set(values)),
            'Too many or duplicate accounts')
    for name in values:
        require(isinstance(name, str) and re.fullmatch('[a-z1-5.]{1,12}', name)
                and not name.endswith('.'), 'Account selector must use a canonical name of at most 12 characters')
    return values


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file, code, message, headers, newurl):
        raise ValueError('Capacity collector refuses HTTP redirects')


class LocalReadOnlyRPC:
    """Explicit loopback, no credentials, proxies, redirects or write methods."""
    def __init__(self, endpoint, timeout=10):
        parsed = urlsplit(endpoint)
        require(parsed.scheme == 'http' and parsed.path in ('', '/') and not parsed.query
                and not parsed.fragment and parsed.username is None and parsed.password is None,
                'Expected a credential-free loopback HTTP origin')
        try:
            address = ipaddress.ip_address(parsed.hostname)
            port = parsed.port
        except (TypeError, ValueError) as error:
            raise ValueError('Expected a literal loopback IP address and explicit port') from error
        require(address.is_loopback and port is not None and 1 <= port <= 65535,
                'Expected a literal loopback IP address and explicit port')
        require(type(timeout) in (int, float) and 0 < timeout <= 30, 'Invalid RPC timeout')
        self.endpoint, self.timeout = endpoint.rstrip('/'), timeout
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())

    def __call__(self, method, payload=None):
        require(method in METHODS, 'Not an allowed read-only capacity RPC')
        request = urllib.request.Request(f'{self.endpoint}/v1/chain/{method}',
            data=json.dumps({} if payload is None else payload, allow_nan=False).encode(),
            headers={'Content-Type': 'application/json'}, method='POST')
        with self.opener.open(request, timeout=self.timeout) as response:
            require(response.status == 200, 'Unexpected capacity RPC status')
            body = response.read(MAX_RESPONSE_BYTES + 1)
        require(len(body) <= MAX_RESPONSE_BYTES, 'Capacity RPC response exceeds byte limit')
        return object_value(json.loads(body), 'RPC response')


def validate_info(info):
    object_value(info, 'chain info')
    digest(info.get('chain_id'), 'chain ID')
    head = unsigned(info.get('head_block_num'), 32, 'head height')
    lib = unsigned(info.get('last_irreversible_block_num'), 32, 'LIB height')
    digest(info.get('head_block_id'), 'head ID')
    digest(info.get('last_irreversible_block_id'), 'LIB ID')
    require(0 < lib <= head, 'A nonzero finalized anchor is required')
    return info


def validate_parameters(response):
    config = object_value(object_value(response, 'consensus parameters').get('chain_config'), 'chain config')
    block_cpu = unsigned(config.get('max_block_cpu_usage'), 32, 'block CPU cap')
    transaction_cpu = unsigned(config.get('max_transaction_cpu_usage'), 32, 'transaction CPU cap')
    minimum = unsigned(config.get('min_transaction_cpu_usage'), 32, 'minimum transaction CPU')
    require(0 < minimum <= transaction_cpu < block_cpu - minimum, 'Invalid CPU cap relationship')
    require(unsigned(config.get('max_block_net_usage'), 64, 'block NET cap') > 0, 'Zero block NET cap')
    return response


def summarize_receipts(block, minimum_cpu_us=None):
    """Receipts only. Supplied minimum must belong to this block, not today's RPC.

    Equality with the minimum is not proof that clamping happened. Statuses
    other than executed still consume receipt resources and are not successful
    business TPS. No inferred 'remaining capacity' subtracts receipts from cap.
    """
    object_value(block, 'block')
    digest(block.get('id'), 'block ID')
    unsigned(block.get('block_num'), 32, 'block height')
    receipts = block.get('transactions')
    require(isinstance(receipts, list), 'Missing block receipts')
    if minimum_cpu_us is not None:
        require(unsigned(minimum_cpu_us, 32, 'supplied minimum CPU') > 0, 'Zero supplied minimum')
    statuses = dict.fromkeys(('executed', 'soft_fail', 'hard_fail', 'delayed', 'expired'), 0)
    cpu = net = equal = below = 0
    seen = set()
    for receipt in receipts:
        object_value(receipt, 'receipt')
        status = receipt.get('status')
        require(isinstance(status, str) and status in statuses, 'Unknown receipt status')
        transaction = receipt.get('trx')
        txid = digest(transaction.get('id') if isinstance(transaction, dict) else transaction, 'receipt transaction ID')
        require(txid not in seen, 'Duplicate receipt transaction ID')
        seen.add(txid)
        used = unsigned(receipt.get('cpu_usage_us'), 32, 'receipt CPU')
        words = unsigned(receipt.get('net_usage_words'), 32, 'receipt NET words')
        cpu += used
        net += words * 8
        unsigned(cpu, 64, 'summed receipt CPU')
        unsigned(net, 64, 'summed receipt NET')
        statuses[status] += 1
        if minimum_cpu_us is not None:
            equal += used == minimum_cpu_us
            below += used < minimum_cpu_us
    return {'block_id': block['id'], 'block_num': block['block_num'], 'receipt_count': len(receipts),
            'statuses': statuses, 'receipt_cpu_us': cpu, 'receipt_net_bytes': net,
            'supplied_historical_minimum_cpu_us': minimum_cpu_us,
            'equal_to_supplied_minimum': equal if minimum_cpu_us is not None else None,
            'below_supplied_minimum': below if minimum_cpu_us is not None else None,
            'warning': 'Receipt totals omit work without receipts. Equality is not proof of minimum clamping. '
                       'No actual pending usage, admission reason, wall time or finalized business TPS is inferred.'}


def capture(rpc, expected_chain_id, accounts=(), contracts=(), clock=time.monotonic_ns):
    """One bounded attempt, including invalid observations and partial failures."""
    digest(expected_chain_id, 'expected chain ID')
    accounts, contracts = account_names(accounts), account_names(contracts)
    result = {'schema': 'pureeos-parallel-capacity-observation-v1', 'status': 'collecting',
              'expected_chain_id': expected_chain_id, 'observations': [],
              'warning': 'Separate RPC observations, not an atomic or historical configuration snapshot. '
                         'Pending state and cached get_info can differ even with a stable head. '
                         'Node instrumentation and a controlled baseline are still required.'}

    def observe(method, payload=None):
        entry = {'method': method, 'payload': {} if payload is None else payload, 'started_ns': clock()}
        result['observations'].append(entry)
        try:
            entry['response'] = rpc(method, payload)
            return entry['response']
        finally:
            entry['finished_ns'] = clock()

    try:
        first = validate_info(observe('get_info'))
        require(first['chain_id'] == expected_chain_id, 'Unexpected chain ID')
        before = validate_parameters(observe('get_consensus_parameters'))
        anchor = observe('get_block', {'block_num_or_id': first['last_irreversible_block_id']})
        require(anchor.get('id') == first['last_irreversible_block_id']
                and anchor.get('block_num') == first['last_irreversible_block_num'], 'Finalized anchor mismatch')
        # Current parameters are NOT used as the historical minimum for this LIB.
        result['finalized_anchor_receipts'] = summarize_receipts(anchor)
        for name in accounts:
            account = object_value(observe('get_account', {'account_name': name}), 'account response')
            require(account.get('account_name') == name and account.get('head_block_num') == first['head_block_num'],
                    'Account response name or head mismatch')
            for resource in ('cpu_limit', 'net_limit'):
                object_value(account.get(resource), resource)
        for name in contracts:
            code = object_value(observe('get_code_hash', {'account_name': name}), 'code response')
            require(code.get('account_name') == name, 'Contract response name mismatch')
            digest(code.get('code_hash'), 'contract code hash')
        after = validate_parameters(observe('get_consensus_parameters'))
        last = validate_info(observe('get_info'))
        require(all(first[key] == last[key] for key in ('chain_id', 'head_block_num', 'head_block_id',
                'last_irreversible_block_num', 'last_irreversible_block_id')), 'Chain/head/LIB changed during observation')
        require(before == after, 'Consensus parameters changed during observation')
        result['status'] = 'stable_observation_not_atomic'
    except Exception as error:
        result['status'] = 'invalid'
        # Do not copy arbitrary HTTP bodies, URLs, credentials or exception args.
        result['error_type'] = type(error).__name__
        result['failed_observation'] = len(result['observations'])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--endpoint', required=True, help='Explicit loopback HTTP origin')
    parser.add_argument('--chain-id', required=True)
    parser.add_argument('--account', action='append', default=[])
    parser.add_argument('--contract', action='append', default=[])
    parser.add_argument('--artifact', type=Path, action='append', default=[],
                        help='Local program/contract to hash; not proof the RPC node runs it')
    parser.add_argument('--output', type=Path, required=True, help='New evidence file, never overwritten')
    args = parser.parse_args()
    rpc = LocalReadOnlyRPC(args.endpoint)
    digest(args.chain_id, 'expected chain ID')
    account_names(args.account)
    account_names(args.contract)
    require(len(args.artifact) <= 16, 'Too many artifacts')
    artifacts = []
    for path in args.artifact:
        with path.open('rb') as source:
            artifacts.append({'path': str(path.resolve()), 'sha256': hashlib.file_digest(source, 'sha256').hexdigest()})
    # Reserve output BEFORE contacting a node; do not overwrite older failures.
    with args.output.open('x') as output:
        result = capture(rpc, args.chain_id, args.account, args.contract)
        result['local_artifacts_not_process_attestation'] = artifacts
        json.dump(result, output, indent=2, allow_nan=False)
        output.write('\n')
    return 0 if result['status'] == 'stable_observation_not_atomic' else 1


if __name__ == '__main__':
    raise SystemExit(main())
