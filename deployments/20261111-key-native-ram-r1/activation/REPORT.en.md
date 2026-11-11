# eosio.key native sender RAM upgrade: activation and three transfer flows

[中文](REPORT.zh-CN.md)

## Active TEST deployment

Proposal5 executed irreversibly at block **686081**, transaction `3a8bfd814b1d12d99f7f8196b9258c019d82b48531f89c63838017d0380a8a4b`, on2026-11-11. The existing1800-chain-second review completed; execution observed22 effective voters,15 required vetoes and0 vetoes. Actual proposal-to-execution interval: 3137.0 chain seconds. No direct setcode or chain reset.

ChainID: `09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532`.
Current KEY code: `cdfe0f44383a356cba98b71695fca8734424e1a19a80f3a5c0d48e7cb3cbfb43`. RawABI `83b0c49b82492d50ca72fd34e66cc879ba150fc7630ac780e372dcfaf2f4244c` is byte-identical to7db. Immutable review `b450981280c88b64fa9ed586ed35d88379ff1c768eb70a94fecd329534af85e1` remains unchanged, published with source commit `a11568207dc24b65a401d67dd567d26fae26be22`.

Both Xiaomi phones already received original-certificate wallettest29 during review: APK `9dc9a777589c4635c79da68a68cd3faf4ae68933b94abe3169bbd5794210b6fb`. UID/data were preserved; completed installation was not repeated. Relay and derived key-history compatibility are active. Browser compatibility is published and passed four normal-HTTPS variants plus six asset digests. Signup was not upgraded: code remains `3c03d8671479cb204e5c4ae31ae0563883882417cf88f8c3c477bd7e1e1a5791`.

## 1. Named account → public key

The sender signs exactly two top-level actions in one transaction: eosio.key::deposit with sender@active and max_fee0, then the exact token principal transfer with sender@active and memo `atomic key principal`. There is no fee-funding action or prepaid fee account.

The deposit frame explicitly authorizes and bills the sender for missing persistent wallet, secondary-index and balance rows. CPU/NET use that sender's native resources. Existing rows keep their original RAM payer and require no repeated allocation. An internal pending lock prevents spending preliminary credit until the exact next-root principal actually arrives. Any mismatch or insufficient sender RAM rolls back principal, ledger and allocations atomically.

Application execution, minimum, relay and RAM-purchase charges are all **zero**. No automatic RAM purchase occurs. New rows still consume the sender's existing RAM quota; a zero RAM fee receipt does not mean zero physical storage. This contract does not automatically move a row's payer to its beneficiary after a later spend or release the original donor's RAM then.

Native USD/CNY deposits require no extra AIC fee balance. Tokens must already pass existing audited-token/custody admission; a sender does not silently onboard an issuer. Old paid three-root deposit packets are rejected. Wallet29 directly uses the new two-root format.

Four public deposits covered USD, CNY, AIC and repeated AIC. Full principal and zero fee receipts were verified, with gptgenesisbp as actual wallet/balance payer. Isolated native tests additionally covered a USD sender holding zero AIC, real insufficient-RAM rollback and reuse of existing rows with nearly exhausted sender RAM.

## 2. Public key → public key

The owner locally signs the recipient/token/principal, complete transaction/expiry, chosen relay/rate and maximum total fee. The relay supplies its native CPU/NET; the contract independently verifies owner proof, transaction binding, eligibility and cap. Principal is fully credited; the sender separately pays AIC fees, including when sending USD/CNY.

Missing recipient wallet/token rows use the existing conservative RAM budget and actual eosio.key RAM purchase from this transfer's fees. Existing rows have no repeated creation charge, but execution/minimum and relay charges remain. A failed purchase or settlement rolls everything back.

Actual RAM spending goes to the system RAM market. Newly collected execution income and relay service fee after RAM spending accrue to the selected relay's claimable earnings. Execution pricing follows reviewed on-chain PowerUp market occupancy; it is not reimbursement of that relay's exact CPU/NET cash cost.

The new-recipient0.0001AIC transfer cost0.0335AIC:0.0037RAM purchase,0.0220execution income,0.0078relay fee. A1536-byte budget bought1585bytes;0.0298AIC accrues to relay earnings. This is a dynamic example, not a fixed tariff.

## 3. Public key → named account

The owner signs withdrawal proof and a transaction-bound request; the relay provides native CPU/NET. The contract checks proof/relay/rate/cap, debits principal plus separate AIC fees and transfers full principal into the named recipient's native token balance.

An existing recipient token row requires no repeated RAM creation. A missing row retains the original public-key withdrawal mechanism: charge the RAM item, actually acquire RAM and open the required record. This upgrade does not change that path. Execution income and relay service fee after RAM go to relay earnings; USD/CNY senders still need AIC fees.

The test recipient already had its AIC row. Its0.0001AIC principal arrived in full; RAM charge was0, execution income0.0268 and relay fee0.0081, total0.0349AIC.

| Tested route | Total | Actual RAM purchase | Execution income: retained_base | Relay service fee |
|---|---:|---:|---:|---:|
| key-to-key | 0.0335 AIC | 0.0037 AIC | 0.0220 AIC | 0.0078 AIC |
| key-to-named | 0.0349 AIC | 0.0000 AIC | 0.0268 AIC | 0.0081 AIC |

No historical claim was replayed; claims are unchanged. The legacy retained_base field name remains ABI-compatible and accrues to relay income on these commercial routes.

## Qualification and limits

- Local production-runtime contract qualification:191 checks; SDK30. Includes funded7db migration, payer preservation, actual RAM billing, real insufficient RAM/rollback and public-route/proof/replay/hot-key regressions.
- Wallet: SDK36,858 core checks,39 API26 helpers,58 unaffected compiled classes byte-identical to28,42 actual Android35 software UI/Handler cases. Synthetic RPC/public test scalars/in-memory metadata/encrypted-save-boundary fixtures; no real-phone payment, complete Keystore or full-Android acceptance claim.
- Browser:270 regressions,TypeScript/build,4preview+4normal-public variants and all six asset hashes.
- Seven canonical receipts cover upgrade plus six payments. All six payments appear in typed public-key history. Two identical complete vault reads verified native custody=reserve=all liabilities: AIC30367403,USD32010990,CNY31979990 atoms.
- External maintenance during review restarted nodes and replaced four banned BPs. After the user confirmed completion, original evidence was preserved and a separate audited26-native-process baseline established. This upgrade did not restart BPs; final processes match that new baseline. Original pre-maintenance PIDs were not claimed unchanged.
- Temporary governance/QA signers are closed. Protected system/token/Signup/issuer bytes, monitored KEY/Signup authorities, supply and relay registry remain unchanged. No consensus, veto, admission, account-authority or user-private-key changes.

Retained failures: after the first two successful payments, an HTTP500 occurred during a read before the third signature. Original successes were canonically reconciled and only the remaining four cases continued. Final audit v1 misclassified journalctl --grep exit1 for no matches; full JSON kernel-log inspection confirmed no newOOM. Neither issue caused a contract change. Earlier build/fixture failures and uninstalled candidates remain preserved. No TPS, indefinite availability or unexecuted-test claims.

## Evidence

`final-public-evidence/MANIFEST.json` binds42 public deployment/test/browser artifacts. `PUBLIC_FEE_DETAILS.json` records actual public-history fee receipts and verifies total=RAM+execution+relay. Signed packets, private keys and passwords remain in private case directories outsideGit. Governance, payment and phone-install controllers are complete and must never be replayed.
