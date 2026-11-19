"""Publishable TEST review sources/evidence only; no chain operations."""
from pathlib import Path
import datetime
import hashlib
import json
import re
import shutil
import subprocess

D = Path(__file__).resolve().parent
R = D.parents[1]
OPS = R / 'PureEOS-Testnet-Ops-20261111'
BRANCH = 'ops/testnet-bank-topup-review-20261119'
TARGET = OPS / 'operations/20261119-testnet-bank-topup-review'

def git(*args):
    return subprocess.check_output(['git', *args], cwd=OPS, text=True).strip()

assert not git('status', '--porcelain'), 'Keep unrelated work untouched'
assert not TARGET.exists(), 'No repeated publication preparation'
assert not git('branch', '--list', BRANCH), 'Existing review branch requires reconciliation'
assert git('rev-parse', 'HEAD') == '428403cb565e25c7d5ea42df8fec8e54039d9063'
qualification = json.loads((D / 'ISOLATED_QUALIFICATION_R8_RESULT.json').read_text())
assert qualification['status'] == 'PASS_ISOLATED_WASM_ONLY' and qualification['checks'] == 20
assert all(r['private_children_stopped'] for r in qualification['reports'])
assert qualification['public_chain_mutations'] == 0

subprocess.run(['git', 'switch', '-c', BRANCH], cwd=OPS, check=True)
TARGET.mkdir(parents=True)
names = [
    'ONE_SHOT_REVIEW.zh-CN.md', 'ONE_SHOT_TOPUP_PLAN.json', 'TEST_ONLY_TOPUP.patch',
    'REVIEW_ARTIFACT_RESULT.json', 'PINNED_CANDIDATE_BUILD_RESULT.json',
    'ISOLATED_QUALIFICATION_R8_RESULT.json', 'FIXTURE_FAILURE_INDEX.json',
    'UNSIGNED_GOVERNANCE_INTENTS.json', 'ISSUANCE_AUTHORITY_PREFLIGHT.json',
    'LIVE_STATUS_BEFORE_REVIEW.json', 'CURRENT_SUPPLY.json',
    'prepare_one_shot_candidate.py', 'qualify_one_shot_r8.py', 'prepare_public_review.py',
]
for name in names:
    shutil.copy2(D / name, TARGET / name)
shutil.copytree(D / 'candidate', TARGET / 'candidate')
shutil.copytree(D / 'reviewed-artifact', TARGET / 'reviewed-artifact')

# The fixed human review document must remain byte exact after the commitment
# was computed. All copied evidence is explicitly public and contains no packet.
review = json.loads((TARGET / 'UNSIGNED_GOVERNANCE_INTENTS.json').read_text())
assert hashlib.sha256((TARGET / 'ONE_SHOT_REVIEW.zh-CN.md').read_bytes()).hexdigest() == review['review_document_sha256']
key_pattern = re.compile(rb'PVT_(?:K1|R1|BLS)_[A-Za-z0-9_-]+|\b5[HJK][1-9A-HJ-NP-Za-km-z]{49}\b')
for file in TARGET.rglob('*'):
    if file.is_file():
        assert not key_pattern.search(file.read_bytes()), 'Private key material in public whitelist'
        assert not any(n in file.parts for n in ['private-fixtures', 'wallet', 'data', 'immutable-packets'])
        if file.suffix == '.json':
            obj = json.loads(file.read_text())
            def packet(v):
                if isinstance(v, dict):
                    return ('signatures' in v and isinstance(v['signatures'], list)) or 'packed_trx' in v or any(packet(x) for x in v.values())
                return isinstance(v, list) and any(packet(x) for x in v)
            assert not packet(obj), 'Packet in public whitelist'

entry = ('- ACTIVE REVIEW ONLY 2026-11-19 TEST bank issuance: user requests extra TEST issuance; '
         'direct issuer eosio remains sealed/code-only, supply100000000/max2100000000AIC. '
         'Prepared isolated compile-gated fixed150000AIC civicbankers one-shot token candidate, '
         'developer-active+bank dual auth/permanent20261119 receipt/issuer+precision+cap+initial-supply guards; '
         'original source rebuilt EXACT f8c5, macro-off candidate EXACT sameWASM; originalABI entries retained. '
         '20new private-loopback native946 WASM checks PASS, all children stopped;7fixture-only initial failures retained. '
         'NO public signatures/proposals/issuance/transfers/authority or service changes. '
         'WAIT user choice specifically on temporary eosio.token upgrade+exact restoration: '
         'two separate full1800s native reviews/veto; temporary wallet+auto-replacement code-pin holds expected, '
         'no pin bypass/mobile update. Root release-work-20261119/testnet-bank-issuance-r1 authoritative; '
         'candidate is NOT deployed or authorized as a generic mint capability. '
         'Never replay old completed controllers; no native/core/newserver/phone/subagents.\n\n')
for p in [R / 'AGENTS.md', OPS / 'AGENTS.md']:
    old = p.read_text()
    assert 'ACTIVE REVIEW ONLY 2026-11-19 TEST bank issuance' not in old
    p.write_text(entry + old)

hashes = {str(p.relative_to(TARGET)): hashlib.sha256(p.read_bytes()).hexdigest()
          for p in TARGET.rglob('*') if p.is_file()}
manifest = {'status': 'PUBLIC_REVIEW_ONLY_WAITING_TEMPORARY_UPGRADE_SCOPE',
            'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'branch': BRANCH, 'public_chain_mutations': 0, 'public_chain_signatures': 0,
            'isolated_checks': 20, 'files': hashes}
(TARGET / 'PUBLIC_REVIEW_MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n')
(D / 'PUBLIC_REVIEW_PREPARED.json').write_text(json.dumps(manifest, indent=2) + '\n')
(D / 'STATE.json').write_text(json.dumps({
    'status': 'WAITING_EXPLICIT_TEMPORARY_TOKEN_UPGRADE_SCOPE',
    'user_authorized': 'Additional TEST issuance, target bank from context',
    'prepared': 'Fixed single-use150000AIC;20isolated WASM checksPASS; exact original token reproduction and restoration verified',
    'question_reason': 'Current sealed issuer requires temporary token-contract upgrade; two full reviews and approximately30min of token-pin holds materially exceed a direct issue call',
    'public_chain_signatures': 0, 'public_chain_mutations': 0,
    'onchain_supply_still': '100000000.0000 AIC', 'bank_still': '2279.9158 AIC',
    'root_case': str(D), 'review_branch': BRANCH,
}, ensure_ascii=False, indent=2) + '\n')
print(json.dumps({'status': manifest['status'], 'files': len(hashes), 'target': str(TARGET)}))
