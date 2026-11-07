# Demand-funded global key-account growth: actual activation and fees

Proposal4 upgraded eosio.key irreversibly after 3803.0 chain seconds (minimum1800), block267777, transaction [25009c181bfa](https://aic-test.matrixai.one/#/transaction/25009c181bfade71bbcdb47c8b8d42b4b6217f542a2201a23742da83d9e265df?block=267777). Exact source/artifacts/review were published before proposal; review SHA2561152e6be7cfbf341647c6cc6578d083fb03677dddce55ff916662a095b93b430. Code `7db295ea153410eed1a1810e6122c594d6a904117db232381cf713d1ab8d8416`, raw ABI `83b0c49b82492d50ca72fd34e66cc879ba150fc7630ac780e372dcfaf2f4244c`. Existing config/action/ledger layouts remain; only growth is added. No re-init, scans or reset. Legacy1000/4000 fields no longer gate allocation. Physical RAM, native resources and checked integer/ID bounds remain finite.

## Three routes and fees

Real RAM purchases are included in additional sender-paid AIC fees, independently of full principal. New wallet+balance budget1536 bytes, new asset balance512, first supported asset metadata another2048. Existing records buy no RAM again. These are conservative allocation budgets, not exact per-row memory metering. The RAM receipt is the actual market purchase including native buy fee and rounding; acquired quota is checked.

### Named account to public key

One native transaction combines the deposit declaration, bounded AIC fee funding and principal transfer. The named sender signs and pays native CPU/NET. KEY buys RAM if needed, credits full principal and refunds unused fee funding immediately; no reusable prepaid credit is required. The receiver needs neither a named account nor AIC. This direct route has no commercial relay markup; its non-RAM execution charge retains the existing KEY network-bucket destination.

### Public key to public key

The holder signs the complete intent, receiver, principal, asset, fee cap and chosen BP rate locally. The relay supplies native resources and its scoped authorization. KEY verifies the holder, charges additional AIC from the sending key, buys any missing storage and credits full principal to the receiving key. Receiving USD/CNY requires no prior AIC; sending them requires AIC fees.

### Public key to named account

Signing, relay submission and AIC fees follow the preceding route; the named receiver must exist. If its native token row is missing, KEY buys the512-byte budget and opens it within the same transaction, then transfers full principal. Existing rows buy no RAM again. Registration is a separate atomic Signup bridge: this run's0.2500 AIC pays0.1000 owned NET+0.1000 owned CPU and remaining RAM, without Signup markup, plus the normal KEY fee. Any late failure rolls the payment and creation back.

For commercial relays, total fee equals actual RAM spending plus the non-RAM execution item plus relay fee. The10%–30% relay rate applies to base (including RAM and any minimum adjustment), rounded upward; the sole HTTP relay here uses30%. Everything except actual RAM becomes that BP's earned balance. Execution pricing is not a per-transaction reimbursement of measured CPU/NET. Claim application fee is zero; BP still supplies native resources. A payout token row must exist or be opened with claimant-paid RAM first. Historical network reserves do not become relay income. Any signed-cap violation or later failure rolls back the whole transaction; principal is never reduced and caps are never silently raised.

## Actual new receipts

All fee amounts are AIC. Bytes mean allocation budget/actual acquired quota.

| Case | Total | RAM | Execution | Relay | Bytes | Irreversible transaction |
| --- | --- | --- | --- | --- | --- | --- |
| named-to-key AIC | 0.0217 AIC | 0.0038 AIC | 0.0179 AIC | 0.0000 AIC | 1536 / 1608 | [9f94d4d03dfa](https://aic-test.matrixai.one/#/transaction/9f94d4d03dfa611baa6c3abaf7822a739dc48dbb7a7147780f0e15744b448d40?block=268332) |
| named-to-key USD | 0.0193 AIC | 0.0014 AIC | 0.0179 AIC | 0.0000 AIC | 512 / 565 | [dd5e66618fc6](https://aic-test.matrixai.one/#/transaction/dd5e66618fc62f237f43a7df679518dec1391b15645fc71ca73f8ef25295577c?block=268858) |
| named-to-key CNY | 0.0193 AIC | 0.0014 AIC | 0.0179 AIC | 0.0000 AIC | 512 / 565 | [de8d6f755a49](https://aic-test.matrixai.one/#/transaction/de8d6f755a49e484a269e630a0232bf4b0b2db1d315dbe3d09c3db5905c73352?block=268881) |
| key-to-key AIC | 0.0311 AIC | 0.0038 AIC | 0.0201 AIC | 0.0072 AIC | 1536 / 1608 | [58fa742ad924](https://aic-test.matrixai.one/#/transaction/58fa742ad9241f535435c9e98b3d8a537c4828c49b9cecb239733680b590f937?block=268906) |
| key-to-key USD | 0.0280 AIC | 0.0014 AIC | 0.0201 AIC | 0.0065 AIC | 512 / 565 | [6eadf184191c](https://aic-test.matrixai.one/#/transaction/6eadf184191c4e1688fe8e8730a4bbb19da74099ae5051193de0d5cf2ddd1650?block=268938) |
| key-to-key CNY | 0.0280 AIC | 0.0014 AIC | 0.0201 AIC | 0.0065 AIC | 512 / 565 | [1702ada9ad63](https://aic-test.matrixai.one/#/transaction/1702ada9ad633900adfbe7d73fad2b3e70abaf9ca21d34c1b8af4c154ddb19f2?block=268967) |
| atomic KEY-to-Signup | 0.0349 AIC | 0.0000 AIC | 0.0268 AIC | 0.0081 AIC | 0 / 0 | [c943ca6d5de3](https://aic-test.matrixai.one/#/transaction/c943ca6d5de317dc587117df7670c5cd3de5ff4a82e9e8b7de7711117bc5ce54?block=268997) |
| key-to-named AIC | 0.0338 AIC | 0.0014 AIC | 0.0246 AIC | 0.0078 AIC | 512 / 565 | [2324dfd40619](https://aic-test.matrixai.one/#/transaction/2324dfd406199ff4a2bd9f2397a1c3e9cb9b5471a0e4ad66c165891b184d3f83?block=269027) |
| key-to-named USD | 0.0338 AIC | 0.0014 AIC | 0.0246 AIC | 0.0078 AIC | 512 / 565 | [d28d386dd200](https://aic-test.matrixai.one/#/transaction/d28d386dd2008ee33c2648ba60df188d23732d0479ba3bd39902d2b34b90e1a2?block=269055) |
| key-to-named CNY | 0.0338 AIC | 0.0014 AIC | 0.0246 AIC | 0.0078 AIC | 512 / 565 | [48f85abf0bd6](https://aic-test.matrixai.one/#/transaction/48f85abf0bd6f34d50b61e01be9c8a995536bb3997a0e9068837cb4244651812?block=269085) |
| named-to-USD-only key | 0.0217 AIC | 0.0038 AIC | 0.0179 AIC | 0.0000 AIC | 1536 / 1608 | [d80339c8c4d6](https://aic-test.matrixai.one/#/transaction/d80339c8c4d6f1e97ce2994b0257de46b5d14cc9ad10c45ff4a1001370bbdc92?block=269111) |

Only this run's new commercial income, 0.2126 AIC, was claimed, [4611dd71c1ee](https://aic-test.matrixai.one/#/transaction/4611dd71c1ee280a56ce6b26da0cbf3b89f45de763c39fb4410e11171e5823b3?block=269124). Prior earned/network reserves and old payments/claims were preserved. New account`keygrowcheck` has the fresh owned key as Owner/Active, without dev references. Named and public-key balances remain distinct.

## Evidence and limits

355 real prior-WASM funded migration/full-route checks;21 separate TEST ONLY seeded boundary checks;28 SDK checks. Seeds prove arithmetic and real purchase rollback, not billions of physical accounts/TiB or TPS. Frontend269 unique cases passed across268 original passes plus the unchanged15-second timeout case separately passing under45seconds; no single full269-green claim. TypeScript/build passed; original256MiB heap failure and scheduling corrections remain. Four real preview and normal-public HTTPS variants before/after activation, exact deployed digests and manual screenshot review passed. Historical broad get_block400 evidence remains; this is focused component acceptance rather than full explorer health.

Wallet test.16 `db6d8f1f6b47f14814dd52dbfe56172564603e03c56b46d0df5e0cb1d3ca2c10` retains the installed QR r58 baseline, original certificate,732 core assertions,32 API26 helpers and exact non-Network compiled classes. No phones were overlaid during the other active physical campaign. This compatible APK is delivered separately; old pins safely demand an update. This is not full Android/Keystore/camera/IME acceptance or a repair of held unrelated bugs.

The first payment hit the native CPU cap. Only its original signed ID was retried after418 consecutive irreversible-block absence and unchanged allocation-state checks; it executed once in268332, distinct from response hint268331. Initial waiter timeout and errors remain; recovery skipped the already successful payment. Its ephemeral original quote/before-bucket values were not durably saved, so no independent first-case historical bucket delta/quote equality is claimed; principal, actual fees/RAM and solvency were checked.

New owned11 payments+one new-income claim passed, including three assets/routes, atomic Signup, actual RAM, fee destinations, full principal and exact reserves. 26 existing native processes, 13 monitored authorities, consensus/resources/21 seats, other code and all token supplies/caps stayed intact. The original ordinary sender's automatic restarts were observed without intervention/replay; no zero-rejection, capacity, production or external-audit claim. Signup/native/allowlist/fee formulas were unchanged. No reset, BP stop, new server, TPS or user-authority change.

Cleanup removed only four inactive regenerated unsigned/aligned APK intermediates:10,280,960 allocated bytes (~9.80MiB). Signed APKs, source, evidence, keys and chain data remain. Concurrent filesystem net change includes other activity and is not wholly attributed to this cleanup.
