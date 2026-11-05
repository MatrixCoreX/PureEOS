# Actual TEST deployment and verification

Status: **all4deployments irreversible; service/public/fees/authorities/90balances PASS**.

Chain ID `09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532`, root/opt/pureeos-devnet-20261105-r1.24testBPs+1nonproducinghistory=25node processes on one retained sharedVM. AI/region-institution names are test identities, not corporate operators; founder remainsgptgenesisbp. Actual native21members/15votes differs from each creation proposal's24frozen effectiveBPs/16veto threshold.

## Parallel review and separate execution

Original1–4were cancelled. Exact new5–8were published first atGit7665192df, manifestSHA256`295c33bd6f88620d0015586b2214bfba43a5cd246d759defaffe1d7811e2733a`, then irreversibly submitted at11818 by09f0eb17fd5be203b25f4b0b80c5552dc46178780096ff51385cbad9e092fec4. Each waited full1800chain seconds through2026-11-05T07:19:55UTC and remained below its effective veto threshold. Each executed separately:

| 账号 / Account | WASM SHA256 | 不可逆块 / LIB block | 规范交易 / Canonical transaction |
|---|---|---:|---|
| eosio.key | `7523bbec364316d06f48283e9cc17cf81fa929bd60b4efd818eadd73e215c788` | 15463 | [552759846b93…](https://aic-test.matrixai.one/#/transaction/552759846b93ef9637ea74be7d52adaffeb8e145be65667c2d02b1f213da097b?block=15463) |
| usd | `ec9e8326c1fef0b561b2b5c45ad5c05628bd172c2afdedfd6f24fa34178354bb` | 16729 | [13b07919eaf0…](https://aic-test.matrixai.one/#/transaction/13b07919eaf0a4458e29ace41b85c2206cbc3dfb59c0ec69c6c8f9b5ca9fa590?block=16729) |
| cny | `ec9e8326c1fef0b561b2b5c45ad5c05628bd172c2afdedfd6f24fa34178354bb` | 16736 | [197b53594303…](https://aic-test.matrixai.one/#/transaction/197b5359430314955245bcd80e535eada8fba37884b096a976f22837d778feec?block=16736) |
| eosio.signup | `996d520ce32c01da431334fc7bf1f03755f4c82f7ae73ddf8b9d931054ae35f3` | 16754 | [0a73444a84ca…](https://aic-test.matrixai.one/#/transaction/0a73444a84caa03bf7fecc4bc32f1ffa42c6fbd26fb4ba1179ce849b2bea492f?block=16754) |

Native creation counter8/pending0; current-chain browser history has4cancelled/4actualexecutions, without mixing archived-chain proposals.

## Authorities and assets

| Role | Actual authority and boundary |
|---|---|
| eosio.dev | Nonprivileged; owner→civicgovlead@owner, active→civicgovlead@active, upgrade→civicgovauth@active. No direct keys/waits. Future multisig must retain nonweaker parent authorities. |
| eosio.key / eosio.signup | Nonprivileged; unsatisfiable owner. Active is sealed eosio@eosio.code OR own code, with no dev/BP/external signing keys. Original own-code-only prose was inaccurate; a separate correction preserves original review bytes. |
| usd / cny | Ordinary nonprivileged issuers, outside the protected list; owner→eosio.dev@owner, active→eosio.dev@active OR own code. Legitimate owner management covers only those accounts. |
| BPs | Own signing/production/votes/rewards and governance veto. No privileged flag, custody management keys or forced-other-user authority/asset admin interface. |
| eosio.prods / wrap / msig / null | Unsatisfiable owner/active; the former BP-multisig administrative path is disabled. |

Code authority is not an importable private key. eosio has sealed owner and code-only active; reviewed system code retains system capabilities. Visible code review and veto are safeguards, not proof that arbitrary future accepted code is harmless. No generic super-executor/system-authority rotation feature was added. Ordinary legitimate holders retain voluntary own-authority changes. USD/CNY are third-party simulations without fiat-reserve claims.

AICmax2100000000.0000 and issued100000000.0000 remain unchanged. OrdinaryUSD/CNY each max1000000.0000 and initial10000.0000. No additionalAIC issuance or forced user-asset mutation.

## Three routes and actual fees

Recipients receive full principal; senders pay additionalAIC. KEY buys missing-record RAM inside the same atomic transaction. Existing rows buy no RAM. Normal transfers need no persistent prepaidfee, manualregtoken/open or operator waterline top-up. These are observed transaction costs, not permanent quotes.

| Scenario | Full principal | Total AIC | Actual RAM | Retained work | Relay | Budget/bought bytes | Canonical receipt |
|---|---|---:|---:|---:|---:|---:|---|
| named-to-key first USD | 100.0000 USD | 0.0251 | 0.0083 | 0.0168 | 0.0000 | 3584/3637 | [c381c9f1b1e8…](https://aic-test.matrixai.one/#/transaction/c381c9f1b1e8da6694727a94a063620c42b28e4eb629d2f0521fee7d8d4e99c4?block=17038) |
| named-to-key first AIC | 20.0000 AIC | 0.0182 | 0.0014 | 0.0168 | 0.0000 | 512/576 | [ee1ef967d8b1…](https://aic-test.matrixai.one/#/transaction/ee1ef967d8b14c92c929416e0188b7540d60412fcacd131186837035df093361?block=17058) |
| named-to-key first CNY | 100.0000 CNY | 0.0228 | 0.0060 | 0.0168 | 0.0000 | 2560/2617 | [cbd04baa5ebd…](https://aic-test.matrixai.one/#/transaction/cbd04baa5ebd4416ddc43e1a2f16d81a4055882149fc20bd6327d4441f8cf1ea?block=17086) |
| key-to-key AIC new | 3.0000 AIC | 0.0294 | 0.0037 | 0.0189 | 0.0068 | 1536/1596 | [82495ef2b3bf…](https://aic-test.matrixai.one/#/transaction/82495ef2b3bfbc7832d9a9289eb679119631670f0350eee5285f7d690261d2c5?block=17099) |
| key-to-key USD new | 2.0000 USD | 0.0264 | 0.0014 | 0.0189 | 0.0061 | 512/576 | [c6403622508b…](https://aic-test.matrixai.one/#/transaction/c6403622508b3ef8c3c5224cc5d1c5a693366f34fbb2701a2f8cfce23ee33d55?block=17124) |
| key-to-key CNY new | 2.0000 CNY | 0.0264 | 0.0014 | 0.0189 | 0.0061 | 512/576 | [e2d91953fc14…](https://aic-test.matrixai.one/#/transaction/e2d91953fc146231a54f524425b2e06b20f9b4e249969c0459a971324bbe062c?block=17148) |
| key-to-key AIC existing | 1.0000 AIC | 0.0246 | 0.0000 | 0.0189 | 0.0057 | 0/0 | [891aee2216b1…](https://aic-test.matrixai.one/#/transaction/891aee2216b1bcef12f2c2ad6a2327fc5c8b54f13db86576a6d5ec9d00ee80c2?block=17197) |
| key-to-named AIC unopened | 1.0000 AIC | 0.0319 | 0.0014 | 0.0231 | 0.0074 | 512/576 | [ec052f92feb1…](https://aic-test.matrixai.one/#/transaction/ec052f92feb16356fac2faca0c3ba004e000f252a355050a32e655d1a2d00d3a?block=17229) |
| key-to-named USD unopened | 1.0000 USD | 0.0319 | 0.0014 | 0.0231 | 0.0074 | 512/576 | [3fdb6932b173…](https://aic-test.matrixai.one/#/transaction/3fdb6932b1737da209fc2940af414ea02e86af3e7c97894645155e1bcaf7f29c?block=17247) |
| key-to-named CNY unopened | 1.0000 CNY | 0.0319 | 0.0014 | 0.0231 | 0.0074 | 512/576 | [7072af411aad…](https://aic-test.matrixai.one/#/transaction/7072af411aade0fba37900052a71208238725392cd12bb5167fe2ccd7f250b38?block=17271) |
| key-to-named USD existing | 0.5000 USD | 0.0301 | 0.0000 | 0.0231 | 0.0070 | 0/0 | [6e977f65d39b…](https://aic-test.matrixai.one/#/transaction/6e977f65d39b51864fee96f9516e8be403d93ec8e283d730eedcc3402af188c6?block=17294) |
| named-to-USD-only key | 2.0000 USD | 0.0205 | 0.0037 | 0.0168 | 0.0000 | 1536/1596 | [2fc70567bd11…](https://aic-test.matrixai.one/#/transaction/2fc70567bd11fd8b24bdd96823acfc178314aa1f630eeeb3f363f9089bfd795b?block=17320) |

Across12payments: total0.3192AIC = actualRAM0.0301 + retainedwork0.2352 + relay0.0539; budget12288bytes, actually bought12902bytes.

- Named→key: one signature covers3roots:deposit declaration, AICfee fundingF, fullprincipaltransfer. RefundF−actualfee inside the same transaction. For100USD,F=1AIC,total0.0251,refund0.9749. No persistent fee credit/relay premium; nativeCPU/NET is the named sender's responsibility.
- Key→key: user signs owner proof/wholetransaction locally; sole founder relay submits under a dedicated child permission and pays nativeCPU/NET. Senderkey pays additionalAIC.3AIC to newB debits3+0.0294;B gets3. RAM0.0037,retained0.0189,relay0.0068.
- Key→named: same relay/resource model; named recipient must already exist. KEY automatically opens a missing token row as its RAMpayer.1AIC example debits1+0.0319;recipient gets1, withRAM0.0014/retained0.0231/relay0.0074. Recipient need not manuallyopen.

RAMcharge equals actual gross nativebuyram spending, including upward-rounded0.5%buyfee and one net-atom margin, which also purchasesRAM.1536/512/0 are conservative budgets, not exact consumed-row metering. First non-AIC admission adds2048bytes. CPU/NET multiplier applies only towork; basefloor0.0100AIC precedes relay. Market/epoch/memo change quotes. AUSD/CNYsender withoutAIC is atomically rejected; receivers need no priorAIC.

Onlygptgenesisbp30%HTTP is active. Four10/15/20/25%comparison rows have empty endpoints/disabledstate, no additional hotkeys/permissions/HTTP. Signedrate/cap constrain fees. Full earned relay claim0.0539AIC paid applicationfee0; claimant still funds native resources/necessary recipient tokenopen.

## Validation and limits

- Remote native58(46governance/8real-contract/4wire), KEY238native and finalSDK26 passed with actualfee WASM/fundedREX at globaldepth4.
- Full backend33 and frontend266/TypeScript/production build passed; all6 published artifacts match normal publicChrome SHA256.
- Public transfer55checks/12successful payments and signup19checks/3actual K1/legacyEOS/splitK1owner-R1active registrations/signatures passed, including full free earned claim, missingAIC, signed cap, legacyfee rejection, HTTP exact-ID replay and rollback.
- Each preview/public phase had4actualChrome variants ×4owned-key pages =16views, match all12canonical payments/balances/fees;6desktop/mobile3route receipt pages verify RAM/retained/relay/total individually.
- Current25nodes,24self-bonds/self-votes and90ordinary random votes covering everyBP with pairwise-distinct totals; actual21/15 finality. All90actual liquid balances match the journal including one proven external0.0001AIC credit.
- Shared Cascadelake4guestvCPUs/2visibleguestcores/8125768KiB functional/security checks only; no TPS, production capacity, parallel whitelist, physical-device or post-quantum activation qualification.

NewSDK/defaultABI is adapted. OldAPK/legacycleos named-deposit shortcuts have no compatibility qualification for this version; use the newSDK three-root builder. Public-keyledger is currentlyK1; actual public signup coveredK1/legacyEOS/splitK1owner-R1active. This is not post-quantum-chain activation.

## Failures and recovery

1. The first execution of new-chain reviews1–4 failed atomically on the sysstage→RAMfees→REX path at depth4. All four were cancelled irreversibly without editing reviewed bytes. Historical57/237fixtures omitted the actual fee/REX route. The consumed-stage ordinal and extra KEY self-payram hop were repaired and fully requalified without raising depth4.
2. Fresh5–8 each waited another full1800chain seconds. KEY had4explicitCPU rejections; the same immutable ID later became canonical at15463 with9758us CPU, after the original verification waiter timed out. Positive-receipt recovery retained it without resending KEY or changing150000us. Three other executions were separate; only the client verification window became600seconds.
3. Authority auditing corrected inaccurate own-code-only prose. Fixed native policies1/2 retain sealed eosio.code OR own code, without external/dev/BP signing keys. No actual authority mutation occurred; separate bilingual corrections leave the original manifest/review hashes intact.
4. The first final browser harness waited on a hidden link inside collapsed details. Selecting the visible canonical link fixed the test; original failure logs remain and no product data was altered.
5. A proposal resource marker credited civicuserone, inside the90actor workload, by0.0001AIC. The closed journal missed that external credit and periodic audits failed. Canonical11818proof authorized only derived-journal reconciliation, preserving the initial total plus a separate external-credit field. Only that owned sender was briefly paused/resumed; a following real batch and all90balances passed. No new payment/native balance/vote/bond change occurred. Future marker recipients lie outside the90actor group.

## Fixed reset workflow and retention

Pin inputs/qualifiedcaches→drain/archiveold→freshcore→roles/founderhandover→BPselfvotes/90randomvotes→history→publish4exactreviewsbefore signing→fullreview→4separateexecutions/canonicalrecovery→allroutes/assets/signup/publicchecks→secret-scanned evidence/Git. Reuse matching artifacts, never shorten review/admission/finality/ban windows. Future resets need fresh directories/keys/ChainID/reviews, never replay current payment controllers. No arbitrary generic-plan end-to-end qualification is claimed.

Old116ea0c3rawblocks/state/SHIP/indices/Germanyban/oldunanimouslyvetoedproposal3remain separate; its26services remain disabled. Cancelled1–4belong to new09chain. No excluded identity revival/newserver/TPS. Allbuilds/tests/Chrome were remote.

89public files were byte-verified offserver before deleting only stopped privatePASSfixture/opt/ka-attempt-15(9859072allocatedbytes). Sources/verifiedobjects/artifacts/failures/oldrawchain remain. Temporarypreview and founderkeosd stopped; ordinarywallet/business/soleHTTP/history/current25nodePIDs remain. No privatekeys/passwords/signedpackets/rawstates in public evidence.

## Provenance

Native42dda604af169845b9ea9b9835fe0d25ecc126a8,ELF946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397. KEYsourcee1847d0d9/signupaeb53ba/custody.token sources accompany exactreviewed WASM/ABI/init. Frontendsourcec3d9809/archive67259a06665729929537042343fbc162e26b3478a4b0105ec4e5373d2ade0af9,release/var/www/aic-test/releases/20261105T075357Z-atomic-ram-r3;backendfunctional sourcea4f11a8. Stable release tags/assets unchanged.

Core review points:apply_context::exec recognizes consumed sysstage ordinal;system_account_proposal.cpp validates per-ID review/single-usecapability/finalseal. KEYbegin_payment/payment_notification queue schedule_ram directly;settleram validates actualquota/funding/caps before settlement. Attached evidence/MANIFEST binds each actual result.
