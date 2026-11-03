# 第一版接口与安全模型

## 资金与费用

合约账号是 `eosio.key`，代币发行合约与钱包托管合约分离。AIC 费用币的发行接口由初始化参数指定；这不是把资金托管进 eosio.token，更不使用 eosio.bond / eosio.bslash。

支持标准 `transfer(from,to,quantity,memo)`、`stat` 和 `accounts` 接口的可替代代币。资产标识包含发行合约与完整 symbol（含精度），同名币不会合并。代币发行方自身仍属于该资产的信任边界：储备检查不能阻止恶意发行方伪造自己的全部数据或日后改变代币代码。不支持扣税、重基、缺少标准通知等改变全额到账语义的代币；检测到差异时整笔交易回滚。

| 操作 | 接口 | 付款方式 |
|---|---|---|
| 文本账号预存手续费 | AIC `transfer` 到钱包，memo=`fee` | 全额计入该账号费用余额 |
| 文本账号转公钥 | 代币 `transfer` 到钱包，memo=`PK1\|PUBLIC_KEY\|费用上限原子单位` | 本金全额给公钥，费用另扣文本账号预存 AIC |
| 公钥转公钥 | `ksend(request,sig)` | 本金与额外 AIC 费从发送公钥扣除 |
| 公钥转文本账号 | `withdraw(request,sig)` | 同上，合约原子转出完整本金 |
| 自助转出 | `selfout(payer,request,sig)` | payer 原生授权提供 CPU/NET；公钥支付基础 AIC 费，无 BP 加价 |
| 取消旧指令 | `cancel` / `selfcancel` | 持有人签名，将 nonce 推进到更大的 next_nonce |
| 领取旧中继收入 | `claim(bp,to,quantity,max_fee)` | BP 自己授权，基础费用另扣其预存费用，不再要求新中继资格 |
| 取回预存费用 | `refundfee(owner,to,quantity,max_fee)` | owner 授权，本金全额转出，基础费用另外支付 |

公钥持有人的手续费来自该公钥在钱包内的额外 AIC 余额。其他币不足时不会自动兑换 AIC。发送 10 个代币、费用 0.1 AIC 时，收款方仍收到完整 10 个代币。AIC 本身作为本金时，发送方必须同时拥有本金和费用。

储备恒等式：非 AIC 资产的实际托管余额 = 其所有公钥余额总和；AIC 实际托管余额 = 公钥 AIC 余额 + 文本账号费用余额 + 基础费用桶 + BP 已赚加价。对外转账必须有标准发出通知、实际储备变化与收款增量；缺失任一步都回滚。检查收款增量发生在标准发送方通知阶段，后续正常收款回调可以使用已收到的代币。

## 动态定价

资源数据来自已锁定代码哈希的 PureEOS 系统合约 `eosio`，`powup.state` 的 **scope 0**。读取 CPU/NET 的 `max(adjusted_utilization, utilization) / weight`，取两者最大值。新租用资源可能先增加当前占用、随后才更新平滑值，这属于正常链上状态。它代表市场资源已售出/调整后占用，**不等于节点服务器 CPU%、实际区块 CPU/NET 使用率或 TPS**。没有 nodeos 新接口和 BP 自报拥堵输入。

全程使用整数，向上收取费用：

```
u = max(floor(max(cpu.adjusted_utilization,cpu.utilization) * 10000 / cpu.weight),
        floor(max(net.adjusted_utilization,net.utilization) * 10000 / net.weight))
multiplier_bps = 10000 + floor((maximum_multiplier_bps - 10000) * u² / 100000000)
base = max(minimum_fee, ceil((unit_fee * work_units + new_row_fee * new_rows) * multiplier_bps / 10000))
commercial_premium = ceil(base * 30 / 100)
total = base + commercial_premium
```

普通存入、自助转出、费用退款与领取收入没有商业加价。网络基础费留在合约费用桶；第一版没有转出或分配基础费的接口。自助路径使用初始化时已创建的固定桶，避免不同资源代付账号制造无限表项；不会把基础费付给代付人。

每个共识时间 epoch 的第一笔收费操作刷新一次价格，其他操作复用缓存；epoch 为初始化选定的 1–60 秒。报价是估算，执行以链上缓存与实际新表项为准，绝不超过用户签署的费用上限。测试费率仅用于功能验证，不是正式经济参数。最低费用、新行费用和存储上限提供第一版的 spam 成本与容量约束，并非已证明能抵御任意经济攻击。

工作单位：存入 `2+ceil(memo字节/64)`，公钥转账 `4+ceil(memo字节/64)`，转出 `6+ceil(memo字节/64)`，取消 `3`，领取/退款 `4`。新公钥钱包、新资产余额行和新资产登记另计新行成本。

## 签名与权限

签名摘要为 ABI 二进制串的 SHA-256：

```
string("PureEOS:eosio.key"), uint32(1), checksum256(chain_id),
name("eosio.key"), checksum256(deployment), name(operation), request
```

字段次序严格按合约 ABI。`ksend` 请求依次为：from 公钥、to 公钥、token_contract、quantity、memo、nonce(uint64)、expires(uint32)、relayer、reward_to、max_fee。`withdraw/selfout` 为 from 公钥、token_contract、to 文本账号、quantity、memo、nonce、expires、relayer、reward_to、max_fee。取消请求为 from、nonce、next_nonce、expires、relayer、reward_to、max_fee。

仅支持 K1。PUB_K1 与旧 EOS 编码归一为同一二进制公钥；新收款公钥额外验证压缩点确实在 secp256k1 曲线上。使用系统的恢复签名原语验证用户指令。nonce 严格单调且永不删除，余额清空不重置。到期时间按共识时间校验，不超过一小时。失败动作的 nonce、费用与余额全部回滚。JS SDK 使用字符串/BigInt 处理 uint64，拒绝不精确的 JavaScript 数值 nonce。

商业中继由父 BP 原生授权登记一个独立子权限；执行必须精确带上这个权限。检查已注册且 active、正常足额 10000 AIC 押金、非永久黑名单、非维护状态、登记启用。用户指定的 relayer 和 reward_to 必须都是同一个 BP。每次执行重新查系统表；换 worker、换服务密钥或重新登记不能绕过父 BP 黑名单。维护等待与已暂停都停止新的商业中继工作。

热中继密钥不得与 owner、active、出块或 BLS 密钥混用。BP 将此子权限 link 到 ksend、withdraw、cancel。中继另外签原生交易支付资源；它的 30% 是服务收入，不承诺净利润。撤销资格不追缴已赚收入，也不新增 BP 罚没条件。

自助请求把 relayer/reward_to 设为空，必须有公钥所有者签名和原生 payer 授权。系统合约代码哈希变化时，商业操作和新本金存入停止，等待适配审查；已有资产的自助转出、取消、旧收入领取和费用退款使用基础倍数继续退出。既有 token 合约失效或 AIC 费用不足仍可能阻止对应操作。

## 查询与中继 HTTP

SDK `KeyWalletClient` 自动读取实际 Chain ID、ABI、配置、nonce、资产标识和余额。签名前使用可信 RPC 与经验证的部署/代码哈希；不要签来源不明的 prepared JSON。配置中 chain_id 在部署时与真实 get_info 对比；合约没有另造节点接口。

表：`config`、`wallets`（bykey SHA256 二级索引）、`balances`（scope=wallet id）、`assets`（byidentity）、`nativefees`、`feebuckets`、`relays`、`pricing`。`receipt` 是仅合约内部可发出的无状态事件，可由历史服务索引；第一版不在 RAM 中保存无限历史。

参考中继 HTTP：`POST /submit` 只接受 operation、request、signature；`GET /status/{intent_digest}` 返回同一原生交易的 pending/submitted/finalized/expired；`GET /health` 返回公开 BP 身份。中继从不接受用户私钥。它在广播前原子保存确切原生报文，重试原报文，扫描实际不可逆区块确定最终结果；超时不等于到账，也不立即换 TAPOS 重签。只有 LIB 时间超过原生交易到期且扫描完成，才标记 expired。

参考实现使用串行提交队列、8 KiB 请求上限、连接/速率限制和 100000 项日志容量。公网需 HTTPS 反向代理。代理侧应按真实客户端配置速率限制；内置服务不信任转发头，所有同一代理连接共享限额。崩溃后的过期 relay.lock 需在确认旧进程停止并检查日志后由运维删除；不能同时运行两个实例共享同一日志目录。
