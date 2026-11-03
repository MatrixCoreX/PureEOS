# 风险控制 / Risk controls

2026-11-03 最新范围：测试白名单为系统 AIC 与标准 `keyusdtoken1::4,USD`，绑定完整币种和审核代码哈希；后续新增通过受保护 eosio.key 代码的原生七天开发者提案/BP 否决流程。第三方文本收款账号必须先付 RAM open。RAM 登记与白名单相互独立；独立远端正常系统74项、替代表夹具74项与SDK/中继14项通过，现已部署公开测试链；旧风险复现保留，稳定版本未更新。详见 [白名单提案流程](TOKEN_ALLOWLIST.md)。

2026-11-03 已选定并实现正常 RAM 分配规则：包括 AIC 在内，每个完整币种先由
原生 sponsor 真实购买 RAM 并原子登记，预算不足拒绝新增行；新增收款记录费由
本次发送方承担，收款本金全额到账。登记仅验证实际出资，不代表代码审核。
这些预算不覆盖发行合约在自己表中借提币原生授权额外计费，因此还须执行独立的代码哈希白名单。

Paid per-token admission now gates ordinary allocation, including AIC. Native
sponsors fund capacity; transfer senders pay extra recipient-creation fees.
Admission proves funding, not reviewed issuer code or native RAM isolation.

历史任意代币版本已复现发行合约借托管授权额外计费 RAM 的风险。当前选择已审核代码准入，完整发行账号/符号/精度与 WASM 哈希均绑定，代码改变后先拒绝再授权。AIC/标准 USD 配置已完成独立远端验证及公开 TEST 部署；这不构成任意代币 RAM 沙箱或稳定版审计结论。

The historical unrestricted profile reproduced shared RAM exposure. The selected AIC/standard USD profile now enforces reviewed identity and current code hashes before authority, passed remote qualification and is deployed on the public TEST chain. Arbitrary issuer code and separate custody isolation remain outside this implementation.

本文是开发阶段的风险清单和验证边界，不能视为零风险承诺或独立审计结论。最终验证状态见同版本 qualification 报告，不能用旧协议测试代替新版本结论。初始顺序 nonce 原型已被协议 2 取代。

This is a development risk register, not a zero-risk guarantee or independent audit. Consult the matching qualification report for actual evidence. The sequential-nonce prototype is superseded by protocol 2.

| 风险 / Risk | 控制 / Control | 仍需注意 / Boundary |
|---|---|---|
| 中继改金额、收款人、币种、费用上限 / Relay tampering | 用户证明绑定完整原生交易 ID、Chain ID、部署盐、版本；链上恢复 K1 公钥 | 用户仍须检查签名前显示的全部字段；不要签来源不明文件 |
| 多设备冲突 / Independent devices | 各自安全随机 32 字节 ID；普通转账不递增 auth_epoch | 总余额不足会按执行顺序拒绝；不保证每笔都成功 |
| 重复广播、压缩或签名变化 / Replay | 原生交易 ID 去重，TAPOS、不可变过期时间；清理与过期无边界空档 | 主动签出新 ID 即授权新付款；不可自动重建超时交易 |
| 同交易 inline 二次使用证明 / Inline proof reuse | 只允许一个根操作、一个 CFA、一个 CFD；禁止 inline 支出 | 第一版不支持把公钥支出作为任意合约组合调用 |
| 无效签名抢占 ID / Invalid proof poisoning | 原生失败交易完整回滚，不登记成功去重状态 | 应区分主观 RPC 限流/拒收与链上拒绝 |
| 错链、错部署 / Wrong domain | 证明域及配置对照；初始绑定实际 Chain ID | RPC/初始化输入仍是信任边界，部署者必须核验代码哈希 |
| 恶意 ABI / Malicious ABI | SDK/cleos 固定协议编码并核对显示字段与根 action 字节 | 仍要可信客户端二进制；原生充值所调用发行合约必须核实 |
| BP 被拉黑后继续中继 / Revoked BP | 每笔商业执行重查有效状态、10,000 AIC 正常押金、维护、黑名单和专用权限 | 不新增罚没条件；备用 BP 也可中继 |
| 中继热密钥泄露 / Relay hot key | 独立子权限，只链接钱包商业操作；不得共用 owner/active/生产/BLS 密钥 | 可消耗该 BP 资源；权限父级原生规则和运维必须正确 |
| 中继拒收、拖延或停机 / Censorship | 原生 payer + 用户证明的 selfout/selfcancel 不依赖 BP 资格 | 仍需资源付款人、额外 AIC、可用链及正常发行合约 |
| 并发余额透支 / Overspending | 链上串行原子账本、精确整数、先检查及扣账再交互 | 并发足额是可用性前提，不存在透支或排队预留承诺 |
| 伪造通知、回调越权 / Forged notifications | 校验实际发行方、from 原生授权、预期出账通知、busy/ticket、结束时储备复核 | 发行方完全伪造其全部表或之后升级代码，不能由托管合约消除 |
| 扣税币、缺失通知、同名假 AIC / Incompatible tokens | 全额到账及储备增减严格校验；资产含发行方和精度；费用只收配置 AIC | 支持标准、无转账税语义的代币，不承诺兼容所有恶意或特殊代币 |
| 储备不足 / Insolvency | 每资产储备与账户余额、费用预存、基础费和中继收入相对应；溢出检查；失败全回滚 | 治理升级和发行方权力依然存在 |
| 费用暴涨、低价 spam / Fees & spam | 有界 PowerUp 占用曲线、非零下限、行费、用户总费用上限 | 市场占用不等于实时 CPU；价格/上限仍需单独经济验证 |
| RAM 耗尽 / Storage exhaustion | 钱包/余额/资产/原生付款人硬上限；新行收费；无永久请求去重表 | 满容量可拒绝新开户/新币种；已有自助退出使用固定费用桶；初始 RAM 必须足够 |
| RPC 超时、分叉 / Unknown outcome | 用户签名前已知 ID；同包重发；仅实际不可逆回执算成功 | HTTP 200/202 或 processed 高度不等于最终到账；RPC 可撒谎，需可信节点复核 |
| 中继崩溃 / Crash recovery | 广播前 fsync、原子替换、目录 fsync；持久保存原包及扫描锚点 | 崩溃遗留锁须人工确认旧进程停止后恢复；备份丢失时以已知交易 ID 查链 |
| 中继被刷 / Service DoS | 8 KiB 请求、连接/队列/速率限制、1 万任务上限；签名先验证 | HTTPS 代理需真实客户端限流；这是有界参考服务，不是高可用商业平台 |
| RPC 凭据外泄 / Credential leakage | 外部 relay 请求独立 HTTP 配置，不转发 RPC headers，独立 TLS 校验 | 本地 keosd、日志、备份及机器本身仍需保护 |
| 管理员直接转走资产 / Administrative custody | 初始化后清理 eosio.key 部署者 owner/active 密钥，保留预定 code 权限并纳入七天治理 | 链治理可升级系统合约，不能承诺对治理无信任 |
| 私钥丢失或被盗 / Owner key loss | 不把私钥给中继；本地签名、复核签名 | 第一版无社交恢复；泄露者可以签出有效转账，撤销代次不能替代换公钥 |
| 网络费用池 / Base-fee pool | 与 BP 收入分账；第一版没有网络费用池转出 action | 费用沉淀，需要以后明确治理/分配策略，不能承诺可提取 |

## 恢复纪律 / Recovery discipline

1. 保留 `prepared.json`、`signed.json` 和其中的原生交易 ID，不保存私钥到提交 JSON。
2. 超时或中继重启后，先查询该 ID 的不可逆回执；只重发原交易。更换中继 worker 不得重新生成正文。
3. 旧交易只有在从签名 reference block 起完整扫描到不可逆时间超过过期时间且未找到回执，才能标记 expired。扫描失败保持未知状态。
4. 需要重新付款时由持有人明确复核并签署新交易，不能由中继自动代办。
5. 保持日志目录 0700、密钥文件 0600。确认进程已停止后才处理遗留锁；不同 worker 使用不同日志目录，通过链上 ID 收敛。

Keep the original prepared/signed envelope and known transaction ID. Retry identical transaction bytes after ambiguous failures. Finality requires canonical irreversible receipts; expiry requires a complete irreversible scan beyond the original expiry. A new owner signature is a new authorization. Never remove a crash lock while its old process may still run.

## 发布门槛 / Release gates

必须记录真实 WASM/ABI/cleos 哈希、源码版本、实际系统 WASM、独立私有链测试结果、预期攻击拒绝和失败尝试。原生系统表测试与替身测试分开。任何关键资金不变量失败，不推广到稳定版本或公开链。性能、长期稳定、多节点共识、经济攻击及第三方独立审计不能用本功能测试代替。

Record exact source/artifacts, actual system WASM, private-chain evidence, expected attack rejections and failed attempts separately. A critical invariant failure blocks release. Functional qualification does not replace throughput, long-duration, multi-node consensus, economic or independent security qualification.

Native recipient token rows newly funded by the wallet also consume the shared balance-row budget and pay the new-row fee; closing/reopening them cannot reset that budget. Initial limits must fit purchased RAM with an operating reserve. Malicious issuers that fabricate state or abuse native RAM billing remain outside the standard-token trust profile.

原生收款方首次建立代币余额行、且由钱包承担 RAM 时，也计入共用余额行预算并收新行费；关闭再重建不会重置预算。初始化上限须按实际已购买 RAM 留出运行余量。恶意发行方伪造状态或滥用原生 RAM 计费仍属于发行方信任边界。

Reference relay admission: at most 128 pending jobs, 8 per holder, 10,000 durable jobs total and 3 automatic broadcasts per immutable transaction. Reconciliation continues after retry exhaustion; unknown outcomes never become an automatic new authorization. It accepts the configured fee issuer by default. Set `RELAY_ALLOWED_TOKENS=keyusdtoken1,...` only after operator review for additional issuers. Operator issuer review can only narrow the compiled on-chain whitelist; it cannot add an unapproved identity or code hash. Independent-device signing is unchanged.

参考中继最多保留 128 笔待定任务、每公钥 8 笔、总日志 1 万笔；每原始交易最多自动广播 3 次，之后继续查不可逆回执，不自动生成新授权。默认接收配置 AIC 发行合约，其他发行方由运营者审核后写入 `RELAY_ALLOWED_TOKENS`。这些是中继服务限额，不改变链上多币种支持或多设备签名规则。

Authority boundary: the contract enforces the registered permission identity and BP eligibility. It cannot introspect the native permission tree. The reference relay additionally verifies active-parent, single-key/threshold-1 child authority, no account/wait bindings and separation from owner/active/production keys. Direct operators must preserve this policy; registering a permission name alone is not proof of good key custody.

权限边界：合约强制匹配已登记权限并检查 BP 资格，但不能直接读取原生权限树。参考中继额外核验 active 下的单密钥、阈值 1 子权限，无账户/等待绑定，并与 owner/active/出块密钥分离。直接运行中继的 BP 也须保持该设置，登记一个权限名称不等于已证明其密钥管理安全。

[代币托管修复方案 / Custody options](TOKEN_CUSTODY_REVIEW.md)

历史任意代币版本实际复现：正常系统 WASM 的私有链上，`eosio.key` 提取测试恶意币时额外被计费 **4,330 字节 RAM**，`balance_count` 未变化，四种资产的储备等式仍成立。此项是已确认的资源隔离失败，见 [FUNCTIONAL_R4.json](../../../performance-results/eosio-key/20261103-v2-security/FUNCTIONAL_R4.json) 中 `unreviewed_issuer_RAM_exposure` 和 `release_qualified=false`。

The normal-system wallet itself reproduced the extra **4,330-byte** charge without changing its row counter, while all four reserve equations remained valid. This is a confirmed resource-isolation failure. Functional execution PASS does not mean release approval.
