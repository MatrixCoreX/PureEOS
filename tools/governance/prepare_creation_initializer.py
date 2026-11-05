#!/usr/bin/env python3
"""Offline raw target::init arguments for version2 creation. No RPC/signing."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
def pack(cleos,argv):
    result=subprocess.run([str(cleos),'convert','pack_hex',*argv],check=True,capture_output=True,text=True)
    value=result.stdout.strip()
    if value.startswith('"'):value=json.loads(value)
    if not re.fullmatch(r'(?:[0-9a-fA-F]{2})*',value):raise ValueError('native converter did not return canonical hex')
    return bytes.fromhex(value)
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['abi','initializer','cleos','output_data','output_manifest']:p.add_argument('--'+name.replace('_','-'),type=Path,required=True)
    p.add_argument('--cleos-sha256',required=True);a=p.parse_args()
    if not a.cleos.is_absolute() or hashlib.sha256(a.cleos.read_bytes()).hexdigest()!=a.cleos_sha256:raise ValueError('reviewed local native converter required')
    for output in [a.output_data,a.output_manifest]:
        if output.exists():raise ValueError('outputs are immutable; use new paths')
    abi=json.loads(a.abi.read_text());initial=json.loads(a.initializer.read_text())
    declared=[x for x in abi.get('actions',[])if x['name']=='init']
    if len(declared)!=1:raise ValueError('exactly one declared init action required')
    data=pack(a.cleos,['--type',declared[0]['type'],'--abi-file',str(a.abi),json.dumps(initial)])
    if len(data)>4096:raise ValueError('initializer exceeds4096bytes')
    manifest={'status':'UNSIGNED_INITIALIZER_NOT_SUBMITTED','action':'target::init','authority_version':2,
      'authorization':'target@active only, inside reviewed native creation stage',
      'init_bytes':len(data),'init_sha256':hashlib.sha256(data).hexdigest(),'init_hex':data.hex(),
      'initializer_json_sha256':hashlib.sha256(a.initializer.read_bytes()).hexdigest(),
      'native_converter_sha256':a.cleos_sha256,
      'scope':'Version2 native review commitment binds initializer and human review hash; raw ABI unchanged; no ordinary setabi/devexec initialization or arbitrary action/target'}
    with a.output_data.open('xb')as f:f.write(data)
    with a.output_manifest.open('x')as f:json.dump(manifest,f,indent=2);f.write('\n')
if __name__=='__main__':main()
