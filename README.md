# PureEOS

[简体中文](#简体中文) · [English](#english)

## 简体中文

PureEOS 是独立维护的 C++ 新链项目，使用原生 Savanna 最终确认，支持跨交易并行合约执行，并按区块中的交易顺序提交状态。配套工具链统一命名为 **PureEOS CDT 1.0.0**；现有 1.0.0 制品此前以 **AIC CDT 1.0.0** 发布。

已发布版本为 **PureEOS 2.0.0（并行首版）＋ PureEOS CDT 1.0.0**。[联合发行页](https://github.com/MatrixCoreX/PureEOS/releases/tag/v2.0.0)提供运行包、CDT SDK 及源码、匹配的系统合约、验收摘要和 SHA256SUMS。发行身份与当前开发配置分别记录；历史制品保持原有版本和摘要。

### 当前开发配置与并行执行

- 当前开发版 `nodeos` 强制启用并行模式。最低运行硬件为 **8 个物理核心、16 个逻辑线程、64 GB 系统可见物理内存**；启动日志输出检测值，低于门槛或显式纯串行配置会被拒绝。
- 开发验证预设使用 **8 worker / batch 24 / early-adoption group 8**。本地并行度按有效硬件及配额配置；链资源限额不随硬件自动放大。
- 开发链预设为 **CPU16：3,200,000 μs/块；NET：16 MiB/块**。P2P 单消息上限保持 64 MiB，生产交易队列保持 1 GiB，单笔限额、授权、完整验证和计费规则保持原约束。
- 同一交易内部保持顺序和原子性；跨交易试算在提交前重新检查依赖。冲突、资源不足或不支持的能力采用安全回退路径。
- 测试使用明确的 JIT 配置和经过核验的合约代码摘要。多个 worker 或多进程拓扑本身不代表 TPS 提升。
- 当前多节点扩展测试是功能与拓扑验证，使用三台物理主机模拟约 30 个独立并行实例。具体候选版本及未完成项独立记录，完整资格确认后才提升开发基线。

已发布并行首版的验收范围见[验收状态](docs/parallel-execution/PX7_V1_CURRENT_STATUS.txt)和[发布清单](docs/parallel-execution/PX7_V1_RELEASE_CHECKLIST.txt)。历史串行及旧 worker 配置的记录保留原身份；后续实机运行遵循当前并行与硬件要求。

### 系统支持范围

构建验证目标为 **Ubuntu 26.04 LTS / Ubuntu 26.10 用户空间，x86_64，JIT**。本轮补充了 Ubuntu 24.04.5 LTS 宿主环境的实机记录：

| 环境 | 已验证方式与范围 |
|---|---|
| Ubuntu 24.04.5 LTS 宿主，x86_64 | 三台 AMD EPYC 4345P 实机，内核 `6.8.0-146-generic`。节点使用匹配的 Ubuntu 26.10 / glibc 2.44 运行环境：主机 A 为隔离 rootfs，另两台为匹配的 loader 和共享库。本轮 30 BP 进程的功能测试仍在进行。 |
| Ubuntu 26.04 LTS 用户空间 | 已有记录覆盖四工具 Release 构建和四个原生组件检查。 |
| Ubuntu 26.10 用户空间 | 已有记录覆盖四工具构建、启用的原生功能、并行引擎矩阵、SDK/合约和真实进程恢复。 |

Ubuntu 24.04 宿主记录的启动方式需要上述匹配运行环境；已发布的 Ubuntu 26.10 运行包仍按其原有用户空间要求分发。各制品按实际构建与运行环境标注。详见[Ubuntu 24.04 宿主实测记录](docs/ubuntu/UBUNTU_24_HOST_VERIFICATION_20261030.md)和[现代 Ubuntu 验证结果](docs/ubuntu/UBUNTU_MODERN_RESULTS.txt)。

### 从源码构建

安装构建依赖：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git openssh-client \
  pkg-config libcurl4-openssl-dev libgmp-dev zlib1g-dev python3 python3-yaml
```

检出对应发行标签或经过确认的开发提交，再准备固定依赖。以下以已发布的 `v2.0.0` 为例：

```bash
git clone https://github.com/MatrixCoreX/PureEOS.git
cd PureEOS
git checkout v2.0.0
git submodule update --init --recursive --jobs=4
python3 -I -B tools/prepare_vm_vendor.py --apply
python3 -I -B tools/prepare_vm_vendor.py --check
```

VM 固定基线由独立公开源码镜像 MatrixCoreX/AIC-VM 提供，提交和继承许可保持不变。已有 CI 在无私有部署钥的环境中递归检出全部 188 个固定模块，并验证四个 VM 头文件的补丁。GitHub 自动生成的源码压缩包不包含子模块。运行包的构建来源摘要与发行 Git 提交分别记录，文档及交付脚本差异见对应来源报告。

现代构建入口使用独立目录：

```bash
bash tools/ubuntu-modern-build.sh --check
bash tools/ubuntu-modern-build.sh --build --jobs=2
bash tools/ubuntu-modern-build.sh --test --jobs=2
```

输出目录为 `build/ubuntu-<版本>-jit/`；节点入口为 `programs/nodeos/pureeos-ubuntu-nodeos`，同时构建 cleos、keosd 和 helios-util。`--test` 执行指定的队列、计时器及生产者组件检查；整链和 TPS 验收另行记录。可用 `--build-dir=/your/isolated-build` 指定目录。

配置使用 `ENABLE_OC=OFF`，无需另装 LLVM 开发包；编译合约需匹配的 PureEOS CDT SDK。首次安装、签名钥、资源初始化和节点加入步骤见[使用说明](docs/parallel-execution/PX7_V1_CANDIDATE_GUIDE.txt)。构建脚本不会自动创建或升级现用链；正式创世应使用自行生成的密钥与分配。

### 使用发行包

现有运行包面向 **Ubuntu 26.10 x86_64 用户空间**，不捆绑主机动态库。在新目录中核验并解压：

```bash
sha256sum -c SHA256SUMS --ignore-missing
mkdir pureeos-v2.0.0
tar -xzf pureeos-v2.0.0-ubuntu26.10-x86_64.tar.gz -C pureeos-v2.0.0
cd pureeos-v2.0.0
sha256sum -c SHA256SUMS
./bin/nodeos --full-version
```

运行包包含 nodeos、cleos、keosd、helios-util 和匹配系统合约。CDT SDK 解压后，将 `aic-cdt-1.0.0/bin` 加入 PATH，并将 SDK 根目录传入 `CMAKE_PREFIX_PATH`；安装路径可以含空格。已有发行包中的 `aic-cdt-1.0.0` 是历史目录名；工具链展示名称为 PureEOS CDT。SDK 自带独立 SHA256SUMS。

该发行版修复了参数封装读写、零额领奖、线程 CPU 计费和 VM 函数入口下标检查。确定性执行工作计量/容量协议在独立分支开发，未纳入该发行版。

### 系统规则与恢复

- 用户以投票账户自身权限直接投票，最多选择一个 BP；代理投票已禁用，BP 不能替其他投票者改票。
- 保留候选线性扩容、顺序单席轮岗、真实出块奖励，以及开发者升级和固定 **14 天**公示机制。注册 BP、进入候选、当前生效委员会和在线进程是不同状态。
- 资源配置保留 **10% 长期质押 / 90% PowerUp**，以及固定奖励减半和治理规则。RAM 采用初始 **64 GiB**，通过既有 `setramrate(1024)` 初始化增长率。
- 保持 `DISABLE_DEFERRED_TRXS_STAGE_1` 和 `DISABLE_DEFERRED_TRXS_STAGE_2` 激活。
- 永久黑名单按账户保留，注销重注册或更换密钥不会清除原账户记录。节点、合约及其版本化初始化配置须匹配。

非正常停机出现 dirty 数据库提示时，先保留区块、finalizer safety 数据和配置，再按日志选择原生重放流程；保留防双签安全数据。历史 40 进程轮岗、主机停启、限定范围的 ASan/UBSan 和真实 24h 领奖证据保留各自的程序与合约摘要；新的候选功能与本轮重跑结果另行记录。

### 许可

自有原创部分见 [AIC 开源许可 1.0](AIC_LICENSE.md)。继承代码与第三方声明按实际许可保留，范围见 [LICENSING.md](LICENSING.md) 和 [LICENSE](LICENSE)。继承基线中 BSL 生产用途适用范围仍在核清；新许可文件不代表全仓库已重新授权。

---

## English

PureEOS is an independently maintained C++ project for new chains. It uses native Savanna finality, executes independent transactions in parallel, and commits state in block transaction order. The contract toolchain is now named **PureEOS CDT 1.0.0**. Existing 1.0.0 artifacts were published as **AIC CDT 1.0.0**.

The published release is **PureEOS 2.0.0, the first parallel release, with PureEOS CDT 1.0.0**. The [joint release page](https://github.com/MatrixCoreX/PureEOS/releases/tag/v2.0.0) contains runtime packages, the CDT SDK and sources, matching system contracts, acceptance summaries, and SHA256SUMS. Published artifacts and current development settings have separate identities; historical versions and hashes are retained.

### Current development profile and parallel execution

- Current development `nodeos` requires parallel mode and at least **8 physical CPU cores, 16 logical threads, and 64 GB of system-visible physical memory**. Startup logs report detected hardware. Insufficient hardware or an explicit serial-only configuration is rejected.
- The development qualification preset uses **8 workers, batch 24, and early-adoption group 8**. Local parallelism follows available hardware and quotas; chain resource caps remain fixed across hardware.
- The development chain preset is **CPU16: 3,200,000 μs per block; NET: 16 MiB per block**. The P2P limit remains 64 MiB per message, and the producer transaction queue remains 1 GiB. Per-transaction limits, authorization, full validation, and billing retain their existing rules.
- Each transaction preserves execution order and atomicity. Dependencies are rechecked before committing parallel work. Conflicts, resource constraints, and unsupported operations use the safe fallback path.
- Tests use an explicit JIT configuration and verified contract code hashes. Worker counts and multi-process topologies alone do not establish a TPS gain.
- The current expanded campaign tests functionality and topology using about 30 independent parallel instances on three physical servers. Candidate versions and incomplete cases are recorded separately. Baseline promotion follows qualification.

For the published first parallel release, see the [acceptance status](docs/parallel-execution/PX7_V1_CURRENT_STATUS.txt) and [release checklist](docs/parallel-execution/PX7_V1_RELEASE_CHECKLIST.txt). Historical serial and older worker configurations retain their original scope. New live runs follow the current parallel-mode and hardware requirements.

### Supported system configurations

Build qualification targets **Ubuntu 26.04 LTS / Ubuntu 26.10 userlands, x86_64, JIT**. The current campaign adds physical-host evidence for Ubuntu 24.04.5 LTS:

| Environment | Verified configuration and scope |
|---|---|
| Ubuntu 24.04.5 LTS host, x86_64 | Three AMD EPYC 4345P servers with kernel `6.8.0-146-generic`. Nodes use matching Ubuntu 26.10 / glibc 2.44 runtime environments: an isolated rootfs on host A, and matching loaders/shared libraries on the other two hosts. The full 30-BP-process functional campaign remains in progress. |
| Ubuntu 26.04 LTS userland | Existing records cover Release builds of all four tools and four native component checks. |
| Ubuntu 26.10 userland | Existing records cover all four tools, enabled native features, the parallel engine matrix, SDK/contracts, and real process recovery. |

The verified Ubuntu 24.04 host deployment uses the matching runtime environment described above. Published Ubuntu 26.10 runtime packages retain their original userland requirements. Artifacts are labeled by their actual build and runtime environments. See the [Ubuntu 24.04 host verification](docs/ubuntu/UBUNTU_24_HOST_VERIFICATION_20261030.md) and [modern Ubuntu results](docs/ubuntu/UBUNTU_MODERN_RESULTS.txt).

### Build from source

Install build dependencies:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git openssh-client \
  pkg-config libcurl4-openssl-dev libgmp-dev zlib1g-dev python3 python3-yaml
```

Check out the relevant release tag or qualified development commit, then prepare pinned dependencies. This example uses the published `v2.0.0` release:

```bash
git clone https://github.com/MatrixCoreX/PureEOS.git
cd PureEOS
git checkout v2.0.0
git submodule update --init --recursive --jobs=4
python3 -I -B tools/prepare_vm_vendor.py --apply
python3 -I -B tools/prepare_vm_vendor.py --check
```

The pinned VM baseline comes from the separate public MatrixCoreX/AIC-VM source mirror, preserving its commit identity and inherited licenses. Existing CI checked out all 188 pinned modules without a private deployment key and verified patches to four VM headers. GitHub-generated source archives omit submodules. Runtime build-source digests and release Git commits are recorded separately; their documentation and delivery-script differences are documented in the corresponding provenance reports.

The modern build entry point uses an isolated directory:

```bash
bash tools/ubuntu-modern-build.sh --check
bash tools/ubuntu-modern-build.sh --build --jobs=2
bash tools/ubuntu-modern-build.sh --test --jobs=2
```

Outputs are under `build/ubuntu-<version>-jit/`. The node entry point is `programs/nodeos/pureeos-ubuntu-nodeos`; cleos, keosd, and helios-util are also built. `--test` runs selected queue, timer, and producer component checks. Whole-chain and TPS qualification are recorded separately. Set `--build-dir=/your/isolated-build` to choose another directory.

This configuration sets `ENABLE_OC=OFF` and needs no additional LLVM development package. Contract compilation requires the matching PureEOS CDT SDK. Follow the [usage guide](docs/parallel-execution/PX7_V1_CANDIDATE_GUIDE.txt) for installation, signing keys, resource initialization, and node onboarding. Build scripts do not create or upgrade an existing chain. Generate appropriate keys and allocations for a production genesis.

### Use a published package

Existing runtime packages target the **Ubuntu 26.10 x86_64 userland** and do not bundle host dynamic libraries. Verify and unpack into a new directory:

```bash
sha256sum -c SHA256SUMS --ignore-missing
mkdir pureeos-v2.0.0
tar -xzf pureeos-v2.0.0-ubuntu26.10-x86_64.tar.gz -C pureeos-v2.0.0
cd pureeos-v2.0.0
sha256sum -c SHA256SUMS
./bin/nodeos --full-version
```

Packages contain nodeos, cleos, keosd, helios-util, and matching system contracts. After unpacking the CDT SDK, add `aic-cdt-1.0.0/bin` to PATH and pass the SDK root to `CMAKE_PREFIX_PATH`. Installation paths may contain spaces. Existing packages retain the historical `aic-cdt-1.0.0` directory name; the toolchain display name is PureEOS CDT. The SDK has its own SHA256SUMS.

That release fixes parameter packing/unpacking, zero-value reward claims, thread CPU billing, and VM function-entry index checks. Deterministic execution-work metering and capacity protocols are being developed on a separate branch and are outside that release.

### System rules and recovery

- Voting requires the voting account's own authority and selects at most one BP. Proxy voting is disabled. A BP cannot change another voter's selection.
- Candidate growth, one-seat sequential rotation, rewards for actual production, developer upgrades, and the fixed **14-day** review period are retained. BP registration, candidate eligibility, active committee membership, and online processes are distinct states.
- Resources retain the **10% long-term staking / 90% PowerUp** split, fixed reward halving, and governance rules. Initial RAM is **64 GiB**, with growth initialized through the existing `setramrate(1024)` action.
- Keep both `DISABLE_DEFERRED_TRXS_STAGE_1` and `DISABLE_DEFERRED_TRXS_STAGE_2` activated.
- Permanent blacklist records belong to accounts. Re-registration or key changes do not clear the original account's record. Node, contract, and versioned initialization settings must match.

After an unclean shutdown and a dirty-database warning, preserve blocks, finalizer safety data, and configuration, then follow the logs to select the native replay procedure. Preserve double-sign protection data. Historical 40-process rotation, host restart, scoped ASan/UBSan, and real 24-hour reward-claim evidence retain their original node and contract hashes. New candidate features and this campaign's rerun results are recorded separately.

### License

Original work is covered by the [AIC Open Source License 1.0](AIC_LICENSE.md). Inherited code and third-party notices retain their applicable licenses; see [LICENSING.md](LICENSING.md) and [LICENSE](LICENSE). The production-use scope of BSL components in the inherited baseline remains under review. New license files do not relicense the entire repository.
