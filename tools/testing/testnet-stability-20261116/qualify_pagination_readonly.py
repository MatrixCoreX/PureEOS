from pathlib import Path
import sys,json,hashlib,datetime,time,os
R=Path('/opt/pureeos-testnet-stall-recovery-20261116-r1');sys.path.insert(0,str(R));import stability_guard as g
assert not(R/'PAGINATION_READONLY_RESULT.json').exists();nodes=g.load('GUARD_NODE_MAP.json');patch=g.load('PAGINATION_PATCH_RESULT.json');samples=[];pids0=None
for n in range(3):
 rows=g.rows();assert len(rows)==47 and sum(bool(v['banned'])for v in rows.values())==21
 s,i,pids,ro,mem=g.status_context(nodes);assert s['all_running']and s['head_age']<5 and s['lib_age']<20 and s['policy_count']==21 and s['threshold']==15 and len(s['recent'])>=19
 if pids0 is None:pids0=pids
 assert pids==pids0,'No node restart expected during pagination-only qualification'
 samples.append({'utc':g.now(),'head':i['head_block_num'],'LIB':i['last_irreversible_block_num'],'head_age':s['head_age'],'LIB_age':s['lib_age'],'strong_recent':len(s['recent']),'rows':len(rows),'banned':21,'memory':mem})
 if n<2:time.sleep(20)
latest=g.load('GUARD_LATEST.json');assert latest['utc']>patch['utc'];assert latest['status']in ['NO_SAFE_OVERSIZED_TARGET','COOLDOWN','CHECK_ONLY_WOULD_RECYCLE_ONE'],latest
old=json.loads((R/'final-public-evidence/DEPLOYED_SOURCE_SHA256.json').read_text());current={n:g.sha(R/n)for n in old};assert current['stability_guard.py']==patch['new_sha256'];assert all(h==old[n]for n,h in current.items()if n!='stability_guard.py')
g.save('DEPLOYED_SOURCE_SHA256_R2.json',current)
result={'status':'PASS_PAGINATION_47_ROWS_21_BANS_39_TESTS_TIMED_GUARD_RECOVERED','utc':g.now(),'samples':samples,'all25_PIDs_unchanged':pids0,'latest_timer_result':latest,'deployed_source_SHA256':current,'software_tests':39,'scope':'Read-only checks only, no new node restart or chain write after pagination patch.'};g.save('PAGINATION_READONLY_RESULT.json',result);print(json.dumps(result))
