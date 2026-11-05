# System-account continuation repair: remote TEST qualification

The two previously failing execution paths are fixed. Remote qualification2
passed **38/38 cases and1255 assertions**:34 governance cases,3 real pinned
contract cases and1 unsigned-wire case. Original programs and services were
restored within the approved60-minute window, after **15.45minutes**.
Full query/index/business progress verification completed after20.31minutes.
The candidate was not installed, activated, proposed or deployed on the live chain.

## Defect and repair

The earlier `sysexec` queued `newaccount` and then immediately queued actions
authorized by the missing target. Inline authorization checks occur when an
action is queued; the account did not yet exist. Those checks correctly rejected
both the successful-creation test and the real signup test. The reviewed previous
attempt7 failure remains preserved in the November5 R1 report and Git history.

The repaired root queues only `newaccount` followed by native-only `sysstage`,
using existing authorizing accounts. Once creation has actually run, the stage
checks the exact top-level `sysexec` parent, ephemeral capability, proposal,
target, payload, sponsor authorization, code pins and temporary authorities.
It consumes the root stage by rebinding the capability to its own ordinal,
then queues funding, RAM, code/ABI installation and final sealing. `sysfinish`
requires that exact stage parent. Nested or duplicate continuations fail, and
any child failure rolls back account, payment, protection and proposal state.
Ordinary actor/permission existence checks and transaction-context layout were
not changed. Public proposal/execution serialization is unchanged. New accounts
remain nonprivileged, with sealed owner and reviewed-governance code updates.

## Actual validation

| Suite | Passed cases | Passed assertions |
|---|---:|---:|
| Governance, native creation, snapshot/restart/replay |34|1149|
| Real pinned system/token/signup contracts |3|101|
| Unsigned helper versus native action serialization |1|5|

The regressions cover pre-feature behavior, direct/forwarded calls, missing
actors and permissions, exact action ordering, nested/duplicate stages, rollback
of both governance lanes, review/veto/cancellation and nonprivileged sealing.
The real-contract suite actually creates and deploys the isolated signup
account, registers K1/R1/legacy EOS public keys and separate owner/active keys,
checks at least4096RAM bytes, recipient-owned0.1000AIC NET and0.1000AIC CPU
stake, zero residual liquid balance, K1/R1 active signatures and an owner-signed
permission update. Explicit fixture-funded PowerUp precedes signature tests;
registration stake does not by itself establish available PowerUp work.
Duplicate names, checksum corruption, insufficient registration RAM, a forged
transfer notification source and insufficient deployment RAM fail atomically.

The unchanged signup WASM SHA256 is
`996d520ce32c01da431334fc7bf1f03755f4c82f7ae73ddf8b9d931054ae35f3`.
Raw ABI is171bytes, SHA256
`ca516e15cc09daa9adf924318e43b7a1efea8573546824102cfd617a9a7c4dd9`.
Pinned system/token hashes and all cases appear in the public evidence. The
previous1760 independent codec/curve checks are retained results, not rerun here.

## Build provenance, failures and limits

All compilation/testing occurred on the retained **shared Intel Cascadelake
4vCPU VM,8125768KiB RAM**, not this local workstation. Guest topology does not
prove dedicated physical cores. Build4 incrementally linked the repaired node
against hash-verified retained objects/dependencies from the earlier144-TU build.
`system_account_proposal.cpp` and `eosio_contract.cpp` reused O0 objects compiled
in the bounded online repair; controller/plugin and the standalone test TU used
O1. Other retained objects are O1. Per-object argv/hashes, source overlays,
base inputs, dependency paths and failed attempts are preserved in `evidence/`.
Inherited `scope`/`optimization` strings in build4 metadata describe the online
driver defaults; actual argv plus this approved maintenance record establish
the mixed profile and paused-chain execution. No metadata was silently rewritten.

Build4 completed in286.44seconds under CPUQuota200%,
MemoryHigh5GiB/Max6GiB, zero swap and bounded window/deadline guards. Online
build1 initially exposed private-accessor/shared-blob API errors, which were
fixed. Online build2 exhausted only its2GiB compiler cgroup; its OOM evidence
and unchanged original26PIDs were retained. Build3 was interrupted for the newly
approved maintenance window. Repair qualification1 passed34+3 cases but its final
helper step failed because staging had omitted the unchanged public helper.
Copying that helper resolved the issue; qualification2 passed all38 cases on
the same native binary. A separate loader/`--version` smoke also passed.

Candidate node SHA256: `d4ff795f3c351ff68f5b8143740ec4997cd8bbfe3f8ce6efb1e2feb7562f50d8`.
Test binary SHA256: `dd7288073b33ae6a8ce106bba0e167afe7931cc7a870990a777fed1ff85fe285`.
These are functional TEST artifacts, not a production release or TPS result.
No version tag/release was published. Production build/review and broader
current-chain compatibility, permanent-ban and resource/adversarial boundaries
remain unqualified. The isolated tester advances its private clock; it does not
shorten live TEST1800seconds or production604800seconds.

## Original-chain restoration

Window start: `2026-11-05T00:21:09.068883+00:00`. Healthy/history25processes paused: `2026-11-05T00:22:00.347515+00:00`.
Original services restored: `2026-11-05T00:36:35.797598+00:00`. Germany's previously banned
PID252912 was never stopped/restarted/unbanned. All26 original node units run
the original binary SHA256`0be5282d1d5a5eb72081e1d1374931d5e79cff139de728f8d230e29df50534e7` with unchanged final
configuration/unit hashes, chain data and keys. The25 restarted PIDs are
recorded in `POST_RESTORE_VERIFICATION.json`; the previous PID baseline is retained.
One active BP used a temporary stale-production startup flag after a common
durable head was verified; normal native quorum recovered and the configuration
was restored byte-for-byte, followed by its original service restart. No safety,
signatures, quorum or key rule was bypassed.

ChainID: `116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0`. Native finalizers/threshold remain21/15,
25registered/24eligible, Germany the only preexisting permanent ban. Proposal3
remains pending with24effective vetoes. Cap2100000000.0000AIC and issued
100000000.0000AIC, votes/bonds, code pins, consensus caps and90ordinary balances
matched before business admission resumed. Four original history containers,
the original10application services, ordinary activity and sole founder relay
were restored; Elasticsearch was required to reach authenticated GREEN before
applications started. Configurations and credentials were not exported.

Read-only native restoration audit passed at `2026-11-05T00:36:59.884519+00:00`; irreversible blocks
advanced by31during its15second observation. TLS-origin checks
passed at `2026-11-05T00:41:27.918356+00:00`, with history indexed block266829.
Canonical history still exposes3proposals,2executions and24vetoes. Key history
retains its honest indexed-receipts coverage and reports neither paused nor stale;
the frontend release and all static file hashes are unchanged. This is an exact
TLS-host loopback-origin check, not a new external-browser availability claim.

The first two origin attempts observed the original API still starting. The
first healthy response preceded history-index advancement; the next strict
advancement check correctly failed. All such attempts are retained. After the
original reader/ingestor initialization, the progress audit passed at
`2026-11-05T00:41:27.935984+00:00` with indexed block266829,
head/index gap7, and ordinary transactions
advancing from5422to5436.
No additional indexer/BP restart, reset or synthetic payment was used.

Public source, reports and allowlisted logs/XML/hashes are retained. Fixture raw
state, private temporary data, runtime configuration backups and signing secrets
are excluded from Git/local evidence. `MANIFEST.json` hashes the public evidence;
cleanup, if completed, is recorded separately in `CLEANUP_RESULT.json`.

The next release step still requires separately authorized validator rollout,
irreversible feature activation, exact published artifacts, the complete native
review and a below-threshold effective veto tally before actual account creation
or contract deployment. No chain reset is required by the design.
