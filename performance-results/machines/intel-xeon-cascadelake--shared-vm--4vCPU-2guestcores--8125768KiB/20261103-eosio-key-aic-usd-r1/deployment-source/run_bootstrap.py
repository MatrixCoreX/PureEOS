from pathlib import Path
import json,time,traceback,hashlib,subprocess
R=Path('/opt/pureeos-keynet-20261103-r1')
exec(compile((R/'bootstrap_definitions.py').read_text(),'TEST-bootstrap-definitions','exec'),globals())
f=None
try:
    f=TestChain();(R/'FOUNDER_PORTS.json').write_text(json.dumps({'rpc':f.http_port,'p2p':f.p2p_port})+'\n');f.setup(register_candidates=False);f.wait_produced('gptgenesisbp')
    f.action('voteproducer',{'voter':'gptgenesisbp','proxy':'','producers':['gptgenesisbp']},'gptgenesisbp')
    deadline=time.monotonic()+120
    while True:
        fin=f.rpc('get_finalizer_info')
        if len(fin['active_finalizer_policy']['finalizers'])==1 and fin['active_finalizer_policy']['finalizers'][0]['description']=='gptgenesisbp'and not fin.get('pending_finalizer_policy'):break
        assert time.monotonic()<deadline;time.sleep(.3)
    cfg=f.root/'config/config.ini';bootstrap=[f.accounts['eosio']['Public key'],f.bls['eosio']['Public key']]
    lines=[l for l in cfg.read_text().splitlines()if l.strip()!='producer-name = eosio'and not any(l.startswith('signature-provider = '+k+'=')for k in bootstrap)]
    cfg.write_text('\n'.join(lines)+'\n');subprocess.run(['systemctl','restart','pureeos-keynet-founder-20261103.service'],check=True)
    deadline=time.monotonic()+60
    while True:
        try:info=f.rpc('get_info');break
        except Exception:assert time.monotonic()<deadline;time.sleep(.2)
    f.wait_lib(info['head_block_num']+6)
    g=f.table('globalv2')[0];g2=f.table('global2')[0];bond=f.table('bpbondcfg')[0]
    assert g['max_block_cpu_usage']==800000 and g['max_block_net_usage']==1048576 and g2['new_ram_per_block']==1024
    assert f.rpc('get_code_hash',{'account_name':'eosio'})['code_hash']=='b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170'
    assert bond['enforced']and bond['refund_delay_sec']==604800
    assert 'eosio.key' in f.governance()['protected_accounts']
    f.bootstrapping=False
    marker=f.tx(['transfer','civicuserone','civicusertwo','0.0001 AIC','KEYNET bootstrap functional marker'])
    result={'status':'PASS','chain_id':info['chain_id'],'first_BP':'gptgenesisbp','CPU':800000,'NET':1048576,'RAM_growth':1024,'system_WASM':'b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170','nodeos_SHA256':hashlib.sha256((V/'libexec/nodeos').read_bytes()).hexdigest(),'bond_config':bond,'governance':f.governance(),'checks':f.checks,'marker':marker['transaction_id'],'genesis_bootstrap_signer_removed':True,'TPS':'explicitly excluded','ordinary_roles':['civicuserone','civicusertwo','civicrenters'],'scope':'Fresh cheap4vCPU/8GB VM TEST chain; old cheap chain archived before this authorized reset; no TPS'}
    (R/'BOOTSTRAP_RESULT.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({'status':'PASS','chain_id':info['chain_id']}),flush=True)
except BaseException:
    error=traceback.format_exc();error=f.redact(error)if f else error
    (R/'BOOTSTRAP_FAILURE.json').write_text(json.dumps({'status':'FAIL','error':error},indent=2));print(error,flush=True);raise
