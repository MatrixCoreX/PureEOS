# Parallel creation proposals and fresh TEST qualification

Remote Intel Cascadelake shared4vCPU/~8GiB qualification passed on2026-11-05. This is functional qualification, not production security, capacity, TPS or post-quantum qualification.

Node SHA256: `e2724552b19803956292bc738b7a104000e6539c5f9a53c9dce7d1e035a0bde0`. Core commit: `1f883392d3d5d994b48c17ce87ee917e9b44b531`; compiled test overlay: `f6c08f7e05dd8254fd80b3fea297b8ddfc289a3e`. Later changes correct test macros, explicit resource payment and owner signer declarations only.

Qualification6 passed57 assertions:46native governance,7real-contract and4wire checks. Scope includes four independent pending proposals, snapshot/global IDs, individual veto/cancel/execute, review and auction conflicts, exact single-use capabilities, committed initialization, new ordinary short issuers and rollback. Controlled clocks qualify boundaries; public proposals must actually wait1800chain seconds. Stable production remains seven days.

Private canary2 used fresh independent keys and chain identity. It verified genesis/features/system code, actual founder production/finality handover, account-delegated eosio.dev and absence of all four services. A later private continuation submitted four proposals in one transaction and verified the actual HTTP native four-row response, irreversible at block351. It reused signup bytes only for RPC shape and executed no creation. It is not a public proposal or service deployment.

USD/CNY policy3 creates new ordinary issuers: owner delegates eosio.dev@owner, active delegates eosio.dev@active or its own code. Existing accounts and eosio.* cannot be replaced this way. Real tests cover legitimate issue and owner handover; they do not claim that both owner and active were handed over. KEY/signup retain sealed service policies. BP governance is veto-only with no forced user authority/freeze/asset powers; dev permissions expand individually, without a universal privileged executor. Exact visible code review and BP veto remain the upgrade safeguard, not a proof that arbitrary accepted code is benign.

Preserved failures include build2 RPC namespace, the repaired snapshot bound, build3 Boost macros, qualification4 missing native resource payment, qualification5 owner key with an active declaration, and canary1 startup recovery. Final qualification6 passed; earlier failures are not rewritten. Public deployment has separate phase receipts and is not established by this report.

The actual-machine20261105-dev-genesis-r1 evidence folder contains public core/test manifests,57check results, private startup and real HTTP snapshots with SHA256SUMS. Raw state, wallets and secrets remain remote and are excluded from Git.
