# 第一版 API · 签名协议 2

2026-11-03 最新范围：测试白名单为系统 AIC 与标准 `keyusdtoken1::4,USD`，绑定完整币种和审核代码哈希；后续新增通过受保护 eosio.key 代码的原生七天开发者提案/BP 否决流程。第三方文本收款账号必须先付 RAM open。RAM 登记与白名单相互独立；独立远端正常系统74项、替代表夹具74项与SDK/中继14项通过，现已部署公开测试链；旧风险复现保留，稳定版本未更新。详见 [白名单提案流程](TOKEN_ALLOWLIST.md)。

`eosio.key` 是代币托管账户，代币仍由原发行合约发行。资产身份为「发行合约 + 精度 + 符号」，同名假 AIC 不能支付真实 AIC 手续费。标准代币的本金全额到账，手续费额外用初始化指定的 4 位 AIC 支付。发行方仍能改变其代币逻辑；储备检查无法证明恶意发行合约所有表均真实。

| 操作 | 接口 | 费用 |
|---|---|---|
| 费用预存 | AIC transfer，memo `fee` | 全额记入原生发送者费用余额 |
| 文本账户给公钥 | token transfer，memo `PK2\|公钥\|费用上限原子数\|随机32字节十六进制ID` | 本金全额给公钥；额外扣原生发送者预存 AIC |
| 公钥给公钥 | `ksend(request)` | 发送公钥承担本金及额外 AIC |
| 公钥给文本账户 | `withdraw(request)` | 文本账户必须已经存在 |
| 持有人自助转出 | `selfout(payer,request)` | 原生 payer 提供 CPU/NET；公钥付基础 AIC 费，无 30% 加价 |
| 撤销未执行授权 | `cancel` / `selfcancel` | 将 auth_epoch 提升到更大 next_epoch |
| 提取过去中继收入 | `claim` | 已验收TEST：全额到账、无AIC应用手续费；领取BP承担原生资源及收款行RAM |
| 退回费用预存 | `refundfee` | 原生费用所有者授权 |

## RAM 登记与付款方费用

所有币种（包括 AIC 本金和 AIC `fee` 费用预存）都先调用
`regtoken(payer, token_contract, token_symbol, min_bytes)` 登记。
任意原生账户均可授权购买真实 RAM；收款公钥不需要开户、签名或提供 RAM。

一次原子交易必须依次且仅包含两个普通根 action，均由同一 payer 授权：

1. `eosio::buyram(payer, receiver=eosio.key, quant)`：payer 同意支付确切的 AIC 金额。
2. `eosio.key::regtoken(payer, token_contract, token_symbol, min_bytes)`：完整币种例如 `keyusdtoken1` / `4,USD`，最低字节数至少 4096。

合约只根据真实 `eosio::logbuyram` 的实际字节记账，不能用声明、转入金额或过去赠送的 RAM 冒充。
购买不足、精度错误或登记失败时两步一起回滚。同一接口用于追加预算，其他账户也可出资；
首次 sponsor 保存在 `ram_sponsor`。RAM 归 eosio.key 使用，当前没有 sponsor 退款、卖出或注销接口。

每币 `ram_purchased` 为累计实际购买字节，`ram_used` 为保守分配预算，不是原生实际 `ram_usage`。
每币预留 2048 字节覆盖登记及托管余额行；新公钥分配 1024 字节；新内部币种余额行或原生
收款余额行分配 512 字节；新原生费用付款人分配 1024 字节覆盖费用余额及费用桶。
预算不足拒绝新分配，已有行仍能接收，不需要新增行的退出仍可执行。
余额归零或原生接收方关闭再打开余额行不会返还预算。

登记人的预购预算保证容量；每次新增收款记录费由本次付款方额外承担：

| 路径 | 新增收款记录费付款方 | 收款方 |
|---|---|---|
| 文本账户充值公钥 | 发送账户自己的预存 AIC | 本金全额入账 |
| 公钥转公钥 | 发送公钥自己的 AIC | 本金全额入账 |
| 提币需建立原生 token 余额行 | 发送公钥自己的 AIC | 本金全额到账 |

已有记录不重复收新建费用，普通执行费仍适用。`new_row_fee` 属于初始化费用策略，
不是实时 RAM 市场逐字节报价，正常手续费也不会自动补充字节预算。
此登记只控制正常分配，恶意发行合约借提币授权额外计费的安全阻断仍保留。
SDK `ramRegistrationActions(payer, token, symbol, ramPayment, {minimumBytes})` 返回上述两项 action。
原生调用示例见 [CLEOS.md](CLEOS.md)。

## 多设备并发和重复提交

不再使用每笔递增 nonce。设备各自生成安全随机 request_id，使用当前 auth_epoch 准备完整原生交易并本地签名。同一代次的不同交易可以并发，普通转账不改代次；资金不足仍按链上执行顺序拒绝，失败原子回滚。只有明确的撤销操作提高代次。撤销前已经执行的交易无法追回。

完整交易只包含一个无状态 `prove` context-free action 和一个普通根 action。持有人签名放入唯一的 CFD 条目；合约通过原生 assert_recover_key 验证。签名域是 `PureEOS:eosio.key:transaction`、协议版本 2、Chain ID、合约名、部署盐及原生交易 ID。金额、接收方、代币、费用上限、BP、资源付款权限、TAPOS 和到期时间均被绑定。根操作禁止 inline 调用，避免一个交易内重复使用证明。客户端使用固定协议结构编码，不使用不可信 RPC ABI 决定签名字节。

原生节点在有效期内按交易 ID 去重；签名或压缩变化不改变该 ID。去重记录仅在 `now > expiration` 时清理，而交易必须满足 `expiration >= pending_block_time`，不存在过期边界空档。清理后旧交易已经过期，不能重新执行。改交易 ID 需要持有人新签名。不得在超时后自动换 ID、TAPOS 或到期时间并重新签名，应查询同一个已知交易 ID 的不可逆回执。用户主动签出的新交易是新付款。

## 动态费用和中继资格

使用 eosio 的 `powup.state` 表、scope **0**，取 CPU/NET 两者较大的 `max(utilization, adjusted_utilization)/weight`，再按初始化的有界平方曲线计算基础费，向上取整。商业中继加收基础费 30%，原子数向上取整。它代表资源市场占用，不是区块实际 CPU、宿主机 CPU 或 TPS。报价是估计，实际扣费不得超过用户签名上限。

每次商业执行重查：注册且有效 BP、正常足额 10,000 AIC 押金、未拉黑、无维护状态、中继启用、指定专用子权限授权。收益只记给选定 BP，换 worker 或密钥不能绕过 BP 黑名单。候补 BP 也可做中继。自助退出不依赖 BP，但仍需要原生资源付款人及额外 AIC；私钥丢失、链停机或代币合约故障不在该路径的恢复能力内。

网络基础费与 BP 收益分账；第一版没有网络费用池转出接口。钱包/资产行收费且有数量上限，余额归零不删除身份或撤销代次；不在 RAM 保存无限请求历史。AIC 储备必须覆盖公钥余额、原生费用预存及所有费用桶，其他资产覆盖相应公钥余额。异常通知、扣税、缺失通知及回调越权导致整笔回滚。

## HTTP 和查询

`POST /submit` 只接收 `{prepared, owner_signature}`；`GET /status/{原生交易ID}` 返回 pending/submitted/finalized/expired。中继必须先落盘原始交易再广播，重试同一份，扫描不可逆区块才确认。不能把 RPC 的预执行响应当最终到账。服务限制请求体、连接数、队列、访问频率与日志容量，公网入口需 HTTPS。崩溃遗留锁须确认旧进程停止后恢复，不能多个进程共享同一日志。

[完整字段、公式和英文说明](API.en.md) · [原生 cleos](CLEOS.md) · [风险矩阵](RISKS.md)

Native recipient token rows newly funded by the wallet also consume the shared balance-row budget and pay the new-row fee; closing/reopening them cannot reset that budget. Initial limits must fit purchased RAM with an operating reserve. Malicious issuers that fabricate state or abuse native RAM billing remain outside the standard-token trust profile.

原生收款方首次建立代币余额行、且由钱包承担 RAM 时，也计入共用余额行预算并收新行费；关闭再重建不会重置预算。初始化上限须按实际已购买 RAM 留出运行余量。恶意发行方伪造状态或滥用原生 RAM 计费仍属于发行方信任边界。

Reference relay admission: at most 128 pending jobs, 8 per holder, 10,000 durable jobs total and 3 automatic broadcasts per immutable transaction. Reconciliation continues after retry exhaustion; unknown outcomes never become an automatic new authorization. It accepts the configured fee issuer by default. Set `RELAY_ALLOWED_TOKENS=keyusdtoken1,...` only after operator review for additional issuers. Operator issuer review can only narrow the compiled on-chain whitelist; it cannot add an unapproved identity or code hash. Independent-device signing is unchanged.

参考中继最多保留 128 笔待定任务、每公钥 8 笔、总日志 1 万笔；每原始交易最多自动广播 3 次，之后继续查不可逆回执，不自动生成新授权。默认接收配置 AIC 发行合约，其他发行方由运营者审核后写入 `RELAY_ALLOWED_TOKENS`。这些是中继服务限额，不改变链上多币种支持或多设备签名规则。

## 2026-11-03 最新选择：领取中继收入免 AIC 手续费

`claim` 全额支付已赚 AIC，收据 fee为0，不读取/扣减/新建nativefees，不改网络费桶和价格缓存。
BP 原生签名承担 CPU/NET；收款账号缺 AIC 余额行时，由领取 BP 原生授权付 RAM open，
可在同一原子交易先open再claim。合约拒绝缺行，不垫付 eosio.key RAM 或额外预算。
`max_fee` 字段兼容保留，可填0.0000 AIC；不改ABI/表布局、收益归属、储备和原子回滚。
所有转账（包括内部公钥转公钥）仍收动态 AIC 手续费，商业中继加价保留。
此前收益内扣费方案已取代。本轮授权远端验证后重建 TEST 链并在封存前部署；新 TEST 节点公示1800秒，正式版仍604800秒。其他已封存链必须遵守各自原生治理期限。
