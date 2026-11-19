"""Bounded policy for owned TEST BP replacement; no I/O or chain effects."""
import hashlib
import re

ALPHABET = '12345abcdefghijklmnopqrstuvwxyz'
FUND_UNITS = 140000000
RAM_UNITS = 10000
RESERVE_UNITS = 5000000
MAX_DAILY_UNITS = 2 * (FUND_UNITS + RAM_UNITS)
MAX_TOTAL_UNITS = 10 * (FUND_UNITS + RAM_UNITS)
TARGET_BPS = 25


def similar_name(old, ordinal):
    if not re.fullmatch('[a-z1-5]{1,12}', old) or not 0 <= ordinal < len(ALPHABET) ** 2:
        raise ValueError('Invalid owned BP name or exhausted name space')
    return old[:10].ljust(10, 'a') + ALPHABET[ordinal // len(ALPHABET)] + ALPHABET[ordinal % len(ALPHABET)]


def job_id(old, ban_id):
    if not re.fullmatch('[a-z1-5]{1,12}', old) or not re.fullmatch('[0-9a-f]{64}', ban_id):
        raise ValueError('Invalid canonical ban binding')
    return old + '-' + hashlib.sha256((old + ':' + ban_id).encode()).hexdigest()[:16]


def validate_job(job):
    if job['id'] != job_id(job['replaces'], job['ban_block_id']):
        raise ValueError('Job ID differs from canonical ban')
    if not re.fullmatch('[a-z1-5]{12}', job['name']) or not job['name'].startswith(job['replaces'][:10].ljust(10, 'a')):
        raise ValueError('Replacement must retain the original name prefix')
    if not 0 <= job['ordinal'] < 10 or job['cpu_units'] != 1000100 + job['ordinal']:
        raise ValueError('Unexpected resource or lifetime allocation')
    if job['reserved_units'] != FUND_UNITS + RAM_UNITS or job['ban_block_num'] <= 0:
        raise ValueError('Unexpected reserved budget or ban block')
    if job['stage'] not in ['RESERVED', 'PREPARED', 'ACCOUNT_CREATED', 'FUNDED', 'SYNCHRONIZED', 'REGISTERED', 'COMPLETE']:
        raise ValueError('Unknown replacement stage')


def admission(context, spend):
    """No keys/accounts created until all bounded funding and health checks pass."""
    if context['eligible'] >= TARGET_BPS:
        return 'TARGET_FILLED'
    if context['eligible'] < 21 or context['policy_count'] != 21 or context['threshold'] != 15:
        return 'WAIT_NATIVE_QUORUM'
    if context['recent'] < 19 or context['head_age'] > 5 or context['LIB_age'] > 20:
        return 'WAIT_FRESH_CHAIN'
    if not context['all_running']:
        return 'WAIT_EXISTING_NODES'
    if context['RAM'] < 1024 ** 3 or context['swap_free'] < 8 * 1024 ** 3 or context['disk'] < 25 * 1024 ** 3:
        return 'WAIT_HOST_HEADROOM'
    if spend['daily_jobs'] >= 2 or spend['daily_units'] + FUND_UNITS + RAM_UNITS > MAX_DAILY_UNITS:
        return 'WAIT_DAILY_BUDGET'
    if spend['total_units'] + FUND_UNITS + RAM_UNITS > MAX_TOTAL_UNITS:
        return 'WAIT_TOTAL_BUDGET'
    if spend['since_last'] < 3600:
        return 'WAIT_ONE_HOUR_COOLDOWN'
    if context['bank_units'] < FUND_UNITS + RAM_UNITS + RESERVE_UNITS:
        return 'WAIT_BANK_FUNDS'
    if context['cpu_pressure60'] > 80:
        return 'WAIT_CPU_HEADROOM'
    return 'QUALIFIED_ONE'


def validate_actions(stage, actions, name, account_key, production_key, bls, peer_key, cpu_units):
    """Exact action envelopes: only the known bank and this fresh identity may sign."""
    def auth(actor):
        return [{'actor': actor, 'permission': 'active'}]
    permission = {'threshold': 1, 'keys': [{'key': account_key, 'weight': 1}], 'accounts': [], 'waits': []}
    def a(account, method, actor, data):
        return {'account': account, 'name': method, 'authorization': auth(actor), 'data': data}
    if stage == 'create':
        expected = [a('eosio', 'newaccount', 'civicbankers', {'creator': 'civicbankers', 'name': name, 'owner': permission, 'active': permission}),
                    a('eosio', 'buyram', 'civicbankers', {'payer': 'civicbankers', 'receiver': name, 'quant': '1.0000 AIC'})]
    elif stage == 'fund':
        cpu = str(cpu_units // 10000) + '.' + str(cpu_units % 10000).zfill(4) + ' AIC'
        expected = [a('eosio.token', 'transfer', 'civicbankers', {'from': 'civicbankers', 'to': name, 'quantity': '14000.0000 AIC', 'memo': 'Automatic owned TEST BP ' + name}),
                    a('eosio', 'delegatebw', name, {'from': name, 'receiver': name, 'stake_net_quantity': '10.0000 AIC', 'stake_cpu_quantity': cpu, 'transfer': False}),
                    a('eosio', 'powerup', name, {'payer': name, 'receiver': name, 'days': 30, 'net_frac': 5000000000000, 'cpu_frac': 5000000000000, 'max_payment': '100.0000 AIC'}),
                    a('eosio', 'bondbp', name, {'producer': name})]
    elif stage == 'register':
        expected = [a('eosio', 'regproducer', name, {'producer': name, 'producer_key': production_key, 'url': 'https://aic-test.matrixai.one', 'location': 0}),
                    a('eosio', 'regfinkey', name, {'finalizer_name': name, 'finalizer_key': bls['Public key'], 'proof_of_possession': bls['Proof of Possession']}),
                    a('eosio', 'regpeerkey', name, {'proposer_finalizer_name': name, 'key': peer_key}),
                    a('eosio', 'voteproducer', name, {'voter': name, 'proxy': '', 'producers': [name]})]
        expected[0]['authorization'].insert(0, {'actor': 'civicbankers', 'permission': 'active'})
    else:
        raise ValueError('Unknown financial stage')
    if actions is not None and actions != expected:
        raise ValueError('Actions differ from the exact owned TEST replacement policy')
    return expected
