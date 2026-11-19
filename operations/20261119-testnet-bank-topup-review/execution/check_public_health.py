"""Unsigned public HTTPS reads after exact restoration. No app/header changes."""
from pathlib import Path
import concurrent.futures
import datetime
import json
import subprocess

D = Path(__file__).resolve().parent
assert not (D / 'PUBLIC_HEALTH_FINAL.json').exists()
SITE = 'https://aic-test.matrixai.one'
requests = {
    'reader': (SITE + '/api/v1/chain/get_info', {}),
    'writer': (SITE + '/rpc/v1/chain/get_info', {}),
    'history': (SITE + '/history/v2/health', None),
    'relay1': ('https://relay1.matrixai.one/health', None),
    'relay2': ('https://relay2.matrixai.one/health', None),
    'token_code': (SITE + '/api/v1/chain/get_code_hash', {'account_name': 'eosio.token'}),
    'supply': (SITE + '/api/v1/chain/get_currency_stats', {'code': 'eosio.token', 'symbol': 'AIC'}),
    'bank': (SITE + '/api/v1/chain/get_currency_balance', {'code': 'eosio.token', 'account': 'civicbankers', 'symbol': 'AIC'}),
}

def read(item):
    name, (url, data) = item
    args = ['curl', '-q', '-sS', '--max-time', '20', '-w', '\n%{http_code}']
    if data is not None:
        args += ['-X', 'POST', '-H', 'Content-Type: application/json', '--data', json.dumps(data)]
    p = subprocess.run([*args, url], capture_output=True, text=True, timeout=25)
    body, code = p.stdout.rsplit('\n', 1) if '\n' in p.stdout else (p.stdout, '0')
    try:
        value = json.loads(body)
    except ValueError:
        value = {'non_json_body_prefix': body[:240]}
    return name, {'HTTP': int(code), 'curl_exit': p.returncode, 'body': value, 'error': p.stderr[:240]}

with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
    results = dict(pool.map(read, requests.items()))
result = {'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'unsigned_public_reads': results}
(D / 'PUBLIC_HEALTH_FINAL.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
assert all(r['HTTP'] == 200 and r['curl_exit'] == 0 for r in results.values()), 'Public HTTP proof retained'
chain = '09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532'
assert all(results[n]['body']['chain_id'] == chain for n in ['reader', 'writer'])
assert results['token_code']['body']['code_hash'] == 'f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c'
assert results['supply']['body']['AIC'] == {'supply': '100150000.0000 AIC', 'max_supply': '2100000000.0000 AIC', 'issuer': 'eosio'}
health = results['history']['body']['health']
assert all(v['status'] == 'OK' for v in health)
elastic = next(v for v in health if v['service'] == 'Elasticsearch')
assert elastic['service_data']['missing_blocks'] == 0
print(json.dumps({'status': 'PASS_PUBLIC_HTTP_AND_RESTORED_TOKEN', 'HTTP': {n: v['HTTP'] for n, v in results.items()},
                  'supply': results['supply']['body']['AIC'], 'bank': results['bank']['body'], 'history_missing_blocks': 0}))
