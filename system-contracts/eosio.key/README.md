# eosio.key · 公钥转账 / Public-key transfers

开发版本，尚未公开部署。自定义代币的托管 RAM 授权隔离仍是发布阻断项，见 [风险记录](docs/RISKS.md)。 / Development only. Custom-token custody RAM authority remains a release blocker; see the risk register.

独立的多代币公钥钱包合约。文本账户可以给任意有效 K1 公钥转币，公钥持有人可以给其他公钥或已存在的文本账户转币。收款本金全额到账，AIC 手续费另外支付。公钥收款人无需先创建链上文本账户。

An independent multi-token public-key wallet contract. Native accounts can deposit to any valid K1 public key; the holder can send to another public key or withdraw to an existing native account. The recipient receives the full principal; AIC fees are additional. A public-key recipient needs no native account or prior registration.

第一版使用链上 PowerUp CPU/NET **资源市场占用**定价，商业中继仅允许已注册、押金正常、未维护且未永久拉黑的 BP。中继收入为基础费用的 30% 加价，合约每次执行都重新检查资格。用户指令在合约内验证签名，绑定完整原生交易、撤销代次、到期时间、收款方、代币、金额、指定 BP 和总费用上限。

V1 prices fees from on-chain PowerUp CPU/NET **market occupancy**. Commercial relays must be registered, normally bonded BPs outside maintenance and the permanent blacklist. Relay revenue is a 30% markup on the base fee; eligibility is checked on every execution. The contract verifies the owner's signature, binding the complete native transaction, revocation epoch, expiration, destination, token, principal, selected BP and maximum total fee.

| 方向 / Direction | 合约路径 / Contract path | cleos |
|---|---|---|
| 文本账户 → 公钥 / Native → key | 标准 token.transfer + PK2 memo | `transfer account PUBLIC_KEY quantity --max-key-fee ...` |
| 公钥 → 公钥 / Key → key | `ksend` | `key prepare` → `key sign` → `key push/submit` |
| 公钥 → 文本账户 / Key → native | `withdraw` / `selfout` | 同上，收款方写已有账号 / same flow, use native recipient |

开发协议版本为 **2**：不同设备可以使用同一撤销代次独立签名并发发送，不需要共享 nonce 分配器。普通转账不递增代次；原生交易 ID、TAPOS 和过期校验阻止重复扣款。超时后必须追踪同一交易 ID，不能自动重新准备并签名。

Development protocol **2** supports independent concurrent devices without a shared nonce allocator. Ordinary sends do not increment the revocation epoch. Native transaction ID, TAPOS and expiration protect against replay. After a timeout, track the original ID; never automatically prepare and authorize a replacement.

详细说明 / Details:

- [中文设计与 API](docs/API.zh-CN.md)
- [English design and API](docs/API.en.md)
- [cleos 使用 / cleos usage](docs/CLEOS.md)
- [部署与验证 / Deployment and qualification](docs/DEPLOYMENT.md)
- [风险控制与边界 / Risk controls and boundaries](docs/RISKS.md)
- [已选范围 / Selected scope](docs/PLAN.md)

```bash
CDT_ROOT=/path/to/pureeos-cdt-1.0.0 bash scripts/build.sh
npm ci --ignore-scripts
npm test
# Native cleos addition belongs to the parent PureEOS CMake target:
cmake --build /path/to/pureeos-build --target cleos -j1
```

SDK 与参考中继使用 Node.js 22.12 以上版本；原生 cleos 不依赖 Node.js。 / The SDK and reference relay require Node.js 22.12 or later; native cleos has no Node.js dependency.

生产 WASM 固定读取 `eosio` 系统表。`KEY_BUILD_FIXTURES=1` 额外产生明确标注的测试替身，不能用于正式部署。保留现有 eosio.system WASM、BP 押金与罚没池、治理、资源规则和禁用延迟交易设置；本功能不修改 nodeos。

The production WASM reads the actual `eosio` system tables. `KEY_BUILD_FIXTURES=1` additionally builds clearly marked test substitutes; never deploy them as production. Existing system WASM, BP escrow/slash pools, governance, resource rules and disabled deferred execution remain intact. No nodeos change is required.

License: repository [AIC noncommercial license](../../AIC_LICENSE.md).
