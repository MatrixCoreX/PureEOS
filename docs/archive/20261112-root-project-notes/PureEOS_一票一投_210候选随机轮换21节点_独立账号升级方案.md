# PureEOS：一票一投、210 个候选随机轮换 21 个出块节点改造方案

> 原始代码梳理基线：PureEOS/Helios 提交 `a171f80`，`reference-contracts` 提交 `c526479`。2026-09-07 修订委员会规模、随机轮换、无节点治理、独立账号升级及文件级实施清单；保留“买入 REX 必须直接投一个候选、持有期间不得清空投票”，并按最新要求改为保留 REX CPU/NET 租赁、30 天续租/到期回收，补齐租金付款修正、资源上限核算和测试。本次只修改计划，不修改 PureEOS、CDT 或系统合约源码。

> 范围前提：本链从 0 号区块就是原生 Savanna 链，不存在 legacy→Savanna 激活或旧链数据迁移。Genesis 配置 `1 <= k <= 21` 个 proposer 和同一批等权 finalizer；逐人扩容到 21 人后，使用最多 210 个合格候选的快照进行随机单席轮换。稳定阶段只有当前 21 人参与出块及最终确认，Strong QC 门槛为 15；其余候选同步待命，不因位于候选池而取得 finalizer 权限。

## 1. 结论

目标结构为“210 个候选、21 个现任、通常 189 个备用”。正常轮换时，从当前 21 人中等概率随机选一人准备退出，从候选池中的非现任合格节点中等概率抽一人补入。先检查补位者，再提出新名单；候选未通过检查时，原 21 人名单保持不变。新名单生效并通过完整观察后，才开始下一次轮换。

增长期每次从 k 增为 k+1；稳定期每个选举 epoch 最多进行一次单席替换。候选池不满 210 时按实际合格数量运行，有足够现任但没有备用时保留原名单。若现任已跌出候选池，备用人数不一定是 189，必须按实际集合差计算。

| 项目 | 本版规则 |
|---|---|
| 投票 | 1 Token = 1 票；每个账户最多选择一个候选，禁止代理和时间放大；持有 REX 时必须保留一个选择，可改投，卖清 REX 后可撤票 |
| REX 买入门槛 | 买入或将 CPU/NET 转成 REX 前，必须由本人直接选择一个有效候选；仅 deposit 预存资金不要求投票，也不取得 REX 收益份额 |
| 候选池 | 按整数票数确定排名，取前 min(210, 合格候选数)，保存本轮快照 |
| 出块与最终确认 | 同一批 21 人；增长期为同一批 k 人；候选池中的其余节点保持同步待命 |
| 正常退出 | 对当前 21 人等概率抽一人，逻辑席位固定而退出顺序随机 |
| 正常补入 | 对本轮快照中的非现任可用候选等概率抽一人 |
| 故障补位 | 优先处理链上确认的故障席位，固定另外 20 人，仅为同一席位继续抽人 |
| 预检失败 | 不改变当前名单、不更换退出者、不更换快照或随机种子，只继续抽下一候选 |
| 正式加入后失败 | 可能造成漏块；隔离失败候选，按共识流程替换同一席位，不能承诺即时无缝接管 |
| 出块节奏 | 500ms/slot，每人 12 slot，即 6 秒；21 人完整轮次为 252 slot，即 126 秒 |
| 最终确认门槛 | quorum(k) = floor(2k/3)+1；21 人为 15，最多缺 6 票仍可达到该门槛 |
| 资源与 RAM | 保留 REX CPU/NET 租赁和 PowerUp；每种资源的直接质押＋REX 出租权重合计长期最多约 10%，PowerUp 占其余主要权重；RAM 四年减半 |

主要工程工作仍包括原生 Savanna Genesis、系统合约选举状态机、候选预检、P2P 预连接、最终确认名单切换和恢复测试。21 人是正常参与共识的规模；名单切换期间还需处理旧、新 finalizer policy，并保留双方的通信，因此不能把轮换成本写成零。210 个候选的同步、心跳、排名与快照也有额外成本。

节点不拥有系统治理权限：取消 eosio.prods 治理授权、BP 多签升级和人工管理入口。系统合约升级交给独立账号，未来可委托 AI 执行；共识签名与治理授权彻底分开，具体见第 3.10 节。

性能目标是：预检失败不占用正式出块席位，日常共识工作量保持在 21 人规模，受控轮换接近固定 21 人委员会的表现。是否达到目标必须以同环境压测验收，不能承诺完全不漏块、TPS 不下降或确认时间不变。

随机退出不保证每个现任在固定轮数内恰好退出一次；某个节点可能较早退出，也可能连续多轮留任。本版不规定固定的全员轮换完成时间。

本方案继续采用以下经济规则：

- CPU 质押、NET 质押和 REX 核心 Token 等价值 `vote_stake` 共同构成投票余额；按拥有者记账，同一 Token 只能计票一次；
- 一个账户的全部有效投票余额分配给唯一候选，拆分或合并账户不改变 Token 总票权；
- PowerUp 是付费资源购买，不生成生产者票权；保留第三方代付；
- 保留 REX `rentcpu/rentnet`、续租储备充值/取回、自动续租和到期回收，同时保留 REX 买卖、正常退出、投票余额和收益分配；REX 租期沿用 30 天；
- `buyrex` 和 `unstaketorex` 必须先直接投一个有效候选；持有 REX 期间只能保留或原子改投，不能清空投票；全部实际卖出、相关票权结清后可撤票，不要求节点给自己投票；
- CPU、NET 各自的直接质押＋REX 出租资源权重合计长期最多约为全网的 10%；直接质押本身也不超过该上限，其余主要由 PowerUp 提供；不是直接质押和 REX 各额外分配 10%；
- RAM 市场初始 64 GiB，以 1024 bytes/500ms slot 增发，每个固定四年期减半，1 byte/slot 阶段结束后归零。

本文中的“Token”指能够由系统合约原子追踪的已锁定投票余额，不能直接按可自由转账的 liquid balance 计票。`1.0000 TOKEN` 对应 1 票，内部使用 `asset.amount` 的整数最小精度单位。

`k=1` 允许技术性启动，但唯一节点停止后，链上扩容和补位都无法执行。自动化只能在当前名单仍能够出块、形成最终确认时推进。所有抽签必须由链上可验证、可重放的规则得出，不能让各节点使用本地随机数决定名单。

## 2. 当前代码的真实职责边界

### 2.1 生产系统合约源码不在 PureEOS 的测试目录

当前工作区中：

- 真正应作为改造基础的系统合约源码位于 `reference-contracts/contracts/eosio.system/`；
- `PureEOS/unittests/contracts/eosio.system/` 主要是 PureEOS 单元测试使用的合约源码/预编译 `eosio.system.wasm`；它的 CMake 只是复制现有 WASM，不是生产合约的主构建入口；
- 合约需要由 `cdt/` 工具链编译，再部署到特权账户 `eosio`；
- 为运行 PureEOS 自身的系统合约测试，才需要把相同版本的 WASM/ABI 更新到 PureEOS 测试夹具中。

因此，正式实施顺序应是先改 `reference-contracts`，再同步测试产物；不要只改 `PureEOS/unittests/contracts/eosio.system/voting.cpp`，否则生产链不会得到该逻辑。

### 2.2 当前行为

当前系统合约的关键逻辑如下：

- `reference-contracts/contracts/eosio.system/src/voting.cpp`
  - `voteproducer()`/`update_votes()` 允许一个账户选择最多 30 个节点；
  - `stake2vote()` 使用质押数量和时间放大系数计算票权；
  - 支持代理投票；
  - `update_elected_producers()` 直接取票数最高的 21 个节点。
- `reference-contracts/contracts/eosio.system/src/producer_pay.cpp`
  - `onblock()` 当前每 120 个 slot（约 1 分钟）尝试更新一次生产者调度；
  - 已经从区块头解析出 `previous_block_id`，所以最小版本的确定性种子不要求新增内核 intrinsic。
- `reference-contracts/contracts/eosio.system/src/finalizer_key.cpp`
  - Savanna 模式下，当前实现把 proposer（出块者）和 finalizer 策略一起更新。
- `PureEOS/libraries/chain/include/eosio/chain/config.hpp`
  - 区块间隔为 500ms；
  - 每个生产者连续生产 `producer_repetitions = 12` 个区块；
  - 当前系统合约和本版目标的 active 规模都为 21 人；完整轮转一次为 `21 × 12 = 252` 个 slot，即 126 秒（2.1 分钟）。本版改变候选来源和轮换方式；
  - legacy DPoS 的 `max_producers = 125` 仍存在于上游代码，但原生 Savanna Genesis 不走该路径；Savanna 的 `max_proposers = 64 × 1024`，可接受 21 人 proposer policy。
- `PureEOS/libraries/chain/webassembly/privileged.cpp` 与 `PureEOS/libraries/chain/controller.cpp`
  - 校验并接受系统合约提交的生产者列表；
  - 当前 `initialize_blockchain_state()` 明确把 Genesis 建成 `block_header_state_legacy`，这是原生 Savanna 创世必须修改的核心位置；完成改造后，初始 `k` 人从 Genesis 起直接走 Savanna policy，再逐步扩容到 21 人。

### 2.3 原始 EOS 投票、REX 与 PowerUp 的实际关系

原始 EOSIO `eosio.contracts v1.9.2` 采用质押加权的多选认可投票，不是本文定义的“一 Token 只投一个候选”：

- `voteproducer()` 允许一个账户直接选择最多 30 个生产者，或把全部票权委托给一个代理；
- `new_vote_weight = stake2vote(voter.staked)`，同一完整权重分别加给每个被选生产者，不在 30 个生产者之间平均分配；
- `stake2vote()` 使用随时间增加的全局系数；由于旧票只在投票、质押、REX 或代理状态变化时惰性刷新，长期未刷新的票会相对新票变弱；
- `voter.staked` 同时吸收 CPU/NET 质押变化和 REX 的核心 Token 等价值 `vote_stake`；
- 原始 `update_elected_producers()` 直接选择票数最高的 21 个 active producer，没有前 210 随机抽签；
- 原始 REX 曾要求持有人至少投 21 个生产者或选择代理；本版不恢复“21 个或代理”的旧规则，而是新增“必须直接投一个候选”的门槛。

当前工作区的 `buyrex()`、`unstaketorex()` 没有强制投票检查，`update_voting_power()` 也允许只创建余额记录而不选择候选。因此，“买入 REX 必须投票”是本计划待实现的新增规则，不是当前源码已经具备的行为；具体见第 3.7.1、5.4 和 13.1.1 节。

PowerUp 与投票是两条独立路径：

- `delegatebw/undelegatebw` 会调用 `update_voting_power()`，因此原始 EOS 中 CPU/NET 质押仍影响投票；
- `powerup()` 只临时调整账户的 CPU/NET 资源权重并把费用导入收益路径，不调用 `update_voting_power()`；
- PowerUp 可以由 `payer` 为 `receiver` 代付，这不会给任一方生成 REX 或生产者票权；
- 因此 PowerUp 上线后，被替代的是大部分 CPU/NET 资源取得方式，而不是自动废除旧 CPU/NET 质押的投票代码。

本文保留原始 EOS 的投票余额来源，即 CPU 质押、NET 质押和 REX `vote_stake` 都计入生产者票权，但改变计票方式：不使用时间放大、不能投多个候选、不能使用代理，同一 Token 只计入一个候选。资源账与票权账分开理解：CPU/NET 自有质押同时产生原始 Token 票权和受限的传统资源权重；持有 REX 产生其 `vote_stake` 对应票权；从 REX 租用资源的付款人/接收者不因租赁取得池内本金、REX 份额或生产者票权；PowerUp 也只产生付费资源，不产生票权。

## 3. 必须先固定的协议规则

下面这些规则必须写成链上协议，不能只存在于节点运维脚本中。

### 3.1 投票余额

票权来自账户实际拥有并锁定在 CPU、NET 或 REX 中的核心 Token：

```text
voting_balance(voter) = owned_cpu_stake(voter)
                      + owned_net_stake(voter)
                      + rex_balance(voter).vote_stake.amount
vote_weight(voter)    = voting_balance(voter)
```

这里的 CPU/NET 数量按现有 `delegatebw` 所有权和 `transfer` 语义归属，不按资源最终接收账户重复计票。不使用账户数量，不使用持币时间倍增，也不能让同一份 Token 同时通过 CPU、NET、REX、代理或多个候选重复计票。拆分账户只会拆分同一总余额：锁定 100 Token 的一个账户和合计锁定 100 Token 的十个账户，总票权都只能是 100。

`delegatebw`、`undelegatebw`、`buyrex`、`sellrex`、已成交卖单、`unstaketorex` 以及任何会改变上述三项余额的路径，都必须在同一状态转换中按净变化量更新唯一候选的票数。CPU/NET 转入 REX 时必须先从 CPU/NET 票权扣除、再加入 REX `vote_stake`，净票权不能凭空增加；PowerUp 购买不得调用生产者票数更新路径。

### 3.2 有效票

定义：

- `proxy` 必须为空；
- `producers.size()` 只能为 0 或 1；
- 空列表表示撤票，但账户仍持有任何 REX 份额时拒绝，只有全部实际卖出并结清对应票权后才允许；只持有 CPU/NET 质押而没有 REX 的账户仍可撤票；
- 非空列表表示把账户当前全部 `voting_balance` 分配给唯一候选；
- 新候选必须已注册、处于 active 状态并有有效 block signing authority；
- 重复提交相同选择应当报“无变化”或成为无状态变化操作，不能重复加票；
- 从 A 改投 B 必须在同一个 action 中完成 A `-voting_balance`、B `+voting_balance`，不能出现中间不一致状态；
- 增加或减少锁定余额时，必须对当前唯一候选原子执行相同数量的增票或减票。

首次买入 REX 前，允许尚无 CPU/NET/REX 余额的账户由本人授权预先选择一个候选：只记录选择，票数为 0；后续买入成功时才把真实余额计入该候选，不能凭空生成票权或激活份额。候选选择不要求其已有正票数、已进入前 210 或已在出块委员会，否则会阻断新候选获得第一票。持有 REX 后的撤票和候选失效边界见第 3.7.1 节。

### 3.3 排名和候选池

候选节点必须已注册且 active，票数大于 0，block signing authority 有效，具备已激活的 BLS finalizer key，并登记有效 peer key 和 endpoint。节点应持续同步并更新签名心跳；登记或自报在线不能替代抽中后的门限预检。没有最终确认密钥的节点不能进入本版候选池，因为加入委员会后要同时承担出块和最终确认。

排名顺序固定为：

1. 整数票数从高到低；
2. 同票时按账户名的 `name.value` 从小到大。

候选快照 C 是前 `min(210, 合格候选数)` 个。其余节点可以注册、获得票数，但未进入该快照前没有本轮抽签资格。冷却期和本轮已拒绝的候选不得被补抽；本轮冻结后不能为了凑数把排名边界之外的节点临时加入。

当前 active 委员会记为 A。正常备用集合为 `C - A - quarantined - rejected_this_draw`。当 C 有 210 人且包含全部 21 名现任、没有额外排除时，备用为 189 人；现任跌出 C 或处于增长期时，应按实际集合差计算，不能硬编码 189。

本版使用候选账户等概率抽签。拆分投票账户不增加总票权，但同一运营者注册多个候选仍可能增加中签机会；候选保证金、主体约束或席位上限如需启用，应另行明确，不能把一票一投等同于防止候选身份拆分。

### 3.4 从 k 人增长到 21 人及随机单席轮换

维护 21 个稳定逻辑席位 `seat[0..20]`。席位号用于标识故障和补位，不能用按账户名排序后的调度下标代替。逻辑席位固定不代表按席位顺序轮出：正常退出者必须随机产生。

Genesis 配置相同 k 个账户的 proposer 和等权 finalizer policy，`1 <= k <= 21`，阈值为 `floor(2k/3)+1`。Genesis 初始名单是启动配置，应公开核对，不宣称来自尚未建立的链上抽签。后续新增席位从冻结候选快照中随机选择非现任，逐个组建到 21 人。

增长阶段：

```text
A = 当前已经稳定激活的 k 人委员会
in = 从本轮前210候选快照中抽取非现任，并通过预检
target proposers  = A + in
target finalizers = 同一批账户，权重均为1
target threshold  = floor(2(k+1)/3) + 1
```

每次只增加 1 人。等待两份目标 policy 都 active，再观察至少 `(k+1) × 12` 个 slot，覆盖新节点自己的完整 12-slot 窗口，并验证其出块、BLS 投票及链的最终确认正常，才允许继续增长。若新节点在加入后失败，固定另外 k 人，为刚加入的席位补抽；目标规模保持 k+1，不再继续扩大，也不通过移除该席位来缩小 policy。

达到 21 人后进入 rotating 阶段：

```text
A = 冻结的当前21人委员会
out_seat = 用本轮种子及EXIT域，对当前21个占用席位等概率抽取
out = seat[out_seat]
standby = C - A - quarantined - rejected_this_draw
in = 用同一轮种子及ENTRY域，从standby等概率抽取并通过预检
target = (A - {out}) ∪ {in}
```

具体约束：

- 正常轮换随机选择退出者；故障队列优先于正常轮换，故障退出者由链上证据确定，不再随机；
- 现任已注销、密钥失效或跌出候选快照时，可按最小逻辑席位号优先替换该类成员；没有此类事件时执行随机退出；
- 补入抽签排除本轮冻结的全部现任，包括准备退出者，不能抽中原节点形成空替换；
- 预检失败或名单提交重试时固定退出席位、快照和种子，只推进补入抽签 counter；
- 补位者继承同一逻辑席位；其正式加入后失败时，固定其他 20 人，继续处理这个席位；
- 同时只允许一份在途变更；两份目标 policy 激活且完整观察通过前，不再提出下一次扩容或轮换；
- 稳定阶段任何普通变更都必须断言新旧 proposer 名单恰好一出一进、大小均为 21，并为 finalizer 提出相同目标账户集合；
- 故障替换保持当时规模不变；不保留节点治理或管理员直接提交紧急多人名单的 action；未知问题按第 3.10 节的独立版本升级边界处理。

随机退出没有“21 次就恰好走遍每个席位”的保证。每轮指选举机会，不保证每 126 秒一定完成一次替换；预检、激活、观察未完成时应延后，而不积压多次轮换。

### 3.5 在线资格、预检与掉线淘汰

生产流程必须先检查补位者，再提交替换名单。候选未通过检查时，当前 policy 和现任职责保持不变。这能够避免已知故障候选占用正式席位，但不能证明它在正式加入后永不故障。

系统合约不能直接 ping IP/端口，也不能仅凭账户签名或 HTTP 成功响应判定 nodeos 正常工作。应把在线资格定义为可验证、可重放的链上状态。

#### 3.5.1 如何证明 P2P 可连通

由不同网络、地域的授权 sentry 或当前 BP 提交签名见证，至少检查：

1. 候选 endpoint 可解析、可建立 TCP 连接，并能完成兼容本链 chain ID 的 nodeos 握手；
2. endpoint 公告绑定登记的 peer key，不能冒用其他正常节点的地址；
3. 候选 head/LIB 差距在配置范围内，并在观察窗口持续收到新区块；
4. 同时测试见证者连接候选，以及候选连接指定 relay，检查入站和出站路径；
5. 连接在最小观察窗口保持稳定，没有严重延迟、反复掉线或握手后不传块。

见证绑定 `chain_id || draw_id || candidate || endpoint || recent_lib_id || nonce || expires_at`，合约校验签名、时效、轮次、最近已确认区块和 nonce，不重复计算同一见证者。采用启动版本固定的跨地域 M-of-N 门限；3/5 只是待确定的示例，不由 BP 审批修改。

候选心跳用于预警与准入辅助，不能独自证明公网可达。单个监控器没有直接变更名单或永久剥夺资格的权限。

#### 3.5.2 上岗前检查与上岗后观察

预检还必须验证：

- producer authority 有效，并对本轮 challenge 给出满足该 authority 的签名；
- 已同步，正确载入 producer name、签名服务和 current/next 连接配置；
- BLS key/PoP 有效，finalizer 服务和 safety state 已正确准备；
- ready 证明未过期，实际提交名单前再次校验候选资格和证明时效。

预检流程：

```text
抽中候选 -> preflight（原名单继续工作）
  -> 证明不足或超时：隔离候选，固定原名单和退出者，继续抽下一人
  -> ready：提出替换后的proposer和finalizer目标名单
  -> 等两份目标policy都active
  -> observing：观察完整轮次，通过后才允许下一次轮换
```

两份 policy 是分别按 Savanna 规则晋升的，不能假设在提交 action 中立即或同时激活。旧节点必须保持服务，直到两类 active/pending policy 均不再需要其职责；过渡期保留旧、新成员连接。

稳定阶段 observing 从两份目标 policy 都被确认 active 后计时，至少 252 个 slot，并且确实覆盖补位者的完整 12-slot 出块窗口。通过标准同时包含：启动版本规定的出块率、持续 P2P/同步正常、补位者有效 BLS 投票可见、Strong QC 持续形成及 LIB 推进。增长或故障替换阶段按目标规模 k 观察至少 k×12 个 slot，使用相同检查口径。

“至少出过 1 个块”只意味着没有触发整窗零出块故障，不能替代完整观察验收。QC 未包含某人的一次投票也不能单独证明它掉线，需结合连续已验证数据判断。

候选在 ready 前失败：不提交名单，原来的 21 人全部保留。候选正式加入后失败：保留实际 policy 的规模，固定其他 20 人，对同一席位继续预检和补抽。旧退出者不会自动恢复权限，也不能直接接管失败候选的时间槽。

正式加入后的故障可能造成每个失败轮值窗口约 6 秒空窗；发现、重抽、预检和 policy 晋升还需要额外时间。预检与预连接用于降低发生概率，不能承诺绝不漏块或瞬间恢复。所有替换都必须经过统一的链上名单和共识生效流程。

#### 3.5.3 基于不出块的自动发现与自动补位

`onblock()` 可依据已收到区块头的时间槽和生产者，识别相邻实际区块之间完全跳过的生产窗口，无需外部机器人单独提交“某节点掉线”交易。

```text
window_id    = floor(timestamp.slot / 12)
schedule_idx = window_id mod active_producers.size()
expected_bp  = active_producers[schedule_idx]
```

这里必须使用对应生效时间段的真实 active proposer 集合及其实际排序；增长期不能硬编码 21，逻辑席位号也不能替代 schedule_idx。

建议增加以下链上状态：

| 状态 | 内容 |
|---|---|
| schedhealth | active proposer 缓存及 digest、last_seen_slot/window、当前变更类型、阶段、目标两份 policy 标识和观察起点 |
| seathealth | 逻辑席位、实际 active 成员、最后出块窗口、失败计数、最近故障以及本 policy 是否已排队 |
| faultqueue | 按 detected_window、逻辑席位号排序的故障记录，绑定 producer 和检测时的 policy digest；同一席位不重复排队 |

每次 onblock 做有界的小步处理：

1. 对已确认生效的目标 policy 更新缓存；切换边界上只统计归属明确的完整窗口，不把旧成员的空窗计给新成员；
2. 对完整跳过的 12-slot 窗口记录 fault observation；至少有一个合法块的窗口不属于“整窗零出块”；
3. 等观察被 Savanna finality 覆盖后，才加入正式故障队列；fork rollback 不得留下错误记录；
4. 一次跨越很多轮时，最多处理当前 k 个唯一席位（k≤21）；使用游标或去重避免无限循环；
5. 丢弃 producer 已离开对应 active policy 的陈旧记录，并小步推进唯一替换流程。

故障状态机：

```text
idle:
    若faultqueue非空，暂停正常随机轮换
    确认队首故障仍属于当前active成员，锁定其逻辑席位

drawing:
    冻结前210候选快照和当前委员会
    固定其他(k-1)人，抽取非现任可用候选

preflight:
    验证候选连接、同步、producer/BLS准备和门限ready证明
    失败则隔离该候选，用同一快照、种子和递增counter继续抽取
    无候选则保留原policy，记录原因并等待

proposed / waiting_activation:
    提出相同目标账户集合的proposer和等权finalizer policy
    规模保持k，threshold = floor(2k/3)+1
    两份目标都active前，不发起下一变更

observing:
    观察至少k×12槽，覆盖补位者完整12槽窗口
    出块率、BLS投票、P2P、Strong QC、LIB均通过才完成
    确认失败则隔离候选，固定其他(k-1)人，为同一席位继续抽取
```

不提供生产环境“先加入、再检查”的无预检捷径。健康候选耗尽或随机源不可用时保持当前 policy 并告警。已提出但尚未生效的 policy 不能假定可撤回；此时发现候选故障，应记录并在协议允许的下一安全状态处理，不叠加第二个提案覆盖当前状态。

为避免同一失败快照永久卡住，预检或补抽在尚无在途 policy 时允许按启动版本规定的链上超时结束本次尝试，记录 `exhausted/aborted` 及原因。后续以新的 draw_id 冻结新快照并请求新随机性。正常轮换此时原委员会保持不变；故障或观察失败后的补位必须继续锁定同一故障席位及其余成员，不借新尝试重选退出者。不能让提交者随意丢弃不喜欢的抽签结果。

系统合约需要由 PureEOS/CDT 暴露以下版本化、只读、确定性接口：

```text
get_active_finalizer_policy_v1() -> generation + policy_digest
get_last_final_block_v1()        -> block_num + block_id + timestamp
get_last_qc_vote_metrics_v1()    -> qc_block_num + policy_generation
                                    + strong/weak/missing vote bitsets
```

这些数据必须来自对当前执行区块确定的、已验证的链上共识状态，不能取决于本机尚未上链的投票到达顺序。只有本地缓存的数据不能直接作为合约判定依据；实现阶段须验证 replay、fork 和历史区块执行时的可用性及一致性。外部监控只能辅助诊断，不能替代共识依据。

稳态检测应为每块 O(1)，出现 slot gap 时最多触碰 21 个唯一席位。排名和快照在有界选举步骤中处理；正常等待、无候选、超时等应转为状态，不让隐式 onblock 因预期错误影响区块执行。

上述自动扩容和补位依赖当前链仍有可用最终确认门槛。21 人时至少需要 15 人，缺 7 票便不能形成 Strong QC；若全网没有后续区块，onblock 也无法执行。因此自动补位不承诺在最终确认或出块已经完全停止后自救。

### 3.6 候选不足时

- Genesis 校验相同 k 个账户的两份 policy，1≤k≤21、BLS key 唯一、权重为 1、threshold 正确；
- 增长期没有可通过预检的非现任时保持当前规模，不加入未运行节点凑数；
- 稳定期 C-A 为空、候选全被隔离或预检失败时，保留原 21 人 policy，不先缩成 20 人；
- 稳定期无需强制凑满 210 个候选；候选快照上限为 210，委员会目标固定为 21；
- 候选池变化不能让当前委员会瞬间重组；现任退出及故障恢复始终按单席串行流程进行；
- 正式候选名单不足以组建 21 人时继续 growing，不把小规模启动成功作为生产验收通过；
- k=1 的唯一节点故障时依赖同身份主备恢复服务，不能依赖没有新区块可执行的自动抽签。

### 3.7 REX、CPU/NET 与 PowerUp 的职责分离

PureEOS 将锁定投票余额、预存资金和付费资源使用权明确分开：

| 路径 | Token/费用状态 | 获得生产者票权 | 获得交易资源 |
|---|---|---:|---:|
| REX `deposit/withdraw` | 预存或取回个人 REX fund 中的核心 Token，尚未购买的资金不属于 REX 份额 | 否 | 否 |
| REX `buyrex/sellrex` | 买入前必须投一个候选；买卖维护 REX 份额和 `vote_stake`，卖清后可撤票 | 是，按 `vote_stake` 原始数量 | 不直接获得传统 CPU/NET 质押权重 |
| REX `rentcpu/rentnet` | 从个人 fund 支付本期租金，另存续租储备；取得 30 天资源使用权 | 否；租金、储备和租来的资源均不给租用者增票 | 是，增加接收者对应资源权重，计入第 3.8 节非 PowerUp 总账 |
| `delegatebw/undelegatebw` | Token 锁入或退出 CPU/NET 质押 | 是，按拥有者锁定的原始 Token 数量 | 是，进入传统质押资源权重 |
| `powerup` | 支付费用购买临时 CPU/NET | 否 | 是，进入 PowerUp 付费资源权重 |

CPU/NET 和 REX 可以共同构成一个账户的 `voting_balance`，但同一份 Token 在任一时刻只能处于其中一条锁定路径；跨路径迁移必须按净变化原子记账。REX 可以继续接收 REX 租金、PowerUp 等系统费用产生的收益，但支付这些费用本身不等于买入 REX，也不会给付款人或资源接收者增加 `vote_stake`。租用者获得的 `total_staked` 是资源权重，不是其拥有的 CPU/NET 投票质押；同一份池内本金的票权仍只归相应 REX 持有人。

`delegatebw(from, receiver, ...)` 可以继续允许持有人为其他账户质押，REX 租赁允许 `from != receiver`，PowerUp 也允许 `payer != receiver`。代付不额外产生 Token 或票权；REX 新租、续租和回收会改变实际出租权重，必须纳入第 3.8 节的统一上界，不能仅因它付了费用就漏记。无需为了防 spam 强制付款人与资源接收者相同。

#### 3.7.1 买入 REX 必须投票，持有期间不得清空选择

面向用户的规则是：先选一个候选，再买入 REX；持有期间可以从 A 改投 B，但不能变成“谁都不投”；不想再投票时，按正常成熟和流动性规则卖清 REX，结清票权后撤票。该限制同时适用于普通用户和节点账户，不要求自投，不允许代理，也不恢复旧的至少投 21 人规则。

具体边界固定如下：

1. `buyrex(from, amount)` 检查 `from`，`unstaketorex(owner, receiver, ...)` 检查资产拥有者 `owner`，不是资源接收者 `receiver`。在扣资金、改质押或增加 REX 份额前，确认拥有者的 `proxy` 为空，`producers` 恰好有一个候选，且该候选满足第 3.2 节的注册、active 和有效出块签名条件。
2. `deposit` 只预存资金，可以不投票；不能把预存余额算成 REX 持仓、收益份额或票权。首次买入可在同一笔交易中依次执行 `deposit → voteproducer(唯一候选) → buyrex`；没有 CPU/NET 质押也能先保存零票选择，不要求额外质押作为开户门槛。
3. 为防止“买入后立刻撤票”绕过门槛，持有 REX 时拒绝空投票列表。持仓以 `_rexbalance.rex_balance.amount > 0` 判断，覆盖成熟、未成熟、储蓄和排队待卖的全部份额；不能只看 `matured_rex`、四舍五入后的 `vote_stake` 或是否发出了卖单。当前表中未成交卖单的份额仍包含在 `rex_balance` 内，不再重复相加。
4. 持有期间允许一个 action 内从 A 原子改投另一个有效候选 B。禁止先清空再投，也禁止改成代理；新选择失败时原选择、候选票数和余额必须完整回滚。选举投票只影响候选票数，不授予节点系统治理权。
5. 部分卖出或仅挂出全部卖单不能解锁撤票。只有 REX 份额实际归零、已成交卖单及对应票权变化均正确结算后，才允许清空选择；账户可保留尚未提现的 REX fund 或零余额表行，不要求先执行 `withdraw/closerex`。若仍有 CPU/NET 质押，撤票应再扣除其剩余已分配票权一次。
6. 所投候选后来注销或失效时，不自动替用户改投，也不要求全表扫描或暂停所有结算。保留原选择及可核对的票权记录，失效候选不能因此取得候选池资格；再次买入/转入 REX 前须先改投有效候选。用户仍可改投或按原有成熟、流动性规则卖出退出，即使暂时没有其他有效候选，也不得因候选有效性检查阻断赎回。
7. 本节只约束买入/持有 REX 份额，不限制单纯租用 CPU/NET、充值或取回续租储备；租用者不必先买 REX 或投票。如果同一账户另行持有 REX，仍遵守持有期规则。收益仍按 REX 份额分配，不新增候选失效时扣收益、罚本金、强制代投或按投票对象分成的逻辑；仅 CPU/NET 投票者不因此取得 REX 收益。本次也不决定取消通胀或把手续费改分给节点。

本链按第 12 节从新规则创世启动，无须给旧的无投票 REX 持有人安排迁移或豁免；若以后改变为已有链升级，必须另行设计迁移，不能直接套用本节假定。

### 3.8 CPU/NET 直接质押与 REX 出租合计最多约 10%

#### 3.8.1 目标配置

PowerUp 使用 `powerup_frac = 10^15` 表示 100%。作为参考，2026-07-29 查询 EOS 主网 `powup.state` 时，CPU 和 NET 都是：

```text
weight_ratio        = 10000000000000
target_weight_ratio = 10000000000000
```

即标称 1%。PureEOS 的目标值为：

```text
CPU current_weight_ratio = 100000000000000
CPU target_weight_ratio  = 100000000000000
NET current_weight_ratio = 100000000000000
NET target_weight_ratio  = 100000000000000
```

即 CPU 与 NET 分别采用标称 10%，不是把二者合计成 10%。保留 REX 租赁后，直接质押和 REX 出租共同计入每种资源的非 PowerUp 权重；两者合计长期上限约 10%，不是各给 10%。这沿用现有出租时增加接收者权重、而不扣 PowerUp 预留权重的模型；本版不另建一个从 PowerUp 侧扣额的 REX 市场。PowerUp 保留其余主要资源权重，并保持动态定价和代付能力。

#### 3.8.2 用最大可质押供应量建立上界

标准 PowerUp 不是物理上分开的 10%/90% 队列，而是把直接质押、REX 出租和 PowerUp 权重放进同一资源权重体系。以下按 CPU 或 NET 分别计算，设：

```text
r = weight_ratio / powerup_frac
A = assumed_stake_weight
S = 该资源的实际直接质押权重（按核心Token最小精度）
L = 该资源仍占用配额的REX租单total_staked之和
P = PowerUp市场权重
M = 协议允许的最大可质押核心Token供应量（同一精度）
```

合约计算关系为：

```text
P = A × (1/r - 1)
非PowerUp资源权重占比 = (S + L) / (S + L + P)
其中直接质押资源占比 = S / (S + L + P)
```

当 `r = 10%` 时，`P = 9A`。CPU、NET 各自的 `assumed_stake_weight` 仍设为 `M`，但现在须验证 `S + L <= M`，而不能仅检查 `S <= M`。成立时：

```text
直接质押与REX出租合计占比 = (S + L) / (S + L + 9M) <= 10%
直接质押自身占比 = S / (S + L + 9M) <= 10%
```

上界依据是同一份核心 Token 不能既作为自有 CPU/NET 质押本金，又作为 REX 池内本金被重复出租。须验证 CPU＋NET 租单的 `total_staked` 总和与池内 `total_lent` 一致，`total_lent <= total_lendable`，且直接质押与池内本金由真实资金支撑。`vote_stake` 是持有人的投票账，不再额外加进资源权重；`total_rent` 是定价连接器参数，也不能视为新本金或资源额度。已到期但尚未回收的租单仍计入 `L`，不能提前从上界中扣除。

因此安全性不依赖自然人或账户数量：拆账户、代付及同时租 CPU/NET 均不能重复计算同一份本金。应使用协议允许的最大供应量，而不是当前实际直接质押量；未来若独立账号发布的版本改变供应上限、资源生成或资金记账规则，须同步重新证明该上界，BP 无修改权限。实现时对共享池和各资源租单维护可校验的汇总，不为每次新租或每个区块全表扫描。

这种保守设置意味着实际 `S + L` 远低于 `M` 时，非 PowerUp 资源份额低于 10%；REX 租赁属于付费业务，但在此权重口径下与直接质押共用该上限。10% 是安全上限，不承诺任何时刻都恰好预留满 10%，也不会因新增租赁入口创造额外 CPU/NET 容量。

#### 3.8.3 配置与升级约束

本链应在首次启用 PowerUp 时直接以 10% 初始化 CPU 和 NET 的 `current_weight_ratio` 与 `target_weight_ratio`。标准 `cfgpowerup` 要求：

```text
target_weight_ratio <= current_weight_ratio
```

因此初始配置直接把 current/target 同时设为 10%。正式开放交易前联合验证 PowerUp `state.weight`、直接质押与 REX 租单权重、资源利用率和两套价格曲线；新链不需要旧参数迁移。不能因保留 REX 重新开放 `cfgpowerup/setrex` 等上线后的人工改价、改资源比例入口。

#### 3.8.4 保留 REX 资源租赁与 PowerUp

最新决定是保留两种付费资源入口，同时保留 `delegatebw` 直接质押。REX 的资源租赁、购买份额和收益结算是不同业务，不能因为买入份额必须投票，就要求租用资源的人也先投票。

##### 3.8.4.1 保留的 action 和授权边界

- 保留 `rentcpu`、`rentnet`，由付款人 `from` 授权，可给不同的 `receiver` 使用资源；无需节点批准，也不取得 REX 份额或额外票权。
- 保留 `fundcpuloan/fundnetloan` 和 `defcpuloan/defnetloan`，仅租单付款人可充值或取回该租单的未消费续租储备；保持未到期、正金额、币种及余额校验。
- 保留 `rexexec` 和相关 REX action 中有界的 `runrex` 结算；任意账户可以自己授权支付执行成本，按确定规则处理到期租单和待卖 REX，不需要治理多签。
- 保留 `buyrex/sellrex/unstaketorex`、储蓄、成熟及正常退出，继续执行第 3.7.1 节的持仓投票约束。
- 保留 `powerup/powerupexec`、动态定价和第三方代付；`cfgpowerup` 与 `setrexmature` 仅用于首次初始化，`setrex` 人工覆盖定价余额仍关闭。用户管理自己的租单不是节点治理，不应与这些系统管理入口一起删除。

##### 3.8.4.2 新租、续租、到期与退款

1. 新租从个人 REX fund 扣除 `loan_payment + loan_fund`；前者为本期租金，后者为未消费续租储备，不把整笔扣款都确认为收入。按池子当前报价计算 `total_staked`，增加接收者 CPU 或 NET 资源权重，创建从当前时间起 30 天的租单；不把对应本金转给租用者自由支配。
2. 保留池可用性和报价保护：没有可用 REX 池、存在阻止出租的未成交卖单，或报价不满足当前 `payment < rented_tokens` 条件时拒绝新租。此比较是合约的报价门槛，不代表租用者买到了可变现的本金或必然获利。
3. 到期结算先释放上一期占用，再按当时价格计算可续租权重。续租储备至少够支付原来一期租金、报价通过且池允许出租时，扣一期储备，更新资源权重，并在原到期时间上加 30 天；每期支付金额沿用租单约定，但租到的权重可变化。
4. 续租条件不满足则关闭租单，只扣除接收者由该租单获得的权重，将剩余储备退回原付款人的 REX fund，随后可 `withdraw` 回钱包；不影响其自有质押、其他租单或 PowerUp 资源。已消费的本期租金不按剩余天数退款；本次不新增提前退租退本期租金功能。
5. 时间到达不意味着无人触发也会自行执行。保留相关 REX 操作顺带处理少量订单，以及任何账户调用 `rexexec(user, max)` 的机制；`max` 按 CPU 租单、NET 租单和待卖单各自计数，执行总量须受 CPU 预算约束。租单到期积压、一次跨过多个租期和资源回收延迟必须可观测；不承诺到期当秒回收，也不在本次给 `onblock` 新增无界结算。
6. REX 持有人出售份额时保留成熟、流动性和卖单排队规则。未成交卖单阻止新租及自动续租，使到期资金可用于退出；不能删除这项检查而让无限续租挤占持有人赎回。结算中候选失效不得阻断租用者续租/退款或其他持有人的退出。

##### 3.8.4.3 租金流向与必须修正的付款账户

计划中的实际收款路径为：用户通过 `deposit` 将钱转入 `eosio.rex` 并记入个人 fund；新租扣个人 fund、续租扣租单储备后，由保管实际资金的 `eosio.rex` 将本期租金转给 `eosio.fees`；在已部署的费用合约按本计划分配且 REX 池可用时，经 `donatetorex` 返回 `eosio.rex` 的收益池，按 REX 份额逐步形成收益。

当前 `rex.cpp::add_loan_to_rex_pool()` 调用的是 `channel_to_system_fees(get_self(), payment)`，实际付款者是系统合约账户 `eosio`，与用户资金存放于 `eosio.rex` 不一致。实施时必须改为从 `rex_account` 转出对应已扣账租金，覆盖新租和续租，不能依赖额外给 `eosio` 充值来掩盖问题。只修改该租金调用点，不能把 RAM、PowerUp 等其他业务的付款来源一并改错。

同时审计 `eosio.rex` 的系统合约 code 授权、`eosio.fees` 的生产部署及收益入池记账，确保租金只入收益一次；本金、个人 fund、续租储备、待释放收益和待结算卖单账目均有真实资金覆盖。`loan_fund` 未消费部分不是收入，`total_rent` 定价参数也不等于可自由支出的现金。转账或后续校验失败必须整笔回滚，不能只扣用户账或只增加收益；不新增管理员/BP 提款权限。

保留租赁是保留收费渠道，不保证总收入上升：用户从 PowerUp 转到 REX 可能只是分流。收益仍按 REX 份额分配，不自动改成节点出块收入，也不在本次取消增发或另定手续费分成。

##### 3.8.4.4 租期、报价及本次未定事项

- REX 沿用当前 30 天新租/续租期限；该期限与 REX 份额成熟期是不同参数，`setrexmature` 不决定租单时长。
- PowerUp 继续使用上线版本确定的统一 `powerup_days`。当前源码默认 30 天；此前提出的“PowerUp 1 天短租＋REX 30 天长租”仅为建议，尚未定案，本次不自动将 PowerUp 改为 1 天，也不新增任意天数套餐。
- 两套定价算法分别保留：REX 根据资金池连接器和可出租本金报价，PowerUp 根据资源利用率、调整后利用率及价格曲线报价。没有“REX 必须更贵”或“30 天自动折扣”的规则；比较成本必须统一资源份额和使用天数，不拿续租储备或 `max_payment` 当作实际收费。
- REX 的初始 `total_rent`、PowerUp 租期/价格参数须在启动版本中明确；若以后变更租期，必须连同单位时间定价、旧租单处理和资源上限重新验证，不能只把常量 30 替换为 1。参数按第 3.10 节冻结或由独立账号发布升级，不交给节点治理。

#### 3.8.5 spam 防护边界

本方案主要约束这种有效交易攻击：

```text
锁定Token到CPU/NET -> 获得可恢复资源 -> 持续发送垃圾交易
```

在上述最大供应量校准成立时，直接质押和 REX 出租合计只能持续竞争最多约 10% 的全网 CPU 权重和最多约 10% 的全网 NET 权重，其中直接质押也不超过该上限。攻击者可自有质押或付费租用 REX 来使用这一部分；要使用 PowerUp 侧资源仍须付费购买。REX 属于付费业务，不能因为资源账与直接质押共用上限，就将租赁流量标记为免费流量。

但必须保留以下边界说明：

- 原始资源模型按恢复窗口和权重计量，10% 是长期/持续权重上界，不是每一个区块精确的 10% 硬分区；账户积累额度后仍可能产生短时突发；
- `weight_ratio` 本身不创建两条独立交易队列，区块生产者的排队和打包策略仍会影响瞬时体验；
- 免费 spam 可以让免费用户互相拥塞，系统只保证它不会长期获得超过设定上界的资源权重；
- REX 长租和 PowerUp 同样可能被用于付费 spam；租金不是网络层防护，也不保证市场定价永远足以阻止资源囤积。测试须覆盖低价 30 天长租、自动续租和资源回收积压；
- 无效交易、握手洪水和交易在被拒绝前产生的验证开销不受链上 CPU/NET 计费完全保护，仍需 P2P 限速、重复交易过滤、连接配额和 peer 惩罚；
- 如果未来另行要求“每个区块的免费交易绝对不超过 10%”，必须在 PureEOS 内核增加独立资源账本、交易分类和共识校验，不能只调整系统合约参数。

本文默认采用参数化的长期上界方案，不把逐区块硬隔离列为首期必需改造。

### 3.9 RAM 按 EOS 初始速率四年减半

#### 3.9.1 固定发行参数

本链的 RAM 市场从第一版采用以下不可变参数：

```text
initial_max_ram_size = 64 GiB
initial_ram_rate     = 1024 bytes/slot
slot_interval        = 500 ms
halving_interval     = 4 × 365 × 24 × 60 × 60 × 2
                     = 252,288,000 slots
```

`ram_emission_start_slot` 取 RAM 市场第一次初始化时的当前区块槽位，写入系统合约 singleton 后不得修改。当前 `setramrate(uint16_t)` 的底层类型以 byte 为单位，因此 1024 可以精确地依次减半到 1；不使用浮点数，也不产生小数字节。令：

```text
era(slot) = floor((slot - ram_emission_start_slot) / 252,288,000)
rate(era) = era <= 10 ? (1024 >> era) : 0
```

因此完整发行曲线为：

```text
第 0～4 年：   1024 bytes/slot
第 4～8 年：    512 bytes/slot
第 8～12 年：   256 bytes/slot
第 12～16 年：  128 bytes/slot
第 16～20 年：   64 bytes/slot
第 20～24 年：   32 bytes/slot
第 24～28 年：   16 bytes/slot
第 28～32 年：    8 bytes/slot
第 32～36 年：    4 bytes/slot
第 36～40 年：    2 bytes/slot
第 40～44 年：    1 byte/slot
第 44 年以后：    0 bytes/slot
```

`1 byte/slot` 再减半会得到半个 byte，协议无法表示，因此下一周期不是四舍五入或继续保持 1，而是直接归零并永久停止自动增发。

“四年”固定定义为 1460 天，不引入闰年、当地时区或治理解释。减半时间由共识区块时间槽位计算，不读取节点墙钟。当前 EOS 合约的 `update_ram_supply()` 本来就是使用 `current_block_time().slot - last_ram_increase.slot` 计算增量，因此这里保留 EOS 的槽位语义：即使某些预定出块槽没有实际产块，后续更新仍按跨过的槽位数累计，不按成功区块计数。

#### 3.9.2 必须跨边界分段累计

RAM 供应量仍采用惰性更新：在买卖 RAM、CPU/NET 质押等既有触发点，先把供应量结算到当前槽位。区别是不能再用一个当前速率乘以整个时间差。如果 `last_ram_increase` 与当前槽位跨过一个或多个减半边界，必须逐段计算：

```text
cursor = last_ram_increase.slot
while cursor < current_slot:
    era       = era(cursor)
    if era > 10:
        cursor = current_slot
        break
    era_end   = ram_emission_start_slot + (era + 1) × halving_interval
    segment   = min(current_slot, era_end) - cursor
    new_ram  += segment × rate(era)
    cursor   += segment

max_ram_size                 += new_ram
rammarket.base.balance.amount += new_ram
last_ram_increase.slot        = current_slot
```

实现必须使用有界循环或直接的几何级数/分段公式。增发时代总数只有 11 个，故即使多年没有 action 触发更新，单次结算的计算量仍有严格上界；44 年后的区间直接使用 0。更新 `max_ram_size` 与 RAM 市场 base reserve 必须在同一 action 中完成，否则 RAM 市场报价会与可分配供应量不一致。

#### 3.9.3 供应量上界

11 个四年增发阶段总计：

```text
252,288,000 × (1024 + 512 + 256 + ... + 2 + 1)
= 252,288,000 × 2047
= 516,433,536,000 bytes
≈ 480.966210365 GiB
```

加上创世初始 `64 GiB = 68,719,476,736 bytes`，最终 RAM 市场供应上限为：

```text
585,153,012,736 bytes
≈ 544.966210365 GiB
≈ 585.153 GB（十进制）
```

第 44 年边界后不再自动增加。这里的 RAM 是链上可购买的状态额度，不等于节点启动时必须配置同等大小的物理内存；节点硬件和 `chain-state-db-size-mb` 仍应按实际已使用状态逐步扩容。

#### 3.9.4 禁止绕过减半规则

固定四年减半后，`new_ram_per_block` 不再是治理可任意设置的经济参数，而只是可查询的当前时代速率或直接由公式派生：

- `setramrate` 从新链第一版起禁用，调用必须失败；
- `setram` 只允许 RAM 市场第一次初始化到 64 GiB，初始化完成后禁止手工增加；否则仍可通过手工调用绕过当前发行曲线；
- 允许速率按整数字节逐级减半到 `1 byte/slot`，但不允许小数字节；第 44 年边界后严格归零；
- 如未来确需改变发行规则，应作为明确的协议/合约版本升级公开执行，不能借紧急运维 action 静默修改；
- RAM 回收或 `ramburn` 不反向减少 `max_ram_size`，也不改变减半时代；回收额度如何处理应沿用 RAM 市场账本规则。

该规则只约束 RAM 供应，不要求节点预先拥有约 545 GiB 物理内存。但生产者准入和运维监控必须确保实际链状态、数据库上限和硬件容量有安全余量，不能因为市场仍可出售 RAM 就让节点状态数据库触碰 guard 并停机。

### 3.10 取消节点治理，系统升级交给独立账号

#### 3.10.1 已确定的职责边界

本版取消出块节点基于“现任 BP 身份”获得的全部链级治理权限。节点承担出块、区块验证、Savanna 最终确认、同步和本机运维，按固定规则获得奖励。持币者的一票一投继续用于产生候选排名，节点不审批参数、升级或人工调度提案。

系统合约升级由独立管理账号负责。文中用 `sysadmin` 作为角色示例，正式账户名与密钥在部署清单中确定。未来可把该账号明确限定的升级执行权限委托给 AI；此次只修订设计，不实施链上授权。

本版不新增持币者提案治理、BP 理事会或 BP 紧急多签。除系统升级外，旧治理 action 按下表关闭或改为确定性自动逻辑，不因为引入 sysadmin 就默认给它新增随意改参数、挪用资产或人工踢人的快捷入口。独立账号具备升级系统代码的实际能力，因此它是规则更新的信任主体，不能把该架构称为“没有任何管理权”。

#### 3.10.2 当前代码中治理多签从哪里来

治理不是固定的一份提案类型清单。大多数管理 action 只写 `require_auth(eosio)` 或要求目标账户/issuer 授权，是否由节点多签控制取决于部署后的权限树。

本地代码已核对：

- [controller.cpp](PureEOS/libraries/chain/controller.cpp) 的 Genesis 初始化创建 eosio.prods，并由 update_producers_authority() 随当前出块名单更新其权限；
- 当委员会为 21 人时，`eosio.prods@active` 为 15 人门槛，`prod.major` 为 11 人，`prod.minor` 为 8 人；三条都是账户授权能力，和 BLS 最终确认门槛是不同机制；
- [eosio.msig_tests.cpp](reference-contracts/tests/eosio.msig_tests.cpp) 明确演示将 eosio@active 授权给 eosio.prods@active，再通过节点审批升级系统合约；
- [eosio.msig.cpp](reference-contracts/contracts/eosio.msig/src/eosio.msig.cpp) 是通用提案/审批协调器，提案与执行都围绕实际交易权限检查；不经过它、直接收集足够签名也能执行已获授权的交易，因此仅删除该合约不能取消节点治理；
- [eosio.wrap.cpp](reference-contracts/contracts/eosio.wrap/src/eosio.wrap.cpp) 以特权合约转发任意 action，[apply_context.cpp](PureEOS/libraries/chain/apply_context.cpp) 中存在特权合约跳过普通 inline 授权检查的路径。

以上结论是当前工作区源码和测试中的能力清单，不是对某条已运行链的全部提案历史或实际部署权限的查询。上线验收必须导出目标链的完整系统账户权限，复核直接、间接和父级授权路径。

#### 3.10.3 治理和提案入口逐项处理

下列“原授权”指代码所需授权；只有权限树委托给 BP 时，才会成为节点多签审批事项。

| 事项与入口 | 原授权或能力 | 本版处理 |
|---|---|---|
| 系统合约升级：native setcode/setabi | 目标系统账户权限；可经 eosio.prods、msig 或直接签名满足 | 移除全部 BP 授权链，只允许独立升级账号对明确清单中的系统合约发起版本更新 |
| 改管理权限：updateauth/deleteauth/linkauth/unlinkauth | 目标账户及其父级权限 | 系统账户权限与 BP 隔离；升级执行权限不自动取得 owner 或任意权限树编辑权；普通用户管理自己的权限照常保留 |
| eosio.prods 的 active/prod.major/prod.minor | 内核随 active producer 名单生成 | 移除自动授权更新；新链不创建可由 BP 控制的治理权限。若兼容需保留账户名，其权限不可满足且不得被系统账户引用 |
| eosio.wrap::exec | 特权执行任意交易，可形成强制改账户权限/代码等入口 | 生产链不部署、不授予特权，关闭重新启用的旧管理通道 |
| eosio.msig::propose/approve/exec | 通用多方交易授权，不专属于治理 | 不用于节点治理；普通用户自愿的账户多签和应用多签可以保留，但不能获得任何系统管理授权 |
| setparams、wasmcfg | eosio 授权；修改链资源/交易/WASM 限制 | 启动时固定，关闭上线后人工 action；确需更新时由独立账号发布明确版本，涉及原生共识限制时另按公开软件版本规则处理 |
| activate、boot::activate、bios::activate、switchtosvnn | eosio 授权或启动能力 | Savanna 所需能力在 Genesis 固定；删除过渡入口，不保留 BP 任意激活协议能力的路径 |
| producer API schedule_protocol_feature_activations | 节点本地 API 可安排部分无需预激活的特性 | 检查并限制为 Genesis/公开协议版本允许的确定性集合；仅关闭合约 activate 不足以满足本项 |
| setpriv 及任意 set_privileged 入口 | eosio/特权合约能力 | 特权账户及用途固定在部署清单，关闭公开的特权授予/撤销 action，不能重新启用 wrap 或另造超级执行器 |
| setalimits、setacctram、setacctnet、setacctcpu | eosio 授权；人工分配/覆盖账户资源 | 关闭人工覆盖，普通买 RAM、质押、REX 租赁/回收、PowerUp 和系统必要记账按公开规则自动调整 |
| setram、setramrate | eosio 授权；人工修改 RAM 总量/速率 | 仅一次 64 GiB 初始化，之后按减半公式自动执行；手工入口关闭 |
| cfgpowerup、setrexmature、setrex | eosio 授权；资源市场、REX 份额成熟参数和 total_rent 人工覆盖；份额成熟参数不是租单期限 | 首次初始化后关闭人工配置/覆盖，保留两套动态定价、用户自主管理租单、REX 自动续租/到期回收和收益结算，不恢复节点管理权 |
| setinflation、setpayfactor | eosio 授权；发行和奖励参数 | 上线前确定公式，之后自动执行，BP 不决定自己的奖励或发行率 |
| setschedule、delschedule、updtrevision | eosio 授权；预定通胀变化或合约行为版本 | 关闭上线后的人工修改；注意这里 setschedule 是通胀时间表，不是出块名单 |
| execschedule | 按已设通胀时间表自动执行，无需 eosio 审批 | 仅保留执行固定公开发行曲线所需行为，不继承可由 BP 改写的时间表 |
| rmvproducer | eosio 授权；人工取消候选 active 状态 | 删除人工踢人入口，以可验证故障、冷却和固定资格规则处理 |
| voteproducer 对 b1 的特殊授权、unvest | 当前代码允许 eosio 代 b1 投票，或收回其未归属质押并销毁 | 新链不迁移旧 b1 特例，移除硬编码代投和收回资产入口；所有普通投票归余额拥有者授权 |
| bios::setprods、bios::setfinalizer、紧急固定名单 | 任意名单配置能力或旧计划的治理多签 | 只保留 Genesis 初始化和系统合约确定性增长/轮换；不保留 BP 或管理员手工指定替补的普通 action |
| 选举参数、见证者、白名单、暂停与恢复 | 旧计划有“由治理确定/紧急治理”的设计 | 参数在启动版本确定；故障、超时和恢复按链上状态自动进行；不让节点投票批准、拒绝或重抽候选 |
| 核心币 create/issue/issuefixed/setmaxsupply/retire | token 合约或 issuer 授权，若 issuer 被 BP 控制则成为治理能力 | 首次发行和发行上限明确，自动奖励发行走固定公式；关闭 BP 人工增发、抬高供应上限与动用发行账户资金的路径 |
| eosio、saving、bpay、vpay、ram、ramfee、stake、rex、fees、reserv 等系统账户资金 | 各自 owner/active/code 权限，可能经 eosio 间接落到 BP | 逐账户审计，无 BP 可满足的管理权限；只保留已定义的收费、退款、奖励和资源结算路径，未定义储备支出规则不得开放提案提款 |
| init、bios/boot 临时管理能力 | 创世/系统初始化账号 | 一次初始化后失效，不以“初始化”名义保留可复用管理后门 |

此表不要求取消所有带 require_auth(get_self()) 的函数。例如 onblock 和 logsystemfee 是内部处理/记账，不是人工治理；误删会破坏正常出块和收费。候选注册/注销自身、更新自己密钥、用户 voteproducer、资源交易、领奖以及用户自己的权限管理也不是 BP 链级治理，应保留其原有所有者授权约束。

#### 3.10.4 独立账号升级及未来 AI 接管

系统合约升级采用独立账号授权交易，不要求 15 名 BP 对升级内容投赞成票。节点负责验证该交易的权限与协议规则、打包并形成共识，不判断升级政策是否合意。

建议权限结构：

- 独立管理账号 owner 负责账号恢复及升级执行密钥更换，与 BP 账户/producer key/BLS key 分离；
- `sysadmin@upgrade` 是专用执行权限，只连接明确受保护系统合约的升级动作；不复用普通转账权限；
- 受保护目标清单至少覆盖实际部署的 eosio.system、核心 eosio.token，以及承担系统收费/奖励/授权能力的其他合约；不能只保护 eosio 的 WASM；
- 管理账号名称、初始权限、目标清单与初始系统合约版本哈希进入 Genesis 或可验证的启动清单，不能由现任出块者自行解释；
- 每次升级记录目标、旧/新 WASM 与 ABI 哈希、变更说明和必要表迁移；迁移必须有版本检查和一次执行约束；
- AI 可以生成变更、运行测试、形成升级包，并在获得明确委托后用专用权限执行；AI 的自然语言输出本身不能让节点跳过链上验签；
- 未来委托 AI 时采用可撤销的专用密钥/权限，保留独立 owner 更换执行密钥的能力；是否采用人工最终确认或自动执行，在正式委托时确定，不把 AI 自动设为 owner。

系统合约升级可以改变选举、奖励和经济逻辑，因此即使旧的独立管理 action 被关闭，独立升级账号仍有通过版本更新改变规则的实际能力。这个信任边界必须公开，不宣称固定参数永远不可能被升级改变。

用户账户多签照常可用。矿工如果以个人账户持币或使用多签钱包，按普通用户规则处理；禁止的是因获得出块席位而自动得到系统管理票权。

#### 3.10.5 内核、权限与自动化不能留下旁路

- 删除 controller 的 update_producers_authority 调用及其治理权限创建/更新逻辑；增长到 21 或后续换人时不得重建节点管理权；
- 审计系统 owner/active/自定义权限、eosio.code 引用和父级恢复权，不把治理权转移到另一条仍由 BP 满足的间接链；
- setcode/setabi 和权限动作有原生处理器，删除 WASM action 或隐藏 ABI 不等于禁止调用；在 native handler/授权层落实受保护系统目标和独立升级授权；
- 保留普通用户对自己合约的部署升级能力，不能为保护系统合约把全链 setcode/setabi 一并禁用；
- 系统内部确需 privileged/intrinsic 的资源记账、奖励结算和 policy 更新应保留，但只能由已授权版本的确定性逻辑调用；审计任意 action 转发和用户可控参数，防止借系统代码绕过权限；
- 抽签、增长、故障补位和在线证明均不请求 BP 审批。证明可以包含节点签名，但只能证明固定 challenge 或链上事实，不能包含任意授权交易；
- 见证者资格及失效替换规则、随机信标成员规则必须在初始版本中确定，不能留“由当前 BP 多签指定”的控制点；
- 无候选、信标超时或在途冲突时按固定条件等待/恢复，删除旧计划中“节点多签暂停抽签、提交紧急固定 policy、人工豁免失败候选”的方案；
- 未知故障可由独立升级账号在链仍能执行交易时发布修复版本；若链已无块或无最终确认，升级交易也不能凭空生效。管理员没有无共识强行插入区块或改写历史的权限。

这里“节点只做矿工”描述协议中的权限边界。它不消除出块者在交易包含、网络转发和自愿选择软件版本方面的实际影响；更换共识软件仍需网络参与者采用，不能由一次管理签名强迫旧节点接受不兼容规则。

## 4. 推荐的数据结构

由于链从 0 号区块启动，不需要继承旧表结构。投票相关字段应从第一版就使用整数 Token 精度，避免长期依赖 `double`。

### 4.1 投票表设计

- `voters.producers`：只允许保存 0 或 1 个账户名，持有 REX 时必须为 1 个；非空时代表全部有效投票余额的唯一去向，也允许首次买入前记录余额为 0 的候选选择；
- `voters.proxy`：新制度启用后恒为空；
- `voters.last_vote_amount`：使用 `int64_t` 保存该账户上次实际分配的 Token 最小精度单位，不保存 `stake2vote()` 时间权重；
- `producers.vote_amount`：使用 `int64_t` 表示分配给该候选的 Token 最小精度单位；
- `global.total_producer_vote_amount`：使用足够宽的整数累计所有当前已分配投票余额，不是投票账户数。

锁定投票余额变化必须同步改变当前候选票数，但不能继续使用时间放大的 `stake2vote()`。`delegate_bandwidth` 与 REX 的锁定余额都计入 `voting_balance`，租来的 CPU/NET 和 PowerUp 不计入；不得从混有租赁/PowerUp 的 `user_resources` 直接推导拥有者票权。CPU/NET 自有质押与 REX 之间迁移时只按净变化更新，任何 Token 只能有一条计票路径。

如果最大供应量乘以最小精度可能接近 `int64_t` 边界，合约设计阶段必须收紧最大发行量或采用经过 CDT/序列化验证的更宽累计表示；不能先用 `double` 再依赖近似排序。

### 4.2 新增选举状态

以下名称可调整，但必须持久化足够状态，使重启、fork 和 replay 后抽签、补位结果一致。

`electioncfg` singleton：

- schema_version、activation_slot、pool_size=210、committee_size=21、replace_count=1；
- producer_repetitions=12、epoch_slots=252、chain_domain、randomness_mode；
- 心跳有效期、ready 门限、预检和观察时限、冷却期、候选快照及历史保留窗口；
- 参数在启动版本固定；后续必要变化经独立账号发布明确系统版本，不开放 BP 多签配置 action。

`electionstate` singleton：

- mode=growing/rotating，change_kind=grow/rotate/fault；
- last_draw_epoch、单调递增 draw_id、当前阶段 idle/frozen/waiting_entropy/drawing/preflight/ready/proposed/waiting_activation/observing；
- snapshot_slot/hash、candidate_count，以及冻结的 active 集合及 active_hash；
- seed、previous_seed、exit_counter、entry_counter、out_seat/out_producer、in_producer；
- 锁定保留成员集合、目标 proposer/finalizer digest/generation、提案结果和重试次数；
- 两份目标分别激活的状态、完整观察起点及结果；
- rejected_this_draw、故障席位续办标识、exhausted/aborted 及原因，防止换轮时丢失同席补位约束。

`commseats`：

- 21 个稳定逻辑席位；增长期未占用席位为空；
- 分开保存实际 active 成员、待生效目标、最近验收通过成员和 observing 状态；
- proposer policy 生效时更新实际成员映射，以便把窗口故障正确归给新成员；两份 policy 生效后进入观察，通过后再更新 confirmed 状态；
- 不把“等待验收”的旧成员错误当成还在实际出块，也不在提交提案时提前改变 actual active；
- 提交 proposer 名单前按账户名排序，正常退出随机数映射则按固定逻辑席位排序。

`bphealth` 与第 3.5 节健康表：

- 签名心跳、ready 证明摘要、独立见证计数、expires_at；
- standby/preflight/ready/active/observing/quarantined 状态；
- 冷却期、失败原因、故障队列去重字段；历史保留有上限。

`drawcands`：

- 每个 draw_id 最多 210 个候选的账户、整数票数、排名，以及判定抽签资格所需的冻结信息；
- 保存快照记录及哈希，不能只存哈希后依赖已变化的当前票数重建；
- 同轮保留已拒绝集合，以便确定性重抽。

`drawhistory`：

- 变更类型、冻结候选和现任摘要、随机证明、退出抽签结果、每次补入尝试、预检、提交、生效和完整观察结果；
- 故障补位注明退出者来自故障证据；正常轮换注明 EXIT 随机流；
- 有固定链上保留期，完整历史通过 state-history 等服务归档，不让系统账户 RAM 无限增长。

## 5. 一票一投的具体改法

### 5.1 投票 action 接口

可以沿用简洁的现有 ABI 形状：

```text
voteproducer(voter, proxy, producers)
```

但新链从第一版就只接受空列表或一个生产者：持有 REX 时禁止空列表，没有 REX 时可以撤票。允许本人授权创建零余额 voter 记录并选择一个有效候选，供首次买入前使用。让 `cleos`、钱包和 Ricardian 条款同步展示这些约束；无需保留多选、代理或旧客户端行为，也无需给 `buyrex` 新增候选参数。

### 5.2 重写 `update_votes()` 的票数路径

逻辑应等价于：

```text
验证proxy 为空、producers 数量为 0 或 1
结清该账户已成交卖单的待处理票权差额，避免后续重复扣减
如果本次 producers 为空且账户仍有 REX 份额：拒绝撤票
如果是本人主动选择新候选：验证候选有效；内部退出结算不得套用该有效性门槛
old = voter.producers 中原来的唯一候选（如有）
new = 本次 producers 中的唯一候选（如有）
old_weight = voter.last_vote_weight
weight = voter当前有效的锁定Token余额（使用asset.amount整数精度）

if old == new:
    if new 存在: new.total_votes += weight - old_weight
else:
    if old 存在: old.total_votes -= old_weight
    if new 存在: new.total_votes += weight

保存新的 producers；清空 proxy/is_proxy/proxied_vote_weight
保存last_vote_weight = new存在 ? weight : 0
维护全局已分配Token票数和投票奖励累计值
```

需要特别处理以下关联代码：

- `stake2vote()` 的时间放大结果不再用于生产者选举；新路径直接使用锁定 Token 的整数数量；
- `propagate_weight_change()` 应改为传播 `new_voting_balance - last_vote_weight`，而不是传播时间权重；
- `delegate_bandwidth.cpp`、解锁和 REX 份额/收益结算路径会改变有效投票余额，必须在同一状态转换中对唯一候选应用相同的原始 Token 票数差额；REX 新租、续租、回收以及 PowerUp 的资源权重变化不得给付款人或接收者生票。REX 租金进入池后产生的真实收益仍按持有人份额和 vote_stake 结算；
- `voteupdate()` 只能按 Token 余额的真实变化校准票数，不能因时间经过自动增长；
- `regproxy(proxy, true)` 在新制度启用后拒绝；可保留 `false` 用于清理历史代理状态；
- `last_vote_weight` 保存已分配的 Token 数量；链激活统计如仍有独立语义，应增加专门字段，不能与当前候选票权混用。

### 5.3 奖励统计要同步

当前 votepay 依赖 `total_votes`、`total_producer_vote_weight` 和随时间累计的 votepay share。改造后 `total_votes` 表示 Token 票数，per-vote 奖励可继续按 Token 票数比例分配，per-block 奖励仍按实际出块数分配；也可以取消 per-vote 奖励，只保留按实际出块支付。

无论选择哪种奖励方案，账户拆分前后的总奖励权重必须相同，所有 votepay 变化只能由 Token 锁定量变化或改投驱动，不能由账户数或持币时间放大驱动。奖励口径必须在上线前确定，否则排名正确但经济账会持续漂移。

### 5.4 REX 强制投票的具体改法

保持 `buyrex`、`unstaketorex`、`voteproducer` 现有参数形状，使用共同的私有检查函数，避免只改钱包或一个入口：

- 在 `eosio.system.hpp` 声明 `require_rex_vote(owner)` 和 `has_rex_shares(owner)`（拟新增私有 helper，不是管理 action）。前者读取 voter 和唯一候选，按第 3.7.1 节校验买入资格；后者只判断完整 REX 份额，不把预存资金、收益汇入或 `vote_stake` 舍入值当成份额。
- 在 `rex.cpp::buyrex()`、`unstaketorex()` 的拥有者授权和基础参数检查之后、任何资产变更之前调用买入检查。`add_to_rex_balance()` 作为共享的增加用户份额入口也防御性检查；以后新增购入或转入入口必须复用，不得绕过。
- 在 `voting.cpp::voteproducer()` 中，在余额刷新和 `update_votes()` 前按需创建 voter 行，由已授权的本人承担 RAM；初始余额和票数为 0，再从真实 CPU/NET/REX 记录同步。没有资产时只能保存候选选择，不计入候选正票数或链激活余额，不能要求用户先买入 REX 才允许投票。
- 在 `update_votes()` 的公共校验路径拒绝 `has_rex_shares(voter) && producers.empty()`，防止直接 action、CLI 或其他调用路径撤票；`voteupdate()` 只刷新已有选择，不得清空、代选或恢复代理。改投 B 时检查 B 有效，但不能把同样的有效性检查无差别放进退出结算。
- 在 `sell_rex()`、`fill_rex_order()`、`runrex()`、`update_rex_account()` 和 `update_rex_stake()` 核对份额、`vote_stake`、voter 余额和候选票数的更新时点。当前排队卖单成交后还会记录 `stake_change` 待拥有者后续结算；改造后须保证成交票权及时更新、待结算差额只消费一次，不能在允许撤票后再次扣同一笔票数。先完成 REX 结算再改票，相关 helper 不得递归触发 `update_votes()`。
- `mvtosavings/mvfrsavings/consolidate` 只改变成熟或储蓄状态，不解除持仓投票约束；`process_buy_rex_to_savings()` 和 `process_sell_matured_rex()` 的自动路径同样保持份额、票权一致。不得在通用 `runrex()` 中要求每个订单拥有者的候选当前有效，否则一个失效候选可能卡住其他用户买卖。
- `sellrex/withdraw`、已成交卖单结算及自动成熟退出不新增“候选必须仍 active”的条件；仍保留原有授权、成熟期、余额和流动性校验。候选注销只改变其选举资格，不清空持有人选择，也不销毁资产记录。
- ABI 的 action 参数无需新增；待实施时同步 header 注释、Ricardian action 条款和生产生成的 WASM/ABI，再更新测试夹具，禁止只修改测试副本或手工修改二进制。

建议稳定错误语义：未满足买入条件提示“买入 REX 前必须直接投票给一个有效候选”；试图撤票提示“仍持有 REX，只能改投；全部卖出并结清后才能撤票”。错误必须由合约校验产生，客户端提示不能代替链上规则。

## 6. 候选快照、随机退出与随机补位

### 6.1 “每轮”的定义

稳定阶段的选举机会：

```text
epoch_slots = committee_size × producer_repetitions = 21 × 12 = 252
epoch_id    = floor(block_timestamp.slot / 252)
```

当 `epoch_id > last_draw_epoch` 且上一流程完全结束时，开始新的正常轮换；必须使用“大于”以处理边界漏块。若上一轮仍在预检、激活或观察，本轮只推进当前状态，不另开抽签，也不在恢复后补做积压轮次。故障队列优先。

Savanna 自身的安全晋升边界和上述业务 epoch 不同；新名单要等待合法共识条件，不能要求在每个第 252 个 slot 精确切换。252 个 slot 是 126 秒，不是交易确认必须等待的时间。

### 6.2 冻结候选及现任快照

本轮先冻结候选 C、实际 active 委员会 A 和逻辑席位映射，再生成随机种子。冻结后改票只影响下一轮，不能改变本轮排名或退出随机样本空间；候选密钥失效、冷却、证明过期等变化可使其被拒绝，但不能临时补进快照之外的候选。

使用确定性索引读取前 210，并保存固定格式记录和哈希。快照分片跨区块推进时必须使用版本化数据或明确的一致冻结机制，不能把不同时点的排名拼成一份声称属于同一时点的快照。

示例序列化：

```text
domain_tag || draw_id_be64 || snapshot_slot_be32 || candidate_count_be32 ||
(rank_be32 || account_value_be64 || votes_be64) * candidate_count
```

对当前委员会的账户、逻辑席位、policy 标识另算 active_hash，一并绑定随机请求。不使用 JSON、std::hash 或未固定布局的结构体内存作为哈希输入。

### 6.3 随机种子

生产环境应在快照冻结后获得可链上验证的随机信标输出，绑定：

```text
chain_domain || change_kind || draw_id || snapshot_hash || active_hash
```

信标可采用门限 VRF/BLS 或其他具备链上证明的方案，具体方案上线前确定。参与者不能任意换种子或选择性重发请求；节点管理者也不能用多签选择随机结果。随机服务签署固定消息的职责，不产生治理权限。

信标超时保持当前 policy 并告警，不静默退回较弱随机源。只有协议预先定义的超时/耗尽条件才能结束一次尝试，新尝试应有链上记录，故障补位继续锁定同一席位。

开发和压力测试可使用：

```text
seed = SHA256(
  "PUREEOS_BP_DRAW_V2" || chain_domain || change_kind ||
  draw_id_be64 || snapshot_hash || active_hash ||
  previous_seed || previous_block_id
)
```

区块 ID 派生种子可重放，但可能被生产者通过区块内容或漏块尝试影响，不能视为生产安全随机源。禁止使用本地墙钟、rand()、mt19937 或节点私有配置。

### 6.4 两个独立随机流

正常轮换既随机退出，也随机补入。使用同一轮可验证种子，按 EXIT/ENTRY 域隔离，并为两者保存独立 counter：

```text
draw_uniform(domain, n, counter):
    assert n > 0
    repeat:
        digest = SHA256(domain || seed || draw_id_be64 || counter_be64)
        counter += 1
        u = digest前8字节按大端解释为uint64
        limit = floor(2^64 / n) * n  // 使用uint128表达
    until u < limit
    return u mod n
```

正常退出：将当前占用的 21 个逻辑席位按席位号排序，用 EXIT 抽一个，并持久化结果。增长期没有退出抽签；故障或明确失效成员的优先替换由证据/固定规则决定退出席位。

补入：将冻结候选中排除本轮现任、冷却者及本轮已拒绝者后的列表按固定顺序排列，用 ENTRY 抽一个。每次失败将候选加入 rejected_this_draw，继续 entry_counter；不重抽退出者，不换种子，不重做快照。列表为空则进入有记录的等待或耗尽状态，禁止对零长度取模。

正式加入后的同席补抽，还要排除当前实际 active 成员和本次尝试已失败成员；固定其余 20 人。即使按协议超时开启下一次尝试，也不能把这个故障流程变成重新抽取整组名单。

拒绝采样和证明处理也应设置单次执行预算；未完成则持久化 counter 在后续步骤继续，不让理论上的重复循环无界占用 onblock。

相同快照、现任、种子、域标签、排除记录和 counter，在不同 CPU/编译优化下必须得到相同 golden vector。随机退出允许某个成员连续多轮未被抽中，测试不能断言 21 轮内每个人必退出一次。

## 7. 调度提交状态机

1. idle：读取真实 active proposer 与 finalizer，确认上一变更已完成完整观察；故障优先。
2. frozen：保存前 210 候选、现任和逻辑席位映射，请求随机信标。
3. drawing：正常轮换用 EXIT 决定唯一退出者；增长不退出；故障固定问题席位。再用 ENTRY 抽取补位者。
4. preflight：旧名单继续服务；收集并校验连接、同步、producer/BLS 和门限 ready 证明。
5. 失败：隔离候选，同一轮继续抽下一人；无候选按协议等待或记录耗尽。
6. ready：再次校验资格和证明时效，构造并检查目标规模、唯一成员变化和两份目标账户集合一致。
7. proposed：按账户名排序提交 proposer，并提出对应等权 finalizer policy。必须处理返回失败、在途冲突及潜在的部分接受，保存实际提交状态，禁止盲目当作两份均成功或开始新抽签。
8. waiting_activation：分别确认两份目标均 active；期间保持旧、新所需连接，不叠加下一变更。
9. observing：从两份目标均 active 后观察至少目标人数×12 个 slot，覆盖补位者完整窗口并检查出块、BLS、P2P、QC/LIB。
10. 通过：更新 confirmed 状态和历史，才允许下一次扩容/轮换。
11. 失败：固定其他成员，锁定同一席位继续预检和补抽，不重新生成整组 21 人。

growing 时规模只能增加 1；达到目标后正常与故障替换都保持 21；增长中新增成员失败时按当时实际规模原席补位。状态推进不能依赖 BP 审批或人工决定下一候选。

## 8. Savanna finalizer 的处理

本版明确采用“当前委员会同时出块及最终确认”。稳定期 21 个 proposer 对应同一批 21 个等权 finalizer；210 个候选的其余节点即使已注册 BLS key，也不因此获得当前投票权。

- Genesis 为同一批 k 人，quorum(k)=floor(2k/3)+1；
- 增长时同一个新账户加入两份 policy；稳定期同一个退出/补入账户更新两份 policy；
- 21 人的 Strong QC 门槛为 15，最多 6 人缺票仍可达到门槛，7 人缺票时不能形成 Strong QC；
- 15 是共识签名门槛，不赋予这 15 个节点升级系统合约、修改参数、动用系统资产等治理权；
- proposer 与 finalizer policy 分别晋升，提交同一目标不等于同时生效；必须等待两份均 active；
- 轮换期间可能同时验证 active/pending policy，旧、新成员并集通常为 22 人，网络和投票处理需覆盖该过渡集合；
- 完整观察必须包括新成员有效 BLS 投票及持续最终确认，不能只看出块；
- 候选池的 210 人提升长期参与范围，不代表单个时刻拥有 210 人委员会的确认门槛；
- 本版不采用“21 人出块、210 人投票确认”，也不把 finalizer 解耦作为待选择的默认分支；
- regfinkey/actfinkey/delfinkey 只允许节点维护自己的密钥；若现任 actfinkey 需要更改 finalizer policy，必须纳入同一个串行更新约束，不绕过在途选举去覆盖目标名单，也不改变其他成员资格。

## 9. P2P 与节点运维必须同步改造

候选池最多 210 人，当前出块/确认委员会最多 21 人。其余候选持续同步待命，但无需加入当前 BLS 共识投票网络。自动 BP peering 必须面向真实现任及待加入成员，不能继续依赖票数 top-50；排名第 200 的候选也可能抽中。

- getpeerkeys 返回当前 active、仍需通信的 pending/退出成员、唯一 preflight/next candidate 的并集；
- 稳定且无过渡时为 21 人，正常单席变更的通信身份并集通常最多 22 人；增长期使用实际 k，不硬编码；
- 在本次两份 policy 激活并观察完成前不预检下一席，避免不断增加在途连接身份；
- 抽签后立即刷新 peer key，或用明确的短轮询周期，避免沿用 120 块刷新延迟拖慢准备；
- relay/sentry 需要正确转发 BLS 投票，候选通过入站、出站和持续同步检查后才进入 ready；
- 采用分层且冗余的网络，控制中继单点和带宽瓶颈；210 个候选不要求全互连；
- 退出成员在旧 policy 不再需要它之前继续服务，不能在“被抽中退出”时马上停机；
- 节点运维程序只响应已确定的候选状态、配置本机密钥与连接、提交事实证明，不批准系统升级或手工选人；
- 参数、见证者资格和冷却规则不能由 BP 多签随意改变；由启动配置及本版权限边界决定。

reference-contracts 当前目录没有与 PureEOS 测试夹具完全同源的 peer_keys.cpp，实施时需确认生产 getpeerkeys 的来源，把所需逻辑正式纳入生产合约，不能只修改测试副本。

节点通过预检后仍可能故障。网络方案必须支持同席重抽，同时记录失联时长、漏块、最终确认延迟和恢复时间，不能把保持 21 人的 policy 误报为有 21 人实际在线。

## 10. 代码改造难度、风险与性能影响

### 10.1 PureEOS 内核：21 数量低难度，原生 Savanna Genesis 中高难度

当前 `config::max_proposers = 64 × 1024`、`config::max_finalizers = 64 × 1024` 已远高于 21。因此“Savanna policy 含 21 人”本身不要求提高任何数量上限，`config::max_producers = 125` 在目标链中没有作用。

真正的内核工作是把当前 `initialize_blockchain_state()` 的 legacy Genesis 改成原生 Savanna Genesis：

1. 扩展或替换 `genesis_state`，加入 `initial_proposer_policy`、`initial_finalizer_policy` 和 Savanna Genesis 版本/域标识；
2. initial proposer policy 包含 `k` 个 producer authority；initial finalizer policy 包含相同 `k` 个账户的唯一 BLS 公钥、每人权重 1、threshold `floor(2k/3)+1`、generation 1；另声明目标 21、候选上限 210 和独立管理员/系统部署清单的承诺字段；
3. `compute_chain_id()` 必须把上述字段纳入 Genesis 序列化哈希，任一 proposer、BLS key、权重或阈值变化都产生不同 chain ID；
4. `initialize_blockchain_state()` 直接构造 `block_header_state`/`block_state`，不再先构造 `block_header_state_legacy`；
5. 使用 `finality_core::create_core_for_genesis_block()` 初始化 finality core，并初始化 active finalizer policy、active proposer policy、finality digest、validation tree、聚合 QC 状态和 finalizer safety 基准；
6. fork database 从启动起使用 Savanna `fork_database_if_t`，不进入 legacy/both 状态，也不等待 critical block；
7. Genesis 起视为已具备 Savanna 所需的协议能力，不能再依赖后续 `set_finalizers` 触发 transition；`switchtosvnn()` 在目标系统合约 ABI 中删除或永久拒绝；
8. 更新 block log、snapshot、state-history、replay、灾难恢复和 Genesis JSON 生成/校验工具，使它们都能从原生 Savanna root 恢复。

建议的 Genesis 逻辑结构如下；字段名可按 C++ 类型调整，但语义必须固定：

```text
genesis_state {
  initial_timestamp
  initial_configuration
  consensus = "savanna_genesis_v1"
  target_committee_size = 21
  candidate_pool_limit = 210
  initial_admin_authority { account_name, owner_authority, upgrade_authority }
  system_bootstrap_manifest_hash  // 系统合约、初始参数、权限和一次初始化清单

  initial_proposer_policy {
    version = 0
    producers[k] {
      producer_name
      block_signing_authority
    }
  }

  initial_finalizer_policy {
    generation = 1
    threshold = floor(2k/3) + 1
    finalizers[k] {
      account_name
      description
      weight = 1
      bls_public_key
    }
  }

  genesis_protocol_features {
    Savanna 与 BLS 所需的固定 prerequisite 集合
  }
}
```

Genesis 只包含公钥和 policy，不包含 producer/BLS 私钥。`1 <= k <= 21`，两份 policy 的账户集合必须完全相同；target size、候选上限、producer authority、BLS 公钥、权重、阈值、generation、协议能力集合、独立管理员初始权限、启动清单摘要和序列化顺序都必须进入 chain ID。

现有 `block_state::create_if_genesis_block()` 已包含 finality core、active policy、validation tree 和 aggregating QC 的大部分初始化逻辑，可以把这些通用部分抽成 `create_savanna_genesis_block()`，让新的直接 Genesis 路径调用；原 transition call site 在验证新路径后删除。不要为了省事伪造一段 legacy transition，因为那会把已经决定删除的双状态复杂度重新带回目标链。

这属于共识初始化改造，难度为中高、测试风险为高，但只影响启动/恢复路径；进入正常 Savanna 稳态后，不会持续消耗额外性能。

只有额外要求“必须在选举 epoch 的精确 slot 立即切换”，而不是等现有 Savanna 安全轮次晋升，才需要继续修改 `block_header_state.cpp` 的 policy 晋升规则。本文不采用该要求。

#### 10.1.1 新链可以抛弃的旧代码边界

目标链不读取旧 block log/snapshot，也不与旧节点组网，因此可以删除或编译期彻底禁用：

- `block_header_state_legacy`、`block_state_legacy` 的生产启动和区块验证路径；
- legacy DPoS 的 `confirm_count`、implied-LIB、`maximum_tracked_dpos_confirmations` 和 `max_producers = 125` 约束；
- `set_proposed_producers_legacy()` 及 controller 中 legacy/Savanna 二选一分派；
- `transition_to_savanna*()`、critical block、legacy/both 双 fork-db 状态和过渡期 snapshot 恢复；
- 依赖 `set_finalizers` 首次调用触发 transition 的协议特性逻辑；
- 系统合约 `switchtosvnn()` action 及相关过渡测试；
- 旧链 block log、旧 snapshot、旧 Genesis JSON 和旧投票/REX 状态的兼容读取器。

以下代码不能因为名称或历史来源而直接删除，需要保留或重构后复用：

- `producer_authority_schedule`、block signing authority 和 `set_proposed_producers()`；Savanna proposer policy 仍使用它们；
- `block_header_state`、`block_state`、`finality_core`、`finalizer_policy`、QC/BLS 和 Savanna fork database；
- `block_state::create_if_genesis_block()` 中初始化 finality core、validation tree、active policy 和 aggregating QC 的逻辑；应抽成原生 Savanna Genesis helper；
- 通用 block log、snapshot、replay 和 state-history 框架；删除的只是 legacy 编码分支，不是这些能力本身；
- 系统合约的投票、资源、奖励和后续 policy 更新功能。

建议实施顺序是先让原生 Savanna Genesis 路径通过创世、出块、QC、replay 和 snapshot 测试，再删除已经不可达的 legacy/transition 分支，最后运行全量编译和静态引用检查。这样最终产物没有旧兼容代码，同时能把“建立新路径”和“清理死代码”两个故障面分开验证。

难度要分两档看：只增加原生 Savanna Genesis、让 legacy 路径永远不可达，属于中高难度；进一步把 controller、fork-db variant、snapshot/block-log 中的全部 legacy 分支物理删除并收敛类型，属于高难度的机械重构。后者不会提高 TPS，只会减少代码面、状态分支和未来维护成本，适合在原生 Genesis 已稳定后完成。

### 10.2 系统合约与权限：中高难度

- 一票一投使用整数余额，CPU/NET 自有质押和 REX 份额原子更新，租用的 REX 资源及 PowerUp 不给租用者计票；
- 从 top-21 直接选举改为前 210 快照、随机 EXIT、随机 ENTRY、单席持续状态机；
- proposer/finalizer 为同一目标集合，串行处理预检、提交、生效和完整观察；
- 移除节点对系统合约升级、关键参数、系统资金和人工名单变更的管理权限；
- 系统合约升级交由独立账号，未来 AI 使用该账号明确委托的升级权限；其他管理入口按第 3.10 节逐项处理。

难点是跨区块、跨 action、fork/replay 后仍保持一致，且节点身份变化不能重新产生治理权限。此项必须同时检查系统合约与内核授权代码。

### 10.3 P2P 与运维：中高难度

正常共识只需当前 21 人，但 210 个候选仍有同步、心跳和预检成本。主要风险是候选准备不足、中继拥堵、局部网络误判和 policy 过渡丢票。

一个 active proposer 离线可能损失自己的 12 个时间槽，即约 6 秒；在其他出块节点及至少 15 名有效 finalizer 正常时，链可继续推进。恢复不应依赖节点治理投票，而应自动处理同一席位；完全失去出块或最终确认时，普通链上补位无法继续。

### 10.4 对出块性能的影响

- 500ms/slot、每人连续 12 slot 均保持；21 人完整轮次为 252 slot，即 126 秒。
- 210 个候选不把交易分成 210 份并行处理，也不要求等待候选池全部节点逐个出块。
- 每次只改变 1/21 的正式席位，可以限制单次网络扰动，但不是零开销。
- 正常预检失败不提交名单，因此不会为已知不合格候选预留正式时间槽；正式加入后的故障仍可能造成漏块。
- 随机退出没有确定的全员轮换周期；不以“21 次轮换恰好更换每个人”作为验收标准。
- 名义 500ms 间隔不代表实际 TPS 或最终确认速度保持不变；排序、快照、随机证明、P2P 和名单过渡都需测量。

### 10.5 对 Savanna/BLS finality 的影响

本版常态由 21 个 finalizer 持续投票，门槛为 15。候选池中的其余节点不参与当前 policy 的计票。名单切换期间要处理旧、新 policy 的投票及阈值，不能因为只有一席变化就跳过 finality 测试。

| 当前规模 k | Strong QC 门槛 | 最多缺票仍可达到门槛 |
|---:|---:|---:|
| 1 | 1 | 0 |
| 2 | 2 | 0 |
| 3 | 3 | 0 |
| 4 | 3 | 1 |
| 21 | 15 | 6 |

任意时刻的确认门槛取决于 active/pending policy，而不是候选总数。15 份共识签名只确认符合规则的区块，不是 15 名节点对治理提案的批准，也不是完整的攻击安全性结论。节点控制比例、网络分区和同一主体多身份仍需单独分析。

### 10.6 系统合约与状态性能

排名索引读取最多 210 个候选，快照分片保持同一时点语义，历史保留有上限。退出和补入使用两个有域隔离的随机流，重试保存 counter。每区块健康检测 O(1)，跨空窗时最多处理 21 个唯一席位，选举大步骤不得堵塞 onblock。

性能验收至少对比：

1. 固定 21 人委员会；
2. 固定同一批 21 人、同时维持 210 候选同步和心跳；
3. 210 候选、21 人委员会、持续随机单席轮换；
4. 在第 3 项中注入预检失败、正式加入后故障、慢节点和网络分区。

四组使用同样硬件、地域拓扑、区块/交易参数和负载，分别记录可持续 TPS、区块传播、漏块率、交易包含延迟、最终确认延迟 p50/p95/p99、CPU/内存/带宽及替补耗时。这样区分候选维护、正常轮换和故障处理开销；具体可接受阈值上线前确定，不虚构“完全无影响”的结论。

### 10.7 主要风险及控制

| 风险 | 控制要求 |
|---|---|
| 出块者影响随机退出/补入 | 快照后可验证信标，EXIT/ENTRY 域隔离，固定结果与重试记录 |
| 抽中未准备好的候选 | 强制预检、ready 时效检查、current/next 预连接 |
| 正式加入后故障 | 完整观察、同席补抽，承认可能漏块和等待 |
| 多份名单变更叠加 | 一份在途状态机，两份 policy 生效并观察通过后才推进 |
| 最终确认不足 | 测试 15 人门槛及缺 6/7 票边界，禁止承诺失活后链上自救 |
| 观察者误判或审查 | 多地域事实证明、固定验证规则、有限冷却；不授予任意选人或踢人权 |
| 节点重新获得治理权 | 移除 eosio.prods 自动授权和间接权限链，测试任意数量 BP 签名都不能治理 |
| 独立升级账号或未来 AI 出错 | 最小升级权限、目标清单、版本哈希和审计、独立 owner 恢复，不把节点纳入审批链 |
| 资源和历史状态膨胀 | 有界索引/快照、保留窗口、对比压测 |

## 11. 文件级改造清单

| 位置 | 改造内容 |
|---|---|
| PureEOS/libraries/chain/include/eosio/chain/genesis_state.hpp、genesis_state.cpp | 同一批 1..21 人 initial policy、动态阈值、目标 21、候选上限 210、协议域；增加独立管理员及系统启动清单的确定性承诺 |
| PureEOS/libraries/chain/controller.cpp | 原生 Savanna 初始化、fork DB、finality；移除 eosio.prods 的创建/自动治理权限更新及调用 |
| PureEOS/libraries/chain/block_state.cpp/.hpp、finality_core.cpp/.hpp | 抽出 Savanna Genesis helper，初始化验证树、active policy、QC 和 safety 基准 |
| PureEOS/libraries/chain/include/eosio/chain/fork_database.hpp | 移除不可达 legacy/both 运行状态，保留 Savanna fork 处理 |
| PureEOS/libraries/chain/protocol_feature_manager.cpp、producer_plugin、producer_api_plugin | Genesis 能力集合和协议特性校验；处理本地激活 API，不能留下 BP 任意更改协议的旁路 |
| PureEOS/libraries/chain/eosio_contract.cpp、authorization_manager.cpp、apply_context.cpp | 原生 setcode/setabi/权限处理器与特权 inline 路径的系统目标保护；独立升级授权、普通用户权限、自动系统调用分别验证 |
| PureEOS/libraries/chain/webassembly/privileged.cpp、对应 intrinsic 注册及 CDT 声明 | 只读 finalizer/最终确认/QC 指标接口；特权写接口最小使用范围，数据由确定的链上状态派生 |
| PureEOS snapshot、block_log、state-history | 原生 Savanna root 与独立权限初始化的 replay/恢复；不读旧链格式 |
| reference-contracts/contracts/eosio.system/include/eosio.system/eosio.system.hpp | 整数投票表、21 席位、前 210 快照、独立随机 counter、健康/故障和历史状态 |
| reference-contracts/contracts/eosio.system/src/voting.cpp | 单候选整数计票、首次买入前零票选择、持有 REX 禁止撤票、随机退出/补入、同席补抽、目标 policy 构造 |
| reference-contracts/contracts/eosio.system/src/producer_pay.cpp | 有界 onblock 检测、自动选举推进、实际出块奖励与固定发行公式；去除旧激活早退对必要检测的影响 |
| reference-contracts/contracts/eosio.system/src/delegate_bandwidth.cpp、rex.cpp | CPU/NET/REX 票权净变化；REX 买入/转入强制投票；保留资源租赁、续租、到期/退出结算并修正租金付款账户；上线后人工 REX 配置仍关闭 |
| reference-contracts/contracts/eosio.system/src/powerup.cpp | 直接质押＋REX 出租合计 10% 上界初始化、动态定价及代付；cfgpowerup 初始化后关闭 |
| reference-contracts/contracts/eosio.system/src/eosio.system.cpp | RAM 四年减半；系统管理 action 按第 3.10 节关闭/初始化限定；保留自动资源和收益处理 |
| reference-contracts/contracts/eosio.system/src/finalizer_key.cpp | 21 人同席 finalizer 更新、BLS 自身密钥维护与选举更新串行；去除 switchtosvnn |
| reference-contracts/contracts/eosio.bios、eosio.boot、eosio.wrap | 启动专用能力不能部署回生产治理路径；wrap 不部署且不授特权 |
| reference-contracts/contracts/eosio.token | 核心币供应上限和固定奖励发行权限；setmaxsupply 等不能由 BP 调用，普通代币所有者能力按明确范围保留 |
| reference-contracts/contracts/eosio.bpay、eosio.fees 及系统账户部署清单 | 检查资金权限和自动分配；当前 bpay 的按票数 top-N 分配不等于实际随机委员会，若部署须同步改为明确奖励口径 |
| reference-contracts/contracts/eosio.msig 与 PureEOS/programs/cleos/main.cpp | 移除节点治理依赖及说明，普通用户多签保留；更新单候选投票、独立账号升级帮助 |
| 生产 getpeerkeys 实现、peer_keys_db.hpp、net_plugin 与 P2P 测试 | current/pending/next 身份，通常最多 22 人的过渡连接、候选同步、门限预检 |
| reference-contracts/tests/eosio.system_tests.cpp、eosio.finalizer_key_tests.cpp、eosio.system_tester.hpp | 投票、210 候选、21 委员会、EXIT/ENTRY、观察、故障和经济规则测试 |
| reference-contracts/tests/eosio.msig_tests.cpp、eosio.wrap_tests.cpp 及内核授权测试 | 旧 BP 治理交易拒绝、独立账号升级成功、用户多签保留、wrap 不可用、间接权限旁路拒绝 |
| PureEOS/unittests/contracts/eosio.system/eosio.system.wasm/.abi 与 tester | 同步生产构建产物；不得只改测试副本 |
| PureEOS proposer/finalizer schedule 测试、nodeos_voting_test.py | k=1..21 增长、单席随机轮换、两份名单生效、恢复、网络与性能验收 |

### 11.1 实施约定

本节列出以后实施时需要修改的具体文件、关键函数和修改方法。目前只修改本计划；下文的“新增”“删除”“修改”都是待实施事项，不表示已经改动任何源码、构建文件、密钥或链上账号。

现有文件路径已按工作区核对；标为“拟新增”的文件尚不存在，不在本次创建。先完成 P0 共识/授权边界，再实现 P1 选举和经济逻辑，最后联调网络、测试与部署。不能对整个仓库执行 210→21 的文本替换：210 是候选上限，21 是委员会规模，15 是最终确认门槛。

### 11.2 P0：原生启动与取消节点治理

| 具体文件 | 关键位置 | 如何修改 |
|---|---|---|
| `PureEOS/libraries/chain/include/eosio/chain/genesis_state.hpp` | genesis_state、operator==、FC_REFLECT | 增加 k 人两份 policy、target=21、candidate_limit=210、独立管理员初始 authority、系统启动清单摘要；同步比较与序列化字段，避免 JSON 有字段但 chain ID 未覆盖 |
| `PureEOS/libraries/chain/genesis_state.cpp` | 构造与 compute_chain_id | 校验 1≤k≤21、账户集合一致、等权/BLS/阈值；按固定 schema 哈希，拒绝缺失管理员/启动清单的旧 Genesis |
| `PureEOS/libraries/chain/controller.cpp` | initialize_blockchain_state、update_producers_authority 及调用点 | 从 Genesis 直接创建 Savanna 状态；初始化独立管理权限；删除对 eosio.prods 三档治理权限的创建和随出块名单自动更新，不删除共识 proposer/finalizer 更新 |
| `PureEOS/libraries/chain/include/eosio/chain/config.hpp` | producers_account_name、prod.major/prod.minor 相关常量 | 取消作为治理授权入口的生产依赖；必要兼容名仅作为不可授权保留名。500ms、12-slot 与 Savanna 容量上限不因本版规模而改动 |
| `PureEOS/libraries/chain/block_state.cpp`、`PureEOS/libraries/chain/include/eosio/chain/block_state.hpp` | create_if_genesis_block 的通用初始化 | 抽取不依赖 legacy transition 的 Genesis helper；由 controller 直接调用；初始化 QC、验证树和 finality 状态 |
| `PureEOS/libraries/chain/finality_core.cpp`、`PureEOS/libraries/chain/include/eosio/chain/finality_core.hpp` | Genesis finality core | 支持原生 root，保证首块和后继 QC 引用正确；不把普通轮换改成共识重启 |
| `PureEOS/libraries/chain/include/eosio/chain/fork_database.hpp` | fork DB 状态类型与启动 | 收敛到 Savanna，验证新 root 后清理 legacy/both 分派及恢复分支 |
| `PureEOS/libraries/chain/eosio_contract.cpp` | apply_eosio_setcode/setabi/updateauth/deleteauth/linkauth/unlinkauth | 对受保护系统目标执行独立升级/权限管理约束；保留普通用户部署和权限操作。原生 handler 在 WASM 之外运行，不能只靠删除 ABI 禁用系统升级路径 |
| `PureEOS/libraries/chain/authorization_manager.cpp`、`PureEOS/libraries/chain/include/eosio/chain/authorization_manager.hpp` | 递归 authority、父级权限和 action 链接校验 | 复用真实权限验证区分独立 upgrade 与 owner；拒绝系统账户经 eosio.prods/BP 身份间接授权；不把 15 份 BLS vote 当成交易审批 |
| `PureEOS/libraries/chain/apply_context.cpp` | execute_inline 及 privileged 跳过授权分支 | 受保护系统升级不能仅凭 inline action 声明管理员名字而通过；验证实际升级授权来源。保留经过审核的自动资源/奖励调用和用户 msig 所需行为 |
| `PureEOS/libraries/chain/protocol_feature_manager.cpp`、`PureEOS/libraries/chain/protocol_feature_activation.cpp` | 协议特性校验/激活 | Genesis 启用固定 Savanna/BLS 能力；对后续能力按公开协议版本规则校验，不允许 BP 临时批准未知特性 |
| `PureEOS/plugins/producer_plugin/producer_plugin.cpp`、`PureEOS/plugins/producer_api_plugin/producer_api_plugin.cpp` | schedule_protocol_feature_activations | 限制本地 API 可安排的能力集合，不能绕过合约 activate 的禁用；暂停本机/调连接等运维功能保留 |
| `PureEOS/libraries/chain/snapshot.cpp`、`PureEOS/libraries/chain/include/eosio/chain/snapshot.hpp`、`PureEOS/libraries/chain/block_log.cpp` | snapshot/block log 版本与 root 读写 | 保留新 Genesis、管理员权限状态、policy/选举表的重放一致性；清理旧 root 分支，不能靠恢复快照重新生成 BP 治理权 |
| `PureEOS/plugins/state_history_plugin/state_history_plugin.cpp`、`PureEOS/libraries/state_history/include/eosio/state_history/types.hpp` | 新 root 和 finality 历史 | 核对原生 Savanna 的历史输出，支持归档抽签/权限/升级记录；只按新状态格式所需修改，通用历史能力保留 |

系统升级的最小授权设计是：受保护目标的升级权限引用独立账号的专用 upgrade 权限，owner 恢复另行分离；不能为图方便把所有系统账户 active 直接交给普通出块账号。权限树修改能力与合约升级能力分开测试。

### 11.3 P1：21 人随机委员会和自动故障补位

| 具体文件 | 关键位置 | 如何修改 |
|---|---|---|
| `reference-contracts/contracts/eosio.system/include/eosio.system/eosio.system.hpp` | voters/producers/global、action 声明和私有辅助函数 | 建立整数票数，声明 pool=210、committee=21、epoch=252；加入第 4 节选举、实际/确认席位、健康、故障和历史表；声明 require_rex_vote/has_rex_shares 私有检查及同步 action 注释；删除公开人工治理 action 或明确初始化限定 |
| `reference-contracts/contracts/eosio.system/src/voting.cpp` | voteproducer、update_votes、propagate_weight_change、voteupdate、regproxy | 每个账户只投 0/1 候选，持有 REX 时禁止空列表；支持本人付 RAM 创建零余额选择，防首次买入死锁；结清待处理 REX 票权后再撤票/改投；余额变化只按整数净值记账；关闭代理和时间放大；移除 b1 由 eosio 代投的特殊分支，投票由本人授权 |
| 同一 `voting.cpp` | update_elected_producers | 替换 top-21 直接提交逻辑：冻结前 210＋现任，EXIT 随机退出、ENTRY 随机补入，预检通过后构造只变一席的名单；增长/故障分支按第 3.4、3.5 节执行 |
| `reference-contracts/contracts/eosio.system/src/producer_pay.cpp` | onblock | 在旧投票激活早退前处理必要健康/选举状态；从已验证区块头读取窗口，finality 后故障入队；单次最多处理 k≤21 个席位；每个新 epoch 只给一次新轮换机会 |
| `reference-contracts/contracts/eosio.system/src/finalizer_key.cpp` | set_proposed_finalizers、switchtosvnn、actfinkey | 同一目标名单生成等权 policy，threshold=floor(2k/3)+1；关闭 switchtosvnn；actfinkey 不再在任意时点绕过状态机立即覆盖 policy，维护自己密钥也须串行 |
| 同一 `finalizer_key.cpp` 与 `voting.cpp` | 两份 policy 提交返回值与已提交缓存 | 分开持久化 proposer/finalizer 已提交、已生效状态，处理部分接受及恢复；不能只看到 proposer 成功就开始下一轮 |
| `PureEOS/libraries/chain/include/eosio/chain/webassembly/interface.hpp` | host function 声明 | 声明 active finalizer policy、确定最终确认块、QC 指标三个版本化只读接口及明确数据布局 |
| `PureEOS/libraries/chain/webassembly/privileged.cpp` | 只读接口实现与现有特权 policy 接口 | 返回对应执行链状态的 generation/digest、final block 和已验证 QC 位图，处理数据未就绪；不能读取不确定的本地投票到达缓存作为合约事实 |
| `PureEOS/libraries/chain/webassembly/runtimes/eos-vm.cpp` | REGISTER_HOST_FUNCTION 注册 | 注册上述只读接口及恰当的调用权限检查，不能误注册为任意写权限；保留现有 set_finalizers/proposer 的系统自动调用 |
| `PureEOS/libraries/chain/genesis_intrinsics.cpp` | Genesis intrinsic 允许列表 | 把初始系统合约所需的新接口纳入确定清单，不能等 BP 治理批准后才可用 |
| `cdt/libraries/eosiolib/contracts/eosio/instant_finality.hpp` | C ABI 声明、类型和 C++ 包装 | 添加与内核完全一致的只读接口包装，固定宽度、长度和错误/未就绪语义；保持现有 set_finalizers 调用可用 |
| `cdt/libraries/eosiolib/contracts/eosio/producer_schedule.hpp`、`cdt/libraries/eosiolib/eosiolib.cpp` | get_active_producers 包装 | 复用现有接口；只在返回大小、空值或序列化契约确有需要时修改，不能把 proposer 顺序当成稳定逻辑席位 |
| `reference-contracts/contracts/eosio.system/CMakeLists.txt` | 生产合约构建输入 | 若抽出辅助实现文件则显式加入；编译生产 WASM/ABI，再同步测试产物，不能仅修改测试目录 |

ready/heartbeat/见证提交 action 的名称可以在实现时确定，但接收的内容只能是绑定本轮身份、区块、挑战、期限的事实证明；不能加入“BP approve candidate/upgrade”之类治理动作。错误、超时和候选耗尽应进入状态，不让 onblock 抛出预期业务异常。

### 11.4 P1：经济规则和剩余管理入口

| 具体文件 | 关键位置 | 如何修改 |
|---|---|---|
| `reference-contracts/contracts/eosio.system/src/eosio.system.cpp` | init、setram、setramrate、update_ram_supply | 一次初始化 64 GiB；按跨时代分段累计 1024→…→1→0；最后时代后零增发；关闭手工改速率和初始化后 setram |
| 同一 `eosio.system.cpp` | setparams、wasmcfg、setpriv、setalimits、setacctram/net/cpu、activate、rmvproducer、updtrevision | 按第 3.10 节关闭或移至一次启动流程；保留底层固定算法所需调用，不能简单删除全部 require_auth(get_self()) |
| 同一 `eosio.system.cpp` | setinflation、setpayfactor、setschedule、delschedule、execschedule | 固定初始发行/奖励曲线，关闭人工修改；execschedule 只推进预先确定的时间规则，不允许 BP 添加新通胀计划 |
| `reference-contracts/contracts/eosio.system/src/delegate_bandwidth.cpp` | changebw、update_voting_power、delegatebw/undelegatebw/refund、unvest | 保留质押所有权与代付语义；同步整数票权净变化，不把仅 CPU/NET 的账户强制纳入 REX 持仓限制；移除旧 b1 特定 unvest 回收路径；买卖 RAM 继续触发正确的分段供应结算 |
| `reference-contracts/contracts/eosio.system/src/rex.cpp` | buyrex、unstaketorex、add_to_rex_balance；拟新增 require_rex_vote/has_rex_shares | 按第 5.4 节在资产变更前校验拥有者已直接投一个有效候选，共享增加份额入口防绕过；deposit 不检查投票、不生成票权或 REX 收益份额 |
| 同一 `rex.cpp` | sell_rex、fill_rex_order、runrex、update_rex_stake、update_rex_account、储蓄和自动成熟路径 | REX 按 vote_stake 参与票权，转入/转出不重复计票；卖单成交及时更新票权，待结算差额不重复消费；全部实际卖出并结清后才可撤票；不因候选失效阻断正常退出或全局结算 |
| 同一 `rex.cpp` | rentcpu/rentnet、rent_rex、rex_loans_available、update_resource_limits | 保留租赁和 from/receiver 分离，按第 3.8.4 节收取本期租金及续租储备；不调用买入份额的投票门槛；租赁只给接收者资源，不增加拥有者质押或票权；保留排队卖单对新租的限制 |
| 同一 `rex.cpp` | fund_rex_loan、defund_rex_loan、runrex、rexexec、remove_loan_from_rex_pool、update_renewed_loan | 保留本人充值/取回储备、30 天自动续租及到期回收；统一期限常量，区分租单期限和份额成熟期；有界处理、跨期追赶、不续租退款；每次租单变动同步维护/校验 L 和池内 total_lent，保护排队赎回 |
| 同一 `rex.cpp` | add_loan_to_rex_pool、transfer_from_fund、donatetorex | 将租金的 channel_to_system_fees(get_self(), payment) 改为由 rex_account 实际付款，覆盖新租及续租；个人 fund/租单储备只扣一次、收益只入账一次；转账失败完整回滚，不由 eosio 补贴租单 |
| 同一 `rex.cpp` | setrexmature、setrex、add_to_rex_pool | setrexmature 只允许启动设置份额成熟规则；关闭 setrex 对 total_rent 的人工覆盖，初始定价连接器按启动规则初始化；不因保留用户租单管理而恢复系统改价权 |
| `reference-contracts/contracts/eosio.system/include/eosio.system/eosio.system.hpp` | rex_loan/rex_pool、租赁 action 声明、期限常量及资源统计字段 | 保留 cpuloan/netloan 表和全部租赁 action；明确租单 L、池内借出总额、个人 fund/续租储备的不同语义；若新增按资源汇总字段，确保所有新租/续租/到期路径原子维护；更新注释和类型，不新增 BP 管理入口 |
| `reference-contracts/contracts/eosio.system/src/exchange_state.cpp` | get_bancor_output 与租赁报价调用约定 | 保留定价接口，核对整数精度、舍入和极端池余额测试；total_rent 不作本金使用，不新增 REX 与 PowerUp 的强制定价高低关系 |
| `reference-contracts/contracts/eosio.system/src/powerup.cpp` | cfgpowerup、powerup、powerupexec、update_weight | 首次 ratio=10%，assumed_stake_weight 按最大供应量校准，并验证 S＋L 上界；保留统一 powerup_days 配置，不擅自改为 1 天；初始化后配置入口关闭；购买、代付、动态定价与到期处理保留，不增加票权 |
| `reference-contracts/contracts/eosio.system/src/producer_pay.cpp` | claimrewards 与发行/分桶统计 | 奖励依固定公式和真实票数/出块计账；BP 不修改分配率，提款仅领取自身已产生收益；若采用取消 per-vote 版本，完整移除对应计账依赖 |
| `reference-contracts/contracts/eosio.token/src/eosio.token.cpp`、`reference-contracts/contracts/eosio.token/include/eosio.token/eosio.token.hpp` | create、issue、issuefixed、setmaxsupply、retire | 核心币上限和发行授权固定，自动奖励调用受范围约束；BP 无人工增发/改上限能力；若同合约仍服务其他用户代币，不能把用户 issuer 能力误禁 |
| `reference-contracts/contracts/eosio.bpay/src/eosio.bpay.cpp` | on_transfer、claimrewards | 若生产部署该合约，把当前按票数 top-N 的收款分配改为明确的实际委员会/工作量规则，避免随机现任收不到应得奖励；提现只允许收益拥有者 |
| `reference-contracts/contracts/eosio.fees/src/eosio.fees.cpp` | on_transfer、noop 与部署权限 | 核实生产费用合约接收 rex_account 租金后按既定 donatetorex 路径分配；与 REX 付款修正联调，避免漏记、重复入账或无穷转账通知；核对 code 授权，不新增管理员自由提款动作 |
| `reference-contracts/contracts/eosio.system/src/native.cpp`、`reference-contracts/contracts/eosio.system/include/eosio.system/native.hpp` | native action 包装与权限变更转接 | 与内核系统目标保护保持一致；删除声明不是安全边界，不阻断普通账户自身 setcode/updateauth |
| `reference-contracts/contracts/eosio.system/src/limit_auth_changes.cpp` | limitauthchg、check_auth_change | 保留用户自设权限变更约束；不能把它当成不可绕过的系统 owner 锁，更不能允许它恢复 BP 管理链 |
| `reference-contracts/contracts/eosio.bios/src/eosio.bios.cpp`、`reference-contracts/contracts/eosio.boot/src/eosio.boot.cpp` | setprods/setfinalizer、setpriv、activate | 仅保留启动/测试实际需要的受限实现；生产不保留可重新部署为任意治理工具的能力 |
| `reference-contracts/contracts/eosio.wrap/src/eosio.wrap.cpp`、`reference-contracts/contracts/eosio.wrap/include/eosio.wrap/eosio.wrap.hpp` | exec | 生产目标排除 wrap，部署清单不授特权；若仓库保留上游示例，标明不属于 PureEOS 生产组件 |
| `reference-contracts/contracts/eosio.msig/src/eosio.msig.cpp` | propose/approve/exec 的授权与执行 | 普通多签继续使用实际交易权限校验；不以修改 msig 代替切断系统权限链。若原生 inline 保护影响正常用户 msig，需配套调整并回归验证 |
| `reference-contracts/contracts/CMakeLists.txt` | 合约目标清单 | 区分生产和兼容测试目标，生产不打包可用 wrap/bios 治理路径；不删除无关上游测试文件 |

当前源码中的 setrex、b1 代投和 unvest 也是治理/特殊管理能力，不能因为未使用 require_auth(get_self()) 的同一种写法而漏查。实际权限审计须覆盖所有 require_auth、issuer 权限和系统资金 code 权限。

### 11.5 P1/P2：P2P、客户端与上线工具

| 具体文件 | 如何修改 |
|---|---|
| `PureEOS/libraries/chain/peer_keys_db.cpp`、`PureEOS/libraries/chain/include/eosio/chain/peer_keys_db.hpp` | 从 top-50 缓存语义改为 current/pending/next 身份，收到抽签/变更后及时刷新，保留旧 policy 需要的连接 |
| `PureEOS/plugins/net_plugin/net_plugin.cpp` | 在不改变链上名单判断权的前提下提前连接候选、中继转发投票、控制连接/消息限速并记录失败；不能本地临时替换 scheduled producer |
| `reference-contracts/contracts/eosio.system/src/peer_keys.cpp`（拟新增） | 当前生产目录缺少与测试夹具同源的实现；确认接口后纳入生产合约，返回实际 current/pending/next，候选 peer 登记须由本人授权 |
| `PureEOS/programs/cleos/main.cpp` | 投票帮助/参数只允许 0/1 候选，说明 REX 买入前先选一个、持有时不可撤票及首次买入组合交易；保留 rentcpu/rentnet、续租资金和 rexexec 命令，提示租用资源无需买 REX/投票；不得自动替用户选候选；禁代理调用提示；升级示例使用独立账号，不生成 BP 多签治理提案；普通 multisig 命令保留 |
| `PureEOS/tutorials/bios-boot-tutorial/bios-boot-tutorial.py` | 该现有示例包含 resign('eosio','eosio.prods') 和旧升级流程；明确标为不适用新链，或提供新链模式移除这些授权步骤，不能直接拿旧教程启动生产链 |
| `PureEOS/scripts/pureeos_bootstrap.py`（拟新增） | 生成/检查新 Genesis、k 个节点配置、独立管理员、初始合约/参数/权限清单；检查 REX 租赁与费用合约可用、rex_account 内联转账权限、30 天期限及 PowerUp 启动参数；部署后关闭一次初始化并导出审计结果；私钥不得写入清单 |
| `PureEOS/scripts/pureeos_upgrade.py`（拟新增） | 生成目标、旧/新 WASM/ABI 哈希、版本和迁移交易的升级包；由独立账号专用权限签名发送；为未来 AI 执行提供结构化输入/结果，不要求 BP approve |
| `PureEOS/scripts/pureeos_permissions_audit.py`（拟新增） | 读取系统账户、issuer、owner/active/custom/eosio.code 和权限链接，递归找 BP/eosio.prods/任意特权转发路径；输出账号、路径、所需签名及处理建议 |
| `reference-contracts/contracts/eosio.system/ricardian/eosio.system.clauses.md` | 同步一票一投、候选抽签、独立升级、资源和 RAM 规则；明确保留 REX 资源租赁及共同资源上限，删除节点管理权和错误的租赁禁用说明 |
| `reference-contracts/contracts/eosio.system/ricardian/eosio.system.contracts.md.in` | 修改 buyrex、unstaketorex、voteproducer、sellrex 条款：单候选门槛、持有期禁止撤票、零余额预选及正常退出；同步 rentcpu/rentnet、充值/取回、rexexec 的 30 天期限、续租条件、退款和触发结算说明，强调租用资源不必投票；不保留“21 个或代理”的旧要求 |
| `PureEOS/docs/02_cleos/03_command-reference/system/system-rex-buyrex.md`、`system-rex-unstaketorex.md`、`system-rex-deposit.md`、`system-rex-sellrex.md`（后 3 个在同一目录） | 区分预存与买入，给出先投票再买入的示例，说明部分卖出/排队不解除限制、全部成交结清后可撤票，以及候选失效时的改投/退出路径 |
| `PureEOS/docs/02_cleos/03_command-reference/system/system-rex-rentcpu.md`、`system-rex-rentnet.md`、`system-rex-fundcpuloan.md`、`system-rex-fundnetloan.md`、`system-rex-defundcpuloan.md`、`system-rex-defundnetloan.md`、`system-rex-rexexec.md`（后 6 个在同一目录） | 保留租赁使用示例；区分本期租金/续租储备、付款人/接收者、资源/票权，说明 30 天续租和到期退款、rexexec 有界处理、资源回收延迟及实际租金流向；不承诺长租更便宜 |
| `reference-contracts/docs/04_guides/01_upgrading-the-eosio.system-contract.md`、`reference-contracts/docs/04_guides/07_how-to-use-eosio.wrap.md` | 说明上游治理教程不适用 PureEOS 新链，链接独立账号升级流程；不指导生产恢复旧权限 |
| `PureEOS/docs/02_cleos/02_how-to-guides/how-to-vote.md`、`reference-contracts/docs/04_guides/04_how-to-vote.md` | 更新单候选、锁定余额及 CPU/NET/REX/PowerUp 区别，说明无余额可先选候选、持有 REX 只能改投及卖清后撤票；解释投票只是候选选择而不是节点治理授权 |

### 11.6 验证文件、构建与产物

| 具体文件 | 要补充/修改的验证 |
|---|---|
| `reference-contracts/tests/eosio.system_tests.cpp`、`reference-contracts/tests/eosio.system_tester.hpp` | 单候选整数票、资源/奖励/RAM、初始化后管理 action 拒绝；构造 210 候选和查询选举状态；第 13.1.1 节买入门槛测试中买入 fixture 显式投票；保留未投票的租用者 fixture，扩展现有 REX 租赁/授权用例覆盖第 13.6.1 节生命周期、实际转账、S＋L 上限和回滚 |
| `reference-contracts/tests/eosio.system_rex_matured_tests.cpp` | 覆盖未成熟、成熟、储蓄、自动卖出和排队卖单；未卖清不能撤票，归零结清后可以；候选失效不阻断成熟/退出、也不影响其他账户订单结算 |
| `reference-contracts/tests/pureeos_election_tests.cpp`（拟新增） | EXIT/ENTRY golden vectors、随机留任、预检失败、实际加入后同席补抽、两份 policy 不同时间生效、完整观察与耗尽恢复 |
| `reference-contracts/tests/eosio.finalizer_key_tests.cpp` | k→21 和 15 门槛、非现任签名不计票、节点自身 key rotation 不覆盖在途选举 |
| `reference-contracts/tests/eosio.msig_tests.cpp`、`reference-contracts/tests/eosio.wrap_tests.cpp` | 旧节点升级/强制执行案例在新链配置下失败；普通用户多签成功；独立账号升级成功 |
| `PureEOS/unittests/pureeos_governance_tests.cpp`（拟新增） | 原生 setcode/setabi 保护、权限树/privileged inline 旁路、eosio.prods 不重建、独立升级/owner 隔离、AI 专用密钥撤销 |
| `PureEOS/unittests/pureeos_genesis_tests.cpp`（拟新增） | 原生 Genesis、管理员/清单参与 chain ID、k=1/2/3/4/21、非法初始 policy 拒绝及首块/QC |
| `PureEOS/unittests/producer_schedule_savanna_tests.cpp`、`PureEOS/unittests/savanna_proposer_policy_tests.cpp`、`PureEOS/unittests/savanna_finalizer_policy_tests.cpp` | 21 人目标只变一席、proposer/finalizer 各自安全晋升、在途不叠加和过渡门槛 |
| `PureEOS/unittests/finality_tests.cpp`、`PureEOS/unittests/finality_core_tests.cpp`、`PureEOS/unittests/fork_db_tests.cpp` | 缺 6/7 票边界、分叉回滚、fault observation 最终确认边界和新 root |
| `PureEOS/unittests/snapshot_tests.cpp`、`PureEOS/unittests/blocks_log_replay_tests.cpp` | 恢复后同一随机/故障/权限结果，旧治理授权不复活 |
| `cdt/tests/integration/instant_finality_tests.cpp`、`cdt/tests/integration/finality_state_tests.cpp`（后者拟新增） | 新只读接口 ABI、长度、未就绪、序列化以及实际 host 返回语义 |
| `PureEOS/tests/nodeos_voting_test.py` | 多节点一票一投、随机退出/补入、预检与故障后的自动恢复，不由测试脚本直接强行改名单 |
| `PureEOS/tests/pureeos_rotation_performance_test.py`（拟新增） | 第 10.6 节四组同环境性能对比，输出 TPS/漏块/最终确认分位数和补位时间 |
| `PureEOS/unittests/eosio_system_tester.hpp`、`PureEOS/unittests/contracts/eosio.system/CMakeLists.txt` | 读取新表及 21 人断言；引用与生产一致的 WASM/ABI，不继续复制不匹配旧合约 |
| `PureEOS/unittests/contracts/eosio.system/eosio.system.wasm`、`PureEOS/unittests/contracts/eosio.system/eosio.system.abi` | 只由生产构建生成后同步，不能手工编辑二进制来“实现”规则 |
| `PureEOS/unittests/CMakeLists.txt`、`PureEOS/tests/CMakeLists.txt`、`reference-contracts/tests/CMakeLists.txt`、`cdt/tests/integration/CMakeLists.txt` | 注册新增测试与 fixtures，移除新链对 legacy 激活/节点治理成功案例的错误依赖；保留其他配置需要的上游回归 |
| `PureEOS/libraries/chain/CMakeLists.txt`、`cdt/libraries/eosiolib/CMakeLists.txt` | 仅在新增实现单元/类型安装确有需要时调整构建与导出，避免声明可编译但运行时缺 intrinsic |

每项实现提交都要注明对应本节文件、规则变化、实际执行的测试，以及尚未覆盖的性能/恢复边界。最终上线必须用生产部署的合约和权限清单验证，不能以测试夹具中的特殊权限代替生产授权。

## 12. 从 0 号区块启动的实施边界

本链从第一版使用新共识、新投票规则和无节点治理权限的账户结构。独立管理员的初始化/升级权限与节点 producer/BLS 密钥分离。

推荐启动顺序：

1. 确定系统升级独立账号、owner/upgrade 权限、受保护系统合约清单和初始版本哈希；不把 eosio.prods 或 BP 账户加入系统管理权限树。
2. 准备实际可启动的 k 个 Genesis 节点，1≤k≤21；核对 producer authority、BLS 公钥/PoP、P2P 身份、endpoint 和 safety 存储。
3. Genesis 写入同一 k 人的两份 policy、等权、threshold=floor(2k/3)+1、generation=1、target_committee_size=21、候选上限及协议版本域；管理员权限/启动清单按确定格式承诺到 Genesis。
4. 所有节点计算并核对同一 chain ID、policy digest 和启动清单摘要。Genesis 不包含任何私钥。
5. 配置节点密钥与网络；k=1 时准备带 fencing 的主备，保证同一身份任一时刻仅一个实例签名。
6. 原生初始化 Savanna，验证首块、Strong QC、LIB、restart、replay、snapshot restore 和 finalizer safety 恢复。
7. 由独立启动权限部署核对过的系统合约及必要账户，建立与 Genesis 成员匹配的注册记录；节点不审批部署交易。
8. 一次初始化投票表、核心币/发行参数、64 GiB RAM、REX 池/租赁定价及 30 天期限、生产费用分配、PowerUp 租期/价格和 S＋L 合计 10% 参数、随机源、预检门限、冷却和观察规则；验证租金从 rex_account 实际付款。
9. 完成权限收敛：关闭重复 init、人工配置、任意名单和 wrap 等路径；保留独立账号的明确升级能力及系统自动记账能力，导出完整权限清单检查无 BP 治理引用。
10. 注册候选、保持同步并参与投票排名。growing 每次随机选择一个非现任、强制预检，再从 k 增至 k+1。
11. 两份 policy 都 active、完整观察通过后才增加下一人；新成员失败时按当前规模同席补抽。无合格候选则等待。
12. 达到 21 人后运行影子抽签，只记录 EXIT/ENTRY、快照和预检结果，核对确定性及性能；影子阶段不能悄悄改变 active 名单。
13. 按启动版本确定的 activation_slot 和链上就绪条件启用正式单席轮换，无需节点审批；未满足就绪条件则自动等待。
14. 运行故障、分区、缺 6/7 票、候选耗尽、恢复及独立账号升级演练，通过后进入生产服务。

候选目标为最多 210 人，不要求先形成 210 人的 active policy。关闭扩容/轮换的异常等待是状态机条件，不能留 BP 手工暂停或紧急固定名单权。

独立升级账号可以在链仍能执行交易时修复软件规则；全网完全停止或失去最终确认时，普通链上升级交易也无法即时恢复服务。节点主备、密钥恢复和公开协调的软件修复是另外的运维过程，不能借此给节点补回治理权限。

同身份主备必须保护最新 finalizer safety state，不能让两个实例同时使用同一 BLS 身份投票；owner/升级密钥不放进普通出块节点配置。

## 13. 测试计划

### 13.1 投票测试

- 锁定 100 Token 投 1 个候选，该候选票数精确增加 100 Token；
- 投 2 个或更多失败；
- 使用 proxy 失败；
- A 改投 B，A 减少完整投票余额、B 增加同一余额；
- 没有 REX 或已全部实际卖出并结清时，撤票只扣除一次完整投票余额；仍持有 REX 时拒绝空列表但允许原子改投；
- 重复投同一候选不重复加票；
- 增减锁定 Token 时，当前唯一候选只增加或减少相同数量，时间经过不增加票数；
- 将 100 Token 拆到 10 个账户后总票权仍为 100 Token；
- 同一份 Token 不能同时通过 CPU、NET、REX、代理或多候选重复计票；CPU/NET 与 REX 相互迁移前后总票权不变；
- REX 新租、续租、回收和储备充值/取回均不因资源使用权给租用者或接收者增减票权；租金引起的池收益只按持有人份额结算；PowerUp 购买、到期和第三方代付不产生票权；
- 未授权、未注册、inactive 候选失败；
- 代理相关 action 从链启动起即失败，不能产生代理票权；
- `Σ producer.total_votes == Σ 已分配voting_balance` 在每个 action 后成立。

#### 13.1.1 REX 强制投票与退出边界测试

- 未投票账户可以 `deposit/withdraw`，但 `buyrex` 和 `unstaketorex` 失败；失败不改变个人 fund、CPU/NET 质押、资源权重、REX 池或票数。
- 首次用户没有 CPU/NET/REX、也没有 voter 行时，可以本人授权创建零票单候选选择；RAM 由本人承担，未授权失败。`deposit → voteproducer → buyrex` 同交易成功并按真实买入金额计票；无效候选或买入失败时整笔交易回滚。
- 有效但当前零票、未进前 210 或未在委员会中的已注册候选，可以被预选并获得首次买入后的票数，不能因排名形成开户死锁；不存在、inactive 或出块签名无效的候选不能满足买入条件。
- 代理、空列表、多候选均不能满足 REX 买入门槛；直接调用合约与经 CLI/组合交易调用的结果一致，不能只靠前端限制。
- `unstaketorex` 检查 owner 而不是 receiver：只有 receiver 投票时仍失败；owner 已投有效候选时可为不同 receiver 转出其拥有的质押，迁移前后不重复生票。
- 持有任意正数 REX 时，空列表失败，A→B 原子改投成功；不要求自投，节点账户和普通账户遵循相同规则。无效 B 失败后 A 的票数和持仓不变。
- 分别覆盖最小精度份额、未成熟、已成熟、REX savings、买入自动转储蓄和 `consolidate`；不能用 `vote_stake` 的舍入或成熟桶变更绕过持仓检查。
- 覆盖部分卖出、全部卖单排队、取消卖单、自动卖出及最终成交：排队期间不能撤票；全部实际归零且票权结清后可撤票，即使 fund 尚未提现或零余额表行仍存在。
- 他人触发 `runrex/rexexec` 使卖单成交后，原持有人撤票、改投、`updaterex` 或 `withdraw` 不得重复扣减待结算 `stake_change`；混合 CPU/NET 余额仍准确计票，全局票权不出现负数或悬空票。
- 候选注销或签名失效后，新增买入/转入拒绝，改投有效候选后恢复；已有 REX 仍可正常成熟和赎回，在没有其他有效候选时也能卖清后撤票。其订单不能阻断其他账户的 `runrex`，不能自动清空所有投票记录或转投管理员指定节点。
- 同交易的 `buyrex → 撤票` 应失败并回滚买入；`改投 B → buyrex` 成功时余额只归 B；`sellrex → 撤票` 仅在实际清仓结清后成功，尚未成交时失败并回滚该交易。
- 仅 CPU/NET 持有人仍可不投或撤票；REX 收益按份额而非投给哪个候选分配，预存资金和单纯投票不取得 REX 收益。单纯 REX 租用者、PowerUp 付款人、RAM 交易者及费用捐赠者不因费用流入 REX 而被要求先投票；另持有 REX 份额时仍受持仓约束。

### 13.2 排名与抽签测试

- 覆盖 0、1、20、21、22、209、210、211 个合格候选，验证前 210 边界及候选不足行为；
- 同票按账户名排序；inactive、零票、无效 producer/BLS、无 peer 登记等不符合规则者排除；
- 相同 C、A、席位映射、seed 和 counter 得到相同退出者、补入者及重试序列；
- 正常退出由 EXIT 流等概率产生，补入由 ENTRY 流产生，两个域不复用；增长没有退出抽签，故障退出绑定证据；
- 补入者在本轮快照中且不属于冻结现任/实际 active/已拒绝集合；准备退出者不能作为本轮补入者；
- C 满 210 且包含全部 21 现任时备用为 189；现任跌出 C 时按实际集合差处理，不错误截断为 189；
- 正常新旧名单各 21 人，恰好一出一进；重复失败只改变同一逻辑席位的候选；
- 预检失败不重抽退出者、不换种子，entry_counter 持续推进；名单提交失败不重新抽签；
- 空备用、拒绝采样边界、端序、counter 和域标签有固定 golden vectors；
- 随机退出允许成员连续多轮留任，不写“21 次必然走遍全体”的断言；
- 快照/信标超时或候选耗尽后，只有预设条件允许结束尝试；后续故障尝试继续锁定原席位；
- 换票风暴与分片快照并发时，本轮仍对应同一冻结状态；统计测试仅辅助检查随机实现。

### 13.3 在线预检、加入后故障与自动补位测试

- 心跳自签或单点 ping/HTTP 成功不能代替门限连接、同步、producer/BLS 就绪证明；
- 错误 chain ID、peer 身份、签名、过期证明、重复见证、旧轮次和 nonce 重放均拒绝；
- 正常候选预检失败时，原 21 人及两份 policy 均不变，计划退出者继续工作；
- 生产路径不能绕过预检先把故障候选写入名单；ready 后资格或密钥变化必须重新校验；
- 正式加入后掉线，其他 20 人保持，失败候选的窗口可能漏块，按同席流程重新预检补人；
- 一窗至少一个合法块不触发整窗零出块，但不足以自动通过完整观察；
- 两份目标都 active 后至少观察目标人数×12 槽，覆盖新节点完整出块窗口及 BLS/QC/LIB 状态；
- fault observation 被 finality 覆盖后才入队，fork rollback 不残留，陈旧记录丢弃、同席不重复排队；
- 多窗口或多轮缺块时检测上界为当前 k 个唯一席位，增长期按真实 k 和 schedule 排序归属；
- policy 切换边界不错误归责，actual active 映射与 confirmed 状态分别正确；
- 多个故障排队逐个处理，上一名单及观察未完成前不处理下一席；
- 冷却期候选不能再次抽中；全部候选耗尽保持 policy，预设恢复条件达成后继续；
- 单个观察者无任意踢人/豁免权，节点无需治理批准即可完成正常自动补位。

### 13.4 共识与恢复测试

- Genesis k 超出 1..21、两份账户集合不同、BLS 重复、权重非 1、阈值或 generation 错误时拒绝；
- initial policy、管理员权限或启动清单的承诺字段变化会改变 chain ID/对应摘要；
- 从 0 号状态直接使用 Savanna，首个后继块为 proper Savanna block，不经过 legacy/transition；
- k=1、2、3、4、21 分别验证阈值 1、2、3、3、15，覆盖首块、QC、LIB、重启和 safety 恢复；
- 从 1 逐人增长到 21，每次新增同一账户的出块/确认身份；没有 ready 候选不增长；
- 增长后新成员失败保持当时规模，对该席补抽；不能继续增加或用缩表回避观察失败；
- 每个新目标只有在两份 policy 安全激活且完整观察通过后才允许后续变更；
- 人为让 proposer/finalizer 在不同时间生效、模拟提交失败或部分接受，验证恢复状态和禁止重复提案；
- 正式单席过渡验证 active/pending 两份 finalizer 门槛和旧、新成员通信；
- 21 人时分别注入 1、6、7 名 finalizer 无法投票：验证达到 15 票与不能形成 Strong QC 的边界；
- 候选池其余 189 人的签名不因“候选身份”计入当前 policy；
- replay、fork、snapshot restore 后随机、故障、观察和授权状态一致；
- 跳过 epoch 边界时只开启允许的一次流程，不补做多次积压轮换；
- 普通链上补位及管理员升级不能在完全无块/无最终确认时被测试伪造为即时自救。

### 13.5 网络与性能测试

- 排名第 200 的节点抽中后，能在正式加入前完成连接、同步和 BLS 就绪；
- getpeerkeys 覆盖实际 current/pending/next，正常单席变化通常为最多 22 个身份；
- 210 候选保持同步时，未经选入者不参与当前 finalizer 计票；
- 验证中继正确转发投票、冗余连接、慢节点、跨地域丢包和隔离恢复；
- 210 候选快照、双随机抽签和反复预检失败均在 action CPU/RAM 限制内，历史清理后状态有上界；
- 按第 10.6 节四组同环境负载对比，运行至少数百次轮换及跨日故障场景，保存实际 TPS、漏块、延迟分位数、CPU/内存/带宽；
- 一轮 252 slot=126 秒；检查轮换等待遵守状态机，不把选举机会间隔作为完成时限；
- 记录预检失败对当前名单无影响，以及正式加入后故障的实际空窗、检测与补位耗时，不伪造零影响指标。

### 13.6 资源与 spam 测试

- CPU、NET 的 `current_weight_ratio` 和 `target_weight_ratio` 都为 `100000000000000`，并验证 `powerup_frac = 1000000000000000` 下的标称比例为 10%；
- 使用最大可质押供应量作为 `assumed_stake_weight`，分别测试 0%、10%、50%、100% 供应量进入直接质押或 REX 池并出租的不同组合；验证每种资源的 S＋L 总量不超过 M、合计权重占比不超过约 10%；
- 一个账户拆成大量账户、多个账户互相 `delegatebw`、`transfer` 所有权变化，都不能增加传统质押总资源权重或总票权；
- 直接质押与 REX 出租侧持续满载时，PowerUp 账户仍可按剩余市场容量取得付费资源；两套市场报价各按其配置变化，不断言 REX 必定比 PowerUp 贵或便宜；
- `rentcpu/rentnet` 在初始化完成、有资金及合法报价时成功，并与 REX 买卖/投票和 PowerUp 共存；池不可用、资金不足、报价不满足或卖单排队时按规则拒绝；
- PowerUp 为第三方代付不产生投票权，也不计入传统质押资源权重；
- 资源恢复窗口下测量免费交易的持续占比与最大突发，文档和监控不得把长期 10% 权重误报成逐区块硬上限；
- 无效交易和 P2P 洪水测试验证节点限速、去重、连接配额和 peer 惩罚，不依赖链上 CPU/NET 配额处理全部网络层攻击。

#### 13.6.1 REX 租赁、续租、回收与费用账测试

- 分别覆盖 CPU/NET、自租/代租、同一接收者多笔租单；新租租期为当前时间起 30 天，实际资源数量按报价记账，租用者不取得池本金、REX 份额或投票质押。
- 未持有 REX、未投票的账户可租赁、充值/取回自己的续租储备；买入 REX 份额仍必须投票。未经付款人授权、操作他人租单、错误币种、非正租金/充值/取回金额、负续租储备、余额不足或已到期充值/取回均失败并回滚。
- `loan_payment` 消费一次，`loan_fund` 只进入可核对的租单余额；追加/取回储备不增加池收益、资源权重或票权，不改变已消费的本期租金。
- 在到期前、恰好到期、到期后分别调用结算；余额恰好够一期时续租，少一个最小单位时关闭并退回余款；续租重新报价并调整权重，期限按原到期时间加 30 天，而不是从触发时重新计算。
- 保留报价不满足、池不可用和有待卖 REX 时停止新租/续租的分支；释放到期本金后按队列处理持有人卖单，不因自动续租无限推迟赎回。
- 关闭租单仅扣除该单资源，不能清空接收者其他租单、直接质押或 PowerUp 资源；余款回到原付款人的 fund，再提现到钱包，不退还已消费租金。
- 覆盖无人触发时的到期滞留、max 为 0/1/2、混合 CPU/NET/卖单积压、连续跨多个租期及重复结算；每次只处理限额，重复执行不重复收租/退款，跨 fork、重启和 replay 结果一致。
- 将 `eosio` 可用核心币余额设为 0，在租用者已经合法预存资金的情况下新租和续租仍能成功；核对实际 transfer trace 为 rex_account→fees_account→REX 收益路径，eosio 余额不被扣减，收益只入账一次。
- 独立核对个人 fund、租单余额、REX 池本金、待释放收益和待结算卖单的资金覆盖；续租储备取回、池收益释放及卖单付款互不侵占。模拟费用转账/code 授权失败时，资金、池、资源和订单状态全部回滚，不用给 eosio 补款让测试通过。
- 混合直接质押和 CPU/NET 租单时，维护每种资源的 L，验证两类租单总额等于 total_lent，且 total_lent 不超过 total_lendable；到期但未回收的租单仍计权重。代付、拆账户、续租涨跌和收益再出租均不能突破第 3.8.2 节的 S＋L 上界。
- 候选注销不影响单纯租用者续租/退款；REX 持有人仍可按规则改投或卖出。setrex、初始化后的 setrexmature/cfgpowerup 继续拒绝人工调整，租赁恢复不恢复 BP 治理。

### 13.7 RAM 四年减半测试

- RAM 市场初始化值精确为 `68,719,476,736 bytes`（64 GiB），起始速率为 `1024 bytes/slot`；
- 每个四年边界前一槽、边界槽和后一槽分别验证速率为预期值，依次为 1024、512、256、128、64、32、16、8、4、2、1、0；
- 一次 action 跨过一个、多个以及全部 11 个时代时，分段惰性结算结果与逐槽模拟完全一致；
- 在没有 RAM action 的长时间区间后首次买卖 RAM，旧时代增量不会错误地按当前速率计算；
- 同一槽位重复触发更新不重复增发；跨 fork replay、snapshot restore 和节点重启后结果一致；
- 到第 44 年边界时累计自动增发精确为 `516,433,536,000 bytes`，最终 `max_ram_size` 精确为 `585,153,012,736 bytes`；
- 第 44 年后的任意槽位都不再改变 `max_ram_size` 或 RAM 市场 base reserve；
- RAM 市场 base reserve 的每次增加量始终与 `max_ram_size` 增量完全相同；
- 任意账户调用 `setramrate` 均失败，系统账户也不能绕过固定曲线；初始化完成后任何 `setram` 增发均失败。

### 13.8 无节点治理与独立账号升级测试

- 任意单个 BP、15/21 BP、多数/少数门槛及全体 BP 的直接签名，都不能仅凭节点身份调用系统升级或管理 action；
- eosio.msig 提案/执行、直接多签交易、嵌套 inline、父级权限和间接账号引用均不能绕过上述限制；
- Genesis、扩容、随机换人、重启和 replay 后，eosio.prods 治理权限不会被内核重新生成；
- eosio.wrap 不存在可用特权执行能力；setpriv、任意参数/人工名单入口不能重新开放它；
- 独立账号在授权系统目标上升级 WASM/ABI 成功，错误账号、未授权目标、仅有 BLS 签名或仅有节点多签均失败；
- 专用 upgrade 权限不能单独更改 owner、系统资金权限或调用被关闭的人工管理 action；
- 替换/撤销未来 AI 专用执行密钥后，旧密钥不能升级，新权限按设定生效；不实际调用 AI 作为共识判定器；
- 升级记录、版本检查、一次迁移和恢复后合约/ABI 哈希一致；被撤销/改动权限后的旧提案不能借缓存批准继续执行；
- setcode/setabi 保护在原生层生效，不能通过移除 ABI 检查之外的方式绕过；普通用户升级自己的合约仍可用；
- 系统参数初始化完成后，cfgpowerup、setrexmature、setparams、wasmcfg、发行/奖励人工配置等均按清单拒绝；
- 核心币 setmaxsupply 不可被 BP 改大，自动奖励按固定公式发行，系统账户无 BP 可满足的提款路径；
- 节点本地协议特性激活 API 不能引入 Genesis/公开版本规则之外的新共识能力；
- 自动 onblock、候选注册/自身密钥更新、轮换、资源购买、退款、奖励及用户多签保持可用；
- 新增节点自身 BLS key 更新与在途选举串行，不得以维护密钥为由覆盖另一份目标名单；
- 导出所有实际部署的系统账户、issuer、owner/active/custom/eosio.code 权限，检查节点治理链完整断开。

## 14. 验收标准

只有同时满足以下条件才算实现完成：

- 一个账户最多投一个候选，票数精确等于其已分配 CPU/NET/REX 锁定 Token；拆账户、跨路径迁移、时间推移均不额外生票，PowerUp 不计票；
- 买入或转入 REX 必须先由拥有者直接选择一个有效候选；零余额首次选择可行，持有期间不能清空投票但可原子改投；全部实际卖出并结清后可撤票，储蓄/排队/舍入不能绕过，候选失效不阻断正常退出；
- 候选快照严格为确定性排名前 min(210, 合格候选数)，冻结后本轮不被换票改变；
- 从 k=1 可以按强制预检和完整观察逐人增长到 21；没有候选时保持当前规模；
- 稳定时只有当前 21 人负责出块及最终确认，常态 Strong QC 门槛为 15；
- 正常退出和补入分别使用可验证的独立随机流，正常一次仅一出一进；重试不任意更换退出者、快照和种子；
- 预检失败保持当前名单；正式加入后失败，固定其他 20 人，仅补同席，并如实记录漏块及恢复耗时；
- 两份 policy 激活且完整观察通过前不开始下一次变更；候选不足、信标超时、故障排队均可确定性等待和恢复；
- 首块、Strong QC、LIB、fork、restart、replay、snapshot restore 及安全状态恢复正确；
- 随机请求、退出/补入、失败、提交、生效、观察记录可审计且历史 RAM 有上界；
- 对固定 21 人、210 候选维护、正常轮换和故障轮换完成同环境对比，公开测量值并达到上线前确定的性能指标，不以 500ms 配置替代实测；
- 节点不再拥有系统升级、参数、资产支出、人工名单或特权管理权；旧 eosio.prods/msig/wrap 路径和间接授权不能恢复这些能力；
- 独立管理账号可以按明确范围升级系统合约，节点只验证、打包和确认；未来 AI 专用权限可委托、可撤销，不与 producer/BLS 或 owner 密钥混用；
- 自动选举、最终确认、节点自身运维、用户多签、正常资源交易和奖励不因移除节点治理而被破坏；
- 奖励总账、整数票数、实际出块统计和随机委员会口径一致；人工修改奖励/发行入口已按方案关闭；
- rentcpu/rentnet、本人续租储备管理、30 天续租/到期回收、REX 买卖/退出和 PowerUp 均可用；CPU、NET 各自的直接质押＋REX 出租权重合计长期不超过约 10%，租用者不因此取得票权；
- 新租和续租的租金由 rex_account 保管的用户资金支付，不扣 eosio 余额；手续费收益按既定路径仅入账一次，未消费储备可退回，结算失败完整回滚；生产 code 权限不落到 BP；
- RAM 初始 64 GiB、1024 bytes/slot、每固定四年减半，第 44 年停止自动增发，最终为 585,153,012,736 bytes；手工 setramrate 和初始化后 setram 不可绕过当前发行版本；
- 故障恢复不依赖 BP 紧急治理或手工固定名单；独立升级账号也没有无共识改写历史的特权。

上线前还需要在独立发布的初始版本中具体确定：管理员正式账户/升级目标清单、奖励公式与供应上限、REX 初始租赁定价参数及份额成熟参数、PowerUp 租期和价格曲线、到期结算运维频率/积压告警、随机信标方案及参与者规则、预检见证者资格及门限、超时/冷却/观察门槛、历史保留窗口和性能验收数值。REX 本版租单期限沿用 30 天；PowerUp 1 天仍未定案。这些是参数定稿工作，不是授权 BP 自行决定。

## 15. 推荐实施顺序

1. 固定 210 候选、21 出块/确认、随机一出一进，以及节点无治理权、独立账号升级的协议边界。
2. 审计原生权限与全部系统账户，确定独立升级目标清单、初始权限及未来 AI 可委托的专用权限形状。
3. 固定原生 Savanna Genesis schema，将 k 人两份 policy、目标规模、初始协议能力及管理员/部署清单承诺纳入确定性初始化。
4. 实现原生 Genesis、首块、QC、LIB、restart/replay/snapshot 和 safety 恢复，通过后清理 legacy/transition 分支。
5. 移除 eosio.prods 治理权限创建/自动更新、BP 系统授权链和 wrap 路径；完成原生系统升级目标保护及独立账号升级测试。
6. 逐项关闭或初始化限定第 3.10 节的旧治理 action，保留系统自动资源、发行、领奖和普通用户授权能力。
7. 实现 CPU/NET/REX 的单候选整数计票，以及 REX 买入强制投票、零余额预选、持有期禁止撤票和完整退出结算；保留 REX 30 天资源租赁/续租/回收，修正租金付款及资金账，并初始化直接质押＋REX 出租合计 10% 的 PowerUp 权重上界。
8. 实现 RAM 64 GiB 起步、1024→…→1→0 的四年分段结算，关闭手工增发入口。
9. 实现前 210 一致快照、growing、EXIT/ENTRY 随机流、21 席位状态、同席失败重试与有界链上审计。
10. 实现强制预检、自动故障队列、proposer/finalizer 串行晋升与完整观察；加入确定性的只读共识接口。
11. 实现 current/pending/next P2P 连接、候选同步、见证验证及生产随机信标；证明/信标不承担治理审批。
12. 同步生产 WASM/ABI 和测试夹具，完成经济、权限、共识、恢复、网络及性能测试。
13. 测试网从 k=1 增长到 21，开展影子抽签、连续随机轮换、加入前/后故障和 15 票门槛演练。
14. 用独立账号演练系统升级及受控迁移，验证撤销执行密钥、普通用户能力和无 BP 治理权限。
15. 固定启动版本全部参数和上线门槛，按 activation_slot 及链上就绪条件启用正式轮换。

核心原则：节点执行确定规则并获得出块/确认收益；系统升级由独立账号负责，未来 AI 通过明确委托的账号权限执行。随机、故障和资源处理可审计、可重放，任何名单切换不能依赖节点治理审批或本地任意决定。

## 16. 核对来源

本文关于原始 EOS 行为和当前工作区职责边界，依据以下源码核对：

- EOSIO `v1.9.2` [`voting.cpp`](https://github.com/EOSIO/eosio.contracts/blob/v1.9.2/contracts/eosio.system/src/voting.cpp)：最多 30 个生产者、代理、`stake2vote()` 时间权重和 top-21 调度；
- EOSIO `v1.9.2` [`delegate_bandwidth.cpp`](https://github.com/EOSIO/eosio.contracts/blob/v1.9.2/contracts/eosio.system/src/delegate_bandwidth.cpp)：CPU/NET 质押变化调用 `update_voting_power()`；
- EOSIO `v1.9.2` [`rex.cpp`](https://github.com/EOSIO/eosio.contracts/blob/v1.9.2/contracts/eosio.system/src/rex.cpp)：REX `vote_stake`、旧 `rentcpu/rentnet` 和贷款生命周期；
- EOSIO `v1.9.2` [`powerup.cpp`](https://github.com/EOSIO/eosio.contracts/blob/v1.9.2/contracts/eosio.system/src/powerup.cpp)：PowerUp 权重、价格、`cfgpowerup` 约束和付费资源路径；
- 当前工作区 [`voting.cpp`](reference-contracts/contracts/eosio.system/src/voting.cpp)：top-21 proposer/finalizer 联动；[`finalizer_key.cpp`](reference-contracts/contracts/eosio.system/src/finalizer_key.cpp)：`switchtosvnn()` 启动条件；
- 当前工作区 [`genesis_state.hpp`](PureEOS/libraries/chain/include/eosio/chain/genesis_state.hpp)：现有 Genesis 只有 timestamp、initial key 和 chain config；[`controller.cpp`](PureEOS/libraries/chain/controller.cpp)：`initialize_blockchain_state()` 当前明确创建 legacy Genesis；[`block_state.cpp`](PureEOS/libraries/chain/block_state.cpp)：现有 Savanna Genesis 转换代码已经展示 finality core、active policies、validation tree 和 aggregating QC 所需状态；
- 当前工作区 [`config.hpp`](PureEOS/libraries/chain/include/eosio/chain/config.hpp)：legacy 125、Savanna proposer/finalizer 65,536 和每人 12 slot；[`transaction_context.cpp`](PureEOS/libraries/chain/transaction_context.cpp)：Savanna proposer 数量校验；[`block_header_state.cpp`](PureEOS/libraries/chain/block_header_state.cpp)：proposer policy 的安全轮次晋升；[`qc.hpp`](PureEOS/libraries/chain/include/eosio/chain/qc.hpp)：BLS 聚合签名、投票位图和 active/pending policy；
- 当前工作区 [`delegate_bandwidth.cpp`](reference-contracts/contracts/eosio.system/src/delegate_bandwidth.cpp)、[`rex.cpp`](reference-contracts/contracts/eosio.system/src/rex.cpp) 和 [`powerup.cpp`](reference-contracts/contracts/eosio.system/src/powerup.cpp)：CPU/NET、REX 和 PowerUp 的票权/资源路径。
- 当前工作区 [`eosio.system.cpp`](reference-contracts/contracts/eosio.system/src/eosio.system.cpp) 与 [`eosio.system.hpp`](reference-contracts/contracts/eosio.system/include/eosio.system/eosio.system.hpp)：64 GiB 初始 `max_ram_size`、以 byte/slot 表示的 `new_ram_per_block`、按 `block_timestamp.slot` 差值结算供应以及现有 `setram/setramrate` 权限路径。

本次节点治理审计另核对：

- [controller.cpp](PureEOS/libraries/chain/controller.cpp)：eosio.prods Genesis 权限和 update_producers_authority；[config.hpp](PureEOS/libraries/chain/include/eosio/chain/config.hpp)：prod.major/prod.minor 权限名；
- [eosio_contract.cpp](PureEOS/libraries/chain/eosio_contract.cpp)、[apply_context.cpp](PureEOS/libraries/chain/apply_context.cpp)：原生账户/代码操作与特权 inline 授权路径；
- [eosio.system.cpp](reference-contracts/contracts/eosio.system/src/eosio.system.cpp)、[rex.cpp](reference-contracts/contracts/eosio.system/src/rex.cpp)、[powerup.cpp](reference-contracts/contracts/eosio.system/src/powerup.cpp)：管理 action 的实际 require_auth、发行时间表和初始化入口；
- [eosio.token.cpp](reference-contracts/contracts/eosio.token/src/eosio.token.cpp)：create、issue、issuefixed、setmaxsupply、retire 的合约/issuer 权限；
- [eosio.bios.cpp](reference-contracts/contracts/eosio.bios/src/eosio.bios.cpp)、[eosio.boot.cpp](reference-contracts/contracts/eosio.boot/src/eosio.boot.cpp)、[producer_plugin.cpp](PureEOS/plugins/producer_plugin/producer_plugin.cpp)：任意启动名单、协议激活及本地 feature API；
- [eosio.bpay.cpp](reference-contracts/contracts/eosio.bpay/src/eosio.bpay.cpp)、[eosio.fees.cpp](reference-contracts/contracts/eosio.fees/src/eosio.fees.cpp)：自动资金流与奖励分配；本地没有发现独立的 eosio.saving 合约源码，不能把其他链上的同名合约行为当成本链已实现；
- [Antelope 官方 eosio.wrap 指南](https://docs.antelope.io/reference-contracts/latest/guides/how-to-use-eosio.wrap/)：典型 eosio→生产者多签授权、部署特权 wrap，以及经其执行账户权限修改的流程；
- [官方 Finalizers and Voting 说明](https://docs.eosnetwork.com/docs/latest/advanced-topics/introduction-finalizers-voting/)：最终确认是持续投票，21 人中 15 人的共识确认不等于治理审批。

2026-07-29 对 EOS 主网 `eosio`、scope `0`、table `powup.state` 的实时查询显示 CPU 与 NET 的 `weight_ratio` 和 `target_weight_ratio` 均为 `10000000000000`，即标称 1%。查询端点可使用 [Greymass EOS 主网 RPC](https://eos.greymass.com/v1/chain/get_table_rows)；该值仅作为历史/现实参照，PureEOS 的目标配置以本文 10% 上界规则为准。
