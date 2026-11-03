# eosio.key development and security ledger · 2026-11-03

No TPS was measured. Protocol-1 sequential-nonce results are historical, not qualification of independent devices. Source and artifacts are separately retained; no public-chain deployment or reset occurred.

| Attempt | Outcome / reason |
|---|---|
| Old compiler r1 | Unsupported `eosio::literals`; corrected; not qualified |
| Old compiler r2–r5 | Successive scope, expiry arithmetic and exit-bucket fixes; early system market-version assumption rejected; superseded |
| Old functional r3–r4b | Retained setup/CPU/asset-name/market-version failures; no complete pass |
| Old functional r6 | Node 18 incompatible with SDK ESM dependency; separate Node 22 installed |
| Old functional r7 | Native CLI/SDK digest and actual relay transaction confirmed; retry error surfaced as BadCast; later reproduction did not establish original RPC cause; error formatter fixed |
| Old functional r7b | Many funds/security cases passed; actual PowerUp reservation exposed valid current-utilization above smoothing value; use max(current,adjusted) |
| Old functional r8 | Cold deposit exceeded unchanged 150 ms CPU budget on overloaded demo host; no qualification |
| Owner capability probe r1 | Atomic bootstrap setcode CPU rejection; retained |
| Owner capability probe r2 | PASS: native CFD proof, tampering, duplicate rejection, failed-proof recovery, inline guard, expiration and two independent transactions; counter fixture only, no token custody qualification |
| Old compiler r9–r10 | Protocol-2 development; r10 compiled complete proof/reference envelope; superseded by external-row budget hardening |
| Old functional r10 | Actual native CLI/SDK digest match; key push executed but returned human output despite JSON mode, so harness rejected empty JSON. CLI output fixed; no complete protocol qualification claimed |
| Old compiler r11 | Stopped as superseded during RAM-row review |
| Old compiler r12 | External recipient balance-row fee/budget added. Work migrated to separate server after user instruction; use independent server artifacts below |
| Independent server build r1 | Native main/http compilation passed; link failed because fresh OS lacked curl/gmp development links and used a different zstd filename; retained |
| Independent server build r2 | PASS: dependencies installed, zstd shared-library path resolved; all native inputs and artifacts hashed; GCC 13.3 / CDT 1.0.0 |
| Independent functional r1 | SDK/relay 13 tests PASS; TEST node rejected cgroup 6 GiB below its 8 GB-class minimum; no chain workload started |
| Independent functional r2 | Native CLI transfer passed; test harness repeated global --header option unsupported by parser; credentials test had not executed. Changed harness to separate header requests |
| Independent functional r3 | PASS: 13 SDK/relay unit tests; 56 production-WASM functional/security checks; 141 canonical irreversible receipts. Does not clear the separate RAM-authority release blocker |
| RAM authority native probe r1 | Reproduced: authorized malicious token first receiver billed extra custody-sender RAM; native recipient-notification billing was rejected. Probe PASS means the risk was demonstrated, not safety approval |
| Independent fixture r3f | PASS; alternate system-table BP blacklist/bond/active and market cases are wallet-logic evidence, not native consensus qualification |
| Client build r3 | Orchestration script string/newline syntax failed before compile; retained; fixed by uploading a plain build script |
| Client build r4 | PASS on Vultr: --dont-broadcast/--skip-sign export guard; retained previous client for original r3 evidence |
| Independent functional r4 | PASS: 13 unit tests and 58 observed checks, including safe native export and one EXPECTED KNOWN VULNERABILITY reproduction. Shared custody RAM isolation is FAILED; release_qualified=false. Do not interpret test PASS as release approval |

Remote host: Vultr `vhp-4c-8gb-amd`, AMD EPYC-Rome guest, 4 allowed logical CPUs / 2 guest cores / 4 threads, 8 GB class. This is functional evidence, not EPYC 4345P performance evidence. The default performance machine remains unchanged.

The prototype native client reuses retained, hashed native libraries and separately recompiles its modified C++ main/http sources on the independent server. This is not an official stable client release or a clean whole-node CMake qualification. Nodeos and system WASM remain the released TEST runtime and qualified system bytes. No node whitelist, consensus, chain defaults or deferred-execution changes were introduced.

After the user's remote-only instruction, local activity is limited to source editing, orchestration, receiving results and hash/report verification. All new build and test execution is on the independently owned Vultr host. Test chains are private loopback namespaces; all child processes stop in finally. No private keys, wallets, configurations, state databases or raw blocks belong in this evidence directory.
