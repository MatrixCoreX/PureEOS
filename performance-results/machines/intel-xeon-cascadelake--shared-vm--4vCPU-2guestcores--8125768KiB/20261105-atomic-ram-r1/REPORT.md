# Atomic RAM candidate qualification / 自动购 RAM 候选验证

Status: **REMOTE QUALIFICATION PASSED; PUBLIC DEPLOYMENT PENDING** (2026-11-05).
Candidate WASM `e006128f25fcda3f2395302212247b436b550d8ffde0f7da3064b8cec624a442`,
JSON ABI `f4d64cd7bd84796fab77886fc8308def72b19076a76c2e2bae58857f7bf3b0c5`.

The retained shared Intel Cascadelake VM has 4 guest vCPUs, 2 visible guest cores and
8125768 KiB RAM. These are isolated functional/security checks, **not throughput or capacity qualification**.
No server was added and no public node, chain, contract, vote, bond, ban or resource cap changed for this qualification.
The old public chain remains116ea0c3 with KEYbc3358d7. Existing public source/release defaults are historical and unchanged.

Attempt14 passed **237 native checks and 26 SDK tests** using the unchanged qualified native0be5282d TEST runtime,
real pinned systemb591ca66 and tokenf8c5fbfb, and real native RAM purchases. Sources and exact receipt/result hashes
are in the attached public evidence. Temporary keys and raw fixture state remain remote only and are not included.

Covered all three routes for AIC/USD/CNY; full principal; zero prior manual fee credit or token RAM sponsorship;
first/non-first asset and balance allocation; actual gross native buy cost/acquired quota; no duplicate buys;
AIC sender-fee requirements; auto native recipient opening; fee caps and wrong issuer; independently signed
concurrent requests and replay/envelope tampering; unauthorized internal calls; all five relay rates;
fee-free full-value claims; and whole-transaction rollback after a queued RAM purchase at capacity.

USD/CNY use the same compiled token code EC9e8326..., with JSON ABI9846bb35.... This qualification used
sealed private issuer fixtures; the user's LATEST ordinary-issuer delegation policy requires separate native
creation tests. Same token bytes do not by themselves qualify new account ownership or public deployment.

Preserved attempts1–13 include staging-placeholder failure, overlong UNIX socket path, measured insufficient
bootstrap quota, rejected inline sender-auth forwarding, fixture initialization/owner-auth order, notification
trace double-counting and JSON-quoted packed-hex test construction. These were not erased or claimed as passes.
The final design uses three explicit sender-signed roots rather than forwarding arbitrary user active authority.

Small bootstrap quota covers code/init/scratch, not a recipient pool. Runtime automatically buys RAM from each
transaction requiring records. Conservative budgets are not exact per-row native metering. Existing rows buy no
RAM. `total = actual_RAM + retained_base + relay` was checked against every native fee receipt and reserve equation.

Detailed bilingual routes/fees and limits:
[中文](../../../../system-contracts/eosio.key/docs/ATOMIC_RAM_TRANSFERS.zh.md),
[English](../../../../system-contracts/eosio.key/docs/ATOMIC_RAM_TRANSFERS.en.md).

原生创建策略、四提案并行、真实新链启动、公示/部署、公开交易、客户端/浏览器更新及最终清理尚未完成。
不能把隔离链交易当作公开链证据，也不能宣称旧 Android APK、旧充值封装或浏览器已经适配。
