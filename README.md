# PureEOS

[简体中文](#简体中文) · [English](#english)

## 简体中文

PureEOS 是独立维护的 C++ 新链项目，采用原生 Savanna 最终确认，支持跨交易并行执行，并按区块交易顺序提交状态。配套合约工具链统一命名为 **PureEOS CDT**。

本次发行标识为 **`baseline-2026.10.30`**，对应本轮功能验收后的开发基线。历史 `v2.0.0`、CDT `v1.0.0` 及其制品保持原样。日期基线标识与程序内部版本分别记录；已验证的二进制不会为改展示名称而重新编译。

### 获取和运行

[PureEOS 发行页](https://github.com/MatrixCoreX/PureEOS/releases/tag/baseline-2026.10.30)提供匹配的节点运行包、完整源码、系统合约、PureEOS CDT SDK、验收摘要和 SHA256SUMS。[PureEOS CDT 仓库](https://github.com/MatrixCoreX/PureEOS-CDT)沿用原访问权限。

```bash
sha256sum -c SHA256SUMS --ignore-missing
mkdir pureeos-baseline
tar -xzf pureeos-baseline-2026.10.30-linux-x86_64.tar.gz -C pureeos-baseline
cd pureeos-baseline
sha256sum -c SHA256SUMS
./bin/nodeos --full-version
```

请通过 `bin/` 启动工具，以使用包内匹配的 loader 和共享库。包内还保留此前已验证的 cleos、keosd、helios-util；它们的来源和逐文件摘要见 `TOOL_PROVENANCE.json`。运行包不携带测试私钥、云凭据、测试链快照或账户分配。使用自有密钥、网络配置和匹配的初始化流程。

PureEOS CDT SDK 的根目录为 `pureeos-cdt-1.0.0`，加入其 `bin/` 到 PATH，并将根目录传入 `CMAKE_PREFIX_PATH`。本次保留已验证的编译器字节；内部版本字符串和部分 CMake 名称可能仍显示历史 AIC CDT 名称。更名后重编译的系统合约 WASM、ABI 与测试版本逐字节一致。

### 运行要求与系统支持

- `nodeos` 强制并行运行，最低 **8 个物理核心、16 个逻辑线程、64 GB 系统可见物理内存**。启动时打印检测值；低于要求、未知物理拓扑或显式串行配置会被拒绝。
- 当前开发预设为 **8 worker / batch 24 / early-adoption group 8**，完整验证，冲突时保留安全回退。
- 链限额固定为 **CPU16：3,200,000 μs/块，NET 16 MiB/块**。P2P 单消息 64 MiB，交易队列 1 GiB，单笔 CPU、NET、授权和计费约束保持。具体参数见 `config/chain-parameters.json`。硬件扩容不会自动放大链限额。
- 本轮使用三台 Ubuntu **24.04.5 LTS** 宿主、内核 `6.8.0-146-generic`、AMD EPYC 4345P（8 核/16 线程/64 GiB）。节点使用匹配的 Ubuntu 26.10 / glibc 2.44 运行环境；发行运行包包含该匹配环境。
- Ubuntu 24.04 的验证范围是上述宿主及匹配运行环境；未将节点直接链接宿主 glibc 2.39，也未声称 CDT 可在未准备运行环境的 Ubuntu 24.04 上原生构建。CDT/合约重编译验证在 Ubuntu 26.10 用户空间完成。历史 Ubuntu 26.04/26.10 构建结果保留各自范围。

### BP 发现、轮岗、停摆和维护

- 用户只能以自身权限直接投票，最多选择一个 BP；代理投票禁用。投票排名候选、轮岗候补、当前生效出块者/确认者和在线节点是不同状态。
- 保留现有候选筛选及 FIFO 顺序单席轮岗。当前等权模式的确认门槛是 `floor(2N/3)+1`：21 人需 15 票，20 人需 14 票。人数取链上原生政策，不取开机数；交接须满足原生旧／新政策要求，完成 proposer/finalizer 政策切换，不能提前套用新门槛。
- BP 发现使用链上当前 peer key 和有签名的 gossip，覆盖撤销、换钥和端点变更。普通静态种子只负责初始连接，不冒充 gossip。缺少本地当前 peer 私钥时停止声明对应 BP 身份并记录日志，普通连接继续，节点不会因此退出。
- 健康链上，单个 BP 连续三个**已最终确认的个人零出块窗口**构成永久账户封禁。注销重注册、换出块/确认/peer 密钥、重新投票或维护申请不会清除原账户有效证据或封禁。BP 封禁不禁止作为普通全节点同步。
- 全链出块或最终确认观察中断达到 **252 slot = 126 秒（含边界）**时，暂停新增个人缺席证据，不把整个停摆期间补算为个人违规。保留此前已结算的 1/2 次违规及永久封禁；恢复后重新观察完整窗口。该保护不降低确认票数门槛，也不能在确认票不足时恢复最终确认；更短中断仍按原窗口规则处理。
- 计划维护会将账户排除于未来轮岗。在岗 BP 必须先完成正常双政策交接，维护才能生效。生效后不新增在岗奖励或分配职责的缺席处罚；历史未领权益、有效违规和永久封禁保留。提前关机仍可能形成真实违规。

本轮同时完成无候补永久封禁后的 **21→20** 政策补测：被封席位通过正常交接移除后不再计入生效委员会；20 人政策下 14 名有效确认者在线可持续确认，13 票时停止新最终确认，短断和长断后恢复，随后正常补回 21 人。少于 21 人时暂停新增个人缺岗处罚，旧违规和永久封禁保留。本轮不足 21 的实机资格范围是 **20 人政策**；历史从单节点启动扩容的证据单独保留。新节点须先同步、准备有效密钥，注册和有选票不能证明节点已就绪。

### 系统合约和验收范围

保留单选投票、原出块/确认/轮岗模式、**10% 长期质押 / 90% PowerUp**、奖励减半及固定 **14 天**升级公示治理。RAM 初始 **64 GiB**，通过既有 `setramrate(1024)` 初始化增长率；不靠修改 C++ 默认值完成配置。`DISABLE_DEFERRED_TRXS_STAGE_1/2` 保持激活。

本次系统合约是独立版本扩展，原已发布 WASM 保留。现网升级仍须遵守原治理，发行不会自动升级其他运行链。完整系统升级数据可能超过单笔 NET 上限，使用标准 zlib 交易压缩，不能通过放宽 NET 或治理检查解决。

验收摘要和覆盖矩阵区分真实三机/30 并行实例、原生测试、组件检查、预期攻击拒绝及原始失败重试。本轮是共享三台物理机的功能/拓扑验证，**没有新测 TPS，也不构成 30 台独立物理机性能验收**。固定第三方 DeFi/NFT 合约和单独标识的 Bancor 授权修复版本保留各自源码、摘要及测试范围。

### 源码和构建

日期基线源码包含实际固定依赖的完整内容；历史 `v2.0.0` 的子模块交付方式保持原样。

```bash
git clone https://github.com/MatrixCoreX/PureEOS.git
cd PureEOS
git checkout baseline-2026.10.30
python3 -I -B tools/prepare_vm_vendor.py --check
bash tools/ubuntu-modern-build.sh --check
bash tools/ubuntu-modern-build.sh --build --jobs=2
```

详细构建依赖及历史入门流程见[现代 Ubuntu 说明](docs/ubuntu/UBUNTU_MODERN_RESULTS.txt)和[使用说明](docs/parallel-execution/PX7_V1_CANDIDATE_GUIDE.txt)。交付提交保留公开仓库历史；二进制编译来源、交付来源和代码等同性见发行清单。

### 许可

自有部分见 [AIC 开源许可 1.0](AIC_LICENSE.md)。继承代码和第三方声明见 [LICENSING.md](LICENSING.md)、[LICENSE](LICENSE) 及各依赖许可。产品更名不改变原版权和许可；继承 BSL 生产用途范围按现有声明保留。

---

## English

PureEOS is an independently maintained C++ project for new chains. It uses native Savanna finality, executes independent transactions in parallel, and commits state in block transaction order. Its contract toolchain is named **PureEOS CDT**.

The release identifier is **`baseline-2026.10.30`**, the development baseline selected after this functional campaign. Historical `v2.0.0`, CDT `v1.0.0`, and their artifacts remain unchanged. The dated release identity and embedded program versions are recorded separately; validated binaries are preserved during the branding update.

### Packages and requirements

The [PureEOS release](https://github.com/MatrixCoreX/PureEOS/releases/tag/baseline-2026.10.30) provides the node runtime, complete source, matching system contracts, PureEOS CDT SDK, acceptance summary, and SHA256SUMS. The [PureEOS CDT repository](https://github.com/MatrixCoreX/PureEOS-CDT) retains its existing access permissions.

Verify SHA256SUMS, extract `pureeos-baseline-2026.10.30-linux-x86_64.tar.gz` into a new directory, verify its internal SHA256SUMS, then run `./bin/nodeos --full-version`. Use the `bin/` launchers to select the bundled loader/libraries. Previously qualified cleos, keosd, and helios-util are included with their exact provenance in `TOOL_PROVENANCE.json`. Supply your own keys, network configuration, and initialization; no private test keys, cloud credentials, snapshots, or fixture allocations are distributed.

The SDK root is `pureeos-cdt-1.0.0`; add its `bin/` to PATH and use its root as `CMAKE_PREFIX_PATH`. Compiler bytes remain unchanged. Embedded version strings and some CMake names retain the historical AIC CDT identity. A rebuild from the renamed SDK produced byte-identical system WASM and ABI.

- Mandatory parallel nodeos: at least **8 physical cores, 16 logical threads, and 64 GB system-visible physical RAM**. Startup reports detected values and rejects insufficient hardware, unverifiable physical topology, and explicit serial configuration.
- Development configuration: **8 workers / batch 24 / early-adoption group 8**, full validation, and safe conflict fallback.
- Fixed chain caps: **3,200,000 μs CPU/block and 16 MiB NET/block**; 64 MiB per P2P message and a 1 GiB producer transaction queue. Transaction limits, authorization, and billing remain intact. See `config/chain-parameters.json`. Hardware scaling does not enlarge chain caps.
- This campaign used three Ubuntu **24.04.5 LTS** hosts, kernel `6.8.0-146-generic`, AMD EPYC 4345P, 8 cores/16 threads/64 GiB, with the matching Ubuntu 26.10 / glibc 2.44 runtime. The node package bundles that runtime. This is not direct linkage against host glibc 2.39 or a claim of native CDT builds on an unprepared Ubuntu 24.04 installation. SDK/contract rebuild validation ran in Ubuntu 26.10 user space. Earlier Ubuntu 26.04/26.10 results retain their own scope.

### Producers, finality, discovery, and maintenance

Users directly authorize their own vote for at most one BP; proxy voting is disabled. Vote-ranked candidates, FIFO reserves, active producers/finalizers, and running processes are distinct. Existing candidate selection and one-seat rotation remain. Under the current equal-weight mode, quorum is `floor(2N/3)+1`:21members require15votes,20require14. Membership comes from native policies, not the number of running processes. Transitions must satisfy the applicable native old/new policy requirements; a lower target threshold cannot be applied early.

BP discovery validates signed gossip against current on-chain peer keys, including revocation, rotation, and endpoint changes. Ordinary static seeds provide initial transport. A missing local current peer private key suppresses that BP claim and logs the condition while ordinary connections continue; it no longer terminates the node.

On a healthy chain, three consecutive finalized personal zero-production windows permanently ban the account. Re-registration, key changes, votes, or maintenance cannot erase valid evidence or the ban. A banned BP may still synchronize as an ordinary full node.

An interruption of block/finality observation reaching **252 slots = 126 seconds, inclusive**, pauses new personal absence evidence. Interrupted time is not retrospectively converted into individual violations. Previously settled strikes and bans remain; recovery starts a fresh complete observation window. Shorter gaps follow existing window rules. This safeguard neither lowers quorum nor restores finality without enough current votes.

Planned maintenance excludes the account from future rotation. An incumbent completes normal dual-policy handoff before maintenance takes effect. Effective maintenance accrues no new assigned-duty penalties or production rewards, while historical unpaid rights, valid violations, and permanent bans remain. Premature shutdown can still create real violations.

The supplementary live tests verified no-standby permanent-ban eviction21to20 through normal handoff. The removed account no longer counts in the active policy. With20effective members,14valid voters continued finalizing;13votes stopped new finality. Short/long interruption and recovery preserved prior evidence and bans, followed by normal restoration to21members. New personal absence penalties pause below21. This campaign's live sub21 qualification specifically covers20members, not all sizes or growth-failure combinations. Prepare synchronized nodes and valid keys before admission; registration and votes do not prove readiness.

### System rules, evidence, and source

The versioned system extension preserves single-choice voting, producer/finalizer/rotation modes, **10% stake / 90% PowerUp**, reward halving, and the fixed **14-day** upgrade review. RAM starts at **64 GiB**, with growth initialized through the existing `setramrate(1024)` action. Both deferred-disable stages remain active. Original released WASM is retained. Publication does not upgrade other running chains or bypass governance. Full system upgrade payloads may require standard zlib transaction compression to fit existing NET caps.

Evidence distinguishes live three-host/30-process validation, native tests, component checks, expected attack rejection, and preserved failed attempts/retries. This shared-hardware functional/topology campaign **measures no new TPS and does not qualify 30 independent physical machines**. Pinned third-party DeFi/NFT contracts and the separately identified Bancor authorization fix retain their own source hashes and test scope.

The dated source tree includes pinned dependency contents. Historical `v2.0.0` submodule delivery remains unchanged. Checkout `baseline-2026.10.30`, run `python3 -I -B tools/prepare_vm_vendor.py --check`, then use `tools/ubuntu-modern-build.sh --check` and `--build --jobs=2`. See the [Ubuntu notes](docs/ubuntu/UBUNTU_MODERN_RESULTS.txt) and [earlier setup guide](docs/parallel-execution/PX7_V1_CANDIDATE_GUIDE.txt). The delivery commit preserves public Git history; compiled source, delivery source, and code equivalence are recorded separately.

Product renaming does not alter copyright or licenses. See [AIC_LICENSE.md](AIC_LICENSE.md), [LICENSING.md](LICENSING.md), [LICENSE](LICENSE), and dependency notices, including existing BSL production-use scope.
