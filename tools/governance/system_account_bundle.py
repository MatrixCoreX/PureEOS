#!/usr/bin/env python3
"""Prepare unsigned review actions only. No RPC, signing, sending or secrets."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def name_value(text):
    alphabet='.12345abcdefghijklmnopqrstuvwxyz'
    if not text or len(text)>12 or text.endswith('.') or any(c not in alphabet for c in text):
        raise ValueError('canonical Antelope name of at most12 characters required')
    value=0
    for i in range(13):
        digit=alphabet.index(text[i]) if i<len(text) else 0
        value|=digit << (64-5*(i+1)) if i<12 else digit
    return value


def varuint(value):
    out=bytearray()
    while value>=128:
        out.append((value&127)|128);value>>=7
    out.append(value)
    return bytes(out)


def action(name, actors, data):
    return {'account':'eosio','name':name,'authorization':[
        {'actor':actor,'permission':permission} for actor,permission in actors], 'data':data.hex()}


def prepare(args):
    name_value(args.target);name_value(args.payer);name_value(args.developer)
    system_name=args.target.startswith('eosio.') and len(args.target)>6
    short_name='.' not in args.target and len(args.target)<12 and args.target!='eosio'
    if not (system_name or short_name) or args.target in {'eosio.dev','eosio.prods','eosio.msig','eosio.wrap','eosio.null'}:
        raise ValueError('new permitted eosio.* or dotless1-11character short service target required')
    if args.payer.startswith('eosio.') or args.payer=='eosio':
        raise ValueError('ordinary sponsor required; native state must also be checked')
    if not 0<args.ram_atoms<=100000000:raise ValueError('bounded exact4,AIC RAM payment required')
    if args.proposal_id<1 or args.proposal_id>=2**64:raise ValueError('expected next proposal id required')
    code=args.wasm.read_bytes();abi=args.raw_abi.read_bytes();review=args.review.read_bytes()
    if not 0<len(code)<=16*1024*1024 or not 0<len(abi)<=1024*1024 or not review:
        raise ValueError('invalid nonempty reviewed file bounds')
    if code[:4]!=b'\0asm':raise ValueError('WASM binary required')
    if abi.lstrip()[:1] in {b'{',b'['}:raise ValueError('canonical raw ABI required, not JSON ABI')
    hashes=[hashlib.sha256(value).digest() for value in [code,abi,review]]
    init=args.init_data.read_bytes() if args.init_data is not None else b''
    if len(init)>4096:raise ValueError('initializer exceeds4096bytes')
    issuer=getattr(args,'ordinary_issuer',False)
    if issuer and (not short_name or args.init_data is None):
        raise ValueError('ordinary issuer requires a NEW dotless short target and reviewed initializer; never eosio.*')
    version=3 if issuer else (2 if args.init_data is not None else 1)
    domain=b'PureEOS developer-owned issuer initializer v1\0' if issuer else b'PureEOS creation initializer v1\0'
    commitment=hashlib.sha256(domain+hashes[2]+hashlib.sha256(init).digest()).digest() if version>=2 else hashes[2]
    proposed=struct.pack('<QQqIII',name_value(args.target),name_value(args.payer),args.ram_atoms,version,len(code),len(abi))+hashes[0]+hashes[1]+commitment
    execution=struct.pack('<Q',args.proposal_id)+varuint(len(code))+code+varuint(len(abi))+abi+varuint(len(init))+init+(hashes[2] if version>=2 else bytes(32))
    upgrade=[(args.developer,'upgrade')]
    execution_auth=upgrade+[(args.payer,'active')]
    links=[]
    for name in ['syspropose','syscancel','sysexec']:
        data=struct.pack('<QQQQ',name_value(args.developer),name_value('eosio'),name_value(name),name_value('upgrade'))
        links.append(action('linkauth',[(args.developer,'active')],data))
    result={
        'status':'UNSIGNED_CANDIDATE_NOT_QUALIFIED_NOT_SUBMITTED',
        'chain_id':args.chain_id,'target':args.target,'resource_payer':args.payer,
        'ram_payment_atoms':args.ram_atoms,'authority_version':version,
        'code_bytes':len(code),'raw_abi_bytes':len(abi),
        'code_sha256':hashes[0].hex(),'raw_abi_sha256':hashes[1].hex(),'review_sha256':hashes[2].hex(),
        'native_review_commitment':commitment.hex(),
        'account_policy':{'kind':'ordinary_developer_owned_issuer' if issuer else 'sealed_system_service',
            'owner':args.developer+'@owner' if issuer else 'sealed',
            'active':[args.developer+'@active',args.target+'@eosio.code'] if issuer else 'code-only',
            'system_protected_after_creation':not issuer},
        'initialization':{'enabled':version>=2,'action':'target::init' if version>=2 else None,'bytes':len(init),'sha256':hashlib.sha256(init).hexdigest() if version>=2 else None},
        'expected_proposal_id':args.proposal_id,
        'permission_setup_actions':links,
        'proposal_actions':[action('refreshgov',upgrade,b''),action('syspropose',upgrade,proposed)],
        'execution_actions':[action('sysexec',execution_auth,execution)],
        'requirements':['Qualified all-node binary and irreversible feature activation',
            'Verify live chain id, target absence, no existing short-name bid/award, sponsor authorization and native next proposal id',
            'Publish exact review; wait complete native review and check effective vetoes',
            'Fill transaction TAPOS/expiration/limits and sign on trusted remote signer only',
            'No direct treasury payment, privileged grant, reset or modification of existing proposal3']}
    if len(args.chain_id)!=64 or any(c not in '0123456789abcdef' for c in args.chain_id):
        raise ValueError('exact lowercase hex chain id required')
    # An immutable output makes changed artifacts a new reviewable attempt.
    with args.output.open('x') as output:json.dump(result,output,indent=2);output.write('\n')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ['wasm','raw-abi','review','output']:parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--init-data',type=Path,help='Optional canonical raw target::init arguments, committed together with review hash')
    parser.add_argument('--ordinary-issuer',action='store_true',help='New ordinary short issuer delegated to developer owner/active; never changes existing or eosio.* accounts')
    for name in ['target','payer','developer','chain-id']:parser.add_argument('--'+name,required=True)
    parser.add_argument('--ram-atoms',type=int,required=True)
    parser.add_argument('--proposal-id',type=int,required=True)
    prepare(parser.parse_args())


if __name__=='__main__':main()
