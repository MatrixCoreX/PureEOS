# Remote build and isolated qualification: system accounts and signup

**Build succeeded; qualification FAILED; the original TEST chain was restored. Do not activate or deploy this candidate.**
This maintenance window authorized builds and isolated validation only. No candidate was installed on the live chain; no feature activation, governance proposal, account creation, deployment, reset, new server or TPS run occurred.

## Reproducible scope

Actual hardware: shared Intel Xeon Cascadelake VM,4 guest logical CPUs,8125768KiB RAM. The remote O1 TEST build rebuilt144 project translation units using verified retained dependencies. One compiler normally had a2CPU quota,5GiB memory high watermark,6GiB maximum and no cgroup swap. It is not a production O3 build, production-review qualification, physical-core guarantee, capacity result or performance benchmark.

Build input baseline: `504ca353cec0ddba692f8f2d4a26f74ea6498baa` plus [source overlays](evidence/native-r1/PATCHES.json). The embedded binary version names that baseline; it does not identify the complete overlay. Test/tool hashes are in [QUALIFICATION_SOURCE_HASHES.json](evidence/QUALIFICATION_SOURCE_HASHES.json). Candidate nodeos SHA256: `19d2ce7fea2a4f83a228c99aeb50a50a5e76e485e5dfc89e0700d0e27248f334`.

## Observed checks

| Scope | Passed / cases | Result |
|---|---:|---|
| Native governance, snapshot and replay |31/32|Successful creation fails|
| Hash-pinned real-contract fixtures |2/3|Narrow genesis-BP veto and pre-feature v10 snapshot/proposal3 preservation pass; signup creation fails|
| Unsigned helper/native serialization |1/1|Proposal, execution and three permission links match|
| Independent OpenSSL codec/curve/field checks |1760 checks pass|Parser qualification only|

See [final attempt7](evidence/native-r1/qualification-7/RESULT.json). Signup WASM SHA256 is `996d520ce32c01da431334fc7bf1f03755f4c82f7ae73ddf8b9d931054ae35f3`. Native raw ABI is171bytes, SHA256 `ca516e15cc09daa9adf924318e43b7a1efea8573546824102cfd617a9a7c4dd9`. A passing wire comparison does not establish successful governance execution.

The root handler queues creation and deployment, but inline authorization is validated immediately. The account does not yet exist, so native validation correctly rejects authorizing actor `eosio.signup` and rolls back. Actual K1/R1/legacy registration, signatures, RAM purchase, stake ownership and registration rollback assertions were NOT REACHED. The [correction plan](../../../../docs/governance/system-account-execution-followup.md) introduces a tightly bound native continuation in the same transaction while retaining normal actor/permission existence checks.

All intermediate failures remain recorded, including compiler APIs, dependency linking, input-integrity rejection, fixture activation and custody setup, electorate-refresh ordering, snapshot11 loading and the isolated2GiB test-compiler cgroup OOM. Intermediate fixes do not turn the final failed qualification into a pass.
Inherited auxiliary scope wording is corrected in [ANNOTATIONS.json](evidence/ANNOTATIONS.json) without rewriting the original bytes. Primary XML and final attempt7 define the actual result.

## Recovery and preserved state

The original budget began2026-11-04 17:53:19UTC;25 healthy/history processes paused17:56:39UTC. Original programs were restored at `2026-11-04T19:22:29.599123+00:00`, after approximately89.18minutes, before both the90minute build cutoff and2hour limit. All26 original services are running again. Germany's preexisting banned process retained PID252912. No temporary stale-production flag was needed.

The original node binary, config and unit hashes match; chainID and real21/15 finality are preserved. Governance, votes, bonds, bans and90 balances were checked before ordinary activity resumed. Proposal3 remains pending/vetoed24/24; maximum supply2100000000.0000AIC and issued supply100000000.0000AIC are unchanged. The original founder remains the only HTTP relay. See [recovery audit](evidence/POST_RESTORE_VERIFICATION.json). Actual history/API health is recorded separately in `POST_RESTORE_ORIGIN.json`; an active process alone is not a healthy API proof.

After the database cold start, the original API and indexer had exited their bounded startup waits. Only these two original application services were started again with unchanged configuration, without another chain pause or BP restart. Full origin/API verification completed at `2026-11-04T19:36:58.850458+00:00`.
At that observation the history index was at232653 and the chain head at232660. Indexing has resumed and is catching up. Public-key history retains its indexed_receipts coverage label; neither complete lifetime coverage nor zero lag is claimed.

## Remaining gates

Implement and qualify the atomic continuation, pass successful creation and real signup/resource/signature/rollback cases, and qualify production review/build and broader current-chain compatibility. A later separately authorized validator rollout must preserve chain data, precede irreversible feature activation, and use exact published artifacts plus the complete native TEST1800second or production7day review and effective BP veto guard. Created accounts remain nonprivileged; proposal3 need not be cancelled and no chain reset is required. This candidate branch is not merged, released or approved for deployment.
