# PureEOS 三机完整功能重跑 r1（2026-10-30）

最新候选Node4ede04b4 / SHA6e9c6685；系统6f3160ed / WASM5f25a258。三台EPYC4345P、各8物理核16线程64GiB，30独立并行进程；21在岗+9候补；CPU16/NET16MiB、P2P64MiB、生产队列1GiB、RAM1024、deferred禁用。
所有旧结果保留；本表是完整新轮，不继承历史PASS。BAN-07/10因用户禁止低于21席实机分支排除。

## R00

| ID | 操作 | 预期 | 状态 | 证据 |
|---|---|---|---|---|
| ENV-01 | 核对三台实例ID、EPYC4345P、8物理核16线程和MemTotal | 记录实际硬件，防止用主机标签代替检测 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R00/ENV-01/attempt-3 |
| ENV-02 | 逐机核对源码、Node二进制、系统WASM/ABI哈希 | 三台相同；原发布WASM保留；候选不提前提升基线 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R00/ENV-02/attempt-3 |
| ENV-03 | 读取完整链参数和配置 | CPU16、NET16MiB/块、P2P64MiB/消息、生产队列1GiB、8worker/batch24/group8、full checks | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R00/ENV-03/attempt-3 |
| ENV-04 | 低核/低线程/低内存/未知拓扑和显式纯串行启动 | 拒绝启动并打印最低配置和检测值；区分继承证据和本候选实测 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R00/ENV-04/attempt-2 |
| ENV-05 | 检查链ID、数据目录、端口、密钥和finalizer safety文件 | 实例独立，同链同一生产/最终确认身份只启动一份 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R00/ENV-05/attempt-3 |
| ENV-06 | 列出完整系统ABI和第三方ABI动作 | 每个支持动作绑定实际用例和trace；未映射/未执行不算覆盖 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R00/ENV-06/attempt-2 |

## R01

| ID | 操作 | 预期 | 状态 | 证据 |
|---|---|---|---|---|
| KEY-01 | 部署候选系统合约并读取链上code hash、ABI | 与编译件一致，旧87动作/37表和原ABI条目不变 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R01/KEY-01/attempt-2 |
| KEY-02 | BP使用自己的账户权限登记K1公钥 | 登记、读取正确，RAM由该BP承担，私钥不上链 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R01/KEY-02/attempt-2 |
| KEY-03 | 其他账户替BP登记或删除公钥 | 拒绝，原记录、余额和RAM保持正确 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R01/KEY-03/attempt-3 |
| KEY-04 | 非注册候选且非当前权威账户登记公钥 | 按现有资格拒绝，不凭peer-key获得BP资格 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R01/KEY-04/attempt-2 |
| KEY-05 | 重复登记、空公钥、WebAuthn公钥 | 按代码拒绝，不破坏现有记录 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R01/KEY-05/attempt-2 |
| KEY-06 | 登记R1、轮换、用正确当前key删除 | 序列化、读取、退款正确，之后真实gossip验证R1 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R01/KEY-06/attempt-2 |
| KEY-07 | 用轮换前旧key删除新key，重复删除不存在记录 | 拒绝，当前有效记录不变 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R01/KEY-07/attempt-2 |
| KEY-08 | 调用实际getpeerkeys只读入口并比较状态哈希 | 返回正确名单，状态不写入，保持原20ms预算 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R01/KEY-08/attempt-3 |
| KEY-09 | 用真实自购RAM、自己质押/投票、提前PowerUp准备账户 | 业务账户10%质押、90%PowerUp，控制账户单列，无资源管理器强制赋值 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R01/KEY-09/attempt-1 |
| KEY-10 | 核对治理封存、单选投票、轮换和双权威名单 | 保持PureEOS现有模式，不换成整套上游EOS系统 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R01/KEY-10/attempt-1 |

## R02

| ID | 操作 | 预期 | 状态 | 证据 |
|---|---|---|---|---|
| GSP-01 | 启动A/B/C，A只填B、C只填B | 链上缓存加载，三台同步同一链 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R02/GSP-01/attempt-1 |
| GSP-02 | 观察C从B学到A并主动建连 | 配置中没有A；保存签名学习、连接和真实区块交换证据 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R02/GSP-02/attempt-1 |
| GSP-03 | 观察A学到C并互传低频业务 | SYS/TKN正确最终确认，三台状态一致 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R02/GSP-03/attempt-1 |
| GSP-04 | 重复发送同一合法地址记录 | 幂等，记录/连接不无限增加 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R02/GSP-04/attempt-1 |
| GSP-05 | 新实例冷启动只给一个健康种子 | 能学习、连接和追块，不手工补全完整peer列表 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R02/GSP-05/attempt-1 |
| GSP-06 | 停止一个种子但保留其他已发现健康连接 | 网络在实际共识门槛允许时继续，恢复种子后可重连 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R02/GSP-06/attempt-1 |
| GSP-07 | 全部种子不可达后再恢复一个 | 等待状态真实，恢复后能建网，不能谎报已发现 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R02/GSP-07/attempt-1 |
| GSP-08 | BP正常变更可达端口并更新签名广播 | 新地址生效，旧动态端点被清理 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R02/GSP-08/attempt-1 |
| GSP-09 | 观察备用BP及待生效BP的连接 | 备用连接有界，待生效成员可以预连接 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R02/GSP-09/attempt-1 |
| GSP-10 | 保留数据库和safety文件重启一个节点 | 恢复发现、追块、出块/投票，不重置防双签状态 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R02/GSP-10/attempt-1 |

## R03

| ID | 操作 | 预期 | 状态 | 证据 |
|---|---|---|---|---|
| SEC-01 | 把一个BP轮换到预置R1 peer-key | 新签名在各节点有效，出块和BLS密钥不变 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R03/SEC-01/attempt-2 |
| SEC-02 | 轮换后重放旧key签名地址 | 缓存刷新后拒绝，不恢复旧身份 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R03/SEC-02/attempt-2 |
| SEC-03 | 删除peer-key后重放原记录 | 撤销生效，发现缓存和不再需要的动态连接被清理 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R03/SEC-03/attempt-2 |
| SEC-04 | 正常重新登记该BP公钥 | 发现恢复，不重建账户或放宽资格 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R03/SEC-04/attempt-2 |
| SEC-05 | 修改已签名记录的地址、账户或有效期 | 同一签名不能授权修改后的负载 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R03/SEC-05/attempt-2 |
| SEC-06 | 把已有签名地址记录改成空initial消息 | 伪造拒绝；真正重新签名的合法initial通过 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R03/SEC-06/attempt-2 |
| SEC-07 | 重放其他chain ID签名 | 拒绝跨链重放 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R03/SEC-07/attempt-2 |
| SEC-08 | 过期和过远未来有效期记录，以及正常续期 | 按既有有效期边界处理，合法续期可继续 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R03/SEC-08/attempt-2 |
| SEC-09 | 同一BP恰好8个地址和第9个地址 | 8可接受，第9受限，验证精确边界 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R03/SEC-09/attempt-2 |
| SEC-10 | 格式损坏、非法地址、重复条目和排序异常 | 拒绝或按协议丢弃，合法网络继续可用 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R03/SEC-10/attempt-2 |
| SEC-11 | 同时改变名单、地址和密钥 | 最终连接符合最新授权状态，旧计划不覆盖新计划 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R03/SEC-11/attempt-1 |
| SEC-12 | 本机缺少当前peer私钥或链上公钥被删 | 不伪造签名、不沿用旧身份，明确日志 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R03/SEC-12/attempt-6 |
| SEC-13 | 测量撤销到各节点生效的区块与时间 | 记录120块缓存刷新和约3秒维护，不能称瞬时全网生效 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R03/SEC-13/attempt-2 |

## R04

| ID | 操作 | 预期 | 状态 | 证据 |
|---|---|---|---|---|
| M30-01 | 生成30份独立身份、端口、目录和safety文件 | 没有同链重复身份运行，每实例并行8worker | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R04/M30-01/attempt-1 |
| M30-02 | 依次从3加入到10、21和30节点 | 每批发现/追块/状态核对后才进入下一批 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R04/M30-02/attempt-1 |
| M30-03 | 每节点仅配置1至2个种子 | 依签名gossip建网，不静态预填完整mesh | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R04/M30-03/attempt-1 |
| M30-04 | 同IP多端口和跨主机互联 | BP账户、端口与连接不串线，不将共享IP误当同一身份 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R04/M30-04/attempt-1 |
| M30-05 | 逐实例检查参数与硬件/并行日志 | 固定CPU16/NET16MiB，不把30进程推算成30台独立服务器性能 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R04/M30-05/attempt-1 |
| M30-06 | 观察节点通过正常注册、质押和投票成为候选 | 资格按原规则改变，观察者不能自授出块/确认权 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R04/M30-06/attempt-1 |
| M30-07 | 记录21名当前委员会与9名候选/观察者连接 | 角色和连接规模符合实现 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R04/M30-07/attempt-1 |
| M30-08 | 分批滚动重启并保留状态 | 正常追齐，不删除安全状态或跳过完整验证 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R04/M30-08/attempt-3 |
| M30-09 | 低频采样每实例RSS/CPU/FD/线程/连接/磁盘 | 出现共享资源争用先减小启动并发，不关闭并行或扩大链资源限额 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R04/M30-09/attempt-1 |

## R05

| ID | 操作 | 预期 | 状态 | 证据 |
|---|---|---|---|---|
| NET-01 | 短暂切断普通P2P连接再恢复 | 重连补块，交易按明确结果重发，最终只记账一次 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R05/NET-01/attempt-4 |
| NET-02 | 隔离BP网络但保留进程 | 区分进程存活和网络隔离，以真实出块/确认为准 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R05/NET-02/attempt-3 |
| NET-03 | 停止BP进程，再用原目录恢复 | 发现、追块和状态恢复，safety文件保留 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R05/NET-03/attempt-2 |
| NET-04 | 隔离少数节点，主连通分区仍满足实际门槛 | 有门槛一侧正常推进，隔离侧不能伪造最终确认 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R05/NET-04/attempt-2 |
| NET-05 | 制造两侧都低于实际确认门槛的分区 | LIB按规则停，不允许冲突状态分别最终确认 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R05/NET-05/attempt-2 |
| NET-06 | 恢复分区后低频提交新业务 | 所有健康节点统一到相同已确认链 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R05/NET-06/attempt-2 |
| NET-07 | 少量延迟、单向异常和重复连接 | 能恢复且连接有界，保存网络规则和恢复证据 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R05/NET-07/attempt-2 |
| NET-08 | 单次错误chain ID握手、坏签名或超限长度头 | 拒绝连接，不做洪泛；不把一次坏包写为链上永久ban | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R05/NET-08/attempt-2 |
| NET-09 | 功能量级下检查发送队列、断连和追块 | 若仍异常积压即记录缺陷，不扩大保护上限掩盖 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R05/NET-09/attempt-2 |

## R06

| ID | 操作 | 预期 | 状态 | 证据 |
|---|---|---|---|---|
| CON-01 | 核对正确时隙、生产者和区块签名 | 有效区块完整验证一致，错误身份/签名拒绝 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R06/CON-01/attempt-1 |
| CON-02 | 读取门槛Q，分别使在线权重为Q和Q-1 | 达到门槛才确认，低于门槛停止LIB，不假定三节点容错一台 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R06/CON-02/attempt-1 |
| CON-03 | 正常投票导致待生效与当前名单变化 | proposer/finalizer双策略安全交接，无提前晋升 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R06/CON-03/attempt-1 |
| CON-04 | 名单交接同时低频转账 | 业务原子性、余额守恒、最终状态一致 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R06/CON-04/attempt-1 |
| CON-05 | 分别轮换出块键、BLS键和peer-key | 三种权限独立，按各自生效流程处理 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R06/CON-05/attempt-1 |
| CON-06 | 待生效BP很晚才广播端点 | 可以补建连接，不只在初次名单通知时尝试 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R06/CON-06/attempt-3 |
| CON-07 | 双策略交接中重启一个节点 | 从持久化状态恢复，不发布旧策略或重复变更 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R06/CON-07/attempt-1 |
| CON-08 | 无替补、交接未完成或候选观察失败 | 按既有状态机等待/重试/记失败，不标成功或改模式 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R06/CON-08/attempt-1 |

## R07

| ID | 操作 | 预期 | 状态 | 证据 |
|---|---|---|---|---|
| BAN-01 | 21人中一个BP错过一个完整个人窗口 | 只用已最终确认证据累计，一次漏块不等于永久ban | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R07/BAN-01/attempt-1 |
| BAN-02 | 同一BP连续两个可结算个人窗口零产出 | 计数2、证据正确，尚未永久封禁 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R07/BAN-02/attempt-1 |
| BAN-03 | 无合格替补时连续三个个人窗口零产出 | banned=true；3份evidence_slots/evidence_policies和ban_final_block可核对 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R07/BAN-03/attempt-1 |
| BAN-04 | 两个零窗口后在下一个个人窗口正常产出 | 连续计数和对应证据按规则清理，不误封 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R07/BAN-04/attempt-1 |
| BAN-05 | 有备用时出现一次已确认零产出窗口 | 验证故障席位优先换出，不为凑三次强留故障BP | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R07/BAN-05/attempt-2 |
| BAN-06 | 永久ban后有替补 | 走正常单席位/双策略交接，名单与发现资格正确更新 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R07/BAN-06/attempt-1 |
| BAN-07 | 永久ban后无替补 | 走安全移除流程，不留已ban席位、不发空委员会 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R07/BAN-07/attempt-1 |
| BAN-08 | 已ban账户重连、换IP、换peer-key或BLS键 | 永久账户记录不消失，不恢复候选资格 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R07/BAN-08/attempt-1 |
| BAN-09 | 已ban账户注销重注册、改票、再质押 | 不能绕过固定候选记录；不假设有未实现的自动解封 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R07/BAN-09/attempt-1 |
| BAN-10 | 少于完整21人委员会时短断/长断 | 核对惩罚暂停，不把三机断线当成完整黑名单资格证明 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R07/BAN-10/attempt-1 |
| BAN-11 | 全链/最终确认暂停达到pause_guard阈值-1、等于、+1 | Current guard252slot/126s: test one slot below, inclusive boundary, one slot above; preserve prior settled evidence. | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R07/BAN-11/attempt-1 |
| BAN-12 | 约三个个人轮次的全局暂停后恢复 | 专门检查集体误罚；不安全集体ban阻止提升基线 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R07/BAN-12/attempt-1 |
| BAN-13 | 21到21名单交接、key-only变化和未完成窗口 | 不完整窗口不当完整证据，已最终确认个人证据按规则保留 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R07/BAN-13/attempt-1 |
| BAN-14 | 没有新最终确认依据，仅本地时钟或RPC变慢 | 不得凭本机猜测提前处罚 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R07/BAN-14/attempt-1 |
| BAN-15 | 新节点追块及快照恢复黑名单状态 | rotcands/rotation、证据和ban_final_block一致 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R07/BAN-15/attempt-1 |
| BAN-16 | 被ban候选排名占位与容量边界 | 保持现有排名规则，不用C+1擅自补位或扩容 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R07/BAN-16/attempt-1 |
| BAN-17 | P2P连接关闭与链上永久黑名单分别核对 | 不混淆两种机制，不因坏包直接封禁正常BP账户 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R07/BAN-17/attempt-2 |
| BAN-18 | ban后的gossip连接与权威交接顺序 | 先完成必要安全交接，再清理不再合格的动态身份 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R07/BAN-18/attempt-1 |

## R08

| ID | 操作 | 预期 | 状态 | 证据 |
|---|---|---|---|---|
| SYS-01 | SYS及TKN转账、余额不足、越权和重复提交 | 成功/拒绝分开，余额守恒，最终只记账一次 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R08/SYS-01/attempt-1 |
| SYS-02 | 自购RAM、开户/关行、退RAM和未授权payer | 正常收费退款正确，越权拒绝 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R08/SYS-02/attempt-2 |
| SYS-03 | 读取初始64GiB和setramrate1024后跨块核对 | global2参数和实际增长一致，不改其他模式 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R08/SYS-03/attempt-1 |
| SYS-04 | 自己质押、委托、解押和当前退款流程 | 权重/余额正确，质押总池10%和PowerUp90%保持 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R08/SYS-04/attempt-1 |
| SYS-05 | 提前PowerUp、租期内使用、续租与到期再租 | 订单、费用、到期和资源正确，准备先完成 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R08/SYS-05/attempt-1 |
| SYS-06 | 单选投票、改票、撤票、无效多选及代理边界 | 只接受当前PureEOS规则，拒绝不改变排名 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R08/SYS-06/attempt-1 |
| SYS-07 | 注册/注销生产者、最终确认键及资格不完整 | 原生和合约名单一致，不能接纳不合格候选 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R08/SYS-07/attempt-1 |
| SYS-08 | 奖励、手续费、供应量和奖励减半边界 | 规则保持，奖励减半与RAM增长独立，原生长周期不改常量 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R08/SYS-08/attempt-1 |
| SYS-09 | developer提案、固定审核期、veto、取消及升级 | 真实权限和审核期；实机验证提前拒绝，完整长周期原生验证 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R08/SYS-09/attempt-3 |
| SYS-10 | 系统敏感动作越权及旧入口限制 | 不能借peer-key功能绕过治理或账户锁 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R08/SYS-10/attempt-1 |
| SYS-11 | 当前支持的msig/自定义权限/权限链接 | 按实际能力逐项测试，禁用项明示原因 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R08/SYS-11/attempt-4 |
| SYS-12 | 完整系统ABI逐项核对动作覆盖 | 每个支持动作有正常/拒绝/边界对应证据，遗漏不计完成 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R08/SYS-12/attempt-1 |

## R09

| ID | 操作 | 预期 | 状态 | 证据 |
|---|---|---|---|---|
| DEF-01 | 部署真实token/converter/network并初始化 | 源码和链上hash一致，不用EVM同名项目或自写简化合约冒充 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R09/DEF-01/attempt-1 |
| DEF-02 | 开行、储备、测试资产与权限准备 | 余额和RAM可核对，准备与业务步骤分开 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R09/DEF-02/attempt-1 |
| DEF-03 | 正反向兑换及多合约路由 | 输出/储备/费用符合固定版本公式，最终状态一致 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R09/DEF-03/attempt-1 |
| DEF-04 | 同符号异合约假币和伪造transfer通知 | 不能凭空记入储备或提走真实资产 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R09/DEF-04/attempt-1 |
| DEF-05 | 余额不足、坏memo、错误接收者、最小输出/滑点边界 | 按固定版本实际接口处理，失败整笔回滚 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R09/DEF-05/attempt-1 |
| DEF-06 | 极小数量、精度差、零/负数及整数边界 | 不溢出、不凭空获利，合法舍入按最小单位核对 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R09/DEF-06/attempt-3 |
| DEF-07 | 固定版本实际支持的充值/兑换/提现或赎回 | 完整流程余额与储备一致，不支持的接口不标通过 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R09/DEF-07/attempt-2 |
| DEF-08 | 跨合约inline、回调及重复消费尝试 | 正确原子组合，不能双花同一余额 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R09/DEF-08/attempt-1 |
| DEF-09 | 兑换后续子动作故意失败 | 付款、储备和费用一起回滚 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R09/DEF-09/attempt-1 |
| DEF-10 | 名单切换、重连和新节点追块后核对兑换 | 交易只执行一次，恢复后业务表与账本一致 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R09/DEF-10/attempt-1 |

## R10

| ID | 操作 | 预期 | 状态 | 证据 |
|---|---|---|---|---|
| NFT-01 | 部署并建立collection/schema/template | 权限、版本、RAM payer正确 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R10/NFT-01/attempt-1 |
| NFT-02 | 铸造、转让、允许的数据修改及销毁 | 所有权正确，不可转让/销毁模板边界正确 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R10/NFT-02/attempt-1 |
| NFT-03 | 无权限、伪造通知、重复ID和错误scope | 拒绝，其他资产和RAM不受影响 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R10/NFT-03/attempt-3 |
| NFT-04 | offer创建/接受/取消/失效 | 只变化一次，失败原子回滚 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R10/NFT-04/attempt-1 |
| NFT-05 | 部署匹配AtomicMarket并配置市场和测试代币 | 版本兼容，权限与token来源正确 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R10/NFT-05/attempt-1 |
| NFT-06 | 挂单/购买/取消/拍卖/报价实际支持流程 | NFT、付款、卖方收入和费用守恒，到期用当前支持入口推进 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R10/NFT-06/attempt-1 |
| NFT-07 | 版税、费用分配和取整边界 | 符合固定版本，无重复收费或可盗残留 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R10/NFT-07/attempt-2 |
| NFT-08 | 旧订单、重复成交和伪造支付 | 不能两次成交或让同一NFT归属两人 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R10/NFT-08/attempt-2 |
| NFT-09 | 结算子动作失败 | 付款、所有权和费用一起回滚 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R10/NFT-09/attempt-1 |
| NFT-10 | 节点重启、跨节点读取和追块 | 合约业务表及最终区块一致 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R10/NFT-10/attempt-1 |
| NFT-11 | 固定版本已废弃或本链停用的接口 | 明确不支持/排除，不重启deferred，不用旧说明冒充新能力 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R10/NFT-11/attempt-1 |

## R11

| ID | 操作 | 预期 | 状态 | 证据 |
|---|---|---|---|---|
| VM-01 | 普通/inline/context-free等当前支持动作 | 执行、上下文限制、trace和返回值正确 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R11/VM-01/attempt-2 |
| VM-02 | 主表、二级索引、scope、迭代和RAM payer | 索引/主表一致，增删改和收费正确 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R11/VM-02/attempt-2 |
| VM-03 | require_auth/require_recipient/通知/get_sender | 授权和发起者语义正确，伪造通知不能越权 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R11/VM-03/attempt-2 |
| VM-04 | 多合约嵌套、深度边界、返回值和只读 | 支持组合通过，超深度或只读写入拒绝且回滚 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R11/VM-04/attempt-2 |
| VM-05 | 支持的哈希、签名验证和BLS等密码学入口 | 合法结果正确，非法/不支持输入按规则拒绝 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R11/VM-05/attempt-1 |
| VM-06 | WASM整数/浮点/内存边界和异常 | 结果确定，越界拒绝，节点仍能处理下一笔正常交易 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R11/VM-06/attempt-1 |
| VM-07 | CPU/NET/内存/动作尺寸单笔边界 | 保持原100ms节点上限及链上单笔规则，不抬预算求过关 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R11/VM-07/attempt-1 |
| VM-08 | 非冲突调用、真实冲突和不支持能力回退 | 顺序/授权/计费/回滚不变，记录实际worker采用及安全回退 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R11/VM-08/attempt-2 |
| VM-09 | 预期攻击拒绝前后插入正常业务 | 攻击和业务分开计数，正常交易不被永久卡住 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R11/VM-09/attempt-1 |
| VM-10 | 检查deferred stages1/2和sync_call状态 | 只检查停用状态，不增加send_deferred/cancel_deferred场景 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R11/VM-10/attempt-1 |

## R12

| ID | 操作 | 预期 | 状态 | 证据 |
|---|---|---|---|---|
| FIN-01 | 逐笔等待成功业务最终确认 | 成功、预期拒绝、意外失败和未确认分开，未确认不算通过 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R12/FIN-01/attempt-1 |
| FIN-02 | 健康连通三机/30实例读取统一检查点 | head/LIB/业务表和完整验证一致，异步采样不误判分叉 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R12/FIN-02/attempt-2 |
| FIN-03 | 正常停止后核对配置、状态及原始区块 | 主服务器解包、逐文件SHA及相同区块逐字节验证 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R12/FIN-03/attempt-2 |
| FIN-04 | 合法快照启动新增实例并追齐 | 恢复路径独立验证，不复用旧失败前结果 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R12/FIN-04/attempt-2 
See "systemctl status aic-bp-full-r1-readonly-api-20261030.service" and "journalctl -xeu |
| FIN-05 | 列明全部缺陷/修复/未完成及覆盖矩阵 | 每轮有真实记录，组件/编译/模拟不得冒充实机全通过 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R12/FIN-05/attempt-1 |
| FIN-06 | 按资格纳入基线，取回源码并Git提交 | 先完成真实发现/撤销/轮换，固定CPU16/NET16MiB不变 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R12/FIN-06/attempt-1 |
| FIN-07 | 更新远端实验总账和本地MD索引 | 只取回源码/MD/必要小证明，大归档留主机 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R12/FIN-07/attempt-1 |
| FIN-08 | 全部扩展测试完成后清理临时进程、规则及额外两机 | 未完成不删，主测试机保留 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R12/FIN-08/attempt-1 |

## R13

| ID | 操作 | 预期 | 状态 | 证据 |
|---|---|---|---|---|
| MAINT-01 | 本人挂起未在岗候选 | 不再入选，链上记录正确，RAM自付 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-01/attempt-1 |
| MAINT-02 | 其他账户/仅peer-key替人操作 | 拒绝且原状态不变 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-02/attempt-1 |
| MAINT-03 | 当前生产者申请维护 | 仍履职到安全交接，不提前免罚 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-03/attempt-1 |
| MAINT-04 | 当前最终确认者/待生效成员申请 | 双政策/在途目标处理正确 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-04/attempt-2 |
| MAINT-05 | 维护替补交接 | 全过程21+21，门槛按原规则 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-05/attempt-1 |
| MAINT-06 | 无合格替补 | 等待，保留原职责，不缩人数 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-06/attempt-1 |
| MAINT-07 | 维护节点跨多轮岗 | 不被挑中，不新增出块积分和零窗口 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-07/attempt-1 |
| MAINT-08 | 维护期间领取既往权益 | 保留既往权益及原冷却，不新增维护产出 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-08/attempt-2 |
| MAINT-09 | 解除维护回候选 | 正常排队，不立即抢席 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-09/attempt-1 |
| MAINT-10 | 申请前已有1/2次有效零窗口 | 不洗白既往计数/证据 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-10/attempt-1 |
| MAINT-11 | 已永久ban账户申请/恢复 | 不能解除永久ban | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-11/attempt-1 |
| MAINT-12 | 重复申请、取消及并发申请 | 幂等/明确拒绝，队列和一次一席有界 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-12/attempt-2 |
| MAINT-13 | 交接中取消/恢复 | 不回滚已提交Native政策，不产生双重权威 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-13/attempt-1 |
| MAINT-14 | 维护后重启和快照 | 维护状态、历史证据和safety保持 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-14/attempt-2 |
| MAINT-15 | 维护申请后提前关机 | 未生效部分仍按原规则处罚 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-15/attempt-1 |
| MAINT-16 | 未挂维护故障 | 原三次已确认个人窗口规则仍有效 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-16/attempt-1 |
| MAINT-17 | 维护与peer-key撤销/轮换 | 发现名单与当前/待生效职责一致 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-17/attempt-1 |
| MAINT-18 | 维护与投票、质押、PowerUp | 不附带改变票权、10/90池及计费 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-18/attempt-2 |
| MAINT-19 | 30实例真实维护交接/恢复 | 全体最终确认、业务余额一致，所有实例并行 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-19/attempt-1 |
| MAINT-20 | 原ABI、模式和发布制品回归 | 原WASM留存；新版本差异、hash和部署值可核查 | PASS | /opt/aic-bp-full-functional-20261030-r1/rounds/R13/MAINT-20/attempt-1 |


R05执行方案：单条P2P断连及同ID重发；对一个BP施加仅限自有P2P地址的双向隔离，保留RPC/SSH，其他20名确认者继续；按实际稳定政策权重选一台主机分成两组（两侧均低于门槛15），隔离20秒检查新块不能最终确认，恢复后检查统一链。延迟只匹配自有P2P流量，50ms；坏包只各发一次，无压力。临时防火墙规则在finally中撤回；现有UFW及防双签状态不清空。

R05执行方案：单条P2P断连及同ID重发；对一个BP施加仅限自有P2P地址的双向隔离，保留RPC/SSH，其他20名确认者继续；按实际稳定政策权重选一台主机分成两组（两侧均低于门槛15），隔离20秒检查新块不能最终确认，恢复后检查统一链。延迟只匹配自有P2P流量，50ms；坏包只各发一次，无压力。临时防火墙规则在finally中撤回；现有UFW及防双签状态不清空。

M30-08执行方案：30实例分10批，每批3个（每主机1个）优雅停止、保留原目录/safety，再以同源码、同参数、同UID正常恢复；恢复不再加载bootstrap snapshot。每批其他27实例低频提交1笔普通转账，恢复后核对30实例的最终区块及ID。每批300秒恢复门槛，当前21人委员会任何一批最多掉3名，真实门槛15保持。

M30-08执行方案：30实例分10批，每批3个（每主机1个）优雅停止、保留原目录/safety，再以同源码、同参数、同UID正常恢复；恢复不再加载bootstrap snapshot。每批其他27实例低频提交1笔普通转账，恢复后核对30实例的最终区块及ID。每批300秒恢复门槛，当前21人委员会任何一批最多掉3名，真实门槛15保持。

M30-08执行方案：30实例分10批，每批3个（每主机1个）优雅停止、保留原目录/safety，再以同源码、同参数、同UID正常恢复；恢复不再加载bootstrap snapshot。每批其他27实例低频提交1笔普通转账，恢复后核对30实例的最终区块及ID。每批300秒恢复门槛，当前21人委员会任何一批最多掉3名，真实门槛15保持。

R05执行方案：单条P2P断连及同ID重发；对一个BP施加仅限自有P2P地址的双向隔离，保留RPC/SSH，其他20名确认者继续；按实际稳定政策权重选一台主机分成两组（两侧均低于门槛15），隔离20秒检查新块不能最终确认，恢复后检查统一链。延迟只匹配自有P2P流量，50ms；坏包只各发一次，无压力。临时防火墙规则在finally中撤回；现有UFW及防双签状态不清空。

R05执行方案：单条P2P断连及同ID重发；对一个BP施加仅限自有P2P地址的双向隔离，保留RPC/SSH，其他20名确认者继续；按实际稳定政策权重选一台主机分成两组（两侧均低于门槛15），隔离20秒检查新块不能最终确认，恢复后检查统一链。延迟只匹配自有P2P流量，50ms；坏包只各发一次，无压力。临时防火墙规则在finally中撤回；现有UFW及防双签状态不清空。


## 最终发布与关闭 / 2026-10-30T14:37:49Z

154/154项目闭环通过（含BAN-07/BAN-10实机补测），没有跳过用例。原生/组件/实机与历史证据按各自范围保留；弃用deferred仍不执行。节点交付0b78fb08a5ca808b7caf4c0d4ecadda9ea4921d9、编译来源00443330a0d29673700a4eebee78de3b0234c281，CDT交付b83d664ad5c05029bd509065e699436138fb98dd、编译代码d5816c442dd03db13413645f99c61a01c82fdda6。PureEOS与PureEOS CDT日期发行均已公开发布（CDT仓库保留原私有权限），14个发行资产下载检查全部SHA256一致。

PureEOS: https://github.com/MatrixCoreX/PureEOS/releases/tag/baseline-2026.10.30 
PureEOS CDT: https://github.com/MatrixCoreX/PureEOS-CDT/releases/tag/baseline-2026.10.30 

私有完整归档及全部原始失败、数据、安全文件留原主服务器；额外两台及其临时云SSH公钥条目已删除，列表和资源API两次确认不存在，原主服务器保留。原发布WASM、旧tags/assets、历史TPS范围不改写。本轮没有新增TPS测量。发布核验回执：/opt/aic-bp-full-functional-20261030-r1/PUBLISHED_BASELINE_DOWNLOAD_VERIFIED.json。
