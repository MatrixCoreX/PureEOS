from pathlib import Path
import json,hashlib,re,datetime
R=Path('/opt/pureeos-testnet-cpu-stability-20261119-r1');G=Path('/opt/pureeos-testnet-stall-recovery-20261116-r1');out=R/'final-public-evidence';assert not out.exists();assert not(R/'PUBLIC_EXPORT_STARTED.json').exists();assert json.loads((R/'FINAL_SHARED_PRIORITY_RESULT.json').read_text())['status'].startswith('PASS_');(R/'PUBLIC_EXPORT_STARTED.json').write_text(json.dumps({'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scope':'Root public JSON and selected guardian JSON only; no templates/private/keys/packets/safety binaries'})+'\n');payload={}
for p in sorted(R.glob('*.json')):
 if p.name in ['AUTO_RUNTIME_TEMPLATE.json','PUBLIC_EXPORT_STARTED.json']:continue
 raw=p.read_bytes();json.loads(raw);payload[p.name]=raw
for p in sorted((R/'retired-unit-configs').glob('*.json')):payload['retired-unit-configs/'+p.name]=p.read_bytes()
for n in ['GUARD_LATEST.json','GUARD_LAST.json','GUARD_INFLIGHT.json','GUARD_NODE_MAP.json']:
 raw=(G/n).read_bytes();json.loads(raw);payload['guardian-current/'+n]=raw
for name,raw in payload.items():
 assert not re.search(rb'PVT_(?:K1|R1|BLS)_[A-Za-z0-9_-]{25,}',raw),name
 assert not re.search(rb'\b5[HJK][1-9A-HJ-NP-Za-km-z]{49}\b',raw),name
 assert not re.search(rb'"(?:packed_trx|signatures|Private key)"\s*:',raw),name
out.mkdir(mode=0o700);files={}
for name,raw in payload.items():
 p=out/name;p.parent.mkdir(parents=True,exist_ok=True,mode=0o700);p.write_bytes(raw);files[name]={'SHA256':hashlib.sha256(raw).hexdigest(),'bytes':len(raw),'text':raw.decode()}
manifest={n:{'SHA256':x['SHA256'],'bytes':x['bytes']}for n,x in files.items()};(out/'MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n');result={'status':'PASS_PUBLIC_WHITELIST_EVIDENCE_EXPORTED','utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'files':files,'archive':str(out),'source_SHA256':json.loads((R/'FINAL_SHARED_PRIORITY_RESULT.json').read_text())['source_SHA256'],'guardian_SHA256':json.loads((R/'FINAL_SHARED_PRIORITY_RESULT.json').read_text())['guardian_SHA256']};print(json.dumps(result))
