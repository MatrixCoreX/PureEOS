from pathlib import Path
import fcntl,json,sys,time,hashlib
B=Path('/opt/aic-bp-discovery-live-20261030');E=Path('/opt/aic-bp-maintenance-30nodes-20261030');M=Path('/opt/aic-bp-maintenance-feature-20261030')
sys.path.insert(0,str(B/'input'));import functional_common as f
with Path('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock').open('a')as lock:
 fcntl.flock(lock,fcntl.LOCK_EX)
 native=M/'reward-maturity-native-lab/fixture/REWARD_MATURITY_RESULT.json';nr=json.loads(native.read_text());assert nr['status']=='PASS'
 live=B/'rounds/R13/MAINT-08/attempt-2/result.json';lr=json.loads(live.read_text());assert lr['status']=='PARTIAL'and lr['observed']['original24hour_cooldown_from_registration_preserved']
 out=M/'evidence/MAINT08_COMPOSITE_QUALIFICATION.json'
 record=dict(status='PASS',case_id='MAINT-08',system_contract_source='63255178dd824948afc723859cebe9321691a0e9',system_wasm_sha256='6ac9dafc4c74fb47f9ce1897be65d98cfc30ddf2c66cfe68b51ca623c8db435d',live21_member_30node_cooldown_and_credits_evidence=str(live),native25hour_clock_positive_claim_evidence=str(native),native_result=nr,live24hour_wait=False,constants_changed=False,earlier_test_harness_maturity_assumption_failure_preserved=True,scope='Live30-node original cooldown/unchanged balances and credits; isolated fresh-genesis virtual-clock maturity and repeat-claim rejection',baseline_promoted=False,TPS='unmeasured')
 f.write(out,record);f.record_status('MAINT-08','PASS',out,'Live30-node cooldown guard plus isolated real-WASM maturity payout; no shortened constants or live25hour wait')
 print(json.dumps(dict(case='MAINT-08',status='PASS',evidence=str(out))))
