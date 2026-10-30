# PureEOS System Contracts

[当前源码与 WASM/ABI 包](pureeos-system-2026.10.31-exact1pct.tar.gz) · [SHA256SUMS](SHA256SUMS) · [奖励复核](../docs/rewards/EXACT_1PCT_20261031.md)

当前合约用于新创世链：精确 1% 计提、1461 天减半，保持全部既有 PureEOS 模式。解压包中 `source/README.md` 提供中英文规则和完整构建命令；`contracts/eosio.system/` 为已验收原生成产物。初始化仍须 `setramrate(1024)`，deferred 两阶段禁用。旧政策状态迁移须另行资格化并遵守治理。

```bash
sha256sum -c SHA256SUMS
tar -xzf pureeos-system-2026.10.31-exact1pct.tar.gz
cd pureeos-system-2026.10.31-exact1pct
sha256sum -c SHA256SUMS
cd source
# Set PUREEOS_CDT_ROOT to your installed SDK; follow README.md to build.
```

Current fresh-chain source and qualified WASM/ABI are packaged together. Extract, verify both manifests and use `source/README.md` for complete build instructions. Exact reward math, all selected modes and source identities are documented in the linked review. Existing-chain policy migration is a separate qualification scope.
