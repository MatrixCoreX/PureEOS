# 最终关闭状态 / Final closure

154/154用例已通过，包括BAN-07/BAN-10三机补测及归档、逐笔、源码基线、本地MD同步和服务器清理。额外两台已删除且API确认不存在，原主服务器保留。新发行已发布，14个发行资产下载检查SHA256均一致，见PUBLICATION_VERIFIED.json。

# PureEOS 三机功能验收 / Three-host functional qualification

本报告由真实用例、逐笔区块审计和归档校验生成。154项全部闭环，基线、本地MD及两台额外服务器清理已有关闭回执；正式发行另有上传/下载摘要核验回执。

节点源码 `00443330a0d29673700a4eebee78de3b0234c281`；nodeos SHA256 `e7776703f09bec15e8476279e76075ac8e2af3cd94e44ece16c3607859ba2f32`。
系统源码 `6f3160ed23b725e8cd97790f7a0e9042afbfdc9a`；WASM `5f25a25816fc6db145dbd811d9c24dd99c9b9b1fd260b8e16a045476d0c3c11a`。

三台 EPYC4345P，每台8物理核/16线程/64GiB；Ubuntu24.04.5宿主及匹配glibc2.44运行环境。30实例共享三台机器，仅功能/拓扑验证，没有新测TPS，也不是30台独立服务器性能验收。

固定CPU16/NET16MiB、P2P64MiB/消息、1GiB队列、单笔CPU/NET、完整验证和计费保持。RAM初始64GiB并通过setramrate(1024)初始化；deferred两阶段禁用。单选投票、无代理、10%/90%资源、奖励减半和14天治理保持。

BAN-07/BAN-10原先按用户要求排除，用户随后明确要求补测，现已纳入实机验收：无合格候补时，一个BP经三个真实已最终确认个人零窗口永久封禁，旧政策正常交接至20人、门槛14；被封账户离开生效政策。20人政策仅14名有效确认者在线时持续确认，13票时停止新最终确认；短断和超过126秒长断恢复，旧证据及永久ban不变。恢复候补后正常回到21人/15票。该补测覆盖20人政策，不能冒充对1至20所有人数和所有扩容故障组合的实机验收。

人数取当前原生政策，不取开机数。交接阶段必须满足原生旧/新政策的确认要求；不能提前套用目标政策较低门槛。永久ban取消未来资格，席位在完成政策移除后才从有效委员会中扣除。少于21时暂停新增个人缺岗证据，已有有效违规和ban保留。

全网/最终确认观察中断达到252slot=126秒（含边界）暂停新增个人缺席证据；恢复后重新观察完整窗口。该保护不降低共识门槛。健康完整委员会的三个连续已最终确认个人零窗口仍永久封禁；维护须完成正常交接才生效。

验收区分live/native/component、成功业务、预期拒绝、无效通知及原始失败。当前ABI九个账户共231项，入口覆盖不等于所有业务配置都有成功正例；废弃deferred入口仍排除。Bancor采用单独版本的授权修复，原上游失败保留，不冒充未修改上游。

Fresh campaign began on4ede04b47. The only final node patch changes peer-proof signing exception handling. Unchanged chain/core source and linked objects were verified; current network/security/rolling/runtime/startup/sub21 regressions use00443330. Individual evidence retains its actual source/hash. No claim that every native test ran as a live node or every earlier result used the final node binary.

| 用例 | 状态 | 范围 | 操作 |
|---|---|---|---|
| ENV-01 | PASS | live30 | 核对三台实例ID、EPYC4345P、8物理核16线程和MemTotal |
| ENV-02 | PASS | live30 | 逐机核对源码、Node二进制、系统WASM/ABI哈希 |
| ENV-03 | PASS | live30 | 读取完整链参数和配置 |
| ENV-04 | PASS | negative-startup-private-mounts-current-binary | 低核/低线程/低内存/未知拓扑和显式纯串行启动 |
| ENV-05 | PASS | live30 | 检查链ID、数据目录、端口、密钥和finalizer safety文件 |
| ENV-06 | PASS | actual-live-ABI-boundaries-and-explicit-native-real-WASM-action-traces | 列出完整系统ABI和第三方ABI动作 |
| KEY-01 | PASS | live30 | 部署候选系统合约并读取链上code hash、ABI |
| KEY-02 | PASS | live30 | BP使用自己的账户权限登记K1公钥 |
| KEY-03 | PASS | live30 | 其他账户替BP登记或删除公钥 |
| KEY-04 | PASS | live30 | 非注册候选且非当前权威账户登记公钥 |
| KEY-05 | PASS | live30 | 重复登记、空公钥、WebAuthn公钥 |
| KEY-06 | PASS | live30 | 登记R1、轮换、用正确当前key删除 |
| KEY-07 | PASS | live30 | 用轮换前旧key删除新key，重复删除不存在记录 |
| KEY-08 | PASS | live30 chain plus separate parallel API observer with20ms read-only budget | 调用实际getpeerkeys只读入口并比较状态哈希 |
| KEY-09 | PASS | live30 | 用真实自购RAM、自己质押/投票、提前PowerUp准备账户 |
| KEY-10 | PASS | live30 | 核对治理封存、单选投票、轮换和双权威名单 |
| GSP-01 | PASS | live30 | 启动A/B/C，A只填B、C只填B |
| GSP-02 | PASS | live30 | 观察C从B学到A并主动建连 |
| GSP-03 | PASS | live30 | 观察A学到C并互传低频业务 |
| GSP-04 | PASS | live30 | 重复发送同一合法地址记录 |
| GSP-05 | PASS | live30 | 新实例冷启动只给一个健康种子 |
| GSP-06 | PASS | live30 | 停止一个种子但保留其他已发现健康连接 |
| GSP-07 | PASS | live30 | 全部种子不可达后再恢复一个 |
| GSP-08 | PASS | live30 | BP正常变更可达端口并更新签名广播 |
| GSP-09 | PASS | live30 | 观察备用BP及待生效BP的连接 |
| GSP-10 | PASS | live30 | 保留数据库和safety文件重启一个节点 |
| SEC-01 | PASS | live30 | 把一个BP轮换到预置R1 peer-key |
| SEC-02 | PASS | live30 | 轮换后重放旧key签名地址 |
| SEC-03 | PASS | live30 | 删除peer-key后重放原记录 |
| SEC-04 | PASS | live30 | 正常重新登记该BP公钥 |
| SEC-05 | PASS | live30 | 修改已签名记录的地址、账户或有效期 |
| SEC-06 | PASS | live30 | 把已有签名地址记录改成空initial消息 |
| SEC-07 | PASS | live30 | 重放其他chain ID签名 |
| SEC-08 | PASS | live30 | 过期和过远未来有效期记录，以及正常续期 |
| SEC-09 | PASS | live30 | 同一BP恰好8个地址和第9个地址 |
| SEC-10 | PASS | live30 | 格式损坏、非法地址、重复条目和排序异常 |
| SEC-11 | PASS | live30 | 同时改变名单、地址和密钥 |
| SEC-12 | PASS | live30 | 本机缺少当前peer私钥或链上公钥被删 |
| SEC-13 | PASS | live30 | 测量撤销到各节点生效的区块与时间 |
| M30-01 | PASS | live30 | 生成30份独立身份、端口、目录和safety文件 |
| M30-02 | PASS | live30 | 依次从3加入到10、21和30节点 |
| M30-03 | PASS | live30 | 每节点仅配置1至2个种子 |
| M30-04 | PASS | live30 | 同IP多端口和跨主机互联 |
| M30-05 | PASS | live30 | 逐实例检查参数与硬件/并行日志 |
| M30-06 | PASS | live30 | 观察节点通过正常注册、质押和投票成为候选 |
| M30-07 | PASS | live30 | 记录21名当前委员会与9名候选/观察者连接 |
| M30-08 | PASS | rolling-parent-to-fixed-node-compatibility | 分批滚动重启并保留状态 |
| M30-09 | PASS | live30 | 低频采样每实例RSS/CPU/FD/线程/连接/磁盘 |
| NET-01 | PASS | live30 | 短暂切断普通P2P连接再恢复 |
| NET-02 | PASS | live30 | 隔离BP网络但保留进程 |
| NET-03 | PASS | live30 | 停止BP进程，再用原目录恢复 |
| NET-04 | PASS | live30 | 隔离少数节点，主连通分区仍满足实际门槛 |
| NET-05 | PASS | live30 | 制造两侧都低于实际确认门槛的分区 |
| NET-06 | PASS | live30 | 恢复分区后低频提交新业务 |
| NET-07 | PASS | live30 | 少量延迟、单向异常和重复连接 |
| NET-08 | PASS | live30 | 单次错误chain ID握手、坏签名或超限长度头 |
| NET-09 | PASS | live30 | 功能量级下检查发送队列、断连和追块 |
| CON-01 | PASS | native-validation-and-explicit-linked-live-checks | 核对正确时隙、生产者和区块签名 |
| CON-02 | PASS | live30 | 读取门槛Q，分别使在线权重为Q和Q-1 |
| CON-03 | PASS | live30 | 正常投票导致待生效与当前名单变化 |
| CON-04 | PASS | live30 | 名单交接同时低频转账 |
| CON-05 | PASS | native-validation-and-explicit-linked-live-checks | 分别轮换出块键、BLS键和peer-key |
| CON-06 | PASS | live30 | 待生效BP很晚才广播端点 |
| CON-07 | PASS | native-real-WASM-and-explicitly-linked-live-evidence | 双策略交接中重启一个节点 |
| CON-08 | PASS | native-real-WASM-and-explicitly-linked-live-evidence | 无替补、交接未完成或候选观察失败 |
| BAN-01 | PASS | 30-node3-host chain; one active BP intentionally offline,29 queried | 21人中一个BP错过一个完整个人窗口 |
| BAN-02 | PASS | 30-node3-host chain; same one active BP offline,29 queried | 同一BP连续两个可结算个人窗口零产出 |
| BAN-03 | PASS | native-real-WASM-and-explicitly-linked-live-evidence | 无合格替补时连续三个个人窗口零产出 |
| BAN-04 | PASS | live30 | 两个零窗口后在下一个个人窗口正常产出 |
| BAN-05 | PASS | 30-node3-host live,21-member policies; one incumbent offline | 有备用时出现一次已确认零产出窗口 |
| BAN-06 | PASS | live failed entrant3 finalized personal windows; healthy20 maintain Q15 | 永久ban后有替补 |
| BAN-07 | PASS | live30 on3physical hosts; user-authorized21to20 no-standby permanent-ban eviction | 永久ban后无替补 |
| BAN-08 | PASS | live30 | 已ban账户重连、换IP、换peer-key或BLS键 |
| BAN-09 | PASS | live30 | 已ban账户注销重注册、改票、再质押 |
| BAN-10 | PASS | live30 on3physical hosts; effective20:14votes continue,13votes stop;20s/130s recovery | 少于完整21人委员会时短断/长断 |
| BAN-11 | PASS | native-real-WASM-and-explicitly-linked-live-evidence | 全链/最终确认暂停达到pause_guard阈值-1、等于、+1 |
| BAN-12 | PASS | live30 | 约三个个人轮次的全局暂停后恢复 |
| BAN-13 | PASS | native-validation-and-explicit-linked-live-checks | 21到21名单交接、key-only变化和未完成窗口 |
| BAN-14 | PASS | live30 | 没有新最终确认依据，仅本地时钟或RPC变慢 |
| BAN-15 | PASS | current-binary live30 plus one isolated parallel snapshot/read-only observer, same3 physical hosts | 新节点追块及快照恢复黑名单状态 |
| BAN-16 | PASS | native-real-WASM-and-explicitly-linked-live-evidence | 被ban候选排名占位与容量边界 |
| BAN-17 | PASS | live30 | P2P连接关闭与链上永久黑名单分别核对 |
| BAN-18 | PASS | live30 | ban后的gossip连接与权威交接顺序 |
| SYS-01 | PASS | live30 | SYS及TKN转账、余额不足、越权和重复提交 |
| SYS-02 | PASS | live30 | 自购RAM、开户/关行、退RAM和未授权payer |
| SYS-03 | PASS | live30 | 读取初始64GiB和setramrate1024后跨块核对 |
| SYS-04 | PASS | native-real-WASM-resource-clock-and-live-fixture | 自己质押、委托、解押和当前退款流程 |
| SYS-05 | PASS | native-real-WASM-resource-clock-and-live-fixture | 提前PowerUp、租期内使用、续租与到期再租 |
| SYS-06 | PASS | native-real-WASM-and-explicitly-linked-live-evidence | 单选投票、改票、撤票、无效多选及代理边界 |
| SYS-07 | PASS | live-role-lifecycle-plus-native-real-WASM-eligibility | 注册/注销生产者、最终确认键及资格不完整 |
| SYS-08 | PASS | native-real-WASM-and-explicitly-linked-live-evidence | 奖励、手续费、供应量和奖励减半边界 |
| SYS-09 | PASS | live30 | developer提案、固定审核期、veto、取消及升级 |
| SYS-10 | PASS | live30 | 系统敏感动作越权及旧入口限制 |
| SYS-11 | PASS | live30 | 当前支持的msig/自定义权限/权限链接 |
| SYS-12 | PASS | live30 | 完整系统ABI逐项核对动作覆盖 |
| DEF-01 | PASS | live30 | 部署真实token/converter/network并初始化 |
| DEF-02 | PASS | live30 | 开行、储备、测试资产与权限准备 |
| DEF-03 | PASS | live30 | 正反向兑换及多合约路由 |
| DEF-04 | PASS | live30 | 同符号异合约假币和伪造transfer通知 |
| DEF-05 | PASS | live30 | 余额不足、坏memo、错误接收者、最小输出/滑点边界 |
| DEF-06 | PASS | live30 | 极小数量、精度差、零/负数及整数边界 |
| DEF-07 | PASS | live30 | 固定版本实际支持的充值/兑换/提现或赎回 |
| DEF-08 | PASS | live30 | 跨合约inline、回调及重复消费尝试 |
| DEF-09 | PASS | live30 | 兑换后续子动作故意失败 |
| DEF-10 | PASS | live30 | 名单切换、重连和新节点追块后核对兑换 |
| NFT-01 | PASS | live30 | 部署并建立collection/schema/template |
| NFT-02 | PASS | live30 | 铸造、转让、允许的数据修改及销毁 |
| NFT-03 | PASS | live30 | 无权限、伪造通知、重复ID和错误scope |
| NFT-04 | PASS | live30 | offer创建/接受/取消/失效 |
| NFT-05 | PASS | live30 | 部署匹配AtomicMarket并配置市场和测试代币 |
| NFT-06 | PASS | live30 | 挂单/购买/取消/拍卖/报价实际支持流程 |
| NFT-07 | PASS | live30 | 版税、费用分配和取整边界 |
| NFT-08 | PASS | live30 | 旧订单、重复成交和伪造支付 |
| NFT-09 | PASS | live30 | 结算子动作失败 |
| NFT-10 | PASS | live30 | 节点重启、跨节点读取和追块 |
| NFT-11 | PASS | live30 | 固定版本已废弃或本链停用的接口 |
| VM-01 | PASS | live30 | 普通/inline/context-free等当前支持动作 |
| VM-02 | PASS | live30 | 主表、二级索引、scope、迭代和RAM payer |
| VM-03 | PASS | live30 | require_auth/require_recipient/通知/get_sender |
| VM-04 | PASS | live30 | 多合约嵌套、深度边界、返回值和只读 |
| VM-05 | PASS | live30 | 支持的哈希、签名验证和BLS等密码学入口 |
| VM-06 | PASS | live30 | WASM整数/浮点/内存边界和异常 |
| VM-07 | PASS | live30 | CPU/NET/内存/动作尺寸单笔边界 |
| VM-08 | PASS | live30 | 非冲突调用、真实冲突和不支持能力回退 |
| VM-09 | PASS | live30 | 预期攻击拒绝前后插入正常业务 |
| VM-10 | PASS | live30 | 检查deferred stages1/2和sync_call状态 |
| FIN-01 | PASS | offline-canonical-audit-of-verified-stopped-live30-archive | 逐笔等待成功业务最终确认 |
| FIN-02 | PASS | live30 | 健康连通三机/30实例读取统一检查点 |
| FIN-03 | PASS | live30 | 正常停止后核对配置、状态及原始区块 |
| FIN-04 | PASS | live30 plus one isolated parallel snapshot/read-only observer, same3 physical hosts | 合法快照启动新增实例并追齐 |
| FIN-05 | PASS | evidence-review-and-action-coverage; separate live/native/component scope | 列明全部缺陷/修复/未完成及覆盖矩阵 |
| FIN-06 | PASS | qualified-source-local-Git-and-remote-development-baseline-promotion | 按资格纳入基线，取回源码并Git提交 |
| FIN-07 | PASS | requested-Markdown-local-source-and-hash-synchronization | 更新远端实验总账和本地MD索引 |
| FIN-08 | PASS | owned-test-process-and-network-cleanup;delete-only-two-verified-extra-baremetals | 全部扩展测试完成后清理临时进程、规则及额外两机 |
| MAINT-01 | PASS | live30 | 本人挂起未在岗候选 |
| MAINT-02 | PASS | live30 | 其他账户/仅peer-key替人操作 |
| MAINT-03 | PASS | live30 | 当前生产者申请维护 |
| MAINT-04 | PASS | native-real-WASM-and-explicitly-linked-live-evidence | 当前最终确认者/待生效成员申请 |
| MAINT-05 | PASS | live30 | 维护替补交接 |
| MAINT-06 | PASS | live30 | 无合格替补 |
| MAINT-07 | PASS | native-real-WASM-and-explicitly-linked-live-evidence | 维护节点跨多轮岗 |
| MAINT-08 | PASS | native-real-WASM-and-explicitly-linked-live-evidence | 维护期间领取既往权益 |
| MAINT-09 | PASS | live30 | 解除维护回候选 |
| MAINT-10 | PASS | native-real-WASM-and-explicitly-linked-live-evidence | 申请前已有1/2次有效零窗口 |
| MAINT-11 | PASS | live30 | 已永久ban账户申请/恢复 |
| MAINT-12 | PASS | live30 | 重复申请、取消及并发申请 |
| MAINT-13 | PASS | native-real-WASM-and-explicitly-linked-live-evidence | 交接中取消/恢复 |
| MAINT-14 | PASS | live30 | 维护后重启和快照 |
| MAINT-15 | PASS | native-real-WASM-and-explicitly-linked-live-evidence | 维护申请后提前关机 |
| MAINT-16 | PASS | live30 | 未挂维护故障 |
| MAINT-17 | PASS | live30 | 维护与peer-key撤销/轮换 |
| MAINT-18 | PASS | live30 | 维护与投票、质押、PowerUp |
| MAINT-19 | PASS | live30 | 30实例真实维护交接/恢复 |
| MAINT-20 | PASS | live30 | 原ABI、模式和发布制品回归 |

## 原始失败及中断记录 / Preserved failures and interruptions

- ENV-05 FAIL：No finalizer safety file found；/opt/aic-bp-full-functional-20261030-r1/rounds/R00/ENV-05/attempt-1
- ENV-06 FAIL：<urlopen error [Errno 111] Connection refused>；/opt/aic-bp-full-functional-20261030-r1/rounds/R00/ENV-06/attempt-1
- KEY-03 FAIL：<urlopen error [Errno 111] Connection refused>；/opt/aic-bp-full-functional-20261030-r1/rounds/R01/KEY-03/attempt-2
- KEY-08 FAIL：RPC send_read_only_transaction failed on bpaaa: {"code":500,"message":"Internal Service Error","error":{"code":3100008,"name":"unsupported_feature","what":"Feature is currently unsupported","details":；/opt/aic-bp-full-functional-20261030-r1/rounds/R01/KEY-08/attempt-1
- KEY-08 FAIL：'account'；/opt/aic-bp-full-functional-20261030-r1/rounds/R01/KEY-08/attempt-2
- SEC-12 FAIL：；/opt/aic-bp-full-functional-20261030-r1/rounds/R03/SEC-12/attempt-1
- SEC-12 FAIL：Missing current peer key must be explicitly diagnosed；/opt/aic-bp-full-functional-20261030-r1/rounds/R03/SEC-12/attempt-2
- SEC-12 FAIL：Missing current peer key must be explicitly diagnosed；/opt/aic-bp-full-functional-20261030-r1/rounds/R03/SEC-12/attempt-3
- SEC-12 FAIL：No freshly signed current-key identity may be fabricated；/opt/aic-bp-full-functional-20261030-r1/rounds/R03/SEC-12/attempt-4
- SEC-12 FAIL：<urlopen error [Errno 111] Connection refused>；/opt/aic-bp-full-functional-20261030-r1/rounds/R03/SEC-12/attempt-5
- M30-08 FAIL：[Errno 104] Connection reset by peer；/opt/aic-bp-full-functional-20261030-r1/rounds/R04/M30-08/attempt-1
- NET-01 FAIL：；/opt/aic-bp-full-functional-20261030-r1/rounds/R05/NET-01/attempt-1
- NET-02 FAIL：Functional transaction not finalized on all participating nodes within the original 60s budget；/opt/aic-bp-full-functional-20261030-r1/rounds/R05/NET-02/attempt-2
- CON-06 FAIL：<urlopen error [Errno 111] Connection refused>；/opt/aic-bp-full-functional-20261030-r1/rounds/R06/CON-06/attempt-1
- CON-06 FAIL：'NoneType' object is not subscriptable；/opt/aic-bp-full-functional-20261030-r1/rounds/R06/CON-06/attempt-2
- BAN-05 FAIL：Incumbent fault did not take priority within300s；/opt/aic-bp-full-functional-20261030-r1/rounds/R07/BAN-05/attempt-1
- SYS-02 FAIL：RPC push_transaction failed on bpaaa: {"code":500,"message":"Internal Service Error","error":{"code":3080001,"name":"ram_usage_exceeded","what":"Account using more than allotted RAM usage","details":[；/opt/aic-bp-full-functional-20261030-r1/rounds/R08/SYS-02/attempt-1
- SYS-09 FAIL：RPC push_transaction failed on bpaaa: {"code":500,"message":"Internal Service Error","error":{"code":3050000,"name":"action_validate_exception","what":"Action validate exception","details":[{"message"；/opt/aic-bp-full-functional-20261030-r1/rounds/R08/SYS-09/attempt-1
- SYS-09 FAIL：；/opt/aic-bp-full-functional-20261030-r1/rounds/R08/SYS-09/attempt-2
- SYS-11 FAIL：RPC push_transaction failed on bpaaa: {"code":500,"message":"Internal Service Error","error":{"code":3090005,"name":"irrelevant_auth_exception","what":"Irrelevant authority included","details":[{"mess；/opt/aic-bp-full-functional-20261030-r1/rounds/R08/SYS-11/attempt-1
- SYS-11 FAIL：RPC push_transaction failed on bpaaa: {"code":500,"message":"Internal Service Error","error":{"code":3050010,"name":"unauthorized_ram_usage_increase","what":"Action attempts to increase RAM usage of a；/opt/aic-bp-full-functional-20261030-r1/rounds/R08/SYS-11/attempt-2
- SYS-11 FAIL：[Errno 104] Connection reset by peer；/opt/aic-bp-full-functional-20261030-r1/rounds/R08/SYS-11/attempt-3
- DEF-06 PARTIAL：Only explicitly recorded subcases executed；/opt/aic-bp-full-functional-20261030-r1/rounds/R09/DEF-06/attempt-1
- DEF-06 FAIL：RPC get_table_rows failed on bpaba: {"code":500,"message":"Internal Service Error","error":{"code":3015010,"name":"abi_serialization_deadline_exception","what":"ABI serialization time has exceeded the；/opt/aic-bp-full-functional-20261030-r1/rounds/R09/DEF-06/attempt-2
- DEF-07 PARTIAL：Only explicitly recorded subcases executed；/opt/aic-bp-full-functional-20261030-r1/rounds/R09/DEF-07/attempt-1
- NFT-03 PARTIAL：Only explicitly listed executed subcases covered；/opt/aic-bp-full-functional-20261030-r1/rounds/R10/NFT-03/attempt-1
- NFT-03 FAIL：<urlopen error [Errno 111] Connection refused>；/opt/aic-bp-full-functional-20261030-r1/rounds/R10/NFT-03/attempt-2
- NFT-07 PARTIAL：Only explicitly listed executed subcases covered；/opt/aic-bp-full-functional-20261030-r1/rounds/R10/NFT-07/attempt-1
- NFT-08 PARTIAL：Only explicitly listed executed subcases covered；/opt/aic-bp-full-functional-20261030-r1/rounds/R10/NFT-08/attempt-1
- VM-01 FAIL：RPC push_transaction failed on bpaaa: {"code":500,"message":"Internal Service Error","error":{"code":3050007,"name":"unaccessible_api","what":"Attempt to use unaccessible API","details":[{"message":"o；/opt/aic-bp-full-functional-20261030-r1/rounds/R11/VM-01/attempt-1
- VM-02 FAIL：RPC push_transaction failed on bpaaa: {"code":500,"message":"Internal Service Error","error":{"code":3050003,"name":"eosio_assert_message_exception","what":"eosio_assert_message assertion failure","de；/opt/aic-bp-full-functional-20261030-r1/rounds/R11/VM-02/attempt-1
- VM-03 FAIL：；/opt/aic-bp-full-functional-20261030-r1/rounds/R11/VM-03/attempt-1
- VM-04 FAIL：list index out of range；/opt/aic-bp-full-functional-20261030-r1/rounds/R11/VM-04/attempt-1
- VM-08 FAIL：Functional transaction not finalized on all participating nodes within the original 60s budget；/opt/aic-bp-full-functional-20261030-r1/rounds/R11/VM-08/attempt-1
- FIN-03 PARTIAL：用户新增BAN-07、BAN-10实机补测，归档阶段中断并保留，补测后重新归档；/opt/aic-bp-full-functional-20261030-r1/rounds/R12/FIN-03/attempt-1
- FIN-04 FAIL：Job for aic-bp-full-r1-readonly-api-20261030.service failed because the control process exited with error code.
See "systemctl status aic-bp-full-r1-readonly-api-20261030.service" and "journalctl -xeu；/opt/aic-bp-full-functional-20261030-r1/rounds/R12/FIN-04/attempt-1
- MAINT-04 PARTIAL：Only explicitly listed subcases covered; remaining branches still required；/opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-04/attempt-1
- MAINT-08 PARTIAL：Only explicitly listed subcases covered; remaining branches still required；/opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-08/attempt-1
- MAINT-12 PARTIAL：Only explicitly listed subcases covered; remaining branches still required；/opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-12/attempt-1
- MAINT-14 PARTIAL：Only explicitly listed subcases covered; remaining branches still required；/opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-14/attempt-1
- MAINT-18 FAIL：list index out of range；/opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-18/attempt-1

## English scope

Functional cases and canonical/archive audits passed. Release closure is recorded separately. This is a three-physical-host,30-process parallel functional campaign with one additional nonproducing observer; no new TPS was measured. Live, native, component and historical evidence retain their actual source, binary and contract hashes. The user restored both previously excluded cases: no-standby permanent-ban eviction21to20,14-vote progress under the active20-member policy,13-vote finality halt, short/long interruption and recovery, and normal return to21members/15votes. Under21 evidence is specifically20members. Joint-policy transitions still require the native applicable old/new quorums. Prior failures and original released WASM remain preserved.
