# eosio.key · 公钥转账 / Public-key transfers

2026-11-04 RAM 市场计费已在 TEST 链完成原生提案升级 / market-priced RAM fees activated on TEST after native review. See [规则、公式与兼容 / rules and compatibility](docs/RAM_MARKET_FEES.md). Qualified code bc3358d7 prices approved bytes from current RAM reserves; old reviewed hashes retain fixed row pricing. Production release defaults are unchanged.


开发测试版本，已部署至 [公开测试链](https://aic-test.matrixai.one/)，未发布为稳定版。当前测试范围为系统 AIC 与标准 USD；只接入绑定完整币种及审核代码哈希的白名单，见 [白名单提案流程](docs/TOKEN_ALLOWLIST.md)。 / Development TEST deployment; not a stable release. The test profile admits native AIC and standard USD through an exact-identity/code-hash whitelist; see the proposal workflow.

独立的多代币公钥钱包合约。文本账户可以给任意有效 K1 公钥转币，公钥持有人可以给其他公钥或已存在的文本账户转币。收款本金全额到账，AIC 手续费另外支付。公钥收款人无需先创建链上文本账户。

An independent multi-token public-key wallet contract. Native accounts can deposit to any valid K1 public key; the holder can send to another public key or withdraw to an existing native account. The recipient receives the full principal; AIC fees are additional. A public-key recipient needs no native account or prior registration.

所有币种（包括 AIC）必须先由原生账户购买真实 RAM 并调用 `regtoken` 登记。每个币种有独立的已购字节预算，可由任何账户补充；未登记或新增行预算不足的充值原子回滚。新增收款公钥及余额行费用由本次付款方承担，已有行不重复收取新建费用。见 [登记接口与费用规则](docs/API.zh-CN.md#ram-登记与付款方费用)。

Every asset, including AIC, requires a native sponsor's real RAM purchase and `regtoken` admission first. Each full token identity has its own purchased-byte allocation budget, which any sponsor may top up. Unregistered or over-budget new allocations roll back. The transfer sender pays additional recipient-creation fees; existing rows pay no repeated creation fee. RAM funding does not grant whitelist admission. Arbitrary custom implementations remain outside this profile.

第一版使用链上 PowerUp CPU/NET **资源市场占用**定价，商业中继仅允许已注册、押金正常、未维护且未永久拉黑的 BP。中继收入为基础费用的10%–30%服务费，由中继BP自行设置；向上取整至0.0001 AIC，最低0.0001 AIC，合约每次执行都重新检查资格。用户指令在合约内验证签名，绑定完整原生交易、撤销代次、到期时间、收款方、代币、金额、指定 BP 和总费用上限。

V1 prices fees from on-chain PowerUp CPU/NET **market occupancy**. Commercial relays must be registered, normally bonded BPs outside maintenance and the permanent blacklist. Relays set a service rate of10%–30% of the base fee, rounded upward to0.0001 AIC with a minimum of0.0001 AIC; eligibility is checked on every execution. The contract verifies the owner's signature, binding the complete native transaction, revocation epoch, expiration, destination, token, principal, selected BP and maximum total fee.

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

2026-11-03 最新选择：领取中继收入 `claim` 全额到账，不收 AIC 手续费；领取 BP
承担原生 CPU/NET 并为缺失的收款 AIC 行付 RAM open。所有转账（包括内部公钥转公钥）
继续动态计费。本轮用户授权在远端验证后重建 TEST 链，封存前部署；新 TEST 节点的升级公示期为30分钟，正式版仍为七天。实际生效以执行报告为准。
参见 [cleos 提现语义](docs/CLEOS.md)。

当前 TEST 已通过原生1800秒提案公示升级到可配置10%–30%中继费率，保留免费、全额领取收益。五项费率供浏览器比较，仅创世中继启用HTTP；公式默认折叠。新版SDK锁定具体报价费率，原生cleos兼容旧请求和总额上限。见[实际部署、交易与验证](../../performance-results/machines/intel-xeon-cascadelake--shared-vm--4vCPU-2guestcores--8125768KiB/20261104-relay-rates-r1/REPORT.md)。
