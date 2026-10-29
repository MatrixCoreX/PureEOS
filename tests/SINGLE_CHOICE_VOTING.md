# PureEOS 唯一选择投票：整数 v2 实验实现

2026-10-07：整数 v2、资源拆分和单席轮岗之上已新增候选扩容与永久禁入，最新规则见文末“轮岗 v2”。**仍不是完整历史计划的生产上线版本**。默认构建不开启，本次未升级之前的手动测试链。旧投票状态和 rotation v1 均不支持直接切换为当前 v2，测试从隔离新链开始。下文带日期的记录保留各阶段实际结果。

## 已实现的边界

- `voteproducer` 只接受空代理和最多一个候选；本人授权，包括 `b1`，不再有系统代其投票的例外。
- `regproxy` 保留 ABI 兼容入口，但登记和注销请求都明确拒绝；内部余额更新检查并拒绝旧代理/多选状态。
- 零质押账号可自付 RAM 预选合法候选，不产生票数；新目标不必已有正票数或已进入出块名单。
- `buyrex` / `unstaketorex` 及公共份额入账 helper 检查份额拥有者已选唯一有效候选。质押转入检查 owner，不误查资源接收者。
- 完整 REX 份额大于零时不能清空选择，涵盖未成熟、储蓄和未成交卖单；可原子改投。全部实际卖清后，可保留个人 fund 并撤票。
- `producersv2`、`votersv2`、`globalv2` 使用最小代币单位的 `int64_t` 票数和分配余额，`votepolicy.version=2`；排序用 `uint128` 复合索引：有效候选整数票数降序，同票账号数值升序。不再以 `stake2vote` 时间倍数或浮点数决定排名。
- 唯一目标按净额扣旧加新，重复投票/刷新不重复加本金；整数加减检查上下溢。CPU/NET 所有权与接收方分开，转入 REX 不重复计票。
- 第三方 `rexexec` 成交立即同步拥有者票数；关闭订单的 `stake_change` 清零，保留待领取款项，拥有者以后结算不再扣第二次。改投前仍执行订单结算。候选注销不阻断正常刷新、赎回和提现；再次买入仍须有效目标。
- CPU/NET-only 的 `voteupdate` 不再因缺少 REX 表行失败。个人 fund 存取、纯租赁不增加投票门槛。
- 治理名单读取整数索引，保留原前 210、资格筛选、冻结名单和 14 天公示规则，不把轮岗扩容自动用于治理。
- `get_producers` 返回 `vote_schema=integer-v2`、精确十进制字符串 `total_producer_vote_amount` 和小数位数 `vote_precision`；各行 `total_votes` 也为字符串，二进制模式对应新 ABI。旧 `total_producer_vote_weight` double 仅为兼容展示，不能用于精确计算。新版本查询错误向调用者报告，不能回退为 active 21 人的零票假排名。
- `get_account` 读取新 voters 表，`staked` / `last_vote_amount` 在 v2 中返回精确十进制字符串。`cleos` 按 ABI 分流新旧命令：v2 禁代理、拒绝多选，`approve` 原子替换选择，`listproducers` 显示精确票数。
- 系统 `claimrewards` 增加 `unpaid_blocks > 0` 门槛；独立 `eosio.bpay` 不再接收触发按排名分发的非系统来款。原系统资金转入及已有 bpay 奖励领取路径保留。**这只是暂时关闭零出块领奖入口，不是最终确认产出账本。**

## 仍未完成，不能据此上线

- REX 全体持有人一致估值/有界刷新尚未实现：当前各人的已结算 Token 等值精确计票，但池收益变化后仍存在个人惰性重估。因此不能把整数表完成说成完整 REX 公平估值已经完成。
- 启用 `PUREEOS_SEQUENTIAL_ROTATION` 后已替换旧前 21 直选，接通单席轮岗、分段线性扩容和三窗口永久禁入。最终确认奖励账本尚未完成；仅启用单选开关不等于同时启用了这些轮岗功能。
- 原有 15% 激活、`unpaid_blocks` 及领取限制仍在；尚未完成离岗/永久禁入后的历史债权独立领取和全链路奖励守恒。沿用原奖励资金及分配参数，不擅自把有产出者的原 per-vote 分配改为全额按块。
- 没有已有链迁移程序。`init` 拒绝旧 voters/producers/global 数据及已有投票/候选/REX 状态；版本标记不匹配时拒绝新投票业务，不把旧 double 二进制重解释为整数。**不要对正式链部署此实验产物，也不要把它视为已完成安全迁移。**

开发者升级的 14 天公示、冻结否决名单及门槛没有被本批次修改。资金流、通胀、租赁期限和资源配额也未调整。

## 独立构建与测试

在 `reference-contracts` 中使用现有 CDT 4.1.1，构建到独立目录；`PUREEOS_SINGLE_CHOICE_VOTING` 要求同时启用既有 `PUREEOS_UPGRADE_GOVERNANCE` 配置：

```bash
cmake -S contracts -B build/pureeos-single-choice \
  -DCMAKE_TOOLCHAIN_FILE=/home/guagua/.local/opt/cdt-4.1.1/usr/opt/cdt/4.1.1/lib/cmake/cdt/CDTWasmToolchain.cmake \
  -DPUREEOS_UPGRADE_GOVERNANCE=ON \
  -DPUREEOS_SINGLE_CHOICE_VOTING=ON
cmake --build build/pureeos-single-choice --target eosio.system eosio.bpay eosio.token rex.results -j 2
```

不要覆盖用于兼容回归的 `build/pureeos-governance`。项目根 CMake 也转发这两个开关；旧合约测试不等于本实验配置的完整验收。

在 `PureEOS` 中复用现有本机构建，指定两套不同 WASM/ABI：

```bash
cmake -S . -B build \
  -DPUREEOS_GOVERNANCE_CONTRACTS_DIR=/home/guagua/AntelopeIO/reference-contracts/build/pureeos-governance \
  -DPUREEOS_SINGLE_CHOICE_CONTRACTS_DIR=/home/guagua/AntelopeIO/reference-contracts/build/pureeos-single-choice
cmake --build build --target native_savanna_test cleos -j 2
build/unittests/native_savanna_test \
  --run_test=pureeos_single_choice_contract_tests --report_level=detailed
```

测试定义在 `PureEOS/unittests/pureeos_governance_tests.cpp` 的独立 suite，执行真实 controller + 系统/bpay WASM 及链查询 API，不改历史嵌入式测试二进制。覆盖授权、多选/代理回滚、零余额预选、REX 入场/持有/退出、部分卖出、挂单/第三方即时成交结算、失效候选退出、所有权、初始化保护，以及超过 `2^53` 的一最小单位差额、同票排序、分页、跨一年刷新、冻结治理名单和零块领奖拒绝。

生成的 Ricardian 说明按构建开关区分新旧规则，并纳入增量编译依赖，避免 ABI 仍展示上一配置的投票规则。

## 本次验证记录

第二批整数 v2：18 个用例、227 项断言全部通过。两套合约（实验开关开启/关闭）的系统、bpay、token 已编译，`native_savanna_test`、`cleos` 构建成功，CLI 帮助冒烟检查通过。最终产物回归命令：

```bash
ctest --test-dir build \
  -R '^native_(savanna_genesis|upgrade_governance|upgrade_governance_contract|single_choice_contract|genesis_blocklog_compat)$' \
  --output-on-failure -j 2
```

2026-10-05 最终结果：5/5 项通过，合计 144 个用例、2,801 项断言（包含上述 18 个用例）；两个源码仓库 `git diff --check` 均通过。这不是全项目测试：尚未完成自动轮岗多进程故障测试，也未复测此前压力测试失败项。CLI 已编译，排名/账户查询经真实链 API 测试，但 CLI 的所有联网命令还未做端到端验收。编译仍有既有的 CDT 版本和缺少部分 Ricardian 说明的警告。

## 真实本地部署测试

在 `PureEOS` 中执行（必须先有上文的实验合约产物）：

```bash
cmake -S . -B build \
  -DPUREEOS_SINGLE_CHOICE_CONTRACTS_DIR=/home/guagua/AntelopeIO/reference-contracts/build/pureeos-single-choice
cmake --build build --target nodeos cleos keosd helios-util -j 2
python3 tests/pureeos_single_choice_local_test.py
# 或使用同一测试的 CTest 入口：
ctest --test-dir build -R '^native_single_choice_nodeos_local$' --output-on-failure
```

此入口生成全新随机测试密钥，启动真实 nodeos 与 keosd，部署 `eosio.token`、`eosio.system`、`eosio.bpay` 和 REX 结果合约。独立开发者 owner/active/upgrade 使用不同密钥；资源按正常 RAM 买入及 EOSIO 自有质押分配，不关闭 CPU/NET、签名或治理校验。

- 节点只监听 `127.0.0.1` 的独立端口，无公网 peer；钱包仅启用私有目录里的 Unix socket，不使用已有钱包。
- 每笔成功交易检查 `executed` 并等待不可逆；负面测试要求错误内容匹配且 CLI 返回非零，不能把 HTTP 成功当作交易成功。
- 检查一票一投、CLI 改投/撤票、REX 持仓限制、CPU/NET 租赁、零块领奖拒绝及开发者公示/节点反对；随后重启和从日志重放，核对票数、REX、治理状态和 chain ID，以及原可逆链头没有被替换、LIB 恢复增长。
- 测试结束停止自己创建的进程，保留 `build/local-tests/integer-v2-*/report.json`、日志、创世与链数据，不删除或覆盖原有目录。目录权限为 700，密钥配置及钱包文件仅本人可读；这些是一次性测试密钥，不能用于正式链，也不要公开整个测试目录。
- 本测试不等待真实 14 天公示或 30 天到期，不调整系统时钟、缩短合约期限或更改共识规则来假装验收。期限边界由前述可控区块时间的 WASM 测试覆盖，实时部署检查期限字段与提前执行拒绝。
- 未达到旧投票激活门槛，因此实际生产者仍为创世单节点；注册两个候选不等于已经运行两节点共识，更不代表 210 候选自动轮岗完成。自动轮岗、增长、永久禁入和最终确认奖励仍按“未完成”记录。

### 2026-10-05 实测与修复

真实本地测试的 30 项检查全部通过，CTest 用时约 124 秒；最终观测 head=245、LIB=243。投票/REX/租赁、受控开发者升级、重启和区块日志重放均在真实 nodeos 中执行；测试节点及钱包已干净退出。报告位于本次机器的 `PureEOS/build/local-tests/integer-v2-yk5e7co_/report.json`，包含合约产物文件哈希、chain ID、检查清单及最终节点状态。

首次执行发现 `cleos push transaction` 对 `send_transaction2` 返回的失败执行 trace 仍以退出码 0 结束。链实际上已拒绝普通用户提案，未出现权限绕过，但自动部署脚本可能误认成功。已在 `programs/cleos/main.cpp` 复用 `send_actions` 的 `get_return_code` 判断，保留完整 JSON 诊断并返回失败状态；新增实测回归验证非零退出码及治理刷新/提案的整体回滚。

同时修正测试脚本的直接 `setcode` 授权：使用 `developer@active` 到达原生升级保护检查，确认即便开发者普通权限正确也不能绕过公示；`upgrade` 子权限直接调用此动作本就会被最低权限检查先行拒绝。没有放宽链权限来让测试通过。

修复后原 5 组 CTest（144 个用例、2,801 项断言）再次全部通过；升级离线工具 3 个用例通过。不能把这些结果扩大为全部轮岗、多节点故障、真实期限到期或全项目压力测试通过。

## 可选资源分配配置：长期资源最多 10%，PowerUp 至少 90%

2026-10-05 新增独立开关 `PUREEOS_RESOURCE_SPLIT=ON`，要求同时启用整数 v2 与开发者治理；默认关闭，使用单独的 `build/pureeos-resource-split`，不覆盖之前的实验产物。此配置只支持**新链**，不是向已有状态直接替换 WASM 的迁移工具。

### 比例的准确含义

CPU 和 NET **分别**统计 `L = 直接质押权重 + 尚未回收的 REX 出租权重`，PowerUp 市场总权重为：

```text
P = max(9 × max(L, 1), 尚未回收的 PowerUp 订单权重)
全网有限资源总权重 = L + P
PowerUp 储备账号权重 = P − PowerUp 已出租权重
```

正常情况下 `P=9L`，长期资源占 10%、PowerUp 占 90%。当退质押或 REX 到期使 `9L` 小于现存付费 PowerUp 订单权重时，保留订单，不强制收回用户已租资源；长期占比暂时低于 10%，订单回收后恢复目标。没有长期资源时采用一单位记账下限，避免零分母，此时长期占比是 0%。

这是**资源权重比例，不是每个区块的 CPU 微秒或 NET 字节硬分区**；未出租的 PowerUp 权重留在储备账号，也计入总权重。系统无限资源账号、底层弹性限制和拥堵计量仍遵循原生规则。原生权重更新在区块提交时生效；测试等待提交后对账。不承诺每份既有租约拥有不变的物理算力百分比：订单保存固定权重，全网总权重仍可能变化。

`powup.state` 的 `weight_ratio` 表示固定 10% 目标，不能在付费订单保护期间拿它冒充实际比例；实际比例要用 `respolicy` 中的 L 和 `powup.state` 中的 P 计算。

### 改动边界

- `eosio.system.hpp`、`eosio.system.cpp`：新增 `respolicy` 版本 1 总账；`init` 初始化；拒绝任意手工 CPU/NET 权重覆盖；`govinit` 要求 PowerUp 已配置，失败时连同原生封存步骤整体回滚。
- `resource_split.cpp`：以受检整数增减总账、同步市场和储备；每次 O(1) 更新，不扫描全体资源持有人。
- `delegate_bandwidth.cpp`：普通质押、退质押及 unvest 都经公共入口维护长期权重，不改变代币所有权和票数算法。
- `rex.cpp`：租入、续租差额、到期回收、质押转 REX 同步维护总账；买入 REX 本身不计入资源权重，只有实际出租部分计入。
- `powerup.cpp`：按实际长期权重动态调节市场，保留现有订单，限定 10% 目标；容量检查用减法避免加法溢出。PowerUp 不增加投票权，不允许储备账号作为租用目标。
- 编译配置和测试启动流程：创建 `eosio.reserv`、部署原有 `powup.results`、购买足够 RAM、配置市场后再 `govinit`。**未新增修改 nodeos 资源调度、P2P 或共识逻辑**。

REX 30 天租期、续租及收费路径不变；PowerUp 价格、租期仍由 `cfgpowerup` 在启动时配置。此次本地配置沿用 30 天，不隐含改为 1 天；WASM 测试会用可配置 1 天订单验证清理分支，REX 到期则实际推进测试链区块时间超过 30 天。PowerUp 费用仍先到 `eosio.fees`，不擅自修改后续分配。RAM 买卖、增发、节点收益模型和治理公示期不改。

到期仍采用原有的有界队列清理：`rexexec`、`powerupexec` 及对应交易触发处理，不是按现实钟表到点便无交易自动清零。用户退掉全部资源后，需要其他账号代买 PowerUp 或交易资源代付才能继续操作；不为退款动作绕过原生 CPU/NET 限制。

### 构建与复现

在 `reference-contracts` 目录执行（CDT 路径按环境调整）：

```bash
cmake -S contracts -B build/pureeos-resource-split \
  -DCMAKE_TOOLCHAIN_FILE=/home/guagua/.local/opt/cdt-4.1.1/usr/opt/cdt/4.1.1/lib/cmake/cdt/CDTWasmToolchain.cmake \
  -DPUREEOS_UPGRADE_GOVERNANCE=ON -DPUREEOS_SINGLE_CHOICE_VOTING=ON -DPUREEOS_RESOURCE_SPLIT=ON
cmake --build build/pureeos-resource-split --target eosio.system eosio.token eosio.bpay rex.results powup.results -j 2
```

在 `PureEOS` 目录执行：

```bash
cmake -S . -B build \
  -DPUREEOS_GOVERNANCE_CONTRACTS_DIR=/home/guagua/AntelopeIO/reference-contracts/build/pureeos-governance \
  -DPUREEOS_RESOURCE_SPLIT_CONTRACTS_DIR=/home/guagua/AntelopeIO/reference-contracts/build/pureeos-resource-split
cmake --build build --target native_savanna_test -j 2
ctest --test-dir build -R '^native_resource_split_contract$' --output-on-failure
ctest --test-dir build -R '^native_resource_split_nodeos_local$' --output-on-failure
```

实时部署入口仍是 `tests/pureeos_single_choice_local_test.py`，需要同时传 `--resource-split` 和新合约目录；不传时保留旧的无 PowerUp 测试配置。新测试使用原 nodeos 二进制、正常资源校验、全新私有链目录，执行成功交易等待最终确认，并在重启/重放前后对账长期总账、PowerUp 状态和订单、储备、REX 贷款及原投票/治理状态。不会将之前已封存但缺少储备账号的测试链强行改成此配置。

### 本轮验收结果（2026-10-05）

- 资源配置、原整数 v2 配置、原治理配置的系统合约均重新编译成功；保留原有 CDT/Ricardian 警告，不宣称零警告。
- 新增 8 个真实 WASM 用例、447 项断言全部通过：启动封存前置条件、固定比例及手工覆盖拒绝、质押所有权及 PowerUp 不增票、退质押时保留付费订单、按 `max=1` 分批到期且重复执行不二次扣减、REX 续租和到期、真实 15% 激活后的退质押/退款/零长期资源、付款失败与大整数越界整体回滚。
- 六组 CTest 合计 **152 个用例、3,248 项断言**通过，包括原有 144 个用例；升级辅助工具 3 个用例通过，Python 语法及两个仓库 `git diff --check` 通过。
- 最终隔离 nodeos 测试 **51 项检查全部通过**，约 148 秒，head=292、LIB=290；正常重启和区块日志重放后账本、订单、储备、贷款及底层权重保持一致。报告：`PureEOS/build/local-tests/integer-v2-v75xu750/report.json`。系统 WASM 文件 SHA-256 为 `ea987da5f914a0230cea58fc28399a6c85137f5d527a07f6c3243922c907225a`。测试进程均停止，链数据保留。

首轮部署发现储备账号部署合约后仅有默认 1,400 字节 RAM、不够执行市场初始化，已修复启动脚本为储备账号购买 RAM。测试还修正了 PowerUp 到期时间为 `time_point_sec`（按秒取整，不能与半秒区块时间直接要求完全相等）的断言，以及模拟跳时前要先提交短有效期交易的测试准备问题；未修改租期或绕过资源检查。

这些测试不是全部项目压力测试，也不覆盖旧链状态迁移、eos-vm-oc、多节点故障或长期经济攻击仿真；正式价格/租期参数仍需单独评估。默认构建没有自动启用此配置，之前链的数据和部署没有被升级。

## 单席顺序轮换配置（2026-10-06）

独立开关 `PUREEOS_SEQUENTIAL_ROTATION=ON`，依赖整数 v2 / 开发者治理。此批只实现**初始 210 候选的顺序单席轮换**，不代表此前综合计划全部完成；四年分段线性扩容、三轮永久黑名单、最终确认奖励账本和统一 REX 估值仍不在本批实现内。

### 运行规则

- 正票数、有效注册和 BLS 密钥的前 210 名入围，同票按账号数值升序。首次排队按排名，后续保留在围者的位置，新入围者加到队尾；不能通过涨票反复插队。
- 原生 `eosio` 启动席位不保留特权：有合格候选后先交接给第一人，再逐人增长到 21；不足 21 时一次最多增加一人，满 21 时按入场先后每次退出一人、补入一人。出围成员优先被单席替换；没有替代者不提交空名单。
- 不检查服务器电源，不设现任证明、在线预检或投票批准。合格但离线者可能真正进入名单。
- 每次冻结目标账号、签名授权和 BLS 密钥；只有原生实际 proposer/finalizer 的版本、内容摘要及成员均匹配，且没有在途策略，才进入观察。注册新密钥不能覆盖已提交的交接，留到下一次统一提交；无成员变化时独立更新密钥，有轮岗时合入同一份目标策略，不能靠连续换密钥抢占优先级、阻止自己退出。运营者须在实际生效前保留所需旧、新签名器。
- 观察从生效后的下一个完整排班轮次开始，长度为 `人数 × 12 slot`；新人在自己的完整窗口至少产生一个合法块，且该轮范围已由执行链的 finality core 最终确认，才完成观察。不要求现任全员出块或全票确认，避免额外赋予任一旧成员阻止轮换的权力。新人未满足时等待并重新观察，不继续积累陌生成员。
- 交接观察完成后至少间隔 `252 slot`（126 秒）再尝试下次轮换；不是每 126 秒强制换人，真实交接和观察会增加时间。物理出块排班仍按账号排序，每人连续 12 slot，而不是按票数排序。
- 新表 `eosio::rotation` 保存队列、入场顺序、目标、阶段、观察证据和完成次数，随正常数据库回滚、重启和重放。`stage=0/1/2/3` 分别为空闲、等待双策略生效、观察、同成员密钥更新。`stalled_windows` 只是未完成观察的诊断计数，**不是永久黑名单**。
- 轮换独立于旧 15% 质押激活门槛，在系统初始化且 `govinit` 封存后开始；此次不改变旧奖励发行/分配或领取激活条件，也不改变资源 10/90、租赁、14 天公示与节点有限否决规则。

### 原生接口与部署边界

增加协议特性 `PUREEOS_ROTATION_CONTEXT`（依赖 Savanna），激活时才开放只读 `get_rotation_context_v1`。返回执行中的父块及其固有 finality core / 实际策略，不读取本地投票进程的最新 LIB、墙上时间或外部网络状态。保留旧创世特性清单，不把新 intrinsic 自动塞入已有链的创世状态。

**需要新 nodeos 和新合约配套；不能只把 WASM 覆盖到旧手动测试链。** 目前只支持新链初始化：在治理封存前，用 `eosio.bios::activate` 激活该特性，再部署该配置系统合约并 `init`，随后正常注册/投票和 `govinit`。原测试脚本尚不能直接以这个 WASM 替换 resource-split 路径，需补特性激活步骤。已有链的轮岗状态迁移未实现。

系统升级本身仍遵守开发者治理；此配置没有新增让节点提交或批准名单的管理 action。2/3 人规模缺确认可能阻塞策略交接；未实施黑名单前，观察中的离线节点若一直不恢复，后续轮换会一直暂停，不能宣称自动跳过或故障无影响。

### 构建与测试入口

在 `reference-contracts`：

```bash
cmake -S contracts -B build/pureeos-rotation \
  -DCMAKE_TOOLCHAIN_FILE=/home/guagua/.local/opt/cdt-4.1.1/usr/opt/cdt/4.1.1/lib/cmake/cdt/CDTWasmToolchain.cmake \
  -DPUREEOS_UPGRADE_GOVERNANCE=ON -DPUREEOS_SINGLE_CHOICE_VOTING=ON \
  -DPUREEOS_RESOURCE_SPLIT=ON -DPUREEOS_SEQUENTIAL_ROTATION=ON
cmake --build build/pureeos-rotation -j2
```

在 `PureEOS` 的原有测试构建上追加：

```bash
cmake -S . -B build \
  -DPUREEOS_ROTATION_CONTRACTS_DIR=/home/guagua/AntelopeIO/reference-contracts/build/pureeos-rotation
cmake --build build --target nodeos -j2
cmake --build build --target native_savanna_test -j2
ctest --test-dir build -R '^native_sequential_rotation_contract$' --output-on-failure -V
```

集成用例加载真实 WASM、正常签名/BLS/QC 和真实名单晋升，不通过 `setprods` 改名单。当前执行环境禁止创建本地 socket，因此使用单个进程内的本地签名器模拟节点，另起独立 controller 做无密钥重放验证；不等于启动了 23 个独立 nodeos / P2P 进程。完整轮次样本按每时隙出块验证，长周期队列用例使用链上测试时钟跨过空闲/等待间隔，但新人的产出和最终确认仍必须真实发生；未修改生产合约的观察/间隔常量。

本批尚未完成大量失效注册记录下的排名扫描预算、TPS/CPU 性能压力测试和 eos-vm-oc 运行验收。入围名单最多 210 人不等于索引遍历最多 210 条；这些后续性能边界不能用本次功能测试代替。

### 本批验证结果

最终 7 组 CTest 全部通过：原有 152 个用例 / 3,248 项断言，加新增 5 个用例 / 21,059 项断言，合计 **157 个用例 / 24,307 项断言**。这不是整个项目所有测试；此前旧直选配置的 3 个委员会边界用例未列入本次最终命令。

新增覆盖：原生启动交接与 1→21 逐人增长；23 候选连续 25 次 FIFO 单席替换；第 23 名涨为最高票也不插队；在途交接重启保持队列；一名旧成员不出块不获得额外否决权；离线第 22 名直接入场、20 人仍最终确认、新人恢复产出后自动继续；无替代者保留非空名单；交接途中 BLS 更新串行处理；2,049 把出块公钥的合法授权超过 64KB 时仍能完成策略更新。

完整连续排班样本验证 21 人每人 12 块、顺序匹配且已最终确认。最终长周期用例由无私钥控制器重放 1,040 块，密钥/大授权用例分别重放 35 / 24 块，head、LIB 和轮岗状态一致。资源 10/90 比例保持，未更改奖励或治理经济规则。配套 nodeos 与全部新配置合约构建成功，保留既有编译警告。

队列模型 Debug（ASan/UBSan）与 Release 各 23 用例 / 10,747 断言通过，覆盖 210 人完整循环。当前环境的 LeakSanitizer 因 ptrace 限制不能运行，Debug 重跑仅设 `ASAN_OPTIONS=detect_leaks=0`，不宣称完成泄漏检查。

首轮发现并修复测试夹具的临时对象生命周期、重启前必须提交隐式 onblock 状态的问题；另修正 Savanna 的名单提交返回值是接收标记而非实际版本，以及去掉会卡住合法大授权的任意 64KB 上下文上限。生产代码用实际策略版本和摘要确认交接，不把提交成功当生效。

## 轮岗 v2：动态候选与永久禁入（2026-10-07）

本节替代上一阶段“固定 210、故障新人只能等待恢复”的限制。开关和新原生宿主不变，仍只支持隔离新链；**不能把旧 `rotation.version=1` 直接解释成 v2**，也没有部署到此前手动链。

- `rotation_growth.hpp`：规则初始化链上 slot 起算，固定 1461 天为四年周期，`210×2^e + floor(210×2^e×r/P)`；容量整数逐名增加，4/8/12 年对应 420/840/1680。治理上限仍 210，实际共识仍最多 21。
- `rotcands`：候选自付 RAM 的固定大小持久行，保存 live/previous 排名、快照代次、队列 ticket、窗口计数和永久禁入证据。四个索引为 `byrank/byprevious/byticket/bydirty`；注册/投票/注销/BLS 更新维护索引，注销不删除此行。
- `rotation` v2：去掉无限增长候选 vector；每次 `onblock` 最多处理 16 个排名扫描或副本同步项、16 个队列项。排名构建冻结容量和 `previous_rank` 索引，期间改票只改变 live 值；dirty 项有界同步完再开始下批，避免高频改票持续制造游标前新键。完整快照才可用于选择；新名额/扫描期间的票数变化在下一快照应用，不重置在围者的队列次序。
- 实际 21 人且 proposer/finalizer 映射一致才开始完整本人 12-slot 窗口观察；窗口整体最终确认后，有产出清零、零产出加一。第三次连续零窗口永久禁止账号再次入场，记录三次 slot/策略版本及最终确认块；不扣资产、不删除历史收益。
- 新旧所有实际成员使用同一观察规则。个人未解决零产出暂停正常轮换；故障新人被禁入后退出阻塞状态，优先单席替换，不把失败入场计入 `completed`。
- 不足 21 不累计；全局出块间隔或确定性最终确认落后超过 1008 slot 时暂停，恢复后重新观察。21→21 交接丢弃不完整窗口，不清已完成证据；改钥不改变个人排班。每块最多结算 8 个窗口，待结算队列上限 192，溢出保护选择暂停而非处罚。
- 注册/改票/改钥不解除禁入。无替代者不清空名单、不撤销当前实际原生签名策略；确认不足也不绕过 Savanna 强制替换。换新账号不属于此账号级禁入可防御的范围。

查询示例（使用自己的节点 URL）：

```bash
cleos -u http://127.0.0.1:8888 get table eosio eosio rotation
cleos -u http://127.0.0.1:8888 get table eosio eosio rotcands --limit 100
```

`active` 按任职先后，不是原生按账号排序的物理出块顺序；候选队列需按 `ticket` 排序并以完整发布快照为准。`ranking=true` 表示仍在分批构建，不能把半完成的 `seen_epoch` 当完整新名单。旧 `zero_windows` 仅在行的 `penalty_epoch` 与全局一致且未暂停时表示当前连续次数。`banned` 一旦成立不受这个代次重置影响。`blocked` 为 0 正常、1 全局/确认暂停、2 无替代者、3 零产出观察受阻；`stalled_windows` 只是入场观察诊断，不是黑名单次数。

最终 v2 产物独立放在 `build/pureeos-rotation-v2`，不覆盖回归中使用的上一阶段产物。四开关构建后指定对应测试目录：

```bash
cmake -S contracts -B build/pureeos-rotation-v2 \
  -DCMAKE_TOOLCHAIN_FILE=/home/guagua/.local/opt/cdt-4.1.1/usr/opt/cdt/4.1.1/lib/cmake/cdt/CDTWasmToolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DPUREEOS_UPGRADE_GOVERNANCE=ON -DPUREEOS_SINGLE_CHOICE_VOTING=ON \
  -DPUREEOS_RESOURCE_SPLIT=ON -DPUREEOS_SEQUENTIAL_ROTATION=ON
cmake --build build/pureeos-rotation-v2 -j 2
# 以下在 PureEOS 目录：
cmake -S . -B build -DPUREEOS_ROTATION_CONTRACTS_DIR=/home/guagua/AntelopeIO/reference-contracts/build/pureeos-rotation-v2
cmake --build build --target native_savanna_test -j 2
ctest --test-dir build -R '^native_sequential_rotation_contract$' --output-on-failure
```

新增覆盖：三次最终确认空窗口与自动单席替换、防注销/重注册/换钥绕过、两次后恢复清零、全局停顿/少确认不批量误罚、20 人长期漏块不处罚、单个名额边界和 4/8/12 年实际链时间、211 候选分页时改票/重启及重放一致。最终本轮结果以 `PureEOS/build/local-tests/rotation-growth-ban-sjff3S/summary.json` 为准；未完成的回归不能计入通过数。

历史 standalone `rotation.hpp/.cpp` 模型和容量函数测试可用 Release、ASan/UBSan 运行，但旧模型中的 `preflight` 等状态不是生产 v2 的在线预检机制；生产分页索引和禁入以实际 WASM 测试为证据。当前环境禁止 socket，多签名器/QC/独立重放不冒称多个独立 nodeos 的 P2P 验收。

最终结果：轮岗 v2 的 16 个用例、102,386 项断言通过；加原有七组回归合计 **171 个用例、113,611 项断言**。25 个模型/增长用例在 Release 与 ASan/UBSan 下各通过 14,923 项断言；升级工具 3 个用例通过。新增的 16 个 live 排名同时移动测试、黑名单快照恢复均通过；最终长周期独立重放 2,360 块一致。nodeos/cleos 重建、版本命令及两仓库 `git diff --check` 通过。编译保留既有警告，LeakSanitizer、真实 P2P、OC、长期压力和旧链迁移不在通过范围内。

报告：[summary.json](../../PureEOS/build/local-tests/sequential-rotation-7EJ3NT/summary.json)、[完整日志](../../PureEOS/build/local-tests/sequential-rotation-7EJ3NT/final.log)、[JUnit](../../PureEOS/build/local-tests/sequential-rotation-7EJ3NT/final.xml)。早期失败和中间复测日志保留在同一目录。
