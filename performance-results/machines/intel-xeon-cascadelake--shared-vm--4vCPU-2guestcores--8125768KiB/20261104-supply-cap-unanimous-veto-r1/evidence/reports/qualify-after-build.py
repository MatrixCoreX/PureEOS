from pathlib import Path
import json,subprocess,time
K=Path("/opt/pureeos-cap-veto-20261104-r1")
for i in range(450):
 if(K/"BUILD_RESULT.json").exists():break
 assert not(K/"BUILD_FAILURE.json").exists()
 time.sleep(2)
assert json.loads((K/"BUILD_RESULT.json").read_text())["status"]=="PASS"
subprocess.run(["python3","-B",str(K/"qualify_candidate.py")],check=True)
