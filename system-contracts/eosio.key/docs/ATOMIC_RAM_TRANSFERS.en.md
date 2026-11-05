# Atomic RAM transfers: deployed TEST routes, fees and actual evidence

On2026-11-05,chain`09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532` deployed after fresh5–8 full1800-secondreviews/BPveto checks and separate irreversible executions. KEYWASM`7523bbec364316d06f48283e9cc17cf81fa929bd60b4efd818eadd73e215c788`. Native58/KEY238/SDK26,public55transferchecks/12payments,signup19,4preview/4publicvariants and6receiptpages passed. HistoricalE006/237omittedactualfees/fundedREX; reviews1–4were cancelled and evidence retained.

KEYowner is unsatisfiable; active actually allows sealed eosio.code OR own code, with no dev/BP/external signing keys. Own-code-only prose was inaccurate; a separate correction preserves originalreview bytes and no authority mutation occurred. OrdinaryUSD/CNYowner references eosio.dev. Full bilingual verification lives under actualmachine20261105-public-atomic-key-r1.

## Principal, fees and storage

Recipients receive full principal. Senders pay additional AIC application fees,
including when sending USD/CNY. A receiving key needs neither AIC nor a named
account. It needs AIC only when subsequently sending.

All arithmetic uses integer 0.0001-AIC units:

```
work = ceil(unit_fee × work_units × PowerUp_multiplier / 10000)
RAM = actual gross AIC passed to eosio::buyram, including native buy fee
base = max(0.0100 AIC, work + RAM)
relay = 0 without relay; otherwise ceil(base × signed_rate / 10000)
total = base + relay
retained network fee = base − RAM
```

Relay rates are 10%–30%, rounded upward to an AIC atom with a one-atom minimum.
The multiplier measures on-chain PowerUp CPU/NET market occupancy, not machine
load, and multiplies work only. Work units: deposit 8; key send
`8+ceil(memo_bytes/64)`; native withdrawal `10+ceil(memo_bytes/64)`.

The conservative inverse RAM quote includes the rounded 0.5% native buy fee and
one additional net atom for rounding. The entire quoted gross amount is actually
spent buying RAM. Native `logbuyram` must match KEY as payer/receiver, the exact
payment, and acquired bytes at least equal to the allocation budget. Rounding
margin becomes real additional RAM, not a duplicate fee.

Budgets are conservative allocations, not exact per-row metering: 1536 bytes for
a new key and first balance; 512 for a new token balance or missing native token
recipient row; zero for existing records. A non-native asset's first admission
adds 2048 bytes for metadata/custody, once. Native AIC metadata exists at init.
The contract checks native quota, per-asset budgets, configured capacity and
reserve equality together. Concurrent transactions creating one recipient buy
its RAM only once.

## Named account → key

One transaction, one named-account signature, three root actions:

1. KEY `deposit` declares sender, asset, receiving key, full principal, fee cap
   and request ID.
2. Native AIC `transfer` sends signed fee funding F to KEY, memo `atomic key fee`.
3. The selected token `transfer` sends full principal to KEY, memo
   `atomic key principal`.

Every action explicitly authorizes the same sender `@active`. KEY checks the
entire envelope and actual receipts, buys required RAM, allocates records,
credits full principal, and refunds `F−total` in the same transaction. There is
no persistent prepaid fee credit or separate funding transaction, and no broad
delegation of the sender's active authority to the contract.

F must cover actual fees and stay within signed `max_fee`. SDK
`nativeDepositActions` accepts `feeFunding`, defaulting to `max_fee`. The sender
temporarily needs F; only total is retained. AIC deposits temporarily need
principal plus F, while USD/CNY deposits need their principal plus F AIC. The
named sender pays native CPU/NET and no commercial relay premium.

Standalone legacy `fee` or PK1/PK2 memo transfers reject atomically. The old
native cleos named-deposit shortcut is not adapted to the three-action envelope;
use the candidate SDK and an authorized signing wallet.

## Key → key

The wallet reads the reviewed chain/code, token, resource quote, recipient state
and selected BP rate. The holder signs locally, binding the complete native
transaction, destination, principal, asset, expiration, revocation epoch, request
ID, BP, rate and total cap. Independent devices need no shared nonce allocator.

The relay checks and submits with its own named account/child permission and
native CPU/NET. KEY independently verifies the owner proof and BP eligibility,
debits exact AIC fees and principal, buys real RAM when records are missing, and
credits full principal. Actual external RAM spending leaves reserves once;
retained fees and relay income are accounted separately and emitted in a receipt.

An AIC sender needs principal plus total AIC; a USD/CNY sender needs token
principal plus total AIC. The recipient needs no AIC. Relay submission authority
cannot replace the user's proof, redirect principal or raise its fee cap.

## Key → named account

Owner proof and relay submission follow the same procedure. The named account
must already exist. If its token balance row is absent, KEY buys a 512-byte
budget from this transaction's fees, opens the reviewed AIC/USD/CNY row with KEY
as RAM payer, and transfers full principal. No recipient opening transaction,
signature or prior AIC balance is required. Existing rows buy no more RAM.

The sender pays work and selected relay fees; the receiver pays no CPU/NET for
this transaction. `selfout` additionally supports an explicitly authorizing
native resource payer plus the key owner's proof, without a commercial relay
premium. Work and required RAM fees still apply.

## Actual isolated-chain examples

These are real qualification receipts, **not public-chain transactions or fixed
future prices**. Amounts are AIC; actual market and row state determine charges.

| Case | Gross RAM | Retained base | Relay | Total |
|---|---:|---:|---:|---:|
| First USD named deposit, including asset admission | 0.0056 | 0.0080 | 0 | 0.0136 |
| Existing key's first AIC balance | 0.0010 | 0.0090 | 0 | 0.0100 |
| Existing AIC deposit balance | 0 | 0.0100 | 0 | 0.0100 |
| New receiving key, 30% relay | 0.0025 | 0.0080 | 0.0032 | 0.0137 |

The first USD deposit budgeted 3584 bytes and actually acquired 3683; the new-key
send budgeted 1536 and acquired 1607. Every principal remained whole and every
receipt satisfied `total = RAM + retained_base + relay`.

## Atomic failure and maintenance

Caps, insufficient funds, changed reviewed token/system code, invalid proofs,
replay/expiration, unauthorized internals, capacity and failed buy/open/transfer
roll back principal, fees, curve, quota and ledger records. Qualification also
forced a capacity failure after a queued real RAM purchase and verified rollback.

Income `claim` remains full-value with zero application fee. Its claimant pays
native CPU/NET and opens a missing payout row with its own RAM; this differs from
ordinary transfer admission.

KEY stays nonprivileged, sealed-owner/code-only-active, without direct dev/BP
ledger-edit, freeze or forced-key-change actions. Future maintenance still uses
visible exact-code proposals, complete review and BP vetoes. This is a governance
safeguard, not a mathematical guarantee about any future approved replacement.
Ordinary USD/CNY issuer ownership is not KEY custody control.

Old deposit clients, explorer fee modules, native cleos and released Android APKs
must not be declared compatible merely because some owner-proof wire fields are
unchanged. New chain IDs, code pins, calls, indexing and interfaces still require
their own updates and verification.

## Actual irreversible payments and costs

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

These observed transaction costs are not fixed quotes. Fullprincipal/nativeRAM/retainedwork/relay/caps/rollback were individually checked. OldAPK/legacycleos deposit shortcuts are not qualified here; use the newSDK. Keyledger isK1; signup supports the listedK1/R1/legacy formats.
