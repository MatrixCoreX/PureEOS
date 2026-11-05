# Atomic RAM transfers: candidate, fees and qualification

2026-11-05 candidate WASM: `e006128f25fcda3f2395302212247b436b550d8ffde0f7da3064b8cec624a442`.
Remote isolated qualification passed 237 native checks and 26 SDK tests. **Public
TEST deployment is still pending** native qualification and the complete
1800-second reviewed proposal/BP-veto process. This document supersedes normal
transfer instructions requiring prepaid fee credit, manual sponsored capacity
or manual USD recipient opening in this branch; historical evidence stays intact.

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
