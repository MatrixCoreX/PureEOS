# TEST stability controls

Deployed on the existing 4 vCPU / 8 GiB TEST host. See the [reports and public evidence](../../../operations/20261116-testnet-stability).

Only `stability_guard.py`, `retire_banned_runtime.py`, `stability_policy.py` and `log_cleanup.py` provide ongoing timer controls. Financial, prepare, stop, delete, roll, install, resume, manual canary and final audit controllers are completed one-shot evidence and **must never be replayed**. Their presence here does not authorize execution. Existing markers, immutable transaction IDs and server-private safety and key records govern any continuation.

Local isolated checks:

```sh
python3 -B -m unittest discover -s tools/testing/testnet-stability-20261116 -p 'test_stability_*.py' -v
```

These use public synthetic files and mocked process control, without real RPC calls, keys or signatures.
