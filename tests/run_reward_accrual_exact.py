"""Check reward token units against an independent exact Fraction oracle."""
from pathlib import Path
from fractions import Fraction
import argparse,hashlib,json,random,subprocess

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--cxx',default='c++')
    parser.add_argument('--sanitize',action='store_true')
    args=parser.parse_args();source=args.source.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    code='#include "eosio.system/reward_halving.hpp"\n#include <iostream>\nint main(){long long s;unsigned long long o,f,t;while(std::cin>>s>>o>>f>>t)std::cout<<eosiosystem::rewards::accrued(s,o,f,t)<<"\\n";}\n'
    (out/'driver.cpp').write_text(code)
    command=[args.cxx,'-std=c++20','-O1','-I'+str(source/'contracts/eosio.system/include'),str(out/'driver.cpp'),'-o',str(out/'driver')]
    if args.sanitize:command+=['-fsanitize=undefined','-fno-sanitize-recover=all']
    with(out/'compile.log').open('w')as log:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
    day=86400*1000000;year=364*day;period=1461*day;origin=1000000;cases=[]
    for supply in [1,99,100,11000000,10**12,21*10**12,(1<<62)-1]:
        for era in [0,1,2,4,31,62,63,64]:
            for delta in [-500000,0,500000,year]:cases.append((supply,origin,origin,max(origin,origin+era*period+delta)))
            if era<64:cases.append((supply,origin,origin+era*period,origin+era*period+year))
    rng=random.Random(20261031)
    for _ in range(4000):
        supply=rng.choice([rng.randrange(1,21*10**12),rng.randrange(1,(1<<62)-1)])
        start=rng.randrange(0,70*period//500000)*500000+origin
        end=start+rng.randrange(0,8*period//500000)*500000;cases.append((supply,origin,start,end))
    cases += [(0,origin,origin,origin+year),(-1,origin,origin,origin+year),(10**12,origin,origin+year,origin),(10**12,1<<63,(1<<64)-2,(1<<64)-1)]
    def oracle(supply,origin,start,end):
        if supply<=0 or end<=start or end<=origin:return 0
        total=Fraction(0)
        for era in range(64):
            left=max(start,origin+era*period);right=min(end,origin+(era+1)*period)
            if right>left:total+=Fraction(supply*(right-left),100*year*(1<<era))
        return total.numerator//total.denominator
    inputs=''.join(' '.join(map(str,row))+'\n'for row in cases);(out/'inputs.txt').write_text(inputs)
    result=subprocess.run([str(out/'driver')],input=inputs,capture_output=True,text=True,check=True)
    (out/'run.stderr').write_text(result.stderr);actual=list(map(int,result.stdout.splitlines()));assert len(actual)==len(cases)
    errors=[{'input':row,'actual':value,'expected':oracle(*row)}for row,value in zip(cases,actual)if value!=oracle(*row)]
    evidence={'status':'FAIL'if errors else'PASS','cases':len(cases),'oracle':'Python Fraction, independent exact 1/100 and 364/1461-day constants','sanitizer':'UBSan'if args.sanitize else None,'errors':errors[:20],'header_SHA256':hashlib.sha256((source/'contracts/eosio.system/include/eosio.system/reward_halving.hpp').read_bytes()).hexdigest(),'inputs_SHA256':hashlib.sha256(inputs.encode()).hexdigest()}
    (out/'report.json').write_text(json.dumps(evidence,indent=2)+'\n');print(json.dumps(evidence));return bool(errors)

if __name__=='__main__':raise SystemExit(main())
