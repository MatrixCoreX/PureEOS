# RAM market fees — 2026-11-04

User selected implementation of market-priced RAM storage fees. Develop and remotely qualify the contract, SDK and matching read-only explorer estimate. Live activation is a separate native TEST 1800-second proposal/execution; no direct setcode, reset, pause, new server or TPS is part of this development task.

- Preserve chain 116ea0c3, node/system/token bytes, 26 original services, history, ordinary activity, votes, bonds, preexisting germanycrest ban and ten excluded identities.
- Keep existing config/table/request wire layouts and owner signatures. Keep fee-free claims, token code whitelist, third-party recipient pre-open checks and separately sponsored token RAM budgets.
- Conservative allocation bytes, not measured RAM: new key+balance 1536; existing key/new balance 512; existing balance 0; supported native recipient creation 512.
- Read qualified system rammarket on every nonzero-byte quote. Net curve cost = ceil(AIC_reserve * bytes / (RAM_reserve - bytes)); gross = ceil(net * 200 / 199), covering the system's rounded-up 0.5% buy fee. No purchase is sent by a transfer.
- CPU/NET work = ceil(unit_fee * units * existing_multiplier / 10000); base = max(existing_minimum, work + RAM_gross). RAM is not multiplied again by CPU/NET. Relay remains ceil(base * selected_bps / 10000), range 10%–30%, minimum one atom. Signed total cap rejects over-budget execution atomically.
- Old new_row_fee stays serialized for existing state/legacy profiles, but the new reviewed WASM ignores it. Clients select reviewed code profiles; unknown hashes fail closed for quoting.
- Remote-only isolated low-priority builds/tests on retained 66.245.194.132. Sequential compilation/frontend/native qualification, bounded resources and private-network test nodes; preserve failed attempts. Do not rerun old controllers.
- Verify native RAM buy/sell changes, 0/512/1536-byte paths, rounding, independent CPU multiplier, fee floor, cap rollback, reserves, legacy signatures and claim resources. Malformed/saturated markets use labeled fixtures. Frontend retains collapsed formulas, five actual relay entries, languages/themes/mobile and old live profile compatibility until activation.

Status: development and all remote qualification complete; user-requested cleanup complete. No live upgrade submitted. See REPORT.md and verified evidence manifest.
