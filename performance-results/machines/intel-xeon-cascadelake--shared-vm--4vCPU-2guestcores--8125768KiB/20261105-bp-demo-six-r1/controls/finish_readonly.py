"""Wait for genuine native completion, then run bounded read-only audits and Chrome."""
from common import *
import traceback

try:
    assert not (R/'READONLY_FINISH_RESULT.json').exists()
    save('READONLY_FINISH_PROGRESS.json',{'status':'WAITING_NATIVE_DEMO_PASS','UTC':now()})
    deadline=time.monotonic()+28800
    while not (R/'NATIVE_DEMO_RESULT.json').exists():
        if (R/'DEMO_PROGRESS.json').exists():
            assert read('DEMO_PROGRESS.json')['status']!='NEEDS_RECONCILIATION','Native campaign requires reconciliation; no completion claim'
        assert time.monotonic()<deadline,'Native completion deadline'
        time.sleep(10)
    assert read('NATIVE_DEMO_RESULT.json')['status']=='PASS'
    (R/'OBSERVER_FINISH').write_text('Native production/bans/maintenance proof complete; close only this owned read-only observer.\n')
    save('READONLY_FINISH_PROGRESS.json',{'status':'AUDITING_CANONICAL_WINDOWS_AND_CURRENT_INVARIANTS','UTC':now()})
    for script,log in [('audit_zero_windows.py','zero-window-audit.log'),('final_audit.py','final-audit.log')]:
        with (R/log).open('x') as out:
            subprocess.run(['/usr/bin/python3','-u',str(R/script)],cwd=R,stdout=out,stderr=subprocess.STDOUT,check=True,timeout=1200)
    save('READONLY_FINISH_PROGRESS.json',{'status':'VERIFYING_REAL_PUBLIC_BROWSER','UTC':now()})
    env={**os.environ,'PUREEOS_PUBLIC':'1','PUREEOS_FINAL_DEMO':'1','LD_LIBRARY_PATH':'/opt/pureeos-key-history-20261103-r1/tools/browser-libs/usr/lib/x86_64-linux-gnu','FONTCONFIG_FILE':'/opt/pureeos-key-history-20261103-r1/tools/fonts.conf'}
    with (R/'demo-public-chrome.log').open('x') as out:
        subprocess.run([str(C/'tools/node-current/bin/node'),str(R/'check_browser.cjs')],cwd=R,env=env,stdout=out,stderr=subprocess.STDOUT,check=True,timeout=1200)
    assert read('ZERO_WINDOW_CANONICAL_AUDIT.json')['status']==read('FINAL_AUDIT.json')['status']==read('DEMO_PUBLIC.json')['status']=='PASS'
    assert len(read('DEMO_PUBLIC.json')['variants'])==4
    save('READONLY_FINISH_RESULT.json',{'status':'PASS','UTC':now(),'chain_id':CHAIN,'native_zero_windows':15,'public_Chrome_variants':4,'scope':'Automatic read-only follow-through after genuine native completion. No replay of enrollment, payments, votes, bond settlement or maintenance. Visual review, Git/report finalization and bounded cleanup remain required.'})
    save('READONLY_FINISH_PROGRESS.json',{'status':'PASS_AWAITING_VISUAL_REVIEW_REPORT_GIT_CLEANUP','UTC':now()})
    print('NATIVE_DEMO_READONLY_FOLLOW_THROUGH_PASS',flush=True)
except BaseException:
    save('READONLY_FINISH_FAILURE.json',{'status':'FAIL','UTC':now(),'error':traceback.format_exc()});raise
