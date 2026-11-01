# BP bond candidate / BP 押金候选

普通 BP 必须本人授权缴纳 10,000.0000 AIC；安全退出交接后冷却 604800 秒，且退出检查点不可逆后才能退款。永久黑名单押金经不可逆决定结算到独立的 eosio.bslash 池。本版不提供罚没池转出、分配或销毁接口。eosio.token 仅为代币合约；押金托管为 eosio.bond。只支持新链封存前初始化。

Ordinary BPs must authorize a fixed 10,000.0000 AIC bond. Refunds require safe duty handoff, 604800 seconds of cooldown and an irreversible exit checkpoint. Finalized permanent-blacklist decisions forfeit bonds into the dedicated eosio.bslash pool, without outbound, distribution or burn actions in this version. Custody uses eosio.bond; eosio.token remains the token contract. Initialization is fresh-chain only, before governance sealing.

- [中文规则](bp-bond.md) / [English rules](bp-bond.en.md)
- [测试链启动流程](../../docs/test-chain-startup.md)
- [资格清单](QUALIFIED.json) / [22 项实际 WASM 验证](FUNCTIONAL_QUALIFICATION.json)
- [完整合约源码及原生测试](pureeos-bp-bond-qualified-source.tar.gz)

Standalone contracts commit: b230c7e12d7cd0bc05ad8269140dc030f8b5fe79; parent: 15dea1aa8a4b158eb771c5b13947ddeaafd28910. SOURCE.patch is relative to that contracts parent, not the node repository root. The complete archive contains all source and native harness files; only the 22 cases listed in FUNCTIONAL_QUALIFICATION.json are qualified by this run. Native test clocks exercise seven-day boundaries; this is not seven days of wall-clock stability or node TPS qualification.

WASM SHA256: b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170. ABI JSON SHA256: 9dab82410be6e25b184c2b7e4d834709d9bb556d3e114ea9504080f6b059f358. Building with PUREEOS_BP_BOND=OFF reproduces the released WASM byte-for-byte (373b66b2994c3131ce6c9c3d4526fdf836c0ac2c36d8265b2239d61e75ca2ceb).

This additive candidate does not replace v1.0.0 files or tags. New-chain deployment and the authorized single-BP TPS round have separate execution receipts; functional PASS alone does not claim deployment or throughput.
