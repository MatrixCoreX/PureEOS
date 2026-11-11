# Founder relay moved to relay1

The canonical founder relay is **https://relay1.matrixai.one**. Its on-chain registration is irreversible. Relay2 remains https://relay2.matrixai.one.

| Service | BP permission | Markup on base fee |
| --- | --- | --- |
| relay1 | gptgenesisbp@keyrelay | 30% |
| relay2 | anthropiclab@keyrelay | 10% |

These percentages apply to the base fee, not the transferred principal.

A dedicated proxied DNS record, ACME certificate and Nginx vhost forward to the existing8787 process. The old /relay path stays a direct proxy to the same backend, with no POST redirect. Existing backend keys, authorities and journals remain. No relay/native node restart was performed.

One endpoint-only regrelay action was authorized by the founder BP. Transaction `27bb007a96d607f8dec2e93def9066c68a2dd3749fffcb27b0e4c738bb2878e6` executed irreversibly in block699709. Other registrations and all rates are unchanged. The existing58jobs (56finalized,2expired) and original journal bytes are unchanged, with no historical re-sign/resubmit. The isolated temporary signer is closed.

Wallet31 uses relay1 by default and canonicalizes only the exact old founder URL/account/keyrelay tuple. Other identities, relay2 and customRPC remain unchanged. Existing signed packet IDs, digests and signatures are preserved. The on-chain endpoint comparison uses the same exact alias while retaining eligibility, identity, permission, enabled-state and fee-rate checks. MainActivity source and financial/fee/registration/encryption bodies are unchanged; its compiled difference is limited to the inlined URL constant.

Both Xiaomi devices received the original-certificate overlay. APK SHA256: `bda1be11da4873c639c2463f949987a14dc471125f17269a884409f77aef14f3`. Version/hash/UID10046+10340 verified. No private wallet files, credential changes, phone payments or forced wallet launches. Mi12foregroundpackage set remained equal. Mi11set changed across observations without an agent launch; no complete foreground-preservation or cause claim is made.

Validation passed: SDK36build,877core checks,39API26helpers,75unchanged protected compiledclasses, MainActivity URI-only proof,46Android35softwareUI/Handler cases. ExactAPK Network classes verified both realHTTPS services and30%/10% eligibility/rates before and after cutover. Software cases use syntheticRPC/inmemorymetadata and do not establish complete physical-device/Keystore/financial acceptance.

Public health, matching existing finalized status through both domains, and rejection of an empty unsigned POST with400/no new jobs passed. No new transfer or claim was made; endpoint registration was the only on-chain action. Explorer source/build unchanged, all six published asset hashes retained; it reads registry data dynamically. No full browser UI rerun is claimed.26native processes/946binary, original monitored services,6contract code/ABI pins, consensus, bans, bonds and maintenance are unchanged. Relay2 process predates this task.

TRIAGE.json records harness corrections and limitations. Only two owned unsigned/aligned intermediates were removed, releasing5,201,920allocated bytes; all signed/source/classes/cache/evidence retained. Never replay completed install or chain-submission controllers. Named-account resource sponsorship remains a separate design-only item.
