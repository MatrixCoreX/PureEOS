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
