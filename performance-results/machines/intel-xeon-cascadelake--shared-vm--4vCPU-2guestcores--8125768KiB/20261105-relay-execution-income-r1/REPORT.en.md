# eosio.key relay execution-income activation and verification

On 2026-11-05, the current TEST chain completed contract activation, application pin updates, public frontend publication and verification. Commercial relay transactions now credit execution fees plus relay fees to the actual submitting BP after excluding actual native RAM spending. Total pricing is unchanged; historical network reserves are not distributed. Earnings of 0.2261 AIC from eight commercial transfers were claimed once in full with zero application claim fee.

## Exact identity and governance

Chain ID: `09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532`. Active KEY WASM: `1092b97ede0d06ab7cc7b54ac0fe5c6a47227e8356e3b7a44877d6a47bd6273d`; previous WASM: `7523bbec364316d06f48283e9cc17cf81fa929bd60b4efd818eadd73e215c788`.

Raw ABI remains `b6db52cd47fdf811bee83030e524de35d0b6f55aece386392b56d1a085df2f83`; JSON ABI remains `f4d64cd7bd84796fab77886fc8308def72b19076a76c2e2bae58857f7bf3b0c5`. Storage and signature wire layouts are unchanged. Exact review/source/artifacts were published first at Git `522485d6734fbb5f9377dc0156c7d2ff9070f7d4`; review SHA-256 `a9560228f74f42684b4b0df73908e8fc3d318330268f79a5604f14bbdab0c414`. The reviewed package and manifest bytes remain immutable.

Native proposal #1 (`2c2d8a7674fb7e961559d0beffbbd28a5eed7b7c0d1fe38074e0f058ccd09ffa`) became irreversible at block 31732. Review ran from 09:43:25.500 to 10:13:25.500 UTC, with 24 effective voters, a 16-veto threshold and zero observed vetoes. Matching execution `b5a4334925216a1ceb068fa4a11e9fb694e4dc45d203ee53a7e2d4d4b26e05bb` became irreversible at block 35304 after 1869.5 chain seconds. Production review remains seven days.

Frontend functional source: `097c1ad18fab190bf91e07ea2bf3397450d2952a`; static release: `/var/www/aic-test/releases/20261105T105459Z-relay-execution-income-r1`. The previous static release remains available.

## Code review and accounting

The internal `credit_fee_bucket` destination flag credits newly earned execution and percentage fees to `relay_earned` for commercial relays. Existing `network` balances stay untouched. `settleram` excludes this transaction's confirmed native RAM purchase before accounting. `charge_key` also covers commercial paths requiring no new RAM, including authorization cancellation. Reserve equations and signed maximum checks remain.

`total = actual_RAM + execution + relay_fee`; `relay_fee = ceil(base × rate)`, with `base = actual_RAM + execution` including any minimum supplement. Commercial relay income equals `total − actual_RAM`. Rates remain 10–30%; the sole enabled founder relay uses 30%. Execution pricing is a public resource quote, not exact measured CPU/NET reimbursement; revenue is not guaranteed profit.

The legacy `feereceipt.retained_base` field still reports execution fees excluding RAM. Interpret its destination using the associated `receipt.relayer` and reviewed code version. Preserve historical receipt meanings. Historical network reserves remain unclaimable: 0.2352 AIC (0.0672 under KEY, 0.1680 under founder). New non-relayed deposits added 0.0684 AIC; total network reserves were 0.3036 before and after claim.

## Three transfer routes and who pays

### Named account to public key

The wallet creates one native transaction with three root actions: `eosio.key::deposit` declares sender, token, full principal, recipient key, request ID and fee cap; `eosio.token::transfer` funds this transaction's AIC fees F with memo `atomic key fee`; the final token transfer delivers the full principal. The named sender signs once.

The contract verifies authority, ordering, actual funds and token identities, then purchases native RAM when new rows are required. Both native RAM payer and receiver are `eosio.key`; the sender's current transaction funds the actual AIC cost. It creates the required rows, credits full principal and returns `F−total` atomically. There is no persistent prepaid fee balance or separate top-up transaction. F defaults to max_fee in the SDK and can be set explicitly: the sender temporarily needs F, while final spending is the actual fee.

The named sender pays native CPU/NET. This route uses no commercial relay, so relay fee is zero and execution fees retain their network-reserve destination. A USD/CNY recipient needs no AIC; the named sender needs AIC in addition to the token principal.

### Public key to public key

The wallet signs locally, binding recipient, full principal/token, expiry, authorization epoch, request ID, actual relay, rate and max_fee. The relay validates the request and submits `ksend` with its named account and native CPU/NET resources.

After owner-proof and relay-eligibility checks, the contract buys RAM atomically when needed. Conservative allocation budgets are 1536 bytes for a new wallet plus balance, 512 for a new token balance, and zero for existing rows. Native purchased bytes may be slightly higher because of integer quotation and allowance. The key sender pays full principal plus AIC fees from its ledger; the recipient receives full principal. USD/CNY is not converted into fee currency: the sending key needs sufficient AIC, while the receiving key does not.

Actual RAM spending goes to the native resource market. Remaining execution and relay fees become the submitting BP's `relay_earned`, redeemable through the existing free claim. Existing rows are not purchased again; a later concurrent transaction sees the earlier committed rows. Signed cap, authorization, funding or resource failure rolls back the complete transaction.

### Public key to named account

The key holder signs `withdraw` with recipient account, token, principal, relay and fee cap. The commercial relay pays native submission CPU/NET. If the recipient lacks a token balance row, the contract supplements its own RAM using the conservative 512-byte budget and opens the recipient row through the supported token contract. Existing recipient rows require no RAM purchase.

The named recipient receives full token principal, and the key sender separately pays AIC fees. Exclude actual RAM spending before crediting execution and relay fees to the submitting BP. A key sending another token still needs AIC. This describes the commercial route; non-commercial `selfout` and related destinations remain unchanged.

## Actual irreversible bills

Fee columns are AIC. Bytes show conservative budget/native purchase. Observed SDK resource multiplier was 21335/10000. These are actual receipts, not permanent quotes; markets, row state and memo size affect later fees.

| Case | Full principal | Total | RAM | Execution | Relay fee | Relay income | Bytes budget/bought | Native transaction |
|---|---|---:|---:|---:|---:|---:|---:|---|
| named-to-key first USD | 100.0000 USD | 0.0208 | 0.0037 | 0.0171 | 0.0000 | 0.0000 | 1536/1597 | [432ce4c2e222…](https://aic-test.matrixai.one/#/transaction/432ce4c2e222d6e3045ed79710e5d0b2f5a1bd47a7dac6d1622782e9c7963622?block=35861) |
| named-to-key first AIC | 20.0000 AIC | 0.0185 | 0.0014 | 0.0171 | 0.0000 | 0.0000 | 512/576 | [81c2dae037c9…](https://aic-test.matrixai.one/#/transaction/81c2dae037c9874934b95c7094cc5fb654c4ca94839f6cf32dbe4f677933fe7c?block=35880) |
| named-to-key first CNY | 100.0000 CNY | 0.0185 | 0.0014 | 0.0171 | 0.0000 | 0.0000 | 512/576 | [a4155df20e49…](https://aic-test.matrixai.one/#/transaction/a4155df20e49db9fef6172d5e30fd58b8274c6a880c7a55a2feb4313094b7055?block=35899) |
| key-to-key AIC new | 3.0000 AIC | 0.0299 | 0.0037 | 0.0193 | 0.0069 | 0.0262 | 1536/1597 | [4417881f2a85…](https://aic-test.matrixai.one/#/transaction/4417881f2a85ffd17bb27b5f9fbc2c82840ad9a3b3390956336d10b2d51533cb?block=35922) |
| key-to-key USD new | 2.0000 USD | 0.0270 | 0.0014 | 0.0193 | 0.0063 | 0.0256 | 512/576 | [e6de44b8fb20…](https://aic-test.matrixai.one/#/transaction/e6de44b8fb2080a641eddc891b79d9d36e474170ca0aa34ebfa49447488a26c2?block=35959) |
| key-to-key CNY new | 2.0000 CNY | 0.0270 | 0.0014 | 0.0193 | 0.0063 | 0.0256 | 512/576 | [7d30df90a87f…](https://aic-test.matrixai.one/#/transaction/7d30df90a87fb88a04f3ba465d9ca1b43298cf75ed1ef82eaf0240b54b2614a5?block=35993) |
| key-to-key AIC existing | 1.0000 AIC | 0.0251 | 0.0000 | 0.0193 | 0.0058 | 0.0251 | 0/0 | [5cc52ecdff06…](https://aic-test.matrixai.one/#/transaction/5cc52ecdff0661246ac6933684f1d8aaf96060b3d2cf116024800612b6ab7486?block=36013) |
| key-to-named AIC unopened | 1.0000 AIC | 0.0324 | 0.0014 | 0.0235 | 0.0075 | 0.0310 | 512/576 | [90dfe0b7af8f…](https://aic-test.matrixai.one/#/transaction/90dfe0b7af8f02154f119b487400e9ef98358a6528f411e090c2a2395a1c234e?block=36037) |
| key-to-named USD unopened | 1.0000 USD | 0.0324 | 0.0014 | 0.0235 | 0.0075 | 0.0310 | 512/576 | [0ba4f4cd3b35…](https://aic-test.matrixai.one/#/transaction/0ba4f4cd3b35c7e6fa2554512956c17f2528c347d64e97f1d3cb3e3fe46243ee?block=36068) |
| key-to-named CNY unopened | 1.0000 CNY | 0.0324 | 0.0014 | 0.0235 | 0.0075 | 0.0310 | 512/576 | [5554f57760bb…](https://aic-test.matrixai.one/#/transaction/5554f57760bb5451deadd661e79bd2b68b331c7885fce5c536ee6a639ca140d9?block=36095) |
| key-to-named USD existing | 0.5000 USD | 0.0306 | 0.0000 | 0.0235 | 0.0071 | 0.0306 | 0/0 | [ed790b1c223f…](https://aic-test.matrixai.one/#/transaction/ed790b1c223f71c2234a0f06d10291936684aac0b18367d8fc9e7bafa3383757?block=36124) |
| named-to-USD-only key | 2.0000 USD | 0.0208 | 0.0037 | 0.0171 | 0.0000 | 0.0000 | 1536/1597 | [3ff39cfe3afe…](https://aic-test.matrixai.one/#/transaction/3ff39cfe3afeff40d4e9ecd9fa1e6742d2156500550c7d716d75b28ec6c0f6a0?block=36150) |

Total fees across 12 transfers were 0.3154 AIC = actual RAM 0.0209 + non-relayed reserve 0.0684 + relay income 0.2261. Each `logbuyram` amount, payer, receiver and purchased bytes matched native quota changes. Both existing-row cases purchased no RAM.

Claim `1eee7ffeb5e7414841fbb732c0e910d01fd6043714fc28981093164bbe9a2881` is irreversible at block 37478. Founder native balance increased exactly from 89734.0860 to 89734.3121 AIC; `relay_earned` changed from 2261 atoms to zero. Application claim fee was 0.0000 AIC; the claimant still pays native CPU/NET. Claim was never resent.

## Validation, retained failures and limits

Remote qualification passed 290 native checks, 26 shipped SDK/relay checks, and 268 frontend checks plus TypeScript/build. Public validation passed 79 checks across 12 transfers: all directions/tokens, new/existing rows, real RAM, full principal, USD sender without AIC rejection, signed zero-cap rollback, rejected legacy standalone fee deposit, HTTP replay returning the same native ID, and full free claim. No successful payment was replayed.

Retained failures: the first native harness assumed JSON traces; a second attempted an intentionally unavailable ABI-decoding RPC. Reviewed offline ABI decoding passed on the same WASM. A legacy test table helper ignored `more/next_key`, briefly reporting truncated reserves; complete pagination and original irreversible claim records corrected verification without another claim. Browser startup first needed retained shared-library paths; a later selector matched a link inside collapsed details. The corrected visible-link check passed; failure evidence remains.

Four preview and four normal public HTTPS Chrome variants passed: zh-dark1440, en-light1440, zh-light390, en-dark390. Each covered 16 owned-key views, all 12 receipt fees, recent transactions, formula/rate controls, and proposal plus actual execution links. Six public asset hashes matched the qualified build in the real browser. Four pre-existing owned demo keys retained exact balances and their 12 historical receipt fees.

All original 25 node PIDs remain (24 BPs plus history), actual native policy 21/15, 24 BP self-votes, 90 ordinary randomized votes covering all BPs with distinct final totals, and bonds unchanged. AIC supply remains 100 million, cap 2.1 billion. Only the founder HTTP relay is enabled; four comparison registrations retain empty endpoints/disabled state. Ordinary activity and indexing continue. The final observer saw retained native `tx_cpu_usage_exceeded` rejection/automatic-restart evidence in the existing ordinary sender. After browser jobs ended, the sender handled its original pending batch and advanced to at least batch 353. The observer did not resend payments or change CPU limits. This is an observed shared-host limitation, not a zero-rejection or capacity pass. KEY/signup owners remain sealed; their actual active authority is sealed eosio.code OR own code, not own-code-only. Dev/BPs gained no privileged authority. USD/CNY retain voluntary dev-account delegation; each supply is 10000 with current cap 1000000. Issuance authorization follows `usd/cny@active → eosio.dev@active → civicgovlead@active`; proposal-only `eosio.dev@upgrade` authority does not replace issuer active authority. The reviewed-token allowlist and hash checks remain; this task does not establish that malicious-token RAM risks are eliminated.

All builds/tests/Chrome ran remotely on the retained shared Intel Cascadelake 4vCPU/8GB VM. No TPS, capacity, production-security, hardware-device or parallel qualification claim. No chain reset, BP pause, server creation, votes/bonds/caps/native-binary changes. Dynamic wallet token discovery/APK work remains with the wallet task as explicitly requested; its dirty worktree was preserved.

## Project cleanup and restoration

51 historical public archive paths representing 27 contents were hash-verified onto the local mounted Windows partition at `/run/media/guagua/Windows/PureEOS-project-cleanup-20261105/public-artifacts`, preserving original paths as symlinks. Measured net Linux space increase: 2285469696 bytes. This partition is on the same NVMe, not an external backup.

Only inactive, ignored generated directory `cdt/build-pureeos` was removed after complete archival and fresh process/tracked-file/per-file checks: 23185 files/links, 2037272576 allocated bytes. Restore archive `cdt-build-pureeos-20261105.tar.gz`: 727965939 bytes, SHA-256 `2b2851c9c371d2732e820702bf22422b14b968e80f973e04482be3dad257f392`. Combined measured net increase: approximately 4.03 GiB. Free Linux space at cleanup completion: 23.58 GiB; unrelated background activity can change this. Sources, Git, active work, signing material and chain/history data remain. Cleanup manifests record exact restoration paths and hashes.
