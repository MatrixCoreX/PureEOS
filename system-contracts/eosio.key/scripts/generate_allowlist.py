#!/usr/bin/env python3
"""Compile the reviewed manifest into WASM; there is no mutable admin bypass."""
import hashlib, json, re, sys
from pathlib import Path

manifest = json.loads(Path(sys.argv[1]).read_text())
if set(manifest) != {'version', 'tokens'} or manifest['version'] != 1:
    raise SystemExit('Unsupported whitelist manifest')
tokens = manifest['tokens']
if not isinstance(tokens, list) or not 1 <= len(tokens) <= 32:
    raise SystemExit('Whitelist requires 1..32 exact token identities')
seen = set()
for token in tokens:
    if set(token) != {'contract', 'symbol', 'code_hash'}:
        raise SystemExit('Whitelist fields must be exact')
    if not re.fullmatch(r'[a-z1-5.]{1,12}', token['contract']) or token['contract'].endswith('.'):
        raise SystemExit('Canonical token account required')
    if not re.fullmatch(r'(?:[0-9]|1[0-8]),[A-Z]{1,7}', token['symbol']):
        raise SystemExit('Canonical full token symbol required')
    if not re.fullmatch(r'[0-9a-f]{64}', token['code_hash']) or token['code_hash'] == '0' * 64:
        raise SystemExit('Nonzero code hash required')
    identity = (token['contract'], token['symbol'])
    if identity in seen:
        raise SystemExit('Duplicate whitelist identity')
    seen.add(identity)
native = {'contract': 'eosio.token', 'symbol': '4,AIC',
          'code_hash': 'f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c'}
if native not in tokens:
    raise SystemExit('The qualified native fee-token rule must be retained')
canonical = json.dumps(manifest, sort_keys=True, separators=(',', ':')).encode()
digest = hashlib.sha256(canonical).hexdigest()
lines = ['// Generated from token_allowlist.json; change only through reviewed wallet code.',
         '#pragma once', 'namespace keywallet {',
         'struct approved_token { eosio::name contract; eosio::symbol sym; eosio::checksum256 code_hash; };',
         'inline const approved_token approved_tokens[] = {']
for token in tokens:
    precision, symbol = token['symbol'].split(',')
    packed = ','.join('0x' + token['code_hash'][i:i + 2] for i in range(0, 64, 2))
    lines.append(' {eosio::name("%s"), eosio::symbol("%s", %s), eosio::checksum256(std::array<uint8_t,32>{%s})},'
                 % (token['contract'], symbol, precision, packed))
lines += ['};', '}', '// Canonical manifest SHA256: ' + digest, '']
Path(sys.argv[2]).write_text('\n'.join(lines))
print('WHITELIST ' + digest + ' identities=' + str(len(tokens)), flush=True)
