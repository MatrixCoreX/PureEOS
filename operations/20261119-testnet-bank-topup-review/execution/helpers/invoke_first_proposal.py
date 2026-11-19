import subprocess,sys
p=subprocess.run(['python3','/opt/pureeos-testnet-bank-issuance-20261119-r1/bank_upgrade.py','propose'],capture_output=True,text=True,timeout=210)
sys.stdout.write(p.stdout);sys.stderr.write(p.stderr);raise SystemExit(p.returncode)
