# 五个 BP 停机、黑名单与押金罚没实测

状态：**PASS**。完成时间：2026-11-02T05:29:58.906077+00:00。全部 206 条断言通过；预期攻击拒绝与成功业务交易分别记录在执行 JSON 中，断言数量不等同于独立安全场景数量。

本阶段在 [40 BP＋历史节点原定测试全部通过](../REPORT.md) 后，按用户追加授权实施。链 ID：`9924d9d6405276c9e4cfee86a5b497a19b5c3fa4d576508689884b8be47a0bac`。没有重建链、修改参数或追加 TPS 测量。五个新增节点、原有 40 个 BP 和一个历史节点均属于同一台保留测试服务器上的实例。

## 执行与真实黑名单证据

新增 `koreamoonlit, indiasunrise, icelandcrown, aurorashield, quantumhaven`，各自缴纳 `10000.0000 AIC` 并自行质押、PowerUp。核对规范不可逆注册、同步和真实已签名发现连接；每个账户另留至少 10,000 AIC 流动余额用于重新缴纳的拒绝对照。

保持原生 FIFO 及观察窗口，等五个 BP 同时处于实际出块和最终确认策略且均已有出块记录，再于 **2026-11-02T05:07:13.553823+00:00** 开始仅停止其五个服务。没有以注销或维护代替停机，没有人工写入黑名单。停机前当前 21 个确认成员采用 15 票门槛；停止五个仍保留 16 个健康成员。

每个账户都由三次真实、最终确认的个人零出块窗口进入永久黑名单。逐个用当时原生排班确认窗口责任人，再用规范链前后区块证明该 12 槽窗口没有任何区块。保存证据块、实际决定块及决定块 ID，结算仅在实际决定块不可逆后进行。

| BP | 停机前已有未领取出块数 | 黑名单决定块 | 规范决定块 ID |
|---|---:|---:|---|
| aurorashield | 144 | 46376 | `0000b52876c8c4329ca47c9bd9f02e1c3828bcf3aefa3b70e84fcb6cb98b2240` |
| icelandcrown | 120 | 46448 | `0000b5703fee7f075f26dcd4d5bc6efe276665ac1cf51185ee3182805d23253d` |
| indiasunrise | 84 | 46448 | `0000b5703fee7f075f26dcd4d5bc6efe276665ac1cf51185ee3182805d23253d` |
| koreamoonlit | 48 | 46448 | `0000b5703fee7f075f26dcd4d5bc6efe276665ac1cf51185ee3182805d23253d` |
| quantumhaven | 3 | 46496 | `0000b5a044bb896f398be349781c4196b079e0ba431aec218524a09857e689bd` |

## 资金与准入检查

| 阶段 | `eosio.bond` 实际余额 | 正常押金账 | 待结算罚没 | 已结算罚没 | `eosio.bslash` |
|---|---:|---:|---:|---:|---:|
| 45 BP 已缴纳 | 450,000 AIC | 450,000 AIC | 0 | 0 | 0 |
| 五个真实黑名单决定后 | 450,000 AIC | 400,000 AIC | 50,000 AIC | 0 | 0 |
| 决定不可逆且逐笔结算后 | 400,000 AIC | 400,000 AIC | 0 | 50,000 AIC | 50,000 AIC |

由普通非 BP 账户 `testusaaaaaa` 触发每笔固定方向结算。五个账户各罚没一次；重复结算拒绝。总发行量仍为 `100000000.0000 AIC`，未增发、销毁或把罚没资金转到专用池外。

已验证退款、退出、足额余额下重新缴纳、`regproducer`、`regproducer2`、错误授权、维护进入/恢复绕过和越权池转账均正确拒绝。拒绝前后托管余额及账目保持一致，罚没池已有实际资金后再次验证越权转出仍被拒绝。

拉黑后的本人投票、更换并激活有效 BLS 密钥、更换发现密钥都未恢复候选资格或清除三窗证据；原生发现查询在完成策略交接后仅返回 40 个健康成员。普通自有 AIC 仍可转账。此前实际出块形成的未领取奖励和领取时间记录保持，线上不足 24 小时的提前领取按原规则拒绝。

## 正常功能与最终状态

- 45 个注册 BP、40 个合格 BP、5 个永久黑名单成员；原生生效排班及确认委员会均为 21 人、门槛 15，已替换全部被拉黑成员。
- 其余 40 份押金各 10,000 AIC 未动，无未违规 BP 被拉黑；40 个 BP 和 1 个非出块历史节点共 41 个 nodeos 继续运行。
- 正常备用 BP 实际进入并恢复维护，押金与缺勤记录保持；公开维护数量、合格数量分别正确变化并恢复至维护零、合格 40。
- 所有 40 个健康节点仍有开放且已签名的 BP 发现连接；链头、不可逆高度与历史源持续同步。
- 公开汇总和轮岗历史与规范链一致；五笔结算都在历史 API 中找到同一交易 ID、区块高度/ID 和 `eosio.bond → eosio.bslash` 的 10,000 AIC 真实转账动作。
- 系统合约、CPU/NET、RAM1024、托管账户权限和七天治理保护保持已验证配置；两个托管账户没有可用普通 owner 私钥，罚没池没有提款代码。

## 保留的中断与修复

停机前的第一版观察脚本于 UTC 2026-11-02 00:57:35 读取 `rotcands` 时收到 HTTP 500 并停止，后续故障步骤没有触发，五个新节点仍在线，押金没有重复扣除。同一只读查询随后成功，链头和不可逆高度持续前进。初次响应正文未捕获，确切服务端原因未确定。保留原脚本、失败状态、日志及诊断；第二版仅给只读查询增加四次有上限的重试并保存错误正文，没有自动重发资金交易，也未修改节点、合约或链参数。

接口预检同时保留了ABI变体编码及查询方式错误：`regproducer2` 参数应使用类型名 `block_signing_authority_v0`；普通交易调用批量 `getpeerkeys` 超过既有256字节返回限制，外部只读交易RPC在默认API节点未启用。最终验收使用节点真实 `/v1/net/bp_gossip_peers` 签名通告及开放签名连接；预检已核对45个通告，拉黑交接后核对40个。节点内部发现仍通过控制器只读执行运行，未改变任何链上上限或启停节点模式。

原有45个BP自投和90个普通账户的随机选择、整数票重及45个各不相同的总票数在故障后继续保持。随机1–20笔业务批次、实际tick/note调用和有规范区块ID的空块持续运行；健康BP生产空块没有被当作缺勤。公网历史逐类核对真实转账与两种应用调用。浏览器已上线押金金额和状态，中英文/深浅主题及移动宽度实际Chrome验证另存；实际拉黑后的显示在最终浏览器凭证中记录。

## 随机业务与公网浏览器最终凭证

已完成普通业务批次审计（2026-11-02T05:30:49.613451+00:00）：3990批、41657笔唯一交易，零重复交易ID；其中转账37433笔、tick 2102笔、note 2122笔。1–20的全部批量大小均已出现，发送器实际观察到14021个有规范区块头的空块。后台继续运行，该快照不代表全链空块总数或额外TPS测量。详见 `execution/activity/CUMULATIVE_AUDIT_FINAL.json`。

公网Chrome实际核对4种语言/主题/宽度组合、45行各不相同的真实总票数和自投/他人票数，以及五个实际罚没账户详情。当前押金0与已罚没10,000AIC分别显示，结算状态和决定块可查询；锁定合计400,000AIC、罚没合计50,000AIC准确。脚本错误0，HTTP错误0。详见 `execution/PUBLIC_EXPLORER_AFTER_BAN.json`；浏览器源码库保持私有，未放入公开凭证归档。

## 版本、机器和范围

沿用保留的 Vultr `vbm-8c-64gb-amd`，AMD EPYC 4345P、8 物理核心/16 线程、标称 64 GiB（实际可见 64,880,889,856 字节），Ubuntu 24.04.5 LTS＋打包运行时。CPU16=`3200000`，NET4MiB=`4194304`，每实例状态容量 `16384 MiB`；并行 worker8/batch24/group8/chainthreads6，RAM 初始64GiB、每块1024字节，deferred 两阶段保持禁用。

- 节点源码：`e62a7ea30632ad5069a9640fc6bd0e1fe819b159`。
- 节点 ELF SHA256：`05d62c4a92497153e21833be7d2796d1d1fc9eeb09ebeb4dc1034f41c72ecf67`。
- 合约源码：`b230c7e12d7cd0bc05ad8269140dc030f8b5fe79`。
- WASM SHA256：`b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170`。
- ABI JSON SHA256：`9dab82410be6e25b184c2b7e4d834709d9bb556d3e114ea9504080f6b059f358`。

本阶段是同一主机上的真实节点/链状态功能故障测试，不是多机故障域或吞吐测试。七天退款边界、资源、奖励、维护交接及黑名单对治理否决人数/分母的影响，另有原定 22 项真实原生控制器/WASM 资格证据；线上本阶段不声称已实际运行七天，也未创建升级提案改变治理状态。完整原始观察和日志保留在服务器，哈希见 `execution/REMOTE_RAW_MANIFEST.json`；本地保存白名单内公开凭证及编排源码，不包含私钥、钱包或节点私有配置。

## English

PASS. After the completed 40-BP plus history qualification, five additional independently bonded BPs entered real proposer/finalizer duty and produced blocks before their five services alone were stopped. Each received a genuine permanent ban from three finalized personal zero-output windows. Canonical native policies and block gaps were independently checked.

A normal non-BP account settled exactly 50,000 AIC into the dedicated locked forfeiture pool only after each decision block became irreversible. Rebonding, refund, registration, maintenance, key/vote bypasses, duplicate settlement and unauthorized custody transfers behaved correctly. Legitimate unpaid rewards and ordinary account balances remained usable under their existing rules. Healthy maintenance/resume, signed discovery, history/API lookup, native 21-member/15-vote replacement and continued finality passed. Final state: 45 registered, 40 eligible, five banned; 40 online BPs plus one history node. Same-host functional qualification only; no additional TPS or seven-day wall-clock claim.

公开凭证：[下载归档](five-offline-public-evidence.tar.gz)，SHA256：`8f7d00d39b84a2390a6374af711d63348cd34dcb704c29742a784b235afef5c0`。逐文件哈希见 [EVIDENCE_SHA256_MANIFEST.json](EVIDENCE_SHA256_MANIFEST.json)。
