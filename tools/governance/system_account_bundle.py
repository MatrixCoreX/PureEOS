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
    if not args.target.startswith('eosio.') or args.target in {'eosio.prods','eosio.msig','eosio.wrap','eosio.null'}:
        raise ValueError('new permitted eosio.* target required')
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
    proposed=struct.pack('<QQqIII',name_value(args.target),name_value(args.payer),args.ram_atoms,1,len(code),len(abi))+b''.join(hashes)
    execution=struct.pack('<Q',args.proposal_id)+varuint(len(code))+code+varuint(len(abi))+abi
    upgrade=[(args.developer,'upgrade')]
    execution_auth=upgrade+[(args.payer,'active')]
    links=[]
    for name in ['syspropose','syscancel','sysexec']:
        data=struct.pack('<QQQQ',name_value(args.developer),name_value('eosio'),name_value(name),name_value('upgrade'))
        links.append(action('linkauth',[(args.developer,'active')],data))
    result={
        'status':'UNSIGNED_CANDIDATE_NOT_QUALIFIED_NOT_SUBMITTED',
        'chain_id':args.chain_id,'target':args.target,'resource_payer':args.payer,
        'ram_payment_atoms':args.ram_atoms,'authority_version':1,
        'code_bytes':len(code),'raw_abi_bytes':len(abi),
        'code_sha256':hashes[0].hex(),'raw_abi_sha256':hashes[1].hex(),'review_sha256':hashes[2].hex(),
        'expected_proposal_id':args.proposal_id,
        'permission_setup_actions':links,
        'proposal_actions':[action('refreshgov',upgrade,b''),action('syspropose',upgrade,proposed)],
        'execution_actions':[action('sysexec',execution_auth,execution)],
        'requirements':['Qualified all-node binary and irreversible feature activation',
            'Verify live chain id, target absence, sponsor authorization and native next proposal id',
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
    for name in ['target','payer','developer','chain-id']:parser.add_argument('--'+name,required=True)
    parser.add_argument('--ram-atoms',type=int,required=True)
    parser.add_argument('--proposal-id',type=int,required=True)
    prepare(parser.parse_args())


if __name__=='__main__':main()
