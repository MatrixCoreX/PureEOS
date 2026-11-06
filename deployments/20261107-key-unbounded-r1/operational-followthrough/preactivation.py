"""Read-only activation gate. Candidate APK delivery does not imply phone testing."""
from pathlib import Path
import json,datetime,hashlib,re
import ops_common as o
R=o.R;assert not(R/'CLIENT_PREACTIVATION.json').exists()
q=json.loads((R/'qualification-3/RESULT.json').read_text());assert q['status']=='PASS'and q['KEY_WASM']==o.NEW
client=json.loads((R/'WEB_QUALIFIED.json').read_text());assert client['status']=='PASS'
assert client['tests']==269 and client['full_attempt_passed']==268 and client['targeted_unchanged_test_passed']==1 and client['no_product_or_test_assertion_change']

wallet=json.loads((R/'wallet-build-7/RESULT.json').read_text());assert wallet['status']=='PASS'
w=wallet['wallet'];assert w['version']=='0.1.0-test.16'and w['core_checks']==732 and w['API26_helpers']==32
build=json.loads((R/'wallet-build-7/android/APK_BUILD_RESULT.json').read_text());assert build['sha256']==w['apk_sha256']and'6a0ecbb495429539e69e5e9e1f33b3a5aa715dd90af8a43c32127f85ed24e4d3'in build['signature']
assert o.sha(Path(build['apk']).read_bytes())==w['apk_sha256']
ui=json.loads((R/'UI_PUBLIC_PRE.json').read_text());assert ui['status']=='PASS'and len(ui['variants'])==4
assert json.loads((R/'VISUAL_REVIEW_PUBLIC_PRE.json').read_text())['status']=='PASS'
publication=json.loads((R/'FRONTEND_PUBLICATION.json').read_text());assert str(Path('/var/www/aic-test/current').resolve())==publication['release']
for a in ui['assets']:assert a['actual']==a['expected']==publication['files'][a['path']]
runtime=json.loads((R/'RUNTIME_SOURCE.json').read_text());assert runtime['exact_qualified_SDK28']and runtime['relay_implementation_unchanged']
for n,h in runtime['files'].items():assert o.sha((R/'relay-runtime'/n).read_bytes())==h
sources=json.loads((R/'CLIENT_SOURCE_PUBLISHED.json').read_text());assert sources['status']=='CANDIDATE_SOURCE_PUSHED_REF_VERIFIED'
o.guard();o.require_codes(o.OLD);o.authority_guard()
o.save(R/'CLIENT_PREACTIVATION.json',{'status':'PASS','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'new_KEY':o.NEW,'wallet_version':w['version'],'APK_SHA256':w['apk_sha256'],'core_checks':732,'API26_helpers':32,'protected_r58_classes':len(w['protected_classes_identical_to_r58']),'preserved_installed_QR_baseline':'r58/test15/57d4af37','phone_install':'Not performed: separate active physical QR/input campaign; original test15 kept. Test16 is qualified/deliverable and must be used after the governed upgrade. Older APK pins safely refuse unknown contract. No phone data/unlock/financial tests claimed.','web_source_commit':sources['refs']['PureEOS-explorer-KeyGrowth-20261107']['commit'],'wallet_source_commit':sources['refs']['PureEOS-Wallet-KeyGrowth-20261107']['commit'],'web_public_release':publication['release'],'web_regression_tests':269,'web_test_provenance':'268 original full-attempt passes + unchanged timed-out case separately passed at45s; original15s timeout retained, not a single full269-green run','TypeScript_and_production_build':'PASS','actual_public_Chrome_variants':4,'public_asset_count':len(ui['assets']),'known_broad_get_block400_retained':ui['knownReadFailuresRetained'],'runtime_manifest_SHA256':o.sha((R/'RUNTIME_SOURCE.json').read_bytes()),'unknown_code_still_rejected':True,'no_full_Android_or_full_explorer_health_claim':True})
print('CLIENT_PREACTIVATION_PASS_READY_FOR_FULL_NATIVE_REVIEW_ONLY')
