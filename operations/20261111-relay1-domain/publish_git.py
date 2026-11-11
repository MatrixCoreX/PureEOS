"""Publish only reviewed source and public qualification; no secrets, packets or APK binaries."""
from pathlib import Path
import json,subprocess,os,shutil,datetime,hashlib
D=Path(__file__).resolve().parent;ROOT=D.parents[1];W=ROOT/'PureEOS-Wallet';O=ROOT/'PureEOS-Testnet-Ops-20261111'
assert not(D/'FINAL_GIT_RESULT.json').exists()
assert json.loads((D/'FINAL_AUDIT.json').read_text())['status']=='PASS_RELAY1_ENDPOINT_MIGRATION'
assert json.loads((D/'WALLET_LIVE_POST_CUTOVER.json').read_text())['status']=='PASS_EXACT_APK_READONLY_DISCOVERY'
author=subprocess.check_output(['git','-C',str(W),'config','user.name'],text=True).strip();email=subprocess.check_output(['git','-C',str(W),'config','user.email'],text=True).strip();env=dict(os.environ,GIT_AUTHOR_NAME=author,GIT_AUTHOR_EMAIL=email,GIT_COMMITTER_NAME=author,GIT_COMMITTER_EMAIL=email)
def git(repo,*args):
 p=subprocess.run(['git','-C',str(repo),*args],capture_output=True,text=True,env=env);assert p.returncode==0,p.stderr;return p.stdout.strip()
def publish(repo,paths,message):
 git(repo,'add','--sparse','-f',*paths);git(repo,'diff','--cached','--check');git(repo,'commit','-m',message);branch=git(repo,'branch','--show-current');commit=git(repo,'rev-parse','HEAD');git(repo,'push','origin',branch);assert git(repo,'ls-remote','origin','refs/heads/'+branch).split()[0]==commit and not git(repo,'status','--porcelain');return{'branch':branch,'commit':commit,'remote_ref_verified':True,'worktree_clean':True}
entry='- COMPLETE 2026-11-11 relay1 domain: founder gptgenesisbp@keyrelay movedto https://relay1.matrixai.one via endpoint-only regrelayIR699709/27bb007a; rate3000bps/key/permissions/journal58jobs56finalized2expired unchanged. DedicatedDNS/ACME/vhost→same8787PID; old /relay directproxy retained; relay2anthropiclab1000bps/process unchanged. Wallet31 originalcert6a0e/APKbda1be11... BOTHhash/version/UID10046+10340PASS, noforcedlaunch; Mi12otherforegroundsetpreserved, Mi11setchanged acrossmetadataWITHOUTagentlaunch so no preservation/causeclaim. Core877/API39/75classesexact30+MainURIconstant-only/46softwareAndroidPASS; exactAPKrealHTTPSdiscoverypre/postPASS. No newfinancialtransfer/claim/privatephonefiles/credentialchanges, oneonchainregistrationonly; invalidunsignedPOST400/nojobcreation. Native26/946/originalservices/codeABI6/consensus/bans/bonds/maint unchanged, sixfrontendassetSHA retained/noUIrerun. Only2ownunsigned/aligned5201920Bfreed; signed/source/classes/cache/failureskept. Root release-work-20261111/relay1-domain-r1 authoritative; completedcutover/installcontrollers MUSTNOT rerun. UserAgent/PIN-UI-01held; nativeaccountsponsorshipdesignonly.\n\n'
for p in [ROOT/'AGENTS.md',W/'AGENTS.md',O/'AGENTS.md']:
 s=p.read_text()if p.exists()else''
 assert not s.startswith(entry);p.write_text(entry+s)
P=O/'operations/20261111-relay1-domain';assert not P.exists();P.mkdir(parents=True)
for n in ['REPORT.zh-CN.md','REPORT.en.md','TRIAGE.json','DNS_CREATED.json','DNS_PROXIED.json','TLS_LAUNCH.json','CUTOVER_LAUNCH_CALL.json','HTTPS_HEALTH.json','FINAL_AUDIT.json','CUTOVER_RESULT.json','REMOTE_EVIDENCE_RECEIPT.json','WALLET_QUALIFICATION_SUMMARY.json','PHONE_UPDATE_BOTH_R31.json','WALLET_LIVE_READONLY.json','WALLET_LIVE_POST_CUTOVER.json','CLEANUP.json','remote_preflight.py','setup_relay1_tls.py','ops_common.py','admin_session.py','cutover_relay1.py','final_audit.py','publish_git.py']:
 shutil.copy2(D/n,P/n)
shutil.copytree(D/'remote-public-evidence',P/'remote-public-evidence')
publicmanifest={str(p.relative_to(P)):hashlib.sha256(p.read_bytes()).hexdigest()for p in P.rglob('*')if p.is_file()};(P/'SHA256_MANIFEST.json').write_text(json.dumps(publicmanifest,indent=2)+'\n')
out={'status':'PASS','utc':datetime.datetime.now(datetime.timezone.utc).isoformat()}
out['ops']=publish(O,['AGENTS.md','operations/20261111-relay1-domain'],'Move founder relay to independent relay1 domain with irreversible endpoint registration');(D/'OPS_PUBLICATION.json').write_text(json.dumps(out['ops'],indent=2)+'\n')
base='https://github.com/MatrixCoreX/PureEOS/blob/'+out['ops']['commit']+'/operations/20261111-relay1-domain/'
Q=W/'qualification/20261111-relay1-domain-test31';assert not Q.exists();Q.mkdir()
for n in ['WALLET_QUALIFICATION_SUMMARY.json','PHONE_UPDATE_BOTH_R31.json','WALLET_LIVE_READONLY.json','WALLET_LIVE_POST_CUTOVER.json','CLEANUP.json','TRIAGE.json','RelayLiveReadOnly.java','TransferBalanceAndroidTest.java','build_wallet.py','prepare_android.py','run_android_fixture.py','update_both_r31.py']:
 shutil.copy2(D/n,Q/n)
for n in ['BUILD_COMPLETE.json','APK_BUILD_RESULT.json','APK_BADGING.txt','APK_SIGNATURE.txt','MIN_API26.json','PROTECTED_CLASSES.json','BUSINESS_LOGIC_IDENTITY.json','SOURCE_MANIFEST.json','core.log','api26.log']:
 shutil.copy2(D/'wallet-build'/n,Q/n)
shutil.copytree(D/'android-ui',Q/'android-ui',ignore=shutil.ignore_patterns('classes','tmp','classpath.txt'))
shutil.copytree(D/'live-readonly',Q/'live-readonly',ignore=shutil.ignore_patterns('*.class','one'))
(Q/'README.md').write_text('# Wallet31 relay1 migration\n\nDefault https://relay1.matrixai.one; only exact legacy founder tuple migrates. Same BP/keyrelay/30% markup; relay2 remains10%. Originalcertificate/BOTHinstalled hash+UIDverified; no forcedlaunch, Mi11foregroundsetchanged withoutagentlaunch/no causeclaim.877Core/39API26helpers/75compiledclassesexact30 plus MainActivityURI-only proof/46Android35softwarecases PASS. ExactAPKrealHTTPSdiscovery before and after canonical chain registrationPASS.\n\nNo new payment/claim/phonefinancial/Keystorevalidation. Only invalid unsigned HTTP transportprobes and existingstatusreads. Completedinstall/cutovercontrollers MUSTNOT rerun. Synthetic fixtures and harness failure kept.\n\n[中文报告]('+base+'REPORT.zh-CN.md) / [English report]('+base+'REPORT.en.md).\n')
out['wallet']=publish(W,['AGENTS.md','README.md','app/build.gradle','app/src/main/java/one/matrixai/wallet/Network.java','app/src/main/java/one/matrixai/wallet/RelayConfig.java','app/src/main/java/one/matrixai/wallet/ApiEndpoint.java','app/src/test/java/one/matrixai/wallet/RelayConfigTest.java','tools/build.py','qualification/20261111-relay1-domain-test31'],'Use relay1 by default and migrate exact legacy founder relay configuration')
(D/'FINAL_GIT_RESULT.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out))
