> Historical profile document. For the 2026-11-05 atomic-RAM candidate, use [current Chinese rules](ATOMIC_RAM_TRANSFERS.zh.md) or [current English rules](ATOMIC_RAM_TRANSFERS.en.md). Prepaid fees, PK memo deposits and manual ordinary-transfer opening are superseded; public activation remains pending.

# RAM 市场计费 / RAM market pricing

2026-11-04 开发及 TEST 激活完成。提案2完整公示1800秒后，经原生 devexec 不可逆启用 `bc3358d79a64a42fcef367079ce77bcc261b51a39ff73481880d7f5bbf35b6b1`；ABI不变。旧 `b6509d…` / `1933629d…` 仍按定额行费计价。SDK 与浏览器按审核代码哈希选择规则，未知版本不报价。实际升级交易与三笔收费验证见实际Cascadelake机器的 `20261104-dev-proposals-ram-activation-r1/REPORT.md`。

## 计价规则

所有金额使用 AIC 最小单位（0.0001 AIC），全程整数计算，`ceil` 表示向上取整。

```text
B = 本笔核定新增字节预算
R = eosio::rammarket 中 base.balance 的 RAM 字节储备
Q = 同一行 quote.balance 的 AIC 原子数储备
RAM_net = B == 0 ? 0 : ceil(Q * B / (R - B))
RAM_fee = ceil(RAM_net * 200 / 199)
execution = ceil(unit_fee * work_units * CPU_NET_multiplier_bps / 10000)
base = max(minimum_fee, execution + RAM_fee)
relay = commercial ? max(1, ceil(base * relay_fee_bps / 10000)) : 0
total = base + relay
```

`RAM_fee` 包含系统买 RAM 时 `ceil(gross/200)` 的 0.5% 手续费。它是按已提交市场储备计算的保守买入报价，并非每笔转账执行一次买入；既不调用 `buyram`，也不调用 `buyrambytes`。系统买卖和供给更新提交新储备后，下一笔新增存储报价即使用新储备，不缓存 RAM 价格。CPU/NET 继续使用原有按共识时间缓存的有界平方倍率，且仅乘执行项。市场价格受曲线影响，并非固定每字节单价或全区间线性价格。

| 场景 | B |
|---|---:|
| 新公钥钱包及首个币种余额 | 1536 |
| 已有公钥首次接收该币种 | 512 |
| 已有该币种余额记录 | 0 |
| 允许的 AIC 命名收款账户缺失余额行 | 512 |
| USD 命名收款账户缺失余额行 | 拒绝，收款方须先 open |
| claim 领取收益 | 应用费为零，领取人先付原生 open RAM |

这些数值来自既有钱包 1024 字节、余额 512 字节的保守分配预算，不是原生 `ram_usage` 精确测量。新币种登记、原生费用预存等既有预算规则保持不变。发送人额外支付费用，收款本金全额到账。市场计价不补充每币 `ram_purchased`；预算不足仍原子拒绝，正常交易不会自动买 RAM，归零或关闭余额行不会返还预算。

当前 `minimum_fee=100`，即 0.0100 AIC。RAM 降价可减少存储项，但执行项与存储项之和低于该值时会补足，因此总费用可能保持不变。中继费率仍为 10%–30%，服务费最低 0.0001 AIC。没有降低保底费、单位执行费或更改系统资源参数。

## 兼容与保护

- 不修改既有 config、余额、储备、预算、费用桶和签名请求的序列化布局；无需数据迁移。`new_row_fee` 保留为旧版兼容字段，新 WASM 忽略其数值；初始化仍接受原有策略结构。
- 原签名 `max_fee` 始终约束总费用；执行时价格上涨超过上限则整笔回滚。中继费率的签名绑定与旧 cleos 的 30% 上限兼容继续适用。
- 非零预算只读取已审核系统哈希下的 `RAMCORE` 市场，核对 RAM/AIC 精度、正储备和 0.5 权重，防止缺表、错误币种、耗尽储备或溢出被当成低价。
- 不需要新增 RAM 的路径不读 RAM 市场。系统代码变化时，既有接收余额行的自助退出继续使用原先执行费兜底；缺失接收行时不能读取未审核市场，必须先由原生付款人 open。
- `claim` 不调用报价、不刷新定价、不扣应用费；代币白名单、USD 预先 open、权限、不可逆回执和储备等式均保留。
- 浏览器默认折叠公式，显示 0/512/1536 字节预算、市场存储项和保底补足。新规则只随对应合约代码启用，不把旧公网合约展示成市场计费。

## English

This TEST-activated version replaces the fixed new-row charge with the integer RAM curve quote above. All values are AIC atoms. The approved allocation budget is 1536 bytes for a new key plus balance, 512 for a new balance or supported native AIC recipient, and zero for existing balances. The quote includes the native rounded 0.5% RAM buy fee; transfers do not purchase RAM. These are conservative allocation budgets, not exact measurements of native RAM usage.

RAM reserves are read afresh for each nonzero-byte charge from the reviewed system `rammarket`; CPU/NET retains its existing epoch cache and multiplier, applied only to execution. RAM price changes affect storage charges, subject to the unchanged 0.0100 AIC base floor. Relay fees remain 10%–30% with upward rounding and a one-atom minimum. Signed total fee caps still reject over-budget execution atomically.

Existing table/request wire layouts are preserved. The serialized `new_row_fee` field remains for legacy profiles and is ignored by the market-priced WASM. The SDK and explorer select reviewed code profiles and reject unknown pricing code. New pricing is not an automatic RAM replenishment mechanism: separately sponsored per-token budgets, allocation limits, exact token-code whitelist and USD named-recipient pre-open remain mandatory. Free income claims remain independent of both markets, with claimant-paid native resources and recipient opening.

Qualification runs remotely on an isolated private chain, with real native RAM buy/sell observations plus clearly labeled malformed-market fixtures. The same qualified bytes completed native TEST proposal2 and irreversible execution b029b9947a608a679baf6445a0e2d106fe2bc8601e59db58a98f5d963f45ff33 after the full review. See the deployment report and actual code hash; production release defaults are unchanged.

SDK/relay may use a separate readEndpoint / NODEOS_READ_URL for queries. Broadcast remains on the configured writer; both chain IDs and the writer code pin are checked. Default single-endpoint behavior is preserved. This is not automatic transaction retry.
