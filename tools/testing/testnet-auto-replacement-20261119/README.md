# TEST automatic replacement and stability controls

These are the current sources for the existing 4 vCPU / 8 GiB TEST host. [Report and public evidence](../../../operations/20261119-testnet-auto-replacement/REPORT.json).

The existing memory guardian checks every30seconds and retires canonically banned owned nodes. The new replacement timer checks every3minutes, finishes retirement and service-config cleanup, then creates a fresh similar name only when the fixed bank, resource and budget checks pass. Its target is25BPs; the observed pool is23BPs plus one history source. Low balance currently yields `WAIT_BANK_FUNDS`, with zero replacement jobs or financial submissions.

Funding is restricted to `civicbankers`:14000AIC plus1AICRAM per new node,500AIC retained, at most one job/hour,two/day and ten lifetime. CPU pressure, fresh native21/15 quorum, memory, swap and disk headroom are required. Only the new identity self-votes. Separate server-only account/production/peer/BLS keys, a Unix-only temporary signer, atomic journals and one original broadcast prevent blind financial retries. An ambiguous transaction is reconciled by its original ID; interrupted signing never creates another signature automatically. New checkpoint bootstrap is allowed once, and initialized databases cannot reset automatically.

Shared founder recycling is excluded. Its two native zero-window records from the documented planned-stop incident remain; it is running and not banned. Its soft CPU/IO weight is1000 without a hard CPU cap.

Local isolated verification:

```sh
python3 -B -m unittest discover -s tools/testing/testnet-auto-replacement-20261119 -p 'test_*.py' -v
```

The94cases use synthetic files and mocked process/financial boundaries. They do not establish live replacement financial acceptance. Deployed units are `pureeos-stability-memory-20261116.timer`, `pureeos-stability-logs-20261116.timer` and `pureeos-auto-replacement-20261119.timer`.

Completed deployment, roll, recovery, cleanup, audit and export controllers are evidence under the operation directory and **must never be replayed**. Do not execute archived controllers just because they exist.
