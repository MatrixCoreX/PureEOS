# PureEOS

[简体中文](#简体中文) · [English](#english)

## 简体中文

PureEOS 是采用原生 Savanna 最终确认的 C++ 区块链项目。独立交易可并行执行，状态按区块交易顺序提交。智能合约工具链为 **PureEOS CDT**。

### 获取和运行

[节点发行](https://github.com/MatrixCoreX/PureEOS/releases/tag/baseline-2026.10.30)提供运行包、PureEOS CDT SDK 和 SHA256SUMS。当前源码从 `main` 获取；新链使用[精确 1% 系统合约发行](https://github.com/MatrixCoreX/PureEOS/releases/tag/system-contracts-2026.10.31-exact1pct)，其中包含配套源码、WASM、ABI 及校验清单。

下载节点运行包和配套摘要，先校验，再解压到独立目录：

```bash
sha256sum -c SHA256SUMS --ignore-missing
mkdir pureeos
tar -xzf pureeos-baseline-2026.10.30-linux-x86_64.tar.gz -C pureeos
cd pureeos
sha256sum -c SHA256SUMS
./bin/nodeos --full-version
```

通过包内 `bin/` 启动工具，使用匹配的 loader 和共享库。按 [配置说明](config/README.txt)准备网络、创世配置和自有密钥；完整链参数见 [chain-parameters.json](config/chain-parameters.json)。PureEOS CDT 的安装及合约编译见 [SDK 说明](https://github.com/MatrixCoreX/PureEOS-CDT#readme)。

### 运行要求和 Ubuntu 支持

- x86_64，最低 **8 个物理核心、16 个逻辑线程、64 GB 系统可见物理内存**。节点启动时检查并打印检测值，拒绝低于要求或显式串行配置。
- 节点必须并行运行。当前预设为 **8 worker / batch 24 / early-adoption group 8**，保留完整验证、授权、计费和冲突安全回退。
- **CPU16：3,200,000 μs/块；NET：16 MiB/块；P2P：64 MiB/消息；生产交易队列：1 GiB**。硬件扩容不会自动提高链限额。P2P 研究结论及候选配置见 [消息大小分析](docs/network/P2P_MESSAGE_LIMIT.md)。
- Ubuntu **24.04 LTS**、**26.04 LTS** 和 **26.10** 的构建、运行及 PureEOS CDT 合约编译范围见 [兼容性报告](docs/ubuntu/COMPATIBILITY_20261030.md)。优先支持服务器 LTS：24.04、26.04。报告区分原生构建、26.04 独立虚拟机内核及 26.10 共享宿主内核；系统支持以已记录的具体版本为准。

### BP、最终确认和维护

用户以自身权限直接投票，最多选择一个 BP，代理投票禁用。投票排名候选、FIFO 轮岗候补、当前生效出块/确认政策及实际在线节点分别管理；每次按现有规则交接一个席位。注册和有选票不代表节点已同步或密钥已就绪。

当前等权政策的确认门槛为 `floor(2N/3)+1`，其中 `N` 是生效确认政策中的人数：21 人需 15 票，20 人需 14 票。人数不按开机数量计算。正常交接完成前仍须满足原生旧/新政策要求；已被永久封禁的账户经正常政策移除后不计入新政策。确认票不足会停止新的最终确认，全链停摆保护不会降低门槛。

BP 通过链上当前 peer key 和签名 gossip 发现彼此，支持撤销、换钥和端点更新。静态种子负责初始连接。缺少本地有效 peer 私钥时停止声明对应 BP 身份并记录日志，普通连接继续。

健康链上，单个 BP 连续三个**已最终确认的个人零出块窗口**构成永久账户封禁。全链出块或最终确认观察中断达到 **252 slot，即 126 秒，含边界**时暂停新增个人缺席证据，恢复后重新观察完整窗口。少于 21 人时也暂停新增个人缺岗处罚。此前有效违规、永久封禁和未领权益保留；注销重注册、换钥、重新投票或申请维护不能清除它们。

计划维护排除未来轮岗；在岗 BP 完成正常 proposer/finalizer 交接后维护才生效。生效后不新增在岗奖励和分配职责的缺席处罚。BP 封禁仍允许账户作为普通全节点同步。

### 系统合约和验证

系统合约保持单选投票、出块/确认/轮岗模式、**10% 长期质押 / 90% PowerUp**、奖励减半及 **14 天**升级公示治理。RAM 初始 **64 GiB**，通过既有 `setramrate(1024)` 初始化每块增长；`DISABLE_DEFERRED_TRXS_STAGE_1/2` 保持激活。

[功能验收记录](docs/baseline-20261030/REPORT.md)包含 **154 项通过、零跳过**，区分三台物理机上的 30 个并行进程、原生测试、组件检查和预期攻击拒绝。20 人政策下验证了 14 票继续、13 票暂停、无候补封禁移除及恢复。该不足 21 人实机范围具体为 20 人。本轮属于功能/拓扑验证，未测新 TPS。

出块奖励以当前已发行量为基数，按每 364 天 **1%** 精确计提，每 1461 天减半；额外 4% 不发行。仅实际出块累计权益，成功领取时发行并填池，首次正权重直接投票激活计时，领取须超过 24 小时间隔。当前系统合约 [WASM/源码包](system-contracts/pureeos-system-2026.10.31-exact1pct.tar.gz) 与[奖励复核报告](docs/rewards/EXACT_1PCT_20261031.md)配套使用，适用于新创世链。

### 从当前源码开发

新开发从 `main` 开始，安装依赖后在独立目录构建。完整源码包含固定依赖内容。

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git pkg-config \
  ca-certificates libcurl4-openssl-dev libgmp-dev zlib1g-dev python3 python3-yaml
git clone --branch main https://github.com/MatrixCoreX/PureEOS.git
cd PureEOS
python3 -I -B tools/prepare_vm_vendor.py --check
bash tools/ubuntu-modern-build.sh --check
bash tools/ubuntu-modern-build.sh --build --jobs=2
./build/ubuntu-$(. /etc/os-release; echo "$VERSION_ID")-jit/programs/nodeos/pureeos-ubuntu-nodeos --full-version
```

该入口使用 `ENABLE_OC=OFF` 的并行 JIT 配置。产物位于 `build/ubuntu-<VERSION_ID>-jit/programs/`，分别为 `nodeos/pureeos-ubuntu-nodeos`、`cleos/cleos`、`keosd/keosd` 和 `helios-util/helios-util`。准备配置后启动：

```bash
./build/ubuntu-$(. /etc/os-release; echo "$VERSION_ID")-jit/programs/nodeos/pureeos-ubuntu-nodeos \
  --config-dir /path/to/config --data-dir /path/to/data
```

测试方法及精确环境见兼容性报告；功能修改须保留现有模式与安全检查，并记录源码、二进制、合约和参数摘要。

### 许可

见 [AIC_LICENSE.md](AIC_LICENSE.md)、[LICENSING.md](LICENSING.md)、[LICENSE](LICENSE) 及依赖声明；继承代码的版权、许可及 BSL 生产用途范围按各自声明执行。

---

## English

PureEOS is a C++ blockchain project with native Savanna finality. Independent transactions execute in parallel and state commits follow block transaction order. Its smart-contract toolchain is **PureEOS CDT**.

### Install and run

The [node release](https://github.com/MatrixCoreX/PureEOS/releases/tag/baseline-2026.10.30) provides the runtime, PureEOS CDT SDK and checksums. Obtain current source from `main` and use the [exact 1% system-contract release](https://github.com/MatrixCoreX/PureEOS/releases/tag/system-contracts-2026.10.31-exact1pct) for a fresh chain; it includes matching source, WASM, ABI and checksums. Verify and extract the runtime into a separate directory using the commands above. Start tools through the package's `bin/` wrappers to use the matching loader and libraries.

Prepare network settings, genesis configuration and your own keys using the [configuration instructions](config/README.txt). The complete chain preset is [chain-parameters.json](config/chain-parameters.json). See [PureEOS CDT](https://github.com/MatrixCoreX/PureEOS-CDT#readme) for SDK installation and contract compilation.

### Requirements and Ubuntu support

- x86_64 with at least **8 physical cores, 16 logical threads and 64 GB of system-visible physical RAM**. Startup reports detected hardware and rejects insufficient hardware or explicitly serial configuration.
- All nodes run in parallel. Current settings are **8 workers / batch 24 / early-adoption group 8**, retaining full validation, authorization, billing and safe conflict fallbacks.
- **CPU16: 3,200,000 μs/block; NET: 16 MiB/block; P2P: 64 MiB/message; producer transaction queue: 1 GiB.** More hardware does not automatically raise chain caps. See the [P2P message analysis](docs/network/P2P_MESSAGE_LIMIT.md) for the proposed next setting and its qualification requirements.
- The [compatibility report](docs/ubuntu/COMPATIBILITY_20261030.md) records node builds, execution and PureEOS CDT contract compilation on Ubuntu **24.04 LTS**, **26.04 LTS**, and **26.10**. Server support prioritizes 24.04 and 26.04 LTS. The report distinguishes native builds, the separately booted 26.04 VM, bundled runtimes and the shared host kernel used for 26.10. Support claims apply to the exact recorded versions.

### BP selection, finality and maintenance

Users vote directly with their own authority for at most one BP; proxy voting is disabled. Ranked candidates, FIFO standbys, effective proposer/finalizer policies and online processes are separate states. Rotation hands off one seat at a time under the existing rules. Registration and votes do not establish synchronization or key readiness.

The current equal-weight quorum is `floor(2N/3)+1`, where `N` is the effective finalizer-policy membership: 15 votes for 21 members, 14 for 20. Online process count does not change this denominator. Handoffs retain native old/new policy requirements. A permanently banned account stops counting after normal policy removal. Insufficient votes halt new finality; interruption safeguards do not lower the quorum.

BP discovery uses current on-chain peer keys and signed gossip, including revocation, key rotation and endpoint updates. Static seeds establish initial connectivity. Without a valid local peer key, the node logs the condition and stops asserting that BP identity while ordinary connections continue.

On a healthy chain, three consecutive **finalized personal zero-production windows** permanently ban the BP account. An interruption in block production or finality observation of **252 slots / 126 seconds, inclusive**, pauses new personal absence evidence; recovery starts fresh complete observation windows. New personal absence penalties also pause below 21 members. Previously valid violations, permanent bans and unpaid rights remain. Re-registration, new keys, votes or maintenance cannot erase them.

Planned maintenance excludes future rotation. An incumbent must complete normal proposer/finalizer handoff before maintenance becomes effective. Effective maintenance accrues no new duty rewards or assigned-duty absence penalties. A banned BP may still synchronize as an ordinary full node.

### System contracts and qualification

The system contract retains single-choice voting, proposer/finalizer/rotation modes, **10% staking / 90% PowerUp**, reward halving and the **14-day** upgrade review. RAM starts at **64 GiB** and grows through explicit `setramrate(1024)` initialization. Both deferred-disable stages remain activated.

The [functional report](docs/baseline-20261030/REPORT.md) records **154 passing cases, zero skipped**, separating 30 parallel processes on three physical hosts, native tests, component checks and expected attack rejections. Live 20-member checks cover continued finality with 14 votes, interruption with 13, permanent-ban removal without standbys and recovery. Live sub-21 coverage specifically means 20 members. This functional/topology campaign measures no new TPS.

Block rewards accrue at exactly **1% of currently issued supply per 364-day contract year**, halved every 1461 days; the additional 4% is never issued. Only actual blocks create unpaid credits. A successful claim issues and funds the pool; a positive-weight direct vote starts the clock and claims require more than 24 hours. Use the matching [WASM/source package](system-contracts/pureeos-system-2026.10.31-exact1pct.tar.gz) and [reward review](docs/rewards/EXACT_1PCT_20261031.md) for fresh genesis chains.

### Develop from current source

Start new work from `main`. The source includes pinned dependency contents. On a supported Ubuntu system:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git pkg-config \
  ca-certificates libcurl4-openssl-dev libgmp-dev zlib1g-dev python3 python3-yaml
git clone --branch main https://github.com/MatrixCoreX/PureEOS.git
cd PureEOS
python3 -I -B tools/prepare_vm_vendor.py --check
bash tools/ubuntu-modern-build.sh --check
bash tools/ubuntu-modern-build.sh --build --jobs=2
./build/ubuntu-$(. /etc/os-release; echo "$VERSION_ID")-jit/programs/nodeos/pureeos-ubuntu-nodeos --full-version
```

The isolated build uses parallel JIT with `ENABLE_OC=OFF`. Under `build/ubuntu-<VERSION_ID>-jit/programs/`, the executables are `nodeos/pureeos-ubuntu-nodeos`, `cleos/cleos`, `keosd/keosd` and `helios-util/helios-util`. To run your node, supply your own configuration and data directories:

```bash
./build/ubuntu-$(. /etc/os-release; echo "$VERSION_ID")-jit/programs/nodeos/pureeos-ubuntu-nodeos \
  --config-dir /path/to/config --data-dir /path/to/data
```

The compatibility report documents exact environments and checks. Changes must preserve selected modes and safety checks and record source, binary, contract and parameter hashes.

### Licenses

See [AIC_LICENSE.md](AIC_LICENSE.md), [LICENSING.md](LICENSING.md), [LICENSE](LICENSE) and dependency notices. Inherited copyrights, license terms and applicable BSL production-use scope remain as declared.
