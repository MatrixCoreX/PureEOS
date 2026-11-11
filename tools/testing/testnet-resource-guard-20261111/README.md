# Low-cost TEST server resource guard (2026-11-11)

This directory contains TEST infrastructure operations for the existing 4-vCPU, approximately 8-GiB server and chain `09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532`. It changes no native, system or token source, binary, consensus rule, quorum, blacklist rule or token supply. It provisions no server or larger plan. These scripts are specific to this dated deployment.

The authoritative live case is `/opt/pureeos-testnet-new-ban-20261111-r1`. Run the isolated policy/lifecycle tests with:

```sh
python3 tools/testing/testnet-resource-guard-20261111/test_recycle_guard.py
```

The tests execute the production guard with isolated filesystem paths and mocked RPC/systemd. They perform no real service, chain, credential or financial action. The report distinguishes those 11 software tests, the live read-only guard check, and the 21 actual same-data runtime restarts. The automatic threshold action did not need to trigger during final observation.

## Persistent settings

- BP `chain-threads=1`, `net-threads=1`; existing vote processing, full validation, JIT runtime, parallel validation settings, state DB size/guard and native Merkle workers remain unchanged.
- `MALLOC_ARENA_MAX=2`, `MALLOC_TRIM_THRESHOLD_=131072` in service environment. Warning/error logging is retained; BP informational logs were reduced with the native SIGHUP logging reload.
- The 10-second health task gives current, pending and proposed producer/finalizer duties CPU/IO weight 500, and standbys 250. Founder RPC and one staged bootstrap retain 500. BP CPU quotas are not introduced. Caps of 10–30% apply only to small operation/monitor/compression workers.
- Existing 56-GiB active/persistent swap is retained. The old swap-growth sampling timer is superseded by the new bounded monitor; no further automatic swap allocation occurs.
- `pureeos-syslog-rotation-20261111.timer` checks hourly. `/var/log/syslog` rotates daily or over 128 MiB, compresses, and keeps at most seven rotations/max seven days. Other log policies, including auth logs, are unchanged. The existing 512-MiB journal budget remains. Dated fault evidence is outside this retention policy.

## Preventive single-node recycling

`pureeos-testnet-memory-guard-20261111.timer` checks every minute. A process must exceed 1280 MiB RSS+Swap before an action can be considered. The guard permits at most one attempt per 600 seconds and never overlaps its own actions.

All 25 expected BP PIDs must exist, the same chain must have head age below five seconds and LIB age below fifteen seconds, native policy must remain 21/15, and there must be at least nineteen recent strong votes, including at least eighteen other voters after excluding the target. The native ban set must match the qualified fifteen, the target must have no personal miss streak, and its next duty must be at least ninety seconds away. Non-duty standby nodes are preferred; pending/proposed policy members are included in the safety decision. The exact native ELF hash is pinned before any restart.

Before a graceful stop, the guard arms an independent systemd task to start that same service after sixty seconds if the controller fails. After the clean stop it preserves and hashes the existing safety file, then starts the same node with its own unchanged data/configuration/keys. It verifies three fresh synchronization observations, eighteen other recent strong votes and all other 24 PIDs. Native safety-file forward writes and atomic renames are expected; no safety reset or restore is performed. Success cancels the extra start timer. Any attempted action failure leaves the start safeguard and latches `RECYCLE_BLOCKED.json`; later cycles do not perform another action until a human reconciles it.

Stale chain, missing BP, a new ban, insufficient votes, a miss streak, no safe duty window or a changed native ELF prevents automatic action. The guard does not repair dirty databases, replay blocks, delete node data, unban, sign transactions, change maintenance/votes/bonds or generate replacement payments.

Inspect `HEALTH_LATEST.json`, `RECYCLE_LATEST.json`, and the three enabled `*-20261111.timer` units. Use `recycle_guard.py --check-only` for a live non-mutating decision. A fixed small server remains CPU constrained; this is operational risk reduction, not an indefinite-uptime or throughput guarantee.

## One-shot historical phases

`retire_new_bans.py`, `cleanup_new_bans.py`, `optimize_canary.py`, `rolling_optimize*.py`, `prepare_four.py`, `reconcile_rex_voter_ownership.py`, `enroll_four.py`, and the installers have already completed for this case. Do not rerun them. Reconcile exact receipts and original transaction IDs before any continuation. Do not reuse old completed recovery, payment, key-generation or deletion controllers.

Four fresh replacements are `alibabaailab`, `deepmindtech`, `openaimodels`, `sydneyunswcs`. The old four identities remain permanently banned. The new accounts have independent account/production/peer/BLS keys retained only on the server, 10000 AIC own bonds each, and normal native registration. Only fourteen ownership-verified external TEST ballots targeting these four old identities moved; all other selections and all fifteen banned self-ballots remain. REX native cached vote refreshes were reconciled against unchanged REX shares and delegated principals.

No private key, wallet, password, signed packet, raw safety file, chain database or raw system log belongs in Git. Public proofs and bilingual reports are in the matching dated machine-evidence directory. `SOURCE_SHA256.json` identifies the shipped scripts. See the report for retained failed attempts and the safety-inode/cooldown corrections completed before final activation.

## Follow-up maintenance and disk request

The user subsequently requested that effective-maintenance BPs remain offline and that stopped old-node runtime logs be deleted instead of permanently archived. [Maintenance verification](../testnet-maintenance-stop-20261111/README.md) confirms montrealmila was already inactive/disabled and is excluded from recycling. [Disk cleanup](../testnet-disk-cleanup-20261111/README.md) records the exact deletion whitelist and results. This later request supersedes preservation of the selected stopped-node runtime-log archives; chain/state history and audit records remain protected. The dated historical reports above remain as-of records.
