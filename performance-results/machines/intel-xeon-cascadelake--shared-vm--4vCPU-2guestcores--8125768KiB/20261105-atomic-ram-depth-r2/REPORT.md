# Actual fee/REX depth repair / 真实手续费路径修复

Remote isolated qualification **PASS238 native checks +26 SDK tests** on2026-11-05. Candidate WASM `7523bbec364316d06f48283e9cc17cf81fa929bd60b4efd818eadd73e215c788`; unchanged JSON ABI `f4d64cd7bd84796fab77886fc8308def72b19076a76c2e2bae58857f7bf3b0c5`. Native raw ABI remainsb6db52cd47fdf811bee83030e524de35d0b6f55aece386392b56d1a085df2f83.

This report covers the KEY contract, not successful public creation. The historical E006 private fixture omitted actualeosio.fees plus a funded REX pool. Public creation subsequently failed atomically at depth4; no target account or deployment committed. Four mature reviews1–4 were irreversibly cancelled by52013496d85ac60ff07409d7adae3ef77a66cfbd8523797df5c7dd6895652626, block5877, chain09f10e50. Their exact review bytes remain unchanged.

Normal owner/funding actions now queue nativebuyram directly and then the guarded settlement action; an extra self-payram hop is removed. The publicpayram ABI remains guarded for compatibility. Payment tickets, notification validation, sender authorization, reserve accounting, state/wire/ABI, fee formulas and globaldepth4 remain unchanged. Actual fees WASM and fundedREX are enabled before all atomic route tests. Full first/existing-record AIC/USD/CNY routes, principal/fee/RAM equality, caps, internal-call guards, replay, rollback and free earned-income claims pass. Exact core and test source hashes match the remote built sources.

Hardware: retained shared Intel Cascadelake VM,4guestvCPUs/2visibleguestcores/8125768KiB RAM. Remote isolated native0be TEST runtime, pinned actualsystemb591/tokenf8, depth4. This is functional qualification; no TPS, production hardware capacity, parallel whitelist or post-quantum claim. Fixtures and secrets remain remote; no private wallet/raw state is exported.

**Public status at this report:** candidate not deployed. Native creation-continuation repair qualification and all-current-node rollout must pass first; then publish exact new IDs5–8 with fresh RAM quote and initializer and wait another complete1800chain seconds. Actual public three-route/token/fee/RAM/history/browser checks remain mandatory. Production7days and stable release assets remain unchanged.

本轮补全真实手续费转REX路径，238项原生检查及26项SDK通过；不把旧237项结果当成完整路径覆盖。合约省去额外自调用，逐笔实际买RAM、发送方另付AIC、收款本金全额和手续费上限不变。原四提案失败回滚并已取消，必须用新代码和新编号重新完整公示。此处不是公开部署成功报告。详细费用流程见[中文](../../../../system-contracts/eosio.key/docs/ATOMIC_RAM_TRANSFERS.zh.md)和[English](../../../../system-contracts/eosio.key/docs/ATOMIC_RAM_TRANSFERS.en.md)。
