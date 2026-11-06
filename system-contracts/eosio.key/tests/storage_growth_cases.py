"""Private native state-compatibility and aggregate-growth qualification.

Never used by a public-chain driver. Synthetic boundary seeds are explicitly
separate from the genuine prior-WASM funded-ledger upgrade case.
"""
import datetime
import hashlib
import json


def native_deposit(t, key, quantity='0.0001 AIC', token='eosio.token', failure=None):
    auth=[{'actor':'keyusersolar','permission':'active'}]
    actions=[{'account':'eosio.key','name':'deposit','authorization':auth,
              'data':{'from':'keyusersolar','token_contract':token,'to':key['Public key'],
                      'quantity':quantity,'max_fee':'10.0000 AIC','request_id':hashlib.sha256(
                          (str(t.nonce)+key['Public key']+quantity).encode()).hexdigest()}},
             {'account':'eosio.token','name':'transfer','authorization':auth,
              'data':{'from':'keyusersolar','to':'eosio.key','quantity':'10.0000 AIC','memo':'atomic key fee'}},
             {'account':token,'name':'transfer','authorization':auth,
              'data':{'from':'keyusersolar','to':'eosio.key','quantity':quantity,'memo':'atomic key principal'}}]
    for action in actions[1:]:
        action['data']=json.loads(t.run(['convert','pack_hex','--type','transfer','--abi-file',
            str(t.contracts/'eosio.token/eosio.token.abi'),json.dumps(action['data'])]))
    info=t.rpc('get_info');block=bytes.fromhex(info['last_irreversible_block_id'])
    transaction={'expiration':(datetime.datetime.fromisoformat(info['head_block_time']+'+00:00')+
                   datetime.timedelta(seconds=120)).strftime('%Y-%m-%dT%H:%M:%S'),
                 'ref_block_num':info['last_irreversible_block_num']&65535,
                 'ref_block_prefix':int.from_bytes(block[8:12],'little'),
                 'max_net_usage_words':0,'max_cpu_usage_ms':0,'delay_sec':0,
                 'context_free_actions':[],'actions':actions,'transaction_extensions':[]}
    return t.tx(['push','transaction',json.dumps(transaction)],failure=failure)


def old_state(t):
    # The actual legacy ABI has no growth table. Read only its existing tables.
    result={n:t.ktable(n)for n in ['config','wallets','assets','nativefees','feebuckets','pricing','relays','relayrates']}
    result['balances']={str(w['id']):t.ktable('balances',str(w['id']))for w in result['wallets']}
    result['native']={token:t.token_balance('eosio.key',token)for token in ['eosio.token','usd','cny']}
    return result


def upgrade_legacy_state(t,args):
    assert not args.growth_fixture, 'Synthetic boundary seeds and genuine legacy migration are separate runs'
    legacy_hash=hashlib.sha256((args.legacy_artifacts/'eosio.key.wasm').read_bytes()).hexdigest()
    assert legacy_hash=='30f6e44225419c30c180a89195c8ace90f4b24a193d868abc447cbb348ad09ae'
    t.check(t.rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash']==legacy_hash,
            'Migration: actual reviewed prior KEY WASM is deployed before funding')
    key=t.key();native_deposit(t,key,'1.0000 AIC')
    before=old_state(t)
    assert int(before['config'][0]['wallet_count'])==1 and before['config'][0]['policy']['max_wallets']==1
    native_deposit(t,t.key(),failure='wallet storage capacity reached')
    t.check(old_state(t)==before,'Migration: actual prior WASM rejects a second key at legacy cap and rolls back all funds')
    t.tx(['set','contract','eosio.key',str(args.artifacts),'eosio.key.wasm','eosio.key.abi','-p','eosio.key@owner'])
    t.check(old_state(t)==before and not t.ktable('growth'),
            'Migration: replacing code/ABI preserves all funded legacy rows and reserves without re-init or eager rewrite')
    t.report['legacy_state_upgrade']={'prior_WASM':legacy_hash,'before_after_state_sha256':hashlib.sha256(
        json.dumps(before,sort_keys=True).encode()).hexdigest(),'scope':'Owned isolated pre-sealing code replacement; public activation separately requires full native governance'}


def ordinary_growth(t,args,deposit):
    cfg=t.ktable('config')[0]
    t.check(all(int(cfg['policy'][x])==1 for x in ['max_wallets','max_balances','max_assets']),
            'Growth: legacy config still contains tiny one-record caps; no administrator increased them')
    while len(t.ktable('wallets'))<24:
        deposit(t.key(),'0.0001 AIC')
    cfg=t.ktable('config')[0];counts=t.ktable('growth')[0]
    t.check(int(counts['version'])==1 and int(counts['wallet_count'])==len(t.ktable('wallets'))==int(cfg['wallet_count']),
            'Growth: exact lazy uint64 wallet counts cross legacy one/20-wallet limits with funded real records')
    internal=sum(len(t.ktable('balances',str(w['id'])))for w in t.ktable('wallets'))
    t.check(int(counts['balance_count'])==int(cfg['balance_count'])>=internal and
            int(counts['asset_count'])==len(t.ktable('assets'))==3,
            'Growth: balance and asset allocation cross old caps while maintaining cumulative counters')
    t.check(all(int(a['ram_used'])<=int(a['ram_purchased'])for a in t.ktable('assets')),
            'Growth: removing count limits never permits unfunded per-asset RAM consumption')
    t.check_reserves()


def boundary_cases(t,args):
    """Run before private govinit, while the fixture account has its own owner.

    Only counters and RAM-budget numbers are synthetic. Native transfers, RAM
    purchases, candidate WASM execution and rollback checks are real.
    """
    assert args.atomic and not args.legacy_artifacts
    fixture=args.artifacts/'test-only-growth'
    def seed(mode):
        t.tx(['set','contract','eosio.key',str(fixture),'growth.seed.wasm','growth.seed.abi','-p','eosio.key@owner'])
        t.action('seed',{'mode':mode},'eosio.key@owner',contract='eosio.key')
        t.tx(['set','contract','eosio.key',str(args.artifacts),'eosio.key.wasm','eosio.key.abi','-p','eosio.key@owner'])
        t.check(t.rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash']==
                hashlib.sha256((args.artifacts/'eosio.key.wasm').read_bytes()).hexdigest(),
                'Boundary: production candidate restored after TEST ONLY seed '+str(mode))
    def snapshot():
        return {'state':t.state(),'RAM':{k:t.rpc('get_account',{'account_name':'eosio.key'})[k]for k in ['ram_quota','ram_usage']},
                'market':t.table('rammarket'),'native':{owner:{token:t.token_balance(owner,token)
                    for token in ['eosio.token','usd','cny']}for owner in ['eosio.key','keyusersolar']},'pending':t.ktable('pending')}
    def reject(key,error,token='eosio.token',quantity='0.0001 AIC'):
        before=snapshot();native_deposit(t,key,quantity,token,failure=error)
        t.check(snapshot()==before,'Boundary: '+error+' atomically restores native funds, RAM market/quota, ledgers and counters')
    seed(1)
    for _ in range(2):native_deposit(t,t.key())
    counts=t.ktable('growth')[0];cfg=t.ktable('config')[0]
    t.check(int(counts['wallet_count'])==2**32 and int(counts['balance_count'])==2**32 and
            int(cfg['wallet_count'])==2**32-1 and int(cfg['balance_count'])==2**32-1,
            'Boundary: seeded legacy uint32 counters migrate and cross 2^32 without wrap or admission rejection')
    a=t.ktable('assets')[0]
    t.check(int(a['ram_purchased'])>2**40 and int(a['ram_used'])>2**40 and int(a['ram_used'])<=int(a['ram_purchased']),
            'Boundary: actual paid allocation crosses synthetic prior 1TiB RAM-budget ceiling')
    t.check_reserves()
    seed(2);last=t.key();native_deposit(t,last)
    counts=t.ktable('growth')[0]
    t.check(int(counts['wallet_count'])==2**64-2 and int(counts['balance_count'])==2**64-1,
            'Boundary: exact uint64 counters retain values at the final representable increment')
    reject(t.key(),'wallet identifier overflow')
    reject(last,'allocation counter overflow','usd','0.0001 USD')
    seed(3);reject(t.key(),'atomic RAM budget overflow')
    t.check_reserves()
    t.report['synthetic_growth_boundaries']={'scope':'TEST ONLY seeded numbers, not billions of real records or physical TiB allocation',
        'checked':['legacy32 lazy seed/carry','uint64 final increment/overflow','identifier overflow','prior 1TiB budget crossing','uint64 RAM-budget overflow','late native purchase rollback']}
