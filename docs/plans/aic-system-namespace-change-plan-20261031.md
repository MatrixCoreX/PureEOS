# PureEOS 系统命名空间改为 AIC：关联分析与更改计划

日期：2026-10-31

状态：**待商议的影响分析与实施草案；未选定实施，未修改代码、构建或启用 AIC 命名空间。**

当前工作继续使用已验证的 `eosio*` 基线进行测试和新测试链重置：系统保留名称不变，BP、节点、普通测试用户及测试合约等非系统原生账号使用 12 位名称。PureHistory 按该现行基线重新索引。本计划不改变上述执行范围。

## 一、结论与范围

将系统根账号从 `eosio` 改为 `aic` 属于**链协议、系统合约及外围工具的联合改造**。修改一个展示标题、WASM 文件名或初始化脚本不足以实现这个目标。

代码中同时存在可复用的 `config::system_account_name`、系统合约常量、直接写死的账号字符串、权限名称、合约标签以及文档示例。必须按语义分类，不能全局替换 `eosio` 字样。

新链方式省去旧状态迁移，但不能省去以下资格验证：原生根权限、治理封存、资金流、并行执行的安全回退、创世一致性、节点互联、客户端与历史解析。

本计划只讨论将来的独立 AIC 命名空间版本。短号租赁与解析见 [短号租赁计划](short-name-leasing-plan-20261031.md)，该功能也仍处于规划阶段。两项变更应分别验证，最终组合版本另做联合验收。

## 二、必须先区分的对象

| 对象 | 当前含义 | AIC 草案 |
| --- | --- | --- |
| `eosio` | 创世原生根账号；原生操作、系统业务与治理的账号身份 | 将来可改为 `aic`，须节点和合约一致 |
| `eosio.system` | 系统合约的工程目标、合约注解及 WASM/ABI 产物名称 | 可命名为 `aic.system`；通常部署在根账号 `aic` 上 |
| `eosio.token` | 实际部署代币合约的链上账号，也常作为构建目标名 | 实际账号及相应合约产物可改为 `aic.token` |
| `namespace eosio`、`eosio::name` | C++/CDT 源码接口 | 建议保持，以保留源码兼容性 |
| `eosio.code`、`eosio.any` | 原生虚拟/特殊权限名称 | 建议保持其名称与语义，不作为系统账号前缀一并修改 |
| `eosio.auth`、`eosio.all` | 节点内部的特殊 scope 常量 | 先保持，不当作待创建的系统账号 |
| `eosio::abi/1.1`、`eosio::abi/1.2` | ABI 格式版本字符串 | 保持，不改成 `aic::abi/...` |
| `/v1/chain/...`、SHIP 消息格式 | 节点协议与 API | 保持协议路径、字段与编码兼容；业务查询中的账号参数才改变 |

当前部署系统合约的根账号是 `eosio`。不能因为工程叫 `eosio.system`，就假定链上还存在一个必须部署同一合约的 `eosio.system` 账号。新版本也不应无意中同时建立两个互相竞争的系统根。

## 三、拟议账号映射清单

以下为待评审映射，不是当前部署配置。清单覆盖已检索到的原生系统账号和系统合约常量；“存在常量”不等于当前链上已创建该账号，也不意味着将来须启用其对应旧功能。

| 当前账号 | 拟议账号 | 用途及关联 |
| --- | --- | --- |
| `eosio` | `aic` | 原生根、初始化、系统合约、治理、`onblock` |
| `eosio.null` | `aic.null` | 原生空账号与禁止恢复的权限边界 |
| `eosio.prods` | `aic.prods` | 节点维护的委员会授权账号及阈值权限 |
| `eosio.token` | `aic.token` | AIC 发行、转账、余额、供应量与核心币识别 |
| `eosio.msig` | `aic.msig` | 多签操作及治理保护、并行排除 |
| `eosio.wrap` | `aic.wrap` | 包装执行及治理保护、并行排除 |
| `eosio.ram` | `aic.ram` | RAM 交易资金 |
| `eosio.ramfee` | `aic.ramfee` | RAM 手续费流转 |
| `eosio.stake` | `aic.stake` | 质押、赎回及授权 |
| `eosio.bpay` | `aic.bpay` | 节点区块奖励账户与转账通知 |
| `eosio.vpay` | `aic.vpay` | 保留系统账户；不能借更名恢复按票分红 |
| `eosio.saving` | `aic.saving` | 保留系统账户；不能借更名重新发行额外 4% |
| `eosio.rex` | `aic.rex` | REX 托管、结果合约、手续费流转 |
| `eosio.fees` | `aic.fees` | 系统费用归集和相关通知 |
| `eosio.reserv` | `aic.reserv` | 90% PowerUp 资源保留及结果合约 |
| `eosio.powup` | `aic.powup` | PowerUp 常量/日志协议标识；须区分与实际保留账户的用途 |
| `eosio.names` | `aic.names` | 当前源码中的拍卖托管常量；将来租赁版本是否保留须按租赁设计确定 |

若未来新链直接采用短号租赁，不得为了补齐账号映射而重新启用旧拍卖，也不引入旧竞价退款或旧中标权迁移。名称租金账户应由租赁方案明确，并从所选命名清单取值。

构建产物另设清单：

| 当前构建对象 | 拟议对外名称 | 实际部署位置 |
| --- | --- | --- |
| `eosio.bios` | `aic.bios` | 根账号 `aic` 的引导阶段 |
| `eosio.system` | `aic.system` | 根账号 `aic`，替换引导合约 |
| `eosio.token` / `eosio.bpay` / `eosio.fees` | `aic.token` / `aic.bpay` / `aic.fees` | 各自映射后的账号 |
| `eosio.msig` / `eosio.wrap` | `aic.msig` / `aic.wrap` | 各自映射后的账号 |
| `rex.results` | 建议随目标命名为 `aic.rex`，最终构建名待定 | `aic.rex`；这是结果/ABI 辅助合约，不是将根账号的全部 REX 逻辑搬走 |
| `powup.results` | 建议随目标命名为 `aic.reserv`，最终构建名待定 | `aic.reserv`；PowerUp 核心业务仍在根系统合约中 |

内部 include 路径可以先保留 `eosio.system/...`，通过生成配置统一账号值；不必在同一改动中重命名所有目录。CDT 的合约注解、传入的合约选择参数、构建目标、Ricardian 文档与产物名称必须成套验证，避免生成空缺的 ABI。

## 四、源码关联及漏改后果

以下行号以本次检查的工作树为准，后续实现应按符号定位。节点路径相对于 `PureEOS-group8-baseline-20261029`，合约路径相对于 `PureEOS-bp-system-candidate-20261030`。

### 4.1 节点创世、原生动作与网络身份

| 源码定位 | 已发现关联 | 必须处理的内容 |
| --- | --- | --- |
| `libraries/chain/include/eosio/chain/config.hpp:22` | 根、空账号、委员会账号 | 建立统一命名清单，不只改根常量 |
| `libraries/chain/controller.cpp:1800`、`:2853` | 创世 proposer、原生账户及委员会权限创建 | 初始生产者、BLS/finalizer 初始化与权限全部使用同一 profile |
| `libraries/chain/controller.cpp:6191` | `onblock` 接收账号及授权 | 必须派发到新根并保持现有业务语义 |
| `libraries/chain/include/eosio/chain/contract_types.hpp` | 原生动作的系统账号定位 | 审查动作包装、ABI 解析及 handler 注册的账号来源 |
| `libraries/chain/eosio_contract.cpp:73`、`:95` | `eosio.*` 创建限制与治理封存后的保护 | 账号前缀规则改为所选系统命名空间，不能让非特权用户注册假系统账户 |
| `libraries/chain/eosio_contract.cpp:277` | 特殊权限名称前缀保护 | 这是权限语义，不可随账号前缀盲改 |
| `libraries/chain/genesis_state.cpp:49` 及 `include/eosio/chain/genesis_state.hpp` | Chain ID 的序列化来源 | 明确命名 profile 如何绑定网络身份，见下文 |

**创世身份必须单独设计。**当前 `compute_chain_id()` 序列化创世字段，系统根账号来自二进制常量，并未作为独立的命名 profile 字段进入该序列化。若仅编译不同根常量并复用同一份 genesis，不能假定 Chain ID 自动区分两个命名空间。

建议在未来独立版本中，将命名 profile 明确、确定性地纳入版本化创世身份与启动校验；同时保留现行 profile 的旧序列化字节和旧 Chain ID。具体采用新增创世字段还是版本化 profile 标识，需要专项设计和验证，不在本文中默认为已实现。未知 profile、二进制与创世 profile 不匹配的情况应在启动时清楚拒绝。

新链使用新时间和新密钥只是操作上的区分，不能替代上述一致性设计。验收必须包含“相同创世输入、错误 profile 二进制”的负向测试，以及混用节点的连接/出块拒绝行为。

### 4.2 治理与特权边界：最高优先级

`libraries/chain/upgrade_governance.cpp` 不只是引用根常量。已定位的直接硬编码包括：

- `:19–20`：永久禁用目标中的 `eosio.prods`、`eosio.wrap`、`eosio.msig`、`eosio.null`。
- `:43–49`：权限链接的根合约账号。
- `:80`、`:97–109`：系统调用者、根 owner 授权和仅创世 proposer 可执行的封存条件。
- `:115–120`：根与代币保护集合，以及通过 `starts_with("eosio.")` 搜集系统账号。
- `:129–161`：特权撤销、系统代码授权、否决权限、初始投票者。
- `:213`：根否决投票的特殊权限校验。

同时检查 `libraries/chain/apply_context.cpp:89–96`、`:231` 和 `libraries/chain/webassembly/privileged.cpp:35–36`、`:312`。这里存在直接比较 `"eosio"_n` 的特权入口及限制。

只修改 `config.hpp` 可能使新根无法完成治理封存，也可能使新系统账号漏进保护集合。必须验证新根获得正确权限、旧名称残留不能得到根特权，且原有开发者升级、审查期、委员会批准、否决、执行许可和禁止恢复系统私钥等规则不变。

### 4.3 系统合约、资产和资源

| 源码定位 | 关联及验证重点 |
| --- | --- |
| `contracts/eosio.system/include/eosio.system/eosio.system.hpp:927–939` | 完整系统账户常量及资金流目标 |
| 同文件 `:949–970`、`:1970`，`include/eosio.system/block_info.hpp:86` | 默认系统账号用于 RAM、REX、区块信息查询，不能继续从旧根读表 |
| `contracts/eosio.system/src/voting.cpp:277`、`rex.cpp:266`、`delegate_bandwidth.cpp:552` | 直接根授权与退休代币授权 |
| `contracts/eosio.system/src/upgrade_governance.cpp:64–66` | 合约侧创世根识别；须与原生治理一致 |
| `contracts/eosio.bpay/src/eosio.bpay.cpp:12`、`:24`、`:39–41` | 代币转账、根来源识别、生产者及 global 表读取 |
| `contracts/eosio.bpay/include/eosio.bpay/eosio.bpay.hpp:50` | 固定的 `eosio.token::transfer` 通知接收器 |
| `contracts/eosio.fees/src/eosio.fees.cpp:11` 和对应 header 的通知注解 | 向根系统合约转发业务及核心代币通知 |
| `contracts/eosio.system/CMakeLists.txt` | 编译目标、源文件、include、结果合约、Ricardian 依赖 |

运行在新账号上的相同业务，WASM 哈希会因常量和通知目标变化而改变。不能继续把原始已发布 WASM 哈希描述为 AIC 命名空间合约的部署哈希。

代币 symbol 为 `AIC` 和代币合约账号 `aic.token` 是两件事。当前 `AIC` 币已经可由 `eosio.token` 管理；改合约账号不能改变精度、发行量、最大供应量或奖励增发政策。

### 4.4 并行执行和本地验证

`libraries/chain/parallel_canonical_adoption.cpp:123`、`:375`、`:443` 的安全排除条件直接写有 `eosio.msig`、`eosio.wrap`。更名时这些排除必须一同映射，不能让新系统治理路径意外进入普通合约的快速提交路径。

`libraries/chain/host_context.cpp:552` 根据系统账号前缀判断 OC 白名单。账号前缀改变可能改变执行器选择、编译缓存及资源开销。还须追踪 `parallel_canonical_adoption`、ABI/native handler 识别、允许的代码哈希配置及所有相应测试夹具，记录新 WASM 的 allowlist 值。

保持所有实例并行运行，保留冲突回退、读写证明、完整授权与验证、RAM/CPU/NET 计费、原子回滚。名称更改不能通过扩大白名单、跳过检查或提高链资源上限来“通过测试”。

### 4.5 BP、最终性、发现、维护和处罚

- `libraries/chain/controller.cpp:1348` 向系统根调用 `getpeerkeys`；改名后必须仍能读取真实链上 peer-key 注册表。
- `plugins/net_plugin/net_plugin.cpp` 的 `bp-peer-key`、`peer-private-key` 及签名 gossip 需与新根注册/查询配套。不能将静态 peer 列表当作发现功能验证。
- `contracts/eosio.system/src/peer_keys.cpp`、`rotation_state.cpp`、`maintenance.cpp`、`finalizer_key.cpp` 的表多使用 `get_self()`，但仍需验证原生上下文授权、根治理及部署账号改变后的完整路径。
- proposer/finalizer 的永久 12 位身份、轮换模式、正常双策略交接、维护生效条件、奖励与历史违规保留必须不变。
- 永久黑名单、全链/最终性中断保护、无候补处理沿用当前规则；命名更改不能重置违规或放宽投票阈值。

### 4.6 RPC、cleos、SDK 与 CDT

1. `plugins/chain_plugin/chain_plugin.cpp:2783` 在 `get_account` 中固定从 `eosio.token` 读取核心币余额。漏改时可能出现链上资产正确、账户 RPC 却没有正确余额的情况。
2. `programs/cleos/main.cpp:160` 固定默认代币账号；多处系统命令使用根常量，默认帮助文本也写有 `eosio.token`。转账、买 RAM、质押、PowerUp、REX、投票、治理及部署工具须覆盖。
3. PureEOS CDT 的品牌名称不等于链上命名空间。已检查本地 `cdt-v1-hardening`：`tools/include/eosio/abi.hpp:108` 生成 `eosio::abi/...`，`tools/include/eosio/abigen.hpp` 和 `codegen.hpp` 按合约标签生成 ABI/dispatch。应保留 ABI 与 C++ 接口，再验证 `aic.system` 等新标签的产物。
4. `[[eosio::on_notify("eosio.token::transfer")]]` 会编译进派发逻辑，仅改 WASM 文件名不会更改通知来源。新通知目标必须重编译并用真实 WASM/交易验证。
5. 保留 WASM host import、协议特征名称/摘要、签名算法、`PUB_K1_`/`PUB_R1_`/BLS 编码与旧公钥文本兼容。它们不是系统账号名。
6. 钱包/SDK 的 system contract、token contract、msig contract 默认值应来自网络配置。通用 EOS 工具不应被假定会自动认出新根。

`EOSIO_DISPATCH` 等宏、`plugin = eosio::chain_api_plugin` 等插件标识也属于工具/插件接口，不是链上账号。原则上先复用已验证的 PureEOS CDT 编译新标签，不为账号更名重写 LLVM 或 ABI 格式；只有实际编译与派发检查暴露必要改动时，才给工具链做独立的最小修复。

建议公开网络元数据包含命名 profile、根账号、核心代币账号、核心币 symbol/精度、msig/wrap 账号、链 ID 和创世摘要。具体 API 形式待设计；不要假称现有 RPC 已提供所有这些字段。

### 4.7 第三方 DeFi/NFT 合约

当前功能证据中固定了 Bancor、AtomicAssets、AtomicMarket 等真实源码版本。更名前已有成功证据不能直接当作更名后成功证据。

先区分：

- 代币账号可配置：使用新配置，不必改第三方源码。
- 通知注解或根账号硬编码：需要单独的最小兼容补丁和新 WASM，保留未修改的上游源码与哈希。
- 只显示示例字符串：可在文档阶段处理，不应扩大到协议改造。

当前测试准备可见 `release-work-20261031/browser-visible-stress-1/deploy_apps_live.py` 的市场核心币配置与 Bancor 的扩展符号配置采用 `eosio.token`；`prepare_complex_live.py`、`run_live_round.py` 也固定核心币及系统根。未来需另建 AIC profile 的测试输入，原始脚本、结果及原始攻击拒绝记录保持不变。

逐一验证扩展 symbol 的 contract 部分、真实转账通知、伪造通知拒绝、白名单、内联回调、RAM payer、NFT 销售与原子回滚。兼容性不能只凭“能部署”判断。

### 4.8 PureHistory

当前 PureHistory 已有部分可配置支持，但**只改一个字段还不够**：

- `src/indexer/workers/deserializer.ts:1864` 的系统表处理器优先读 `settings.system_contract`，并据此建立 `.msig` 处理器。
- `src/indexer/modules/config.ts:50–54` 仍独立初始化 `EOSIO_ALIAS`/`eosio_alias`，默认 `eosio`；其他 parser/过滤路径仍读取旧字段。
- `src/indexer/modules/parsers/base-parser.ts:165`、`3.2-parser.ts:67` 根据 alias 识别 `.null`；`deserializer.ts:1311`、`:1555` 也直接读旧字段。
- `src/cli/sync-modules/sync-permissions.ts:10` 固定 `eosio.token`。
- `src/cli/sync-modules/sync-proposals.ts:34`、`:74`、`:91` 固定 `eosio.msig`。
- `src/indexer/helpers/system-profile.ts`、连接管理器及投票同步器已接受系统账号参数；继续保持整数单选和 legacy EOS 分别处理。

未来应统一解析链 profile，根、核心代币、多签及其他系统账号从同一配置解析。过渡阶段如仍保留两个 alias 配置字段，必须一致并在冲突时明确报错。

禁止把旧 EOS 解析路径全局改成 AIC。至少分别验证：旧 EOS profile、当前 PureEOS/eosio profile、拟议 PureEOS/aic profile。索引、MongoDB、RabbitMQ 队列及 Redis 缓存按链隔离；新链不能沿用旧缓存制造错误查询。

### 4.9 浏览器与钱包

当前 `PureEOS-explorer/src/lib/api.ts` 在以下位置固定账号：`:146` 供应量、`:155` 余额、`:173` 排除 onblock、`:215–245` 系统 ABI/状态、`:250–254` 多签。`src/lib/config.ts` 目前公开配置端点、链 ID 和 symbol，不足以替代上述系统账号配置。

未来将账号配置提升为网络 profile，配套修改页面标签、合约查询示例及完整测试。页面上的已发行量必须继续来自真实 `stat.supply`，不能在根/代币账号读失败时使用最大供应量或演示数据。

浏览器由独立工作流维护，本次不改其代码。签名钱包还须验证 action.account、authorization.actor、核心币合约、目标链 ID、ABI 缓存及多签提案中的真实接收账号。

## 五、建议的实现组织

### 5.1 独立 profile 与版本

建议建立两个明确的编译/部署 profile，命名仅为草案：

- `pureeos-eosio-v1`：现行系统账号；默认保持当前行为和已发布证据。
- `pureeos-aic-v1`：将来另行实现的 AIC 系统命名空间；必须使用配套节点、合约、CLI、创世和网络配置。

节点与合约各自使用一份生成的账号常量表，来源为同一受版本控制的语义映射清单；运行时设置项不能任意重定向链的原生根。PureHistory、钱包、脚本消费对应的公开网络清单。

不要允许“节点根为 aic、合约核心币为 eosio.token、CLI 默认另一个根”的静默混搭。构建/部署预检须验证 profile、源码、WASM/ABI、二进制、创世和参数摘要一致。

保留已发布源码、WASM、ABI、节点二进制和历史测试输入。不同命名空间的构建目录、缓存、数据目录及哈希清单分开，避免复制错误的旧产物。

### 5.2 必须保持的基线

- 原生 Savanna、现有 proposer/finalizer/轮换模式、签名 BP gossip 与 peer-key 撤销/轮换。
- 维护必须正常交接后生效，保留历史未付权益、有效违规和永久黑名单。
- 整数单选投票，无代理、无时间加权；现有治理及 14 天升级审查规则。
- 10% 质押/REX 与 90% PowerUp；RAM 初始 64 GiB，通过初始化动作设置每块增长 1024 字节。
- 精确 1% 按当前已发行总量计提，额外 4% 不发行；364 天计提年、1461 天减半、按产块分配及大于 24 小时领取间隔。
- 测试币种按当前配置为初始 1 亿 AIC、最大 21 亿 AIC；不预发上限空间。
- CPU16 (`3200000`)、当前开发预设 NET4MiB/块（吞吐验证中）、P2P64MiB/消息、1GiB 生产交易队列，现有单交易上限和计费。
- 全部 live nodeos 使用并行模式，保留 8 物理核/16 线程/64 GB 硬件下限，禁止显式 serial-only。
- 两个 deferred-disable 阶段继续启用；不新增 send_deferred/cancel_deferred 的业务或测试场景。

系统保留账号是 12 位普通账号规则的明确例外。未来即使根为 `aic`，也不能把所有系统名强行填成 12 位或与租赁别名混用。

## 六、分阶段计划与交付门槛

| 阶段 | 工作 | 交付/进入下一阶段的条件 |
| --- | --- | --- |
| P0 范围与清单 | 确定账号映射、保留接口、创世身份方案、构建标签 | 语义清单和未决项明确；当前基线保持原样 |
| P1 节点候选 | 根常量、原生 handler、治理、特权边界、并行排除和 RPC/cleos | 独立源码/构建，关键正负向单元与组件检查通过 |
| P2 合约候选 | 常量、授权、通知、构建/ABI/Ricardian、结果合约 | 远端编译；新 WASM/ABI 摘要完整且部署来源可追溯 |
| P3 新链引导 | 独立创世、完整系统账户、12 位非系统账号、RAM/资源/治理初始化 | 出块、最终性、封存和所有资金路径正确 |
| P4 外围兼容 | PureHistory、钱包、浏览器、第三方固定版本合约 | 双命名 profile 与 legacy EOS 的边界符合设计，历史无漏项 |
| P5 联合资格验证 | 下表完整验证与故障恢复 | 保留原始失败及定向重试，形成最终报告 |
| P6 基线决策 | 检查证据和发布清单 | 通过资格验证后再决定是否纳入新链开发基线；不自动替换现行版本 |

构建、修改和测试实现工作均应在选定的远端测试服务器进行。本计划不创建服务器、不启动压力负载；若将来需要额外机器或 TPS 范围，应按该轮明确任务安排。

## 七、资格验证矩阵

以下均为将来要执行的项目，当前没有 AIC namespace 的 PASS 结论。

| 编号 | 场景 | 验收重点 |
| --- | --- | --- |
| NS-01 | 命名清单完整性 | 所有系统账号、部署地址、标签及客户端网络配置可追溯到 profile |
| NS-02 | 创世身份 | 重复计算确定；旧 profile 原字节保持；新命名空间与网络身份明确绑定 |
| NS-03 | 错误二进制/profile | 启动时拒绝错误匹配，不在同 Chain ID 下静默产生不同根状态 |
| NS-04 | 原生账号 | 根/null/prods 初始化正确；非系统账号保持 12 位；系统前缀不可被普通用户占用 |
| NS-05 | 原生权限 | owner/active、updateauth/linkauth、eosio.code/eosio.any 语义正确，拒绝伪造特殊权限 |
| NS-06 | 旧根隔离 | 残留 eosio* 名称不获得新链根特权；错误接收账号不能进入 privileged intrinsic |
| NS-07 | 治理封存 | 创世条件、系统保护集合、特权撤销、不可恢复系统私钥规则一致 |
| NS-08 | 治理全流程 | 提案/审查/委员会批准/否决/许可执行及多签/wrap 限制，拒绝绕过路径 |
| NS-09 | 代币 | aic.token 创建、发行、转账、stat/accounts、最高供应限制和总量守恒 |
| NS-10 | 账户 RPC/CLI | get_account 核心余额、核心 symbol、cleos 默认合约、ABI 缓存和帮助内容一致 |
| NS-11 | RAM | 初始量、setramrate(1024)、买卖及手续费接收账号和权限正确 |
| NS-12 | 资源 | 10/90 份额、质押/赎回、REX、PowerUp、保留账号、费用和结果通知正确 |
| NS-13 | 奖励 | 激活钟、精确 1%、减半、区块份额、领取间隔、错误领取回滚，不出现额外 4% |
| NS-14 | Savanna 与轮换 | proposer/finalizer 正常交接，永久身份和委员会策略不变 |
| NS-15 | 发现 | 从新根读取 peer-key；签名 gossip 发现、撤销、轮换和伪造消息拒绝 |
| NS-16 | 维护与处罚 | 维护交接、未付权益、正常缺岗违规、永久黑名单及全链中断保护一致 |
| NS-17 | 并行安全边界 | 新 msig/wrap/根不误入普通快路径；冲突回退、授权、RAM payer 和原子回滚不放宽 |
| NS-18 | 第三方合约 | 固定版本 DeFi/NFT 真实内联转账、通知、交易和失败回滚；区别上游与补丁产物 |
| NS-19 | PureHistory 三配置 | legacy EOS、PureEOS/eosio、PureEOS/aic 的根/token/msig 解析及 CLI 同步正确 |
| NS-20 | 历史完整性 | 从创世回填、按交易 ID 查询、内联动作、账户/投票状态与链一致，不能只看区块头高度 |
| NS-21 | 钱包/浏览器 | 供应量、余额、系统表、多签、ABI、签名目标、链 ID 与错误提示正确 |
| NS-22 | 重启/重放/快照 | 相同 profile 恢复状态一致；不混用旧命名空间数据库或编译缓存 |
| NS-23 | 多节点 | 21 个活跃成员与候补的既有策略条件下，同 profile 同步和交接正常，错误 profile 明确隔离 |
| NS-24 | 资源与禁用项 | 固定 CPU/NET/P2P/队列、并行硬件检查和 deferred-disable 保持，不借放宽参数通过 |
| NS-25 | 短号组合版本 | 若以后组合租赁：新系统保留名不能抢注，租约到期/转让不能影响系统和 BP 身份 |

不为本计划重复 live serial 对照或未经请求的 sub-21 场景。多进程共享硬件只能提供功能证据，不能当成多机器性能资格。

若将来另行测 TPS，纯转账与 90%/10% 稳定性负载分开，准备步骤移出计时。名称更改本身不代表性能提升；未测量时须标为未测量。

## 八、证据、发布与回退

每个候选需保留：实际 CPU/物理核/线程/内存和 Ubuntu 版本、节点源码提交、编译选项、二进制摘要、CDT 版本、每份 WASM/ABI 摘要、命名 profile、创世与链 ID、完整参数、测试范围和原始失败/重试原因。结果按机器与版本保存在 `performance-results/`，远端保留原始证据。

新命名空间不能直接打开旧链数据库或对旧链日志做原地“改名重放”。候选使用独立数据/索引/缓存目录；旧链、旧合约和旧证据继续保留。回退应启动相应 profile 的原目录及原配套服务，而不是将同一数据目录反复交给两个命名空间的二进制。

PureHistory 新链配置须绑定明确 Chain ID，保持旧 EOS profile。API 路径、ABI 编码、密钥格式和工具源码接口尽量稳定；兼容例外以已验证清单列出，不能笼统宣称所有 EOS 应用自动兼容。

## 九、本次源码检查范围与未决项

本次只读检查的主要工作树：

| 工作树 | 检查时 HEAD | 说明 |
| --- | --- | --- |
| `PureEOS-group8-baseline-20261029` | `4a117a16fdb2cffac1d12e36432d6bc6d7ce09ee` | 用于定位节点依赖；实际二进制编译来源还须查发布清单 |
| `PureEOS-bp-system-candidate-20261030` | `d126cae28057edfcd0e2eb84654afd8efd6da257` | 当前合约工作树；实际部署 WASM 的来源和摘要独立记录 |
| `PureHistory` | `dbaf7e3eaf2987c52d6da4c3134759c5dad8140f` | 当前历史服务源码 |
| `cdt-v1-hardening` | `714543c5a001ac4a9f5b2baa4b55fb5064dadd62` | 用于检查本地 ABI/codegen 接口，不等于宣称该树为当前发布 SDK 的精确构建来源 |

仍待商议：

1. 是否实施 AIC 系统命名空间，及其与短号租赁功能的先后/组合关系。
2. 对外系统合约标签与辅助产物的最终名称、内部 include 目录是否另行更名。
3. 创世 profile 的确定性身份方案与版本兼容策略。
4. 将来新链实际启用的系统账户清单；不能只依据源码常量假定全部存在。
5. 客户端和第三方合约兼容范围、发布节奏及相应验证资源。

**本轮继续按现有 eosio 基线测试和重置服务器链；本文件不构成更名实施、上线或基线晋升记录。**
