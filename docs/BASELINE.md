# PureEOS 1.0.0 当前基线

## 简体中文

本文件统一定义当前发行。机器可读配置见 [BASELINE.json](../config/BASELINE.json)，完整链参数见 [chain-parameters.json](../config/chain-parameters.json)。配置文件须经正常链上初始化或授权更新生效，不自动改变已有链。

| 项目 | 选定基线 |
|---|---|
| 配套产品 | PureEOS、PureEOS CDT、PureHistory、PureEOS Explorer，产品版本均为 1.0.0 |
| CPU / NET | CPU16：3,200,000 μs/块；NET4MiB：4,194,304 字节/块 |
| 状态库 | 16GiB，chain-state-db-size-mb=16384；这是容量上限，不是预占物理内存 |
| 单笔交易 | NET512KiB、CPU150000 μs；保留既有计费、折扣、权限与原子回滚 |
| 并行 | 强制 parallel；8 workers、batch24、group8、chain-threads6；冲突安全回退 |
| 硬件 | 至少8物理核心、16逻辑线程、64GB系统可见内存；显式串行及低配启动拒绝 |
| 网络与队列 | P2P64MiB/消息、100MiB在途交易保护、1GiB生产交易队列 |
| RAM | 初始64GiB；既有 setramrate(1024) 初始化每块增长1024字节 |
| 投票与资源 | 精确整数单选、本人直接投票、无代理、无时间加权；10%长期质押/90%PowerUp |
| 奖励 | 按当前发行量每364天精确1%计提，每1461天减半；另4%不发行；仅实际出块累计权益，成功领取时发行填池，领取间隔严格超过24小时 |
| 创世资产 | AIC精度4；初始发行1亿，最大供应21亿；剩余不预发，额外发行机制保持现有方式 |
| 委员会 | 合格稳态21名出块者、21名最终确认者，21人需15票；法定人数按生效政策计算，不按在线数量计算 |
| 轮岗 | 排名进入候选，FIFO候补，正常一次交接一个席位；初始容量210，按1461天周期增长，公式见机器清单与源码 |
| 永久黑名单 | 健康链上连续3个已最终确认的个人零出块窗口；重注册、换钥、投票或维护不能清除 |
| 停摆保护 | 全链出块/最终确认观察中断达到252slot=126秒（含边界），或少于21人，暂停新增个人缺岗证据；历史有效证据、永久封禁和未领权益保留 |
| 黑名单不占位 | 排名容量计算前排除永久封禁，下一名合格候选递补，高票封禁BP也不占席位 |
| 维护 | 在岗完成正常出块/确认交接后才生效；有效维护排除未来轮岗，不新增职责奖励/处罚；历史权益和违规保留 |
| 7天治理 | 仅指定开发者提案和执行，BP在审查期否决；新提案604800秒，提前执行拒绝 |
| 否决计数 | 冻结名单扣除当前永久封禁者得到N；已投否决也剔除封禁者；门槛ceil(2N/3)，N=0禁止执行；原始记录和旧提案期限保留 |
| 延迟交易 | DISABLE_DEFERRED_TRXS_STAGE_1/2保持激活；不启用或测试废弃功能 |
| BP发现 | 链上peer key和签名gossip，支持撤销、换钥、端点变更；静态种子只负责初始连接 |
| 历史服务 | 出块节点SHIP关闭；独立非出块并行历史节点开启trace-history和chain-state-history，服务PureHistory |
| 账号与短号 | 普通/BP/测试账号12位，eosio系统名保留；旧拍卖代码与出价接口保留，成交仍有原14天等条件；短号租金和aic更名尚未实施 |

### 使用、升级和测试范围

安装与编译见 [README](../README.md)，节点启动/同步/注册/维护见 [操作指南](network/BP_STARTUP_REGISTRATION.md)。出块节点、历史节点及状态库容量应按各自用途配置；链上RAM市场与本地数据库容量不同。

本发行绑定系统合约源码15dea、WASM373b66和原生序列化ABI038ffb；包内有完整摘要。新链启用对应原生治理特性并正常初始化；已有封存治理的链须通过开发者提案、审查和正常执行升级。发布本版本不会直接替换当前测试链合约。7天特性前的回放语义及旧提案已定期限保留。

最终确认门槛 floor(2N/3)+1 与治理否决门槛 ceil(2N/3) 不同。全链停摆保护不能降低最终性法定票数，也不能保证票数不足时仍继续最终确认。永久封禁从候选中排除后，旧生效政策仍按正常交接退出。

Ubuntu24.04LTS、26.04LTS的精确原生构建、打包运行和SDK范围见 [兼容性报告](ubuntu/COMPATIBILITY_20261030.md)。本次发行元数据重链和完整检查重放单独记录，不冒称新建26.04虚拟机重测。治理/黑名单93个原生用例、111次成功执行，另有即时历史签名重放15521断言、完整状态哈希、668个物理表元数据及四个二级索引一致性证据，见 [验收记录](../tests/qualification/governance7-20261101/)。实际PureHistory模块读取新合约也通过。已有三物理机拓扑、旧不足21人及吞吐结果保留各自范围；本发行未新增TPS测量或7天现实时间稳定性结论。

PureHistory按ABI识别PureEOS整数表及旧EOS表，保留旧EOS兼容。浏览器仅只读，展示精确总票权/自投/他投、维护、黑名单、紧凑排班、默认展开的12块活动，链上最高TPS取服务端不可逆记录，不再保存浏览器峰值。

## English

PureEOS 1.0.0 uses CPU16 (3,200,000 μs/block), NET 4 MiB/block and 16 GiB local state capacity. It requires parallel execution, at least 8 physical cores, 16 logical threads and 64 GB visible RAM. Defaults retain 8 workers, batch 24, group 8, 6 chain threads, P2P 64 MiB/message, 100 MiB transaction inflight protection and a 1 GiB producer queue. Per-transaction NET 512 KiB, CPU 150,000 μs, billing, authorization and conflict fallbacks remain. RAM begins at 64 GiB with explicit setramrate(1024) initialization. The complete policy and chain parameters are linked above; they require normal on-chain initialization/update.

Voting is exact integer single choice, directly authorized by the voter, without proxies or time weighting. Staking/PowerUp remains 10%/90%. Rewards accrue at exactly 1% of currently issued supply per 364-day contract year, halved every 1461 days; the extra 4% is never issued. Actual blocks accrue rights; successful claims issue and fund the pool, with strictly more than 24 hours between claims. Fresh-chain AIC has 4 decimals, 100 million initially issued and 2.1 billion maximum supply. Separate issuance remains unchanged.

Steady-state proposer/finalizer policies have 21 members and 15-vote finality. Equal-weight finality is floor(2N/3)+1 of effective policy membership, independent of online process count. One seat hands over normally at a time. Ranked candidates and FIFO standbys follow the capacity growth rule beginning at 210. Three consecutive finalized personal zero-production windows on a healthy chain permanently ban a BP account. Global interruption of 252 slots/126 seconds inclusive, or a sub-21 committee, pauses new personal absence evidence and preserves valid prior evidence. These safeguards never lower quorum. Permanent bans are excluded before ranked capacity counting and the next eligible candidate backfills. Re-registration, keys, votes and maintenance do not erase bans. Maintenance becomes effective only after normal incumbent proposer/finalizer handoff; it excludes future rotation and new duty rewards/penalties while retaining historical rights.

Only the designated developer may propose/execute an upgrade; BPs veto during the full 604800-second review. Effective frozen electorate and already-cast vetoes exclude current permanent bans; the veto threshold is ceil(2N/3), and N=0 blocks execution. Raw records and existing proposal deadlines remain. Fresh chains activate the native governance feature and initialize normally; existing sealed chains use normal review. Publishing this release does not replace the running test chain contract.

Both deferred-disable stages remain active. Signed BP gossip uses current on-chain peer keys with revocation/rotation. Producers keep SHIP off; a nonproducing parallel history source supplies PureHistory. Ordinary accounts use 12 characters. Protocol eosio names and existing auctions remain, including the auction closing conditions; short-name rentals and aic namespace changes remain plans. PureHistory retains ABI-detected legacy EOS compatibility. The explorer is read-only, with exact vote breakdowns, maintenance/blacklist data, compact schedules, a default-open 12-block activity view and server-owned finalized lifetime peak TPS.

See the linked build/operator/Ubuntu guides and qualification records for exact scope: 93 unique native cases/111 successful executions, plus 15521 assertions in one historical signed-chain integration scenario, complete-state/668-table metadata/four-index agreement and actual PureHistory module reads. Metadata relink qualification is separate from inherited native Ubuntu compatibility and historical topology/TPS evidence. No new TPS or seven-day wall-clock stability claim is made.
