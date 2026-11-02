> **PureEOS TEST 1.0.0-test.1 — 4 vCPU / 8 GB-class test-only prerelease. 独立测试版。**
> 使用、参数对照和隔离目录说明见 [TEST guide](docs/TEST_LOW_COST.md)。下方正式版要求不适用于带 PUREEOS_TEST_BUILD 的测试二进制。

# PureEOS 1.0.0

[简体中文](#简体中文) · [English](#english)

## 简体中文

### 1. 认识 PureEOS

PureEOS 是一套以 AIC 为默认代币、支持并行处理独立交易的区块链软件。它把区块生产、BP 轮岗和最终确认放在可查询的链上规则中，方便节点运营者判断自己何时在岗、何时可以维护，以及哪些区块已经不可逆。

共识协议名为 **TITAN（泰坦共识协议）**。它在 DPoS 基础上结合单选投票、确定性轮岗、BLS 最终确认、维护交接和基于已最终确认证据的故障处理。独立交易可以并行执行，存在依赖或冲突的交易保留安全回退，状态仍按区块中的交易顺序提交。

本 README 介绍 **1.0.0 当前发行**的配置和使用方法。完整规则见[基线说明](docs/BASELINE.md)，可供程序核对的参数见[基线清单](config/BASELINE.json)和[链参数预设](config/chain-parameters.json)。

### 2. 这一版包含什么

| 项目 | 用途 | 发行 |
| --- | --- | --- |
| PureEOS | 节点、命令行工具和配套系统合约 | [v1.0.0](https://github.com/MatrixCoreX/PureEOS/releases/tag/v1.0.0) |
| PureEOS CDT | C/C++ 合约编译器、ABI 工具和 SDK | [pureeos-v1.0.0](https://github.com/MatrixCoreX/PureEOS-CDT/releases/tag/pureeos-v1.0.0) |
| PureHistory | SHIP 历史索引和查询 API，兼容旧 EOS | [v1.0.0](https://github.com/MatrixCoreX/PureHistory/releases/tag/v1.0.0) |
| PureEOS Explorer | 只读区块浏览器 | [v1.0.0](https://github.com/MatrixCoreX/PureEOS-Explorer/releases/tag/v1.0.0) |

本版包括 **7 天升级审查**、**永久黑名单不占候选名额并由后续合格者递补**，以及审查期内从有效否决票和选民人数中同时剔除永久封禁者的规则。配套系统合约的源码、WASM、ABI 和摘要一起提供，安装时应作为同一套发行核验。

PureEOS CDT 沿用已验证的编译器和运行库；部分工具输出及 CMake 名称保留 AIC/CDT 兼容标识，安装与编译方法见其 README。

### 3. 运行基线

| 项目 | 当前配置 |
| --- | --- |
| CPU 预算 | **CPU16：3,200,000 μs/块** |
| NET 预算 | **4 MiB/块：4,194,304 字节** |
| 本地状态库 | **16 GiB：`chain-state-db-size-mb=16384`** |
| 单交易上限 | CPU 150,000 μs；NET 512 KiB；最低 CPU 计费 100 μs |
| 区块时间槽 | 0.5 秒 |
| 执行方式 | 强制并行；8 workers / batch 24 / early-adoption group 8 |
| 链线程 | `chain-threads=6` |
| P2P 消息上限 | 64 MiB/消息 |
| 在途交易保护 | 每连接 100 MiB |
| 生产交易队列 | 1 GiB |
| RAM 市场 | 初始 64 GiB；`setramrate(1024)` |
| 资源模式 | 10% 长期质押 / 90% PowerUp |
| 投票 | 精确整数、单选、直接投票；无代理、无时间加权 |
| 治理 | 开发者提案、BP 否决；新提案完整审查 7 天 |
| 延迟交易 | `DISABLE_DEFERRED_TRXS_STAGE_1/2` 均保持激活 |

CPU16 是链上 CPU 计费预算的倍率，不是 worker 数量，也不表示节点必须占用 16 个物理核心。更换更强的机器不会自动提高链上的 CPU 或 NET 上限。

16 GiB 指本地当前状态库的容量，用来保存账户、权限、余额和合约表，并非启动时就占满 16 GiB 内存。链上的 64 GiB RAM 市场、账户可用资源和本机物理内存是不同概念。完整计费和折扣字段以链参数预设为准；配置文件需要通过正常链上初始化或授权更新才会生效。

### 4. 最高实测 TPS 与测试设备

在当前 **CPU16 / NET4MiB / 状态库16GiB** 组合下，目前保留且完成逐笔审计的最高固定窗口入块记录是 **36,892.67 TPS**。下面同时列出不可逆和整批口径，便于判断这个数字是怎样得到的。

| 测试条件 | 实际记录 |
| --- | --- |
| 设备 | **Vultr AMD EPYC 4345P 裸金属服务器** |
| CPU | **8 物理核心 / 16 硬件线程** |
| 内存 | **标称 64 GiB**；系统可见 64,880,889,856 字节 |
| 系统 | Ubuntu 24.04.5 LTS 主机，使用配套打包运行时 |
| 测试方式 | 单节点出块；投送程序与节点在同一台机器 |
| 参数 | CPU16 / NET4MiB / 状态库16GiB；8 workers / batch24 / group8 |
| 负载 | 纯 AIC 转账；81,920 对账户轮转；8 个投送连接 |
| 投送目标 | 80,000 TPS，仅表示发送端目标速率 |
| 窗口 | 1 秒预热；事前固定 12 秒计时；90 秒排空 |
| 历史服务 | **测量时出块节点 SHIP 关闭，历史节点、PureHistory 及全部历史后端停止** |
| 校验 | 保留实际验签、授权、计费与冲突回退；逐笔原生账务和 blocklog 审计通过 |

| 测量口径 | 结果 |
| --- | --- |
| 固定 12 秒窗口入块 TPS | **36,892.67** |
| 固定 12 秒窗口不可逆 TPS | **35,810.17** |
| 整批语料入块 TPS | 38,083.13 |
| 整批语料不可逆 TPS | 37,804.93 |
| 发送 / 最终确认 | 1,040,000 / 1,040,000 笔 |
| 排空后未确认 | 0 笔 |

账号准备、RAM、自己质押、自己付费并授权的 PowerUp、合约初始化和签名都在计时前完成；节点在计时中仍实际校验交易。固定窗口和整批处理使用不同分母，不能混成一个数字；80,000 TPS 也不是实际入块结果。

这组数据来自 2026-10-31 的已归档测量：节点源码 `00443330a0d29673700a4eebee78de3b0234c281`，nodeos SHA256 `e7776703f09bec15e8476279e76075ac8e2af3cd94e44ece16c3607859ba2f32`，系统 WASM 为 `6ed4cccc3aac05231f95e63b5464325315668bea2997ddb15de0b29768bf3a62`。完整条件见[性能与节点分工记录](docs/network/CPU16_NET4_SHIP_ROLES_20261031.md)。本次 1.0.0 的治理和黑名单更新完成了功能验收，未为此次发布另跑 TPS 压测。

历史服务恢复后会与出块节点共享这台机器的 CPU、内存和磁盘，因此本表不代表历史服务同时运行时的持续容量，也不等于多机共识吞吐。浏览器的“链上最高 TPS”按当前 Chain ID 的不可逆区块统计，与这里的压测窗口分别记录。

### 5. 设备要求与 Ubuntu 支持

运行 nodeos 至少需要 **8 个物理核心、16 个逻辑线程和 64 GB 系统可见物理内存**。启动时会记录检测值；低于门槛或显式设置为纯串行运行时会拒绝启动。合约编译工具不受 nodeos 的这项硬件门槛限制。

服务器优先支持 **Ubuntu 24.04 LTS 和 26.04 LTS**。[兼容性报告](docs/ubuntu/COMPATIBILITY_20261030.md)列出各环境的原生构建、运行和 CDT 合约编译范围，也保留了 26.10 的检查。原生系统构建、独立虚拟机和打包运行时分别记录；复用同一内核的用户态环境不算另一种原生系统测试。

已有兼容性证据与本次发行包检查各自绑定源码和摘要；PureHistory 的 Node.js/容器依赖则以它自己的 README 为准。

### 6. 下载并安装运行包

从[1.0.0发行页](https://github.com/MatrixCoreX/PureEOS/releases/tag/v1.0.0)下载运行包与校验清单。在已安装并登录 GitHub CLI 的环境中，可以用下面的命令安装到新目录：

```bash
mkdir pureeos-download
cd pureeos-download
gh release download v1.0.0 --repo MatrixCoreX/PureEOS \
  --pattern 'pureeos-1.0.0-linux-x86_64.tar.gz' --pattern 'SHA256SUMS'
sha256sum -c SHA256SUMS --ignore-missing
mkdir node
tar -xzf pureeos-1.0.0-linux-x86_64.tar.gz -C node
cd node
sha256sum -c SHA256SUMS
./bin/nodeos --full-version
```

第一遍校验下载文件，第二遍校验解压后的包内文件。通过 `bin/` 下的入口启动工具，才能使用随包提供的 loader 和共享库。包内还包括 `cleos`、`keosd`、`helios-util`、配置示例、文档和配套系统合约。

已有节点的替换应结合后面的维护流程，避免在尚未交接职责时直接停掉出块进程。

### 7. 从源码编译

要复现这一版，从固定的 `v1.0.0` 标签获取代码；后续开发从 `main` 开始。源码已包含节点固定依赖的内容，推荐先运行检查入口，再执行构建：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git pkg-config \
  ca-certificates libcurl4-openssl-dev libgmp-dev zlib1g-dev python3 python3-yaml
git clone --branch v1.0.0 https://github.com/MatrixCoreX/PureEOS.git
cd PureEOS
python3 -I -B tools/prepare_vm_vendor.py --check
bash tools/ubuntu-modern-build.sh --check
bash tools/ubuntu-modern-build.sh --build --jobs=2
./build/ubuntu-$(. /etc/os-release; echo "$VERSION_ID")-jit/programs/nodeos/pureeos-ubuntu-nodeos --full-version
```

构建入口使用 `ENABLE_OC=OFF` 的并行 JIT 配置。产物位于 `build/ubuntu-<VERSION_ID>-jit/programs/`，节点启动入口是 `nodeos/pureeos-ubuntu-nodeos`；同目录树下还有 `cleos/cleos`、`keosd/keosd` 和 `helios-util/helios-util`。

`--full-version` 可核对实际编译提交。源码、二进制和合约的精确身份以发行清单为准。系统合约使用[PureEOS CDT](https://github.com/MatrixCoreX/PureEOS-CDT#readme)编译，须保留本版的单选、轮岗、资源、治理、peer 和维护构建模式；配套合约包中有对应源码与说明。

### 8. 启动节点与注册 BP

加入已有网络时，先取得该网络公布的创世文件、Chain ID 和初始连接地址。新建网络则先准备自己的创世公钥与初始化流程。`genesis.template.json` 和 `config.example.ini` 是配置起点，不能代替网络参数的核对。

在解压后的运行包目录中，可以这样准备独立数据目录。启动前先编辑配置中的网络地址和本机所需设置：

```bash
mkdir node-config node-data
cp config/config.example.ini node-config/config.ini
# Edit node-config/config.ini for your network before starting.
./bin/nodeos --config-dir "$PWD/node-config" --data-dir "$PWD/node-data" \
  --genesis-json /path/to/network-genesis.json
```

节点启动后，先核对 Chain ID、链头和不可逆高度：

```bash
curl -sS http://127.0.0.1:8888/v1/chain/get_info \
  -H 'Content-Type: application/json' -d '{}'
```

后来加入的 BP 应遵循“**先启动、同步和检查密钥，再注册参选**”的顺序。注册记录和选票本身不证明节点已经同步，也不证明它能出块或提供有效最终确认票。普通、BP 和测试账户使用 12 位原生账号；协议固定的 `eosio*` 系统账号保留。

完整命令和出块、最终确认、peer 密钥检查见[BP启动与维护指南](docs/network/BP_STARTUP_REGISTRATION.md)。密钥由运营者自行生成和保管，浏览器不收集签名私钥。

### 9. 投票、轮岗与最终确认

用户以自己的权限直接投票，每次最多选择一个 BP。票数采用精确整数，不使用代理或时间加权。票数排名决定候选准入；进入候选后，FIFO 候补和正常交接负责安排轮岗，每次交接一个席位。

候选容量从 210 开始，从链上记录的增长起点计算：首个 1461 天周期逐步增长到 420，下一周期再到 840，具体整数容量按合约公式计算。出块/最终确认的合格稳态名单为 21 人。注册 BP 总数、有效候选数、正在生效的排班和在线进程数分别统计；永久黑名单在容量计数前排除，后续合格候选可以递补。

当前等权最终确认门槛为 `floor(2N/3)+1`，N 取**生效最终确认政策中的成员数**，不取开机数量：

| 生效等权确认者 N | 至少需要的有效确认票 |
| --- | --- |
| 1 | 1 |
| 10 | 7 |
| 20 | 14 |
| 21 | 15 |

因此，少于 21 人并不必然停链，关键是生效政策能否收到足够的有效确认票。注册 30 个 BP、其中 15 个关机，也不能直接把法定人数当成 15：要看当前排班里哪些成员能够签名。候补还没有完成交接时，不能替旧政策凑票。

成员切换期间仍须满足原生旧/新政策的确认条件。若票数不足，新的最终确认会暂停；全链停摆保护负责避免错误归责，不降低这个门槛。

### 10. 维护、缺岗和永久黑名单

计划停机时先申请维护，并确认链上状态已经生效。页面中的三个状态分别表示：

- **正常**：可以按规则进入候选和轮岗。
- **等待维护**：申请已记录，仍在等待正常出块/最终确认职责交接；此时继续运行节点。
- **维护中**：交接已完成，维护正式生效，可以按运维流程停机。恢复后由 BP 账户按权限解除维护，再等待正常准入。

有效维护排除未来轮岗，不再累计新的职责奖励或分配职责的缺岗处罚；此前未领权益和有效违规保留。维护不能清除已有永久封禁。

没有提前进入有效维护而持续缺岗时，健康链上连续三个**已最终确认的个人零出块窗口**会形成永久账户封禁。只对实际分配的职责结算，候补等待不会算作个人缺岗。

全链出块或最终确认观察中断达到 **252 slot，也就是 126 秒，含边界**时，暂停新增个人缺席证据；少于 21 人时也暂停新增个人缺岗处罚。恢复后重新观察完整窗口，避免把全链停摆误记成仍在运行 BP 的个人违规。此前已成立的证据和封禁不会被抹掉。

永久封禁者不再占用新的候选排名名额；高票也不能挤掉下一位合格者。重注册、换钥或加票不会恢复原账户的 BP 资格。新账号属于新的 BP，需要重新准备、注册并获得投票，原账号的票不会自动迁移。

### 11. 7 天升级审查

升级流程采用**指定开发者提案、BP 在审查期内否决、开发者按权限执行**的方式。普通用户和 BP 不能自行发起这种升级提案。

开发者提交目标代码、序列化 ABI 和审查文档摘要后，新提案等待 **604,800 秒，即完整 7 天**。审查期间 BP 可以否决；到期后，满足链上条件且未达到否决门槛时，由指定开发者正常执行。提前执行会被拒绝。

计数以提案冻结的名单为基础，只按当前永久封禁状态剔除成员。有效人数为 N，有效否决数为 V，阻止升级的门槛是 `ceil(2N/3)`；封禁者已经投下的否决也不计入 V。N 为 0 时禁止执行。原始名单与原始否决保留，便于核查；维护、退出注册或票数变化不借此重写冻结权利，新加入者也不会插入旧提案。

这套否决门槛与最终确认的 `floor(2N/3)+1` 是两种用途不同的规则。7 天规则从原生特性启用后的新提案生效，已有提案沿用自己的 `execute_after`。部署二进制或发布 Git 版本不会自动替运行中的链执行合约升级。

可查询节点上的实际审查状态：

```bash
curl -sS http://127.0.0.1:8888/v1/chain/get_upgrade_state \
  -H 'Content-Type: application/json' -d '{}'
```

以返回的特性状态、有效票数、到期时间、实际 WASM/ABI 摘要和不可逆高度判断升级进展。配套发行清单记录本版绑定的合约源码和摘要。

### 12. AIC、奖励和 RAM

新链测试配置使用精度为 4 的 AIC：**初始发行 100,000,000.0000 AIC，最大供应量 2,100,000,000.0000 AIC**。最大供应量不是预先发出的余额，剩余部分不一次性发行，既有额外发行方式保持不变。

出块奖励以当前已发行量为基数，按合约每 **364 天精确 1%** 计提，并每 **1461 天减半**；另外 4% 不发行。只有实际出块累计未领权益，成功领取时才发行并填充奖励池；首次正权重直接投票激活相关计时，领取间隔须严格超过 24 小时。

奖励按实际出块权益结算，票数排名不附加奖励倍率。排名影响候选准入，维护与封禁影响参与资格，两者不应被当成每块奖励的额外倍数。

RAM 市场初始为 64 GiB，通过既有 `setramrate(1024)` 初始化增长速度，并按合约的区块时间槽规则结算。10% 质押/90% PowerUp 模式、本地 16 GiB 状态库和 RAM 市场分别管理不同资源。初始化后应读取链上 `global2.new_ram_per_block`，确认实际值为 1024。

### 13. 历史节点、PureHistory 与浏览器

推荐把职责分开：出块节点保持 SHIP 关闭；非出块历史节点持续同步，开启 `trace-history` 和 `chain-state-history`，供 PureHistory 索引区块、执行轨迹和状态变化。普通区块日志保存区块，不等同于供浏览器检索的交易/账户索引。

历史来源停下后，已经索引的数据仍可查询，新数据则等待来源恢复和追平。PureHistory 读取链上的投票、维护与黑名单结果，不自己决定共识处罚。它按 ABI 区分 PureEOS 整数投票与旧 EOS 数据，旧 EOS 的代理/权重语义保持原样。

| 服务 | 默认入口 |
| --- | --- |
| 浏览器 | https://aic.matrixai.one |
| 节点 HTTPS API | https://aic-api.matrixai.one |
| PureHistory API | https://purehistory-api.matrixai.one |

浏览器只读，展示真实区块、当前排班、有效 BP、维护和黑名单；票数按精确整数拆分为总票权、自投和他投。12 块活动默认展开，链上最高 TPS 来自服务端不可逆记录。前端直接访问已配置的 HTTPS API，连接失败不会改用虚构数据。

当前测试部署把这些服务放在同一台机器，职责分开仍会共享硬件。需要衡量独立历史机或多机吞吐时，应另行设计对应测试，不能直接套用前面的单节点峰值。

### 14. 短号与系统账号

本版继续使用 `eosio`、`eosio.token` 等协议系统账号，也保留原有短号拍卖逻辑。普通 12 位原生账户可按现有规则创建；拍卖获得的短原生账户仍由自己的 owner/active 权限控制。

`bidname` 和 `bidrefund` 接口仍存在。没有出价记录不代表拍卖代码被禁用；成交仍受原规则约束，包括链激活超过 14 天、最后出价超过 24 小时和关拍频率等条件。**这 14 天是拍卖条件，与合约升级的 7 天审查期无关。**

把短号改为“抢注＋租金、到期释放、解析/转让给 12 位原生账户”的方案，以及把系统账号整体改成 `aic*` 的方案，仍在讨论和设计中，尚未纳入本版实现。

### 15. 如何查看验证结果

| 验证项目 | 记录范围 |
| --- | --- |
| 治理与黑名单原生用例 | 93 个不同用例，111 次成功执行 |
| 跨 7 天签名历史场景 | 15,521 个断言；保留正式 604,800 秒规则 |
| 发行节点完整重放 | 完整状态哈希、668 个物理表元数据、四个二级索引一致 |
| 浏览器 | 179 项测试、构建及既有实际公开 Chrome 检查 |
| PureHistory | 35 项模块/兼容检查；本次构建和 18 项兼容复核 |
| 发行包 | 解压后的运行文件摘要、CLI 版本、CDT 实际合约编译 |

[治理验收目录](tests/qualification/governance7-20261101/)与发行附件保留测试源码、摘要、原始失败及针对性复测。一次原生程序中的断言数量、不同业务用例数量和重复执行次数分别统计。

跨 7 天的功能验收使用独立历史创世时间和真实签名区块，再由正式 nodeos 完整检查重放；没有缩短正式审查期或修改主机时钟。它验证期限边界和状态一致性，不代表现实时间已连续运行 7 天。多节点功能/拓扑测试和 TPS 记录也按各自范围保存。

新增代码从当前 `main` 继续开发。提交改动时应同时记录源码、二进制、WASM/ABI、参数和测量范围，保留原失败结果，完成相关验证后再更新发行。

### 16. 许可与第三方声明

许可按组件分别适用。使用、修改或分发时，请一并保留随包的版权、许可和第三方声明，并核对所用组件的实际条款。

项目说明见 [AIC_LICENSE.md](AIC_LICENSE.md)、[LICENSING.md](LICENSING.md)、[LICENSE](LICENSE) 及 [licenses](licenses/)；继承代码和依赖的原有权利及适用的 BSL 生产用途范围，以各自文件为准。

---

## English

### 1. About PureEOS

PureEOS is blockchain software that uses AIC as its default token and processes independent transactions in parallel. Block production, BP rotation and finality follow queryable on-chain rules, helping operators see when they are on duty, when maintenance is safe and which blocks are irreversible.

Its consensus protocol is **TITAN (Titan Consensus Protocol)**. Built on DPoS, it combines single-choice voting, deterministic rotation, BLS finality, maintenance handovers and fault handling based on finalized evidence. Independent transactions can run in parallel; dependent or conflicting transactions retain safe fallbacks, and state commits follow transaction order within the block.

This README describes the **current 1.0.0 release**. See the [baseline guide](docs/BASELINE.md), [machine-readable baseline](config/BASELINE.json) and [chain parameter preset](config/chain-parameters.json) for the complete rules.

### 2. What is included

| Project | Purpose | Release |
| --- | --- | --- |
| PureEOS | Node, command-line tools and matching system contracts | [v1.0.0](https://github.com/MatrixCoreX/PureEOS/releases/tag/v1.0.0) |
| PureEOS CDT | C/C++ contract compiler, ABI tools and SDK | [pureeos-v1.0.0](https://github.com/MatrixCoreX/PureEOS-CDT/releases/tag/pureeos-v1.0.0) |
| PureHistory | SHIP history indexing and APIs, including legacy EOS support | [v1.0.0](https://github.com/MatrixCoreX/PureHistory/releases/tag/v1.0.0) |
| PureEOS Explorer | Read-only block explorer | [v1.0.0](https://github.com/MatrixCoreX/PureEOS-Explorer/releases/tag/v1.0.0) |

This release includes **seven-day upgrade review**, **permanent bans excluded from candidate capacity with eligible successors filling the vacancy**, and the removal of currently banned members from both effective vetoes and electorate counts during review. Matching system-contract source, WASM, ABI and checksums are supplied together and should be verified as one set.

PureEOS CDT retains its qualified compiler and runtime libraries. Some tool output and CMake names keep AIC/CDT compatibility identifiers; its README explains installation and compilation.

### 3. Runtime baseline

| Setting | Current value |
| --- | --- |
| CPU budget | **CPU16: 3,200,000 μs/block** |
| NET budget | **4 MiB/block: 4,194,304 bytes** |
| Local state capacity | **16 GiB: `chain-state-db-size-mb=16384`** |
| Per-transaction limits | CPU 150,000 μs; NET 512 KiB; minimum billed CPU 100 μs |
| Block slot | 0.5 seconds |
| Execution | Parallel required; 8 workers / batch 24 / early-adoption group 8 |
| Chain threads | `chain-threads=6` |
| P2P message limit | 64 MiB/message |
| Transaction inflight protection | 100 MiB per connection |
| Producer transaction queue | 1 GiB |
| RAM market | Initially 64 GiB; `setramrate(1024)` |
| Resource mode | 10% staking / 90% PowerUp |
| Voting | Exact integer, single-choice, direct voting; no proxy or time weighting |
| Governance | Developer proposals, BP vetoes; seven full days for new reviews |
| Deferred transactions | `DISABLE_DEFERRED_TRXS_STAGE_1/2` both remain active |

CPU16 is a multiplier of the chain's billed CPU budget. It is neither a worker count nor a requirement to occupy 16 physical cores. Upgrading the hardware does not automatically raise the chain's CPU or NET limits.

The 16 GiB setting is local capacity for current account, permission, balance and contract-table state; it does not preallocate 16 GiB of physical RAM at startup. The chain's 64 GiB RAM market, account resource availability and host memory are separate concepts. The full preset defines billing and discount fields, and takes effect through normal on-chain initialization or authorized updates.

### 4. Highest measured TPS and test hardware

For the current **CPU16 / NET4MiB / 16GiB state** combination, the highest retained fixed-window inclusion result with a complete per-transaction audit is **36,892.67 TPS**. Irreversible and whole-corpus rates are listed separately so the measurement is clear.

| Test condition | Recorded setup |
| --- | --- |
| Machine | **Vultr AMD EPYC 4345P bare-metal server** |
| CPU | **8 physical cores / 16 hardware threads** |
| Memory | **Advertised 64 GiB**; 64,880,889,856 bytes visible to the OS |
| System | Ubuntu 24.04.5 LTS host with the matching bundled runtime |
| Topology | Single producing node; load generator on the same machine |
| Parameters | CPU16 / NET4MiB / 16GiB state; 8 workers / batch24 / group8 |
| Workload | Pure AIC transfers; 81,920 rotating account pairs; 8 sender connections |
| Submission target | 80,000 TPS, describing the sender target only |
| Timing | 1-second warmup; fixed 12-second measurement; 90-second drain |
| History services | **Producer SHIP off; history node, PureHistory and all history backends stopped during measurement** |
| Validation | Signature, authorization, billing and conflict checks retained; per-transaction native ledger/blocklog audit passed |

| Measurement scope | Result |
| --- | --- |
| Included TPS, fixed 12-second window | **36,892.67** |
| Irreversible TPS, fixed 12-second window | **35,810.17** |
| Included TPS, whole corpus | 38,083.13 |
| Irreversible TPS, whole corpus | 37,804.93 |
| Sent / finalized | 1,040,000 / 1,040,000 transactions |
| Unconfirmed after drain | 0 transactions |

Accounts, RAM, self-owned staking, self-paid/authorized PowerUp, contract initialization and signing were prepared before timing. The node still validated transactions during measurement. Fixed-window and whole-corpus rates have different denominators; the 80,000 TPS submission target is not an inclusion result.

The archived measurement is dated 2026-10-31: node source `00443330a0d29673700a4eebee78de3b0234c281`, nodeos SHA256 `e7776703f09bec15e8476279e76075ac8e2af3cd94e44ece16c3607859ba2f32`, system WASM `6ed4cccc3aac05231f95e63b5464325315668bea2997ddb15de0b29768bf3a62`. See the [performance and node-role record](docs/network/CPU16_NET4_SHIP_ROLES_20261031.md). The governance and blacklist changes in 1.0.0 passed functional acceptance; publication did not add another TPS workload.

Once restored, history services share CPU, memory and disk with the producer. These numbers therefore do not establish sustained throughput with history running or multi-machine consensus throughput. The explorer's lifetime peak is calculated from irreversible blocks for its current Chain ID and is recorded separately from this benchmark window.

### 5. Hardware requirements and Ubuntu support

Running nodeos requires at least **8 physical cores, 16 logical threads and 64 GB of system-visible physical RAM**. Startup records the detected values and rejects insufficient hardware or an explicitly serial-only configuration. This nodeos hardware floor does not apply to the contract compiler.

Server support prioritizes **Ubuntu 24.04 LTS and 26.04 LTS**. The [compatibility report](docs/ubuntu/COMPATIBILITY_20261030.md) records native builds, runtime checks and CDT contract compilation for each environment, including retained 26.10 checks. Native systems, independently booted VMs and bundled runtimes have separate scopes; user-space environments sharing a kernel are not presented as separate native-system tests.

Existing compatibility evidence and current package checks retain their own source/hash bindings. PureHistory's Node.js/container dependencies are documented in its own README.

### 6. Download and install the runtime

Download the runtime and checksums from the [1.0.0 release](https://github.com/MatrixCoreX/PureEOS/releases/tag/v1.0.0). With GitHub CLI installed and authenticated, the following commands install into a new directory:

```bash
mkdir pureeos-download
cd pureeos-download
gh release download v1.0.0 --repo MatrixCoreX/PureEOS \
  --pattern 'pureeos-1.0.0-linux-x86_64.tar.gz' --pattern 'SHA256SUMS'
sha256sum -c SHA256SUMS --ignore-missing
mkdir node
tar -xzf pureeos-1.0.0-linux-x86_64.tar.gz -C node
cd node
sha256sum -c SHA256SUMS
./bin/nodeos --full-version
```

The first check verifies downloaded assets; the second verifies files inside the extracted package. Start tools through `bin/` so they use the bundled loader and shared libraries. The package also includes `cleos`, `keosd`, `helios-util`, example configuration, documentation and matching system contracts.

For an existing BP, use the maintenance process below when replacing its executable so duties are handed over before shutdown.

### 7. Build from source

Use the pinned `v1.0.0` tag to reproduce this release; start subsequent development from `main`. The node source includes pinned dependency contents. Run the preflight checks before building:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git pkg-config \
  ca-certificates libcurl4-openssl-dev libgmp-dev zlib1g-dev python3 python3-yaml
git clone --branch v1.0.0 https://github.com/MatrixCoreX/PureEOS.git
cd PureEOS
python3 -I -B tools/prepare_vm_vendor.py --check
bash tools/ubuntu-modern-build.sh --check
bash tools/ubuntu-modern-build.sh --build --jobs=2
./build/ubuntu-$(. /etc/os-release; echo "$VERSION_ID")-jit/programs/nodeos/pureeos-ubuntu-nodeos --full-version
```

The build entry point uses parallel JIT with `ENABLE_OC=OFF`. Outputs are under `build/ubuntu-<VERSION_ID>-jit/programs/`; the node launcher is `nodeos/pureeos-ubuntu-nodeos`, alongside `cleos/cleos`, `keosd/keosd` and `helios-util/helios-util`.

Use `--full-version` to identify the actual compiled commit and the release manifest to match source, binaries and contracts. Build system contracts with [PureEOS CDT](https://github.com/MatrixCoreX/PureEOS-CDT#readme), retaining this release's single-choice, rotation, resource, governance, peer and maintenance modes. Matching source and build notes are included in the contract package.

### 8. Start a node and register a BP

To join an existing network, obtain its published genesis file, Chain ID and initial peer endpoints. For a new network, prepare your own genesis public key and initialization process. `genesis.template.json` and `config.example.ini` are starting points, not substitutes for verifying network parameters.

From the extracted runtime directory, prepare independent data directories as below. Edit network endpoints and local settings before starting:

```bash
mkdir node-config node-data
cp config/config.example.ini node-config/config.ini
# Edit node-config/config.ini for your network before starting.
./bin/nodeos --config-dir "$PWD/node-config" --data-dir "$PWD/node-data" \
  --genesis-json /path/to/network-genesis.json
```

Once running, verify Chain ID, head and irreversible height:

```bash
curl -sS http://127.0.0.1:8888/v1/chain/get_info \
  -H 'Content-Type: application/json' -d '{}'
```

A joining BP should **start, synchronize and verify its keys before registering**. Registration and votes do not prove synchronization, block production or valid finalizer participation. Ordinary, BP and test identities use 12-character native accounts; protocol-fixed `eosio*` system accounts remain.

See the [BP startup and maintenance guide](docs/network/BP_STARTUP_REGISTRATION.en.md) for commands and producer/finalizer/peer-key readiness checks. Operators generate and retain their own keys; the explorer does not collect signing private keys.

### 9. Voting, rotation and finality

Users vote directly with their own authority for at most one BP. Vote amounts are exact integers, without proxies or time weighting. Vote ranking controls candidate admission; FIFO standbys and normal handovers organize rotation one seat at a time.

Candidate capacity starts at 210 from the recorded on-chain growth origin. It grows gradually to 420 over the first 1461-day period, then to 840 over the next; the contract formula determines the current integer capacity. Qualified steady-state proposer/finalizer rosters have 21 members. Registered BPs, eligible candidates, effective schedules and online processes are counted separately. Permanent bans are excluded before capacity counting, allowing the next eligible candidate to fill the vacancy.

The current equal-weight finality threshold is `floor(2N/3)+1`, with N taken from **effective finalizer-policy membership**, not the number of powered-on machines:

| Effective equal-weight finalizers N | Required valid confirmations |
| --- | --- |
| 1 | 1 |
| 10 | 7 |
| 20 | 14 |
| 21 | 15 |

Having fewer than 21 members therefore does not automatically halt the chain; the effective policy must still receive enough valid confirmations. Registering 30 BPs and switching off 15 does not make the quorum denominator 15: participation within the effective schedule matters. Standbys cannot supply votes for the old policy before completing handover.

Membership transitions retain native old/new policy confirmation requirements. Insufficient votes pause new finality. Global-interruption safeguards prevent false attribution; they do not lower quorum.

### 10. Maintenance, absence and permanent bans

Request maintenance before planned downtime and confirm that it is effective on chain. The three displayed states mean:

- **Normal**: eligible for candidate admission and rotation under the rules.
- **Awaiting maintenance**: the request is recorded, but normal proposer/finalizer handover is pending; keep the node running.
- **In maintenance**: handover is complete and maintenance is effective. The node may be stopped following the operator procedure. After recovery, the BP account clears maintenance with its own authority and waits for normal admission.

Effective maintenance excludes future rotation and new assigned-duty rewards/absence penalties. Earlier unpaid rights and valid violations remain; maintenance cannot erase a permanent ban.

Without effective planned maintenance, three consecutive **finalized personal zero-production windows** on a healthy chain permanently ban the BP account. Only assigned duties are settled; waiting as a standby is not personal absence.

A block-production or finality-observation interruption of **252 slots/126 seconds, inclusive**, pauses new personal absence evidence. A sub-21 committee also pauses new personal absence penalties. Recovery starts fresh complete windows so a global halt is not attributed to an otherwise running BP. Previously valid evidence and bans remain.

Permanent bans no longer occupy new ranked candidate slots, even with high votes. The next eligible candidate can backfill. Re-registration, keys or more votes do not restore the banned account's BP eligibility. A new account is a new BP that must prepare, register and obtain votes; votes do not move automatically.

### 11. Seven-day upgrade review

Upgrades use **designated-developer proposals, BP vetoes during review and authorized developer execution**. Ordinary users and BPs cannot independently submit these upgrade proposals.

After the developer submits target code, serialized ABI and review-document hashes, a new proposal waits **604,800 seconds—seven full days**. BPs may veto during review. After maturity, the designated developer executes normally if chain conditions are satisfied and the veto threshold has not been reached. Early execution is rejected.

Counting starts with the frozen proposal electorate and excludes current permanent bans. Let N be effective membership and V effective vetoes; the blocking threshold is `ceil(2N/3)`. Vetoes already cast by a subsequently banned member are also excluded. N=0 blocks execution. Raw electorate and veto records remain auditable. Maintenance, unregistration and vote changes do not rewrite frozen rights, and new members are not inserted into an existing proposal.

This veto threshold serves a different purpose from the finality threshold `floor(2N/3)+1`. Seven-day review applies to new proposals after native feature activation; existing proposals retain their own `execute_after`. Installing binaries or publishing a Git release does not execute a running chain's contract upgrade.

Query the node for actual review state:

```bash
curl -sS http://127.0.0.1:8888/v1/chain/get_upgrade_state \
  -H 'Content-Type: application/json' -d '{}'
```

Use the returned feature state, effective counts, deadline, actual WASM/ABI hashes and irreversible height to assess progress. The release manifest binds this version to its contract source and hashes.

### 12. AIC, rewards and RAM

The fresh test-chain profile uses AIC with 4 decimals: **100,000,000.0000 AIC initially issued and 2,100,000,000.0000 AIC maximum supply**. Maximum supply is not a pre-issued balance. Remaining headroom is not issued up front, and the existing separate issuance mechanism remains unchanged.

Block rewards accrue at exactly **1% of currently issued supply per 364-day contract year**, halved every **1461 days**. The additional 4% is never issued. Only actual blocks accrue unpaid rights; a successful claim issues tokens and funds the pool. The first positive-weight direct vote starts the relevant clock, and claims must be strictly more than 24 hours apart.

Rewards settle actual production rights. Vote ranking adds no reward multiplier: it affects candidate admission, while maintenance and bans affect participation. Neither is an extra per-block reward factor.

The RAM market begins at 64 GiB. Explicit `setramrate(1024)` initializes its growth rate, settled under the contract's block-slot rules. Staking/PowerUp at 10%/90%, the local 16 GiB state database and the RAM market govern different resources. After initialization, read on-chain `global2.new_ram_per_block` and verify 1024.

### 13. History nodes, PureHistory and the explorer

Separate the roles: keep producer SHIP off, and use a synchronized nonproducing history node with `trace-history` and `chain-state-history` enabled for PureHistory. Ordinary blocklogs retain blocks; they are not the searchable transaction/account index used by the explorer.

If the history source stops, already indexed data remains queryable while new data waits for recovery and catch-up. PureHistory reads on-chain voting, maintenance and blacklist decisions rather than imposing consensus penalties. ABI detection preserves the distinction between PureEOS integer voting and legacy EOS proxy/weight semantics.

| Service | Default endpoint |
| --- | --- |
| Explorer | https://aic.matrixai.one |
| Node HTTPS API | https://aic-api.matrixai.one |
| PureHistory API | https://purehistory-api.matrixai.one |

The explorer is read-only and shows real blocks, effective schedules, eligible BPs, maintenance and bans. Exact integer votes are split into total/self/other values. Its 12-block activity view opens by default, and lifetime peak TPS comes from server-owned irreversible records. The static frontend queries configured HTTPS APIs directly and does not substitute invented data after failure.

The current test deployment places these roles on the same machine, so they still share hardware. Dedicated-history-host or multi-machine throughput requires its own test scope rather than reuse of the single-node peak above.

### 14. Short names and system accounts

This release retains protocol system accounts such as `eosio` and `eosio.token`, along with existing short-name auctions. Ordinary 12-character native accounts follow the existing creation rules; short native accounts obtained through auction still have their own owner/active permissions.

`bidname` and `bidrefund` remain available. An empty bid table does not mean auction code is disabled. Closing follows existing conditions, including more than 14 days since chain activation, more than 24 hours since the last bid and the closing-frequency rule. **Those 14 days are an auction condition, separate from seven-day contract upgrade review.**

First-come rental aliases with expiry/re-registration and binding/transfer to 12-character native accounts remain a design under discussion. Renaming system accounts to an `aic*` namespace is also a plan, not an implementation in this release.

### 15. How to read the validation evidence

| Validation | Recorded scope |
| --- | --- |
| Native governance/blacklist cases | 93 distinct cases, 111 successful executions |
| Historical signed-chain seven-day scenario | 15,521 assertions; real 604,800-second rule retained |
| Full release-node replay | Complete state hash, 668 physical table metadata entries and four secondary indexes agree |
| Explorer | 179 tests, build and retained real public Chrome checks |
| PureHistory | 35 module/compatibility checks; release build and 18 compatibility reruns |
| Distribution packages | Extracted runtime hashes, CLI versions and actual CDT contract compilation |

The [governance qualification directory](tests/qualification/governance7-20261101/) and release assets retain source, hashes, original failures and targeted reruns. Assertions within one native program, distinct business cases and repeated executions are counted separately.

Seven-day functional acceptance uses an isolated historical genesis and real signed blocks followed by full-check production-nodeos replay. It does not shorten the review rule or alter the host clock. It verifies deadline boundaries and state consistency, not seven days of continuous wall-clock operation. Multi-node functional/topology evidence and TPS measurements likewise retain their own scope.

Continue new development from current `main`. Bind changes to source, binaries, WASM/ABI, parameters and measurement scope; preserve original failures and complete relevant validation before updating the release.

### 16. Licenses and third-party notices

Licenses apply per component. Preserve the accompanying copyright, license and third-party notices when using, modifying or distributing the software, and consult the terms of the components you use.

See [AIC_LICENSE.md](AIC_LICENSE.md), [LICENSING.md](LICENSING.md), [LICENSE](LICENSE) and [licenses](licenses/). Inherited code/dependency rights and any applicable BSL production-use scope remain governed by their respective files.
