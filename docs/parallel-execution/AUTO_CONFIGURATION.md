# Parallel worker sizing

This branch supports 1–128 parallel workers and batches of 1–256 transactions.
The defaults remain `0` workers (disabled) and a batch of `8` transactions.

To size workers and batches at startup:

```ini
experimental-parallel-validation-threads = auto
experimental-parallel-validation-batch-size = auto
```

On Linux, `auto` counts distinct physical cores within the process's allowed CPU
affinity, then applies the smallest visible cgroup v1/v2 CPU quota, including
visible ancestors. Fractional quotas round down, with a minimum of one worker.
The final worker count is capped at 128. When topology is unavailable, the
allowed logical CPU count is used; when affinity is unavailable, hardware
concurrency is used. Unreadable or malformed detected quotas cap automatic sizing
at one worker and produce a warning. A container cannot inspect hidden ancestor
quotas or prove the physical topology of its underlying host.

An automatic batch is `min(3 * workers, 256)`. Setting workers to `auto` also
automatically sizes an omitted batch option. An explicit numeric batch overrides
that choice. Numeric worker settings always override CPU detection, for example:

```ini
experimental-parallel-validation-threads = 32
experimental-parallel-validation-batch-size = 96
```

Manual workers with `batch-size = auto` are also supported. Zero workers with an
automatic batch resolves to the disabled default `0/8`. Detection runs once at
startup; restart after changing affinity or quotas.

These options configure the existing restricted parallel path. Full validation,
reviewed contract code hashes, a matching WASM engine and the existing production
opt-in are still required; see [operations](OPERATIONS.md). The resolved CPU and
worker/batch counts are logged at startup.

The private production phase remains capped at 50 ms. The shared preparation
deadline, discovery-round limit, retained-state/environment byte caps and total
batch write-operation budget retain their previous values. Canonical adoption
remains ordered and failed speculation falls back through the existing path.
Larger batches can therefore reach a shared limit earlier. Automatic sizing is
a starting point for local scheduling, not a measured throughput guarantee.

Shared capture metadata now receives a bounded allowance of four entries per
actual batch slot, with the previous table/account limits as floors and a maximum
of 1024 each. Individual transaction read limits stay at 64 tables and 256 accounts;
row/query counts, value bytes, environment bytes and deadlines retain their limits.
