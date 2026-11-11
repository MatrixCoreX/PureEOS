"""Complete ownership proof for the two known REX TEST fixtures before any enrollment signatures."""
from finance_common import *
assert not(R/'REPLACEMENT_ENROLL_STARTED.json').exists()
assert not(R/'REX_VOTER_OWNERSHIP_RECONCILED.json').exists()
baseline=read('REPLACEMENT_LIVE_BASELINE.json');owned=read('VOTE_SIGNER_OWNERSHIP.json');intent=read('AUTHORIZED_VOTE_INTENTS.json')
assert set(owned['unowned_unchanged'])=={'rexliquidaaa','rexliquidaad'}
source=Path('/opt/pureeos-rex-liquidity-batch-20261106-r1/private/keys.json');keys=json.loads(source.read_text())
rex={x['owner']:x for x in table('rexbal')};mapping=read('REPLACEMENTS_PREPARED.json')['mapping'];voters={x['owner']:x for x in baseline['affected_external']};proof={}
for n in owned['unowned_unchanged']:
 k=keys[n]['active'];a=rpc('get_account',{'account_name':n});auth=next(p['required_auth']for p in a['permissions']if p['perm_name']=='active')
 assert auth==permission(k['Public key'])
 owned['accounts'].append({'account':n,'public_key_sha256':hashlib.sha256(k['Public key'].encode()).hexdigest(),'owned_test_signer_found':True,'key_source':{'source':str(source),'json_path':[n,'active'],'public_field':'Public key','private_field':'Private key'}})
 old=voters[n];after=sorted(set(mapping.get(p,p)for p in old['producers']));intent['intents'][n]=after;intent['moves'].append({'owner':n,'before':old['producers'],'after':after,'staked_before':old['staked']})
 proof[n]={'rexbal':rex[n],'self_delegated':table('delband','eosio',n),'resources':a['total_resources']}
baseline['rex_owned_baseline']=proof;baseline['unowned_unchanged']=[];owned['unowned_unchanged']=[]
save('REPLACEMENT_LIVE_BASELINE.json',baseline);save('VOTE_SIGNER_OWNERSHIP.json',owned);save('AUTHORIZED_VOTE_INTENTS.json',intent)
x={'status':'PASS_14_OWNED_AFFECTED_VOTERS','utc':now(),'additional_owned_REX_accounts':sorted(proof),'ownership_verified_against_current_active_authority':True,'no_authority_changes':True,'financial_submissions':0,'principals_and_shares_baseline':proof}
save('REX_VOTER_OWNERSHIP_RECONCILED.json',x);print(json.dumps(x))
