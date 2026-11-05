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
developer_role, founder_handover, history_and_admission, key proposal/execution,
signup proposal/execution, verify_and_publish and bounded cleanup. Core bootstrap
does not precreate/preinstall eosio.key/eosio.signup. Each gets a separate complete
real TEST1800second creation/deployment review after startup. Production remains
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
