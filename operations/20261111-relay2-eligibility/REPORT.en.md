# BP relay eligibility, independent relay2 and wallet30 deployment

[中文](REPORT.zh-CN.md)

## Completed behavior

The published fee estimator now follows eosio.key::check_bp: exclude banned/inactive producers, missing/abnormal/underfunded bonds and pending/effective maintenance. Eligible disabled registrations remain explicitly comparison-only. Failed eligibility reads clear stale quotes; no eligible registrations means no quote. A disqualified selection falls back to an eligible enabled relay. Normal standby BPs need not occupy the current21production slots.

Read same-chain producersv2/rotcands/bpbonds/bpmaint with complete pagination and cursor/identity/duplicate/integer/boolean validation. Unknown data fails closed. Chain identity and reviewed system code are checked across the read. The contract still authoritatively checks execution; polling cannot remove state-change races. Real banned singaporentu is hidden without deleting historical registration.

## Independent10percent service

Endpoint **https://relay2.matrixai.one**, health `/health`, BP `anthropiclab`, dedicated permission `keyrelay`, rate1000bps:10% of the base fee, not principal. Original founder30percent service continues.

Registration `335e693194b3e7aa0c946aff5a3a3cef5c2ffe784c9747a26e306a010beea324` is irreversible at695971. The former inactive15percent comparison entry became an enabled10percent service. New DNS uses the first endpoint's Cloudflare proxy mode; an independent valid TLS certificate and newNginxvhost proxy loopback8788. Existing vhost bytes are unchanged. Body limits/rate limits/no-store responses are enabled.

A new hot key is separate from account-management/production keys and linked only to ksend/withdraw/cancel. Owner/active and production keys are unchanged; cold keys are not loaded by the relay. Existing reviewed SDK/ABI/relay code is reused with independent journal/state. Service is enabled; temporary admin/test signers are closed. No key/password/certificate private material was exported or published.

This service supports existing public-key→public-key/named-account transfers. The previously discussed native-account CPU/NET/RAM sponsorship remains design-only; no such contract entry was implemented here.

## Wallet and phones

Wallet30 verifies BP eligibility during discovery/save validation, quoting, signing and broadcast. Invalid, missing, truncated or unavailable eligibility blocks use. Current relay/RPC selection is preserved; no automatic relay replacement, new signature or resend of pending packets. Native sender-funded zero-fee two-root deposits and all other productionJava source remain unchanged29.

Original-certificate APK `7521c789aacd5155b26f5d6c7a39b7b83bd839cb61356ff070c3954a51e00f50`, certificate `6a0ecbb495429539e69e5e9e1f33b3a5aa715dd90af8a43c32127f85ed24e4d3`, installed on both Xiaomi11/12 from29to30. Exact package/version/hash/UID10046+10340 checks passed. Both other foreground apps were preserved, with no forced wallet launch, data clear, credential change, private phone-file access or phone payment.

To select relay2, enter its endpoint in Settings→Change relay, verify anthropiclab/10percent, then explicitly Save. Detection does not pay. This task did not change users' saved phone relay selection.

## Qualification

- Remote browser289tests,TypeScript/build;4preview+4normal-public language/theme/viewport variants and6asset hashes PASS. Explicit synthetic read-only RPC cases covered newban/outage/emptyeligibility/recovery without actual chain changes.
- WalletSDK36,872core checks,39API26helpers,71unaffected compiledclasses byte-identical29,44actualAndroid35softwareUI/Handler cases. BadBPcannotreachSave; eligible10percent relay2 requires explicitSave.
- Exact APK Network production classes also discovered both services through real publicHTTPS metadata only:1000/3000bps, with no signing/submission.
- New isolated test keys received0.5000AIC funding, followed by two0.0001AIC transfers submitted through relay2 publicHTTPS. Full principal, exact signed1000bps, fee math and canonical irreversible receipts PASS. No phone keys/payments.

| Tested route | Total | Actual RAM purchase | Execution income | Relay fee |
|---|---:|---:|---:|---:|
| key-to-key | 0.0283 AIC | 0.0037 AIC | 0.0220 AIC | 0.0026 AIC |
| key-to-named | 0.0295 AIC | 0.0000 AIC | 0.0268 AIC | 0.0027 AIC |

Tests include a memo and dynamic market pricing; homepage empty-memo estimates may differ. 0.0541AIC accrued as new test relay earnings; no claim was replayed. Both jobs finalized; funding and both transfers are in public-key history.

## Integrity and limits

Four distinct registration/funding/payment IDs are canonical. Two identical complete vault reads prove native custody=reserve=ledger liabilities forAIC/USD/CNY. Original34monitored processes including26physicalnative nodes and946binary remain. Protected6code/ABIpins,consensus,existingbans,bonds andmaintenance remain. No native source/contract upgrade, reset, BP/history restart, vote/bond changes or newserver.

Retained harness issues: old ApiEndpointTest lacked data newly required by broadcast eligibility; healthy fixtures were completed before872PASS. First browser script expected a different existing English sentence; only its own preview was stopped/preserved and the harness corrected. No product translation was changed for that issue. Sources/classes/caches/signedAPKs/failures remain; only2own unsigned/aligned intermediates were removed, freeing5201920bytes. Successful registration/payment/install controllers are complete and must never be rerun.

SoftwareAndroid used syntheticRPC/publictestscalars/inmemorymetadata/encrypted-save-boundary fixtures. No real-phone financial/fullKeystore/fullAndroid/pixel/TPS/indefinite-uptime/net-profit claim.33public deployment artifacts have aSHA256manifest; private keys/passwords/signedpackets stay private.
