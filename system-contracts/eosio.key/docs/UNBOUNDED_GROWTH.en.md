# Demand-funded global public-key account growth

This version removes global wallet/balance/asset admission-count ceilings and the artificial 1TiB cumulative RAM-budget ceiling per asset. Legacy max_wallets/max_balances/max_assets/max_native_payers remain serialized compatibility fields and no longer gate admission. Old values such as1000/4000 do not block new paid records.

Each missing record still requires sender-paid additional AIC fees, a real native RAM purchase and confirmation, then full principal credit/delivery. Existing records buy no RAM again. Signed fee caps and complete rollback remain. Preserve reviewed token identities/code hashes, holder proofs/revocation, fees/income/free claims, Signup and native resource rules. Actual RAM supply/market, node storage, transaction resources, native constraints and integer representation remain finite; this is not a throughput or infinite physical-capacity claim.

## State compatibility

Existing config/wallet/balance/asset/fee layouts, action/signature domains and receipts remain unchanged. The sole ABI addition is growth: version:uint32=1 and exact uint64 wallet_count/balance_count/asset_count.

The first subsequent allocation seeds growth from legacy cumulative counts with an ID/count consistency check. Seeding, paid allocation and ledger effects commit or roll back together. No account reset, ledger scan or administrator balance-migration action is added. An absent growth row before the first allocation is normal; readers may use legacy counts until then.

After initialization growth is authoritative. Legacy uint32 count views continue to update and saturate at their representable maximum without wrapping or rejecting new allocations. Balance counts include cumulative KEY-paid external token-row openings, not just current internal balance rows. Counter, identifier and RAM additions retain overflow checks.

Only KEY is upgraded. Signup remains3c03 and is still pinned by KEY; Signup relies on actual AIC payment rather than pinning KEY, so no paired Signup upgrade is required. Clients must explicitly accept the reviewed old/new KEY hashes before activation while continuing to reject unknown code.

## Qualification and activation

Build/tests run remotely. Qualification includes a funded actual30f6 legacy ledger, its old-cap rejection, byte-preserving code/ABI replacement, new paid records beyond old limits, and three routes/assets plus fees/rollback/Signup/claim regressions. Independent TEST ONLY counter/RAM seed vectors exercise uint32/uint64/1TiB boundaries with real new purchases and rollback; they do not represent billions of actual records or physical TiB allocation. Never deploy the seed program publicly.

Publish exact source, artifacts and review hashes first. Then use native eosio.dev review and effective BP veto, with full1800seconds on TEST and unchanged production rules. Candidate compilation/publication is not activation; irreversible execution and actual post-upgrade checks are required.
