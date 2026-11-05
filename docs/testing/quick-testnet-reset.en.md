# Repeatable fast TEST reset workflow

Use immutable per-run plans and durable phase receipts for preparation,
qualification, cutover, bootstrap, governed service deployment and verification.
The November5 full reset is explicitly user-selected. Future public resets must
belong to the selected task; isolated functional tests use independent paths/ports.

Pin host identity, old ChainID, TEST profile, roles/names/budgets, actual source
inputs, compiler/dependencies, binary/WASM/rawABI and phase-source hashes. Keys,
wallet passwords, signed packets and raw test state stay remote/private. Reuse
qualified artifacts only for matching inputs and required test scope; documentation
edits do not require rebuilding every native object, protocol changes do.

Standard phases are prepare, qualify, archive_old, bootstrap_core,
developer_role, founder_handover, history_and_admission, four independent parallel service reviews,
four separate exact executions, verify_and_publish and bounded cleanup. Core bootstrap
does not precreate/preinstall eosio.key/eosio.signup. KEY/USD/CNY/signup each gets a complete real TEST1800second review after startup,
submitted together without merging their electorates, vetoes or commitments. Production remains
seven days. Preserve real bonds/self-votes, randomized ordinary votes, native
handover/quorum, new history namespace and sole founder relay.

`tools/testing/testnet_reset.py prepare|run|status|resume` takes an actual
`--plan /opt/pureeos-RUN/PLAN.json --run-dir /opt/pureeos-RUN` on the retained
remote host. RUN is an actual unique generated directory, not an executable
template. The runner checks host, TEST/no-TPS scope, immutable plan/source hashes,
dependencies, receipts and an exclusive lock. Completed phases are verified and
skipped. Interrupted phases require their canonical reconciliation operation;
never blindly sign/broadcast another payment. Reconcile exact transaction ID,
packet hash, expiration and complete irreversible receipt coverage first.

Keep controller diagnostics private and export only allowlisted actual evidence.
Never mix old-chain history, signatures, code/chain pins or derived caches with
the new genesis. Archive old data and authentic bans/proposals separately; no
automatic re-registration of old banned/maintenance identities.

Speed comes from qualified build reuse, pre-generated configuration, bounded
bootstrap batches, independent preparation and resuming only unfinished phases.
Do not shorten review/finality/admission/cooldown/ban windows. Report actual phase
durations and failures; this run must demonstrate the workflow before it is
claimed qualified. No new server or TPS workload is implicit in a reset.

## Concrete November5 controllers

The actual scripts are retained under `tools/testing/devnet-20261105/`. They pin a unique dated root, the exact old chain, native57/canary2 qualification, node hash and durable phase results. They must never be rerun unchanged against the successful current chain. The generic `testnet_reset.py` is a plan/phase framework; this concrete run does not qualify arbitrary generic plans end to end.

Actual order: archive old chain; first fresh bootstrap; history node; enroll24; audit real finality; prepare four exact reviews; publish/verify Git; submit four unchanged reviewed payloads; wait native irreversible review maturity; execute each separately; post-creation relay/transfer/signup/ordinary phases. New read-only history/UI work can proceed during review. The governance electorate24/veto16 is distinct from the initial actual native2/2 finalizer policy.

Preserved recoveries: nonsecret wrapper-directory permissions before any wallet/block/transaction existed; an audit that mistakenly hashed the dynamic loader instead of the mapped node ELF; redundant upgrade links already installed by govinit. The last transaction was atomically rejected, fully reconciled as absent from irreversible canonical receipts with zero native proposals, then the unchanged published proposal payloads were submitted without redundant linkauth. No successful genesis/payment/deployment phase was replayed.

Each future authorized reset needs fresh roots, keys, chain identity, old-chain/unit inventory and exact reviews. Reuse only matching qualified artifacts/dependencies; changed scope requires fresh qualification. Never copy old signed packets, business cursors, balances, indices or completed governance controllers. Remote private keys/wallets remain private and old raw chain/history/bans/proposals remain archived separately.

## Effective recovery after the2026-11-05 depth failure

The first execution of reviews1–4 failed atomically on the actual fees→REX path at depth4. All four were cancelled; do not reuse them. Historical57/KEY237 fixtures omitted that path. Repaired native58, KEY238 and final SDK26 passed; all25current nodes rolled to node946a52fb on the same09f10e50 chain, unchanged parameters and actual21/15. Exact reviews were first published at Git7665192df, then IDs5–8 were irreversibly submitted by09f0eb17fd5be203b25f4b0b80c5552dc46178780096ff51385cbad9e092fec4 at11818. All mature2026-11-05T07:19:55UTC, each with24effective voters and16required vetoes. This paragraph records review-in-progress, not completed deployment.

Use the corrected sequence: `roll_depth_runtime_r2.py` → `prepare_public_proposals_r2.py` → publish and verify every exact Git blob → `submit_public_proposals_r3.py` → `execute_public_proposals_r2.py` → `setup_key_projection_r2.py` → `setup_key_service_r2.py` → `verify_public_transfers_r2.py` → `verify_public_signup_r2.py`. Do not rerun oldpost_creation_phases or already-completed90ordinary preparation/voting. SetPUREEOS_GENESIS_ROOT to the exact unique run directory; a missing environment must fail before signing rather than guess a root.

Only the validated, consumed native sysstage bookkeeping hop is omitted from child depth. KEY queues normalRAM buys directly rather than adding a self-payram hop. Ordinary/CFA/globaldepth4, state/ABI, review timing, signatures and authority boundaries are unchanged. Full qualification must deploy the actualeosio.fees and fundREX.

### Fixed checkpoints for the next fast reset

1. Pin a unique run ID, owned host, actual oldChainID, names/units/directories/ports, source/toolchain/flags/dependency/artifact/test digests. Generate private keys remotely apart from public plans. Reuse only matching qualified caches; environment or relevant source changes require affected rebuilds/checks. Preserve verified native objects and WASM. File presence is not qualification.
2. Stop new business submission, drain/reconcile every signed ID against complete irreversible canonical receipts and balances, then archive oldblocks/state/SHIP/indices/bans/proposals/configs. Never mix chains, payment cursors or index namespaces.
3. Bootstrap only core system/resources/token/roles. KEY/signup/USD/CNY remain absent. Use account-basedeosio.dev authorities and sealed system/KEY custody; do not copy old payments, keys or governance state.
4. Perform genuine founder handover, eachBP's own10000AIC bond/self-vote, and90ordinary owner-signed random votes covering allBPs with pairwise-distinct actual totals. Observe actual native proposer/finalizer policy and continuing irreversibility, then restore founder stale-productionfalse. Process counts do not qualify hardware capacity.
5. Start independent history/read-onlyAPI and verify recent transactions. Requote code/ABI/small-initRAM from the new market. Publish four independent exact creation reviews before signing, each binding ID/payer/WASM/rawABI/human review/init/RAM/core pins. Four concurrent slots still each wait the full1800chain seconds.
6. Wait irreversible chain time, then check frozen fields and effective vetoes perID. Execute four separate immutable packets. On failure reconcile raw response, packet ID, complete canonical receipts and target state; never accept missingreceipt, blindly resign or change reviewed bytes while retaining old timing.
7. Verify realbuyram/logbuyram, fullprincipal, additionalAIC fees, signed cap, exact reserves, no duplicate existing-row buy, all3routes/assets, free earned claims and rollback. Verify signupK1/R1/legacy and resource ownership. Enable only founderHTTP; four comparison entries keep empty endpoints/disabled state without extra keys/permissions/services.
8. Pin clients/projection/browser to the verified code. Run four actual preview and four normal publicChrome variants; verify history/fees/receipts/static digests. Export secret-scanned evidence, bilingual reports, verified remoteGit refs and bounded cleanup. Preserve failures and old ban/governance evidence.

Review, veto, irreversibility, admission/bond and permanent-ban windows cannot be shortened for speed. No newserver/TPS or generic arbitrary-plan end-to-end qualification is implied. Completed phases are not replayed because a later phase failed; reconcile first and create only a justified unique new attempt.

## Final acceptance and future-run cautions

Current09 IDs5–8are separately irreversible.4cancelled/4executed history,55transfer/19signupchecks,4preview/4publicvariants(eachphase16keyviews),6receiptpages/6digests passed. Current25nodes/90exactbalances/votes retained; temporarypreview/founderkeosd stopped while ordinarywallet/business/soleHTTP/history continue. Only stoppedprivatePASSfixture was removed; qualifiedcaches/oldrawchain retained.

Use tools/testing/devnet-20261105/RECOVERY_MATRIX.md. Positive canonical KEYreceipt after a waiter timeout is retained, not resent. Client600second verification does not change150ms/depth4/1800seconds. Actual serviceactive is sealed eosio.code OR owncode, without externalkeys; original own-code-only prose is separately corrected. Proposal resource markers must target outside the closed90actor workload. This run proves existing11818one-atomcredit and adjusts only derivedjournal, preserving originalinitialtotal plus explicitexternalcredit. Actual source and unexecuted futuretemplates have distinct filenames and qualification status.
