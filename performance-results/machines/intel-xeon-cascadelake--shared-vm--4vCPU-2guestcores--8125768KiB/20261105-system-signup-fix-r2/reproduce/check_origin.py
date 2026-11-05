"""Remote read-only origin checks with exact TLS host and pinned chain identity."""
from pathlib import Path
import datetime,hashlib,json,subprocess,time,urllib.parse
R=Path('/opt/pureeos-system-signup-fix-20261105-r2');base='https://aic-test.matrixai.one'
chain='116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0'
report={'status':'RUNNING','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scope':'Read-only TLS origin checks via loopback; no public-browser or external-path claim','checks':{}}
def get(path,data=None):
    args=['curl','--noproxy','*','--resolve','aic-test.matrixai.one:443:127.0.0.1','--max-time','18','--fail-with-body','-sS']
    if data is not None:args += ['-H','Content-Type: application/json','--data',json.dumps(data)]
    value=subprocess.run(args+[base+path],capture_output=True,check=True).stdout
    report['checks'][path]={'HTTP_status':200,'bytes':len(value),'sha256':hashlib.sha256(value).hexdigest()}
    return value
try:
    info=json.loads(get('/api/v1/chain/get_info',{}));assert info['chain_id']==chain
    html=get('/');assert b'<html' in html.lower()
    relay=json.loads(get('/relay/health'));report['relay']=relay
    health=json.loads(get('/history/v2/health'));report['history_health']=health
    assert health.get('health') and all(x['status']=='OK' for x in health['health']), 'History service health not OK'
    es=next(x['service_data'] for x in health['health'] if x['service']=='Elasticsearch')
    baseline=json.loads((R/'BEFORE_CHAIN.json').read_text())['info']['head_block_num']
    assert int(es['last_indexed_block'])>=baseline,'History has not caught the preserved head'
    previous=[]
    for p in (R/'origin-attempts').glob('*.json'):
        value=json.loads(p.read_text())
        if value.get('status')=='PASS' and 'indexed_block' in value:previous.append(value)
    if previous:
        old=sorted(previous,key=lambda x:x['UTC'])[-1]
        assert int(es['last_indexed_block'])>int(old['indexed_block']),'History index has not advanced since prior HTTP check'
        report['observed_index_progress']={'from':old['indexed_block'],'to':es['last_indexed_block'],'since_UTC':old['UTC']}
    proposals=json.loads(get('/history/v2/history/get_upgrade_proposals?offset=0&limit=20'))
    assert proposals['chain_id']==chain
    assert [[x['proposal_id'],x['status'],len(x['vetoes'])] for x in proposals['entries']]==[['3','pending',24],['2','executed',0],['1','executed',0]]
    report['canonical_proposals']=proposals
    wallets=json.loads(get('/api/v1/chain/get_table_rows',{'code':'eosio.key','scope':'eosio.key','table':'wallets','json':True,'limit':1}))
    assert wallets['rows'],'No retained public-key wallet row'
    key=wallets['rows'][0]['key']
    receipts=json.loads(get('/history/v2/history/get_key_transactions?'+urllib.parse.urlencode({'public_key':key})))
    assert receipts['chain_id']==chain and not receipts['indexing']['paused'] and not receipts['indexing']['stale']
    report['key_history_indexing']=receipts['indexing']
    site=Path('/var/www/aic-test/current').resolve();expected=json.loads(Path('/opt/pureeos-upgrade-history-20261104-r1/DEPLOYMENT.json').read_text())
    assert str(site)==expected['release']
    assert {str(p.relative_to(site)):hashlib.sha256(p.read_bytes()).hexdigest() for p in site.rglob('*') if p.is_file()}==expected['files']
    report.update(status='PASS',chain_id=chain,head=info['head_block_num'],LIB=info['last_irreversible_block_num'],
        indexed_block=es['last_indexed_block'],frontend_release=str(site),frontend_files_unchanged=True)
except Exception as error:
    report['status']='FAIL';report['error']=str(error)
report['finished_UTC']=datetime.datetime.now(datetime.timezone.utc).isoformat()
p=R/'POST_RESTORE_ORIGIN.json';p.write_text(json.dumps(report,indent=2)+'\n')
attempts=R/'origin-attempts';attempts.mkdir(exist_ok=True)
(attempts/(report['UTC'].replace(':','-')+'.json')).write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k not in ['canonical_proposals','history_health','relay']}))
if report['status']!='PASS':raise SystemExit(1)
