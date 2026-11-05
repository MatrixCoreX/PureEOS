# 2026-11-06: three additional BPs stopped, five node datasets cleared

The latest user instruction adopts the two existing native bans, directly stops three additional new BPs, and deletes all block datasets of those five nodes. This supersedes the old five-new-node campaign, whose mutation controller is now explicitly guarded against resumption.

Stopped and disabled: `tokyorikenai` and `singaporentu` (already natively banned), plus `tokyoaistlab`, `pariscnrslab` and `londonturing` (stopped now). `genevacernai` and `sydneyunswai` remain online. `montrealmila` remains online in native maintenance state2.

The pre-stop check retained native21/15 policy and16 online surviving finalizers with recent strong votes. Irreversible height advanced after stopping; after deletion a retained canonical block anchor was still readable through another node with the same Chain ID. No native timing/quorum, key, vote, bond or issuance changes, unban, synthetic ban or forfeiture settlement occurred.

Six datasets were cleared: one per node plus the Tokyo initial slow genesis replay. Block/reversible/state/cache data were deleted without raw-data backups. Five265-byte `finalizers/safety.dat` records (1325bytes total) were preserved and verified, alongside unchanged configuration/key files. Tiny safety copies remain remote and private.

Dataset allocation fell by **186.78 MiB**. Net available filesystem space rose **185.18 MiB** while unrelated background writes continued, reaching **83.13 GiB** available. Sparse logical file sizes are not claimed as physically freed storage.

At `2026-11-05T23:36:18.669350+00:00`, all five units were inactive/PID0/disabled; only verified safety records remained in the six directories. Native LIB was#105676 and progressing. Two permanent bans existed; the additional three still awaited native window decisions. A separate background observer only reads native outcomes, with no further stop/delete/payment/permission actions. The user's no-foreground-wait preference is retained.

Earlier failures remain explicit: the old campaign exited at2026-11-05T14:45:49UTC on an incomplete maintenance-table page and never executed its five-node outage. The read helper now passes7remote regressions. Later kernel OOM kills and dirty-database restart failures affected original nodes, of which two were natively banned. The incident evidence records their actual origin and irreversible checkpoints. `baiduaiworks` still has an unrepaired dirty-database startup failure and was not included in these five deletions.

This is an operations task on the existing shared Cascadelake4-vCPU/8-GB host, not a TPS/capacity qualification. No server/reset/new browser/build load was introduced. Primary receipts are`USER_THREE_STOP_RECEIPT_20261106.json`,`USER_FIVE_DATA_CLEANUP_RESULT_20261106.json` and`USER_FIVE_RETIREMENT_FINAL_CHECK_20261106.json`.
