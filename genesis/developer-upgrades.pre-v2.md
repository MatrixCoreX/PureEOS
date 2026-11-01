# 开发者升级与节点否决（原生创世 profile 2）

本轮规则：普通用户不再有系统治理提案入口；开发者提交指定版本，固定公示 14 天后执行；节点只保留反对权，不再通过 BP 多签任命、升级或管理系统参数。节点不投票不等于赞成，但不会计入反对票，也不需要签署批准。

这是一套**显式启用的新链规则**，不是对既有链的自动迁移。仅部署新的 `eosio.system.wasm` 不足以启用原生保护。不要修改已有数据目录的 genesis 来切换模式。

## 当前规则及待确认配置

- 否决名单：提交时按现有投票排名选出最多 210 个合格候选，不是当班 21 人。资格要求有效注册、正票数、可用出块授权、已注册有效 BLS 密钥。票数相同按账号排序。独立开发者账号不占否决席位；不按服务器是否在线临时缩小分母。
- 不足 210 个时，当前实现按实际入围人数 `N`，门槛为 `ceil(2N/3)`；210 人为 140 票，1/2/3/4 人分别为 1/2/2/3 票。此口径是对“节点总数”的实现解释，**固定 210 分母还是实际人数仍待用户确认**。禁止把该公式用于 Savanna 最终确认，后者没有被修改。
- 尚无合格报名者且唯一在岗生产者仍是创世 `eosio` 时，名单暂为 `eosio` 一人；成熟链没有合格候选时拒绝发布，不降级为当前 21 人。
- 公示期固定为 **14 天，即 1,209,600 秒**，从发布时的链上区块时间起算。原生内核强制校验；`govinit` 保留 `review_seconds` 字段以保持动作编码，但只接受 `1209600`，不是可配置参数。开发者、节点以及升级后的系统 WASM 均不能自行缩短该公示期。
- 每次只允许一个待处理升级。目标账号、旧/新 WASM 哈希、旧/新**二进制 ABI** 哈希、公示文档哈希、名单和截止时间均固定。反对票不可撤销且每人只计一票。
- 按链上区块时间计算公示期：截止前可以反对，截止时刻起才可以执行。到期时反对票未达到 `ceil(2N/3)`，开发者即可执行；**零反对票可以升级，但也必须等满 14 天**。无需批准票，不会因为无人投票而卡住，也不会自动安装，仍由开发者提交执行交易。达到门槛后永久阻止该提案执行。开发者可取消后重新发布，但必须重新走完整 14 天公示期、重新计票。
- 不能移除受保护账号的系统代码。任何一项执行失败，代码、ABI 和提案状态一起回滚。

哈希保证“执行包与公示包相同”，不保证内容安全，也不保证文件可下载。发布者必须公开可获取的 WASM、ABI、源码提交、构建方法、变更说明及数据兼容/回滚分析；节点需主动审核。新固件只能核验链上规则，无法阻止运营者在链外协商另起硬分叉。

## 为什么不能只修改提案合约

旧系统还存在直接 `setcode/setabi`、账号权限修改、`eosio.prods` 自动授权、特权 `eosio.wrap` 转发等路径。本实现同时处理：

| 位置 | 本轮改造 |
| --- | --- |
| `libraries/chain/upgrade_governance.cpp`、对应头文件 | 原生初始化、开发者发布/取消/执行、节点反对、冻结名单与哈希、一次性执行许可 |
| `controller.cpp`、`genesis_state.cpp` | profile 2 显式启用；不再创建或更新可用的 `eosio.prods` 管理授权；注册新状态和动作 |
| `eosio_contract.cpp` | 受保护账号的代码/ABI 只能由经过公示的原生执行入口修改；封锁权限树改写 |
| `apply_context.cpp`、`host_context.cpp` | 拒绝旧 `eosio.msig/eosio.wrap` 动作；保护名单刷新顺序；治理动作必须为顶层交易，特权转发不能冒充签名 |
| `transaction_context.cpp`、对应头文件 | 保存交易内的一次性升级许可与名单刷新标记；中断重试时清空，禁止复用前次执行残留 |
| `webassembly/privileged.cpp`、WASM 接口与运行时注册 | 提供受限名单刷新接口；封存后只有 `eosio` 保留特权，禁止重新赋予其他账号特权 |
| 快照相关文件 | 快照版本 10 保存原生治理状态；读旧快照时不自动启用治理 |
| `chain_plugin`、`chain_api_plugin` | 只读 `/v1/chain/get_upgrade_state`，公开名单、反对票、版本哈希及执行时间 |
| `reference-contracts/.../upgrade_governance.cpp` | 从真实 producers/finalizers 表确定性筛选名单；不接收调用者指定名单 |
| `reference-contracts/.../eosio.system.hpp`、`finalizer_key.cpp`、CMake | 原生动作 ABI、profile 2 编译开关及原生 Savanna 识别 |
| `unittests/pureeos_governance_tests.cpp` | 原生边界与真实系统 WASM 集成测试 |
| `scripts/pureeos_upgrade.py` | 离线生成未签名升级动作，不签名、不广播 |
| `tutorials/bios-boot-tutorial/bios-boot-tutorial.py` | 拒绝用旧启动教程初始化原生创世，拒绝把管理权交给不可满足的 `eosio.prods` 权限 |

原生治理数据不是 WASM 多索引表，不能通过旧系统合约修改表内容绕过。`eosio.system` 的新名单接口仍属于受审核系统代码的一部分；不能部署任意可手工填写名单的启动合约。

## 初始化流程：只适用于新的隔离链

1. 将 `initial_savanna.version` 设为 `2`；密钥、BLS PoP、单节点启动方法见 [原生创世说明](native-savanna.md)。版本参与 chain ID，profile 1 保持原规则。
2. 使用带本轮内核改造的 nodeos。编译系统合约时加 `-DPUREEOS_UPGRADE_GOVERNANCE=ON`。该 WASM 引用新宿主函数，不能部署到旧 nodeos。
3. 保持创世生产者只有 `eosio`。先创建所有需要的系统账号和独立开发者账号，部署正确合约、创建并初始化代币/资源系统、准备必要的 RAM/CPU/NET。不要沿用最后将 `eosio` 管理权交给 `eosio.prods` 或 `eosio.msig` 的旧启动脚本。
4. 为开发者建立独立的 `owner`、`active` 和 `upgrade` 权限/安全密钥；`upgrade` 的父权限为 `active`。不要复用出块密钥，不要引用即将封存的系统权限。未来交给 AI 时，修改这个开发者账号的 `upgrade` 授权或签名服务，而不是恢复节点治理。开发者自行保管 owner 的恢复能力。
5. 在测试网确认系统合约是正确的 profile-2 构建，备份启动清单，明确全部额外系统目标后，由 `eosio@owner` 执行一次：

   ```text
   eosio::govinit(developer, 1209600, protected_accounts)
   ```

   `protected_accounts` 是额外保护账号，不需重复全部 `eosio.*`。系统自动加入现存 `eosio.*`、`eosio`、`eosio.token`，总数最多 64。

6. 初始化同时要求已部署系统 WASM 完成名单刷新握手；误装旧系统/启动合约会使整笔初始化回滚，不会只封存权限。握手不代替源码审计。等待初始化交易不可逆，查询治理状态和各账号权限，再开放后续节点。初始化不是日常功能，不能重复运行。

**封存不可逆：**受保护账号的 owner/自定义权限清空为不可满足授权，active 只保留必要的合约代码授权以维持资金/资源操作。`eosio.prods`、`eosio.msig`、`eosio.wrap`、`eosio.null` 不保留此能力。创世 `eosio` 原始密钥仅保存在专门链接 `devveto` 的 `eosio@veto` 中，不能用于升级、改权限或设置参数。之后不能直接新建 `eosio.*`、更换保护名单、开发者账号名称或公示时长；后续如需改变这些内核规则，要另行设计协议升级，不能假称 WASM 升级即可完成。

封存后旧的 `setparams/setpriv/rmvproducer/setinflation/cfgpowerup` 等管理动作不能由 BP 或开发者直接签名调用。自动执行的系统逻辑、用户操作自己的资源和节点管理自己的普通账号不受此概念上的取消治理影响；参数变化需通过经过审核的新系统代码落实。

普通账号仍可部署自己的应用、管理自己的权限、使用自己的钱包多签。本轮禁止的是系统治理提案与系统多签升级，不是禁止全链出现任何形式的应用投票。

## 日常升级

读取 `/v1/chain/get_upgrade_state` 与目标的 `/v1/chain/get_raw_abi`，保存响应供离线工具使用；查询不是最终确认凭证，应核对不可逆状态及 chain ID。

```bash
python3 scripts/pureeos_upgrade.py \
  --state upgrade-state.json --base eosio-raw-abi.json --target eosio \
  --wasm release/eosio.system.wasm --abi release/eosio.system.abi \
  --review release/review.md --cleos build/programs/cleos/cleos
```

工具输出一个包含 `refreshgov`、`devpropose` 的未签名动作组合，两者必须同一交易相邻执行。核验后自行通过 cleos/钱包填充交易头、签名并提交，声明 `developer@upgrade`。不要把两个动作分开广播。普通用户即便调用名单刷新，也没有发布升级的权限。

节点反对：提交 `eosio::devveto(voter, proposal_id)`，由本人的账号授权；创世 `eosio` 必须声明 `eosio@veto`。14 天公示到期后，开发者可用同一工具的 `--execute` 模式准备执行包；此模式仍只输出未签名动作，内核会再次检查时间、反对票及所有哈希。

在正式签名前检查 endpoint、chain ID、提案编号、目标、完整公示时间、精确二进制哈希。`review_document_hash` 不会由工具猜测为网页 URL 的哈希，而是输入公示文档的实际字节哈希。

## 构建与测试

在 `reference-contracts` 目录：

```bash
cmake -S contracts -B build/pureeos-governance \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_TOOLCHAIN_FILE=/home/guagua/.local/opt/cdt-4.1.1/usr/opt/cdt/4.1.1/lib/cmake/cdt/CDTWasmToolchain.cmake \
  -DPUREEOS_UPGRADE_GOVERNANCE=ON
cmake --build build/pureeos-governance --parallel 1
```

在 `PureEOS` 目录：

```bash
cmake -S . -B build \
  -DPUREEOS_GOVERNANCE_CONTRACTS_DIR=/home/guagua/AntelopeIO/reference-contracts/build/pureeos-governance
cmake --build build --target native_savanna_test nodeos --parallel 2
ctest --test-dir build -R '^(native_upgrade_governance|native_upgrade_governance_contract|native_savanna_genesis|native_genesis_blocklog_compat)$' --output-on-failure
python3 tests/pureeos_upgrade_helper_test.py
python3 tests/native_savanna_genesis_test.py --build-dir build --profile 1
python3 tests/native_savanna_genesis_test.py --build-dir build --profile 2
```

CDT 与仓库路径按实际环境调整。旧快照可读取但没有治理对象；所有新导出快照为版本 10，旧二进制不能读取。本实现尚不提供已有链 profile 1 → profile 2 的迁移，也没有替代整套 210 候选顺序轮换、故障检测和奖励改造的后续工作。

### 本轮验证结果（2026-10-04）

- `nodeos`、聚焦测试目标和 CDT 治理模式全部合约构建成功；旧模式 `eosio.system` 构建通过。
- 固定 14 天后的四组 CTest 共 126 个用例、2574 个断言通过，其中治理相关 24 个用例、611 个断言，包含真实系统及代币 WASM。覆盖非 14 天初始化被拒绝、零反对票仍不能提前执行、恰好到期可以执行、到期停止接收反对、未达/达到 2/3 门槛的区别，以及取消重发后必须重新公示完整 14 天。
- 离线工具 3 个 Python 用例、相关 Python 语法检查及两个仓库的 `git diff --check` 通过。
- 真实 nodeos 的 profile 1、2 均通过创世、重启、重放及治理状态 RPC 检查，每阶段至少新增 16 个不可逆块。进程测试尚未执行封存后完整部署流程；封存和升级由原生/真实 WASM 集成测试覆盖。
- 实际测试运行时为 eos-vm-jit；执行许可在交易中断重试时的清理已做模拟中断测试，未构建运行 eos-vm-oc。

未部署正式链。前批 `test_producer_plugin` 批量交易压力用例仍有未解决失败，本轮没有复测或解决，不宣称全项目回归通过或生产可用。更完整实施记录见工作区最终计划第 10.3 节。

### 2026-10-05 本地真实合约部署补充验证

`tests/pureeos_single_choice_local_test.py` 在独立回环地址 profile-2 新链上，实际部署整数 v2 系统合约、token、bpay 及 REX 结果合约，完成资源准备和 `govinit` 封存。已验证普通用户不能发布升级、开发者不能直接 `setcode`、开发者可发布、冻结名单来自整数排名、公示字段固定 14 天、不能提前执行、合格节点可反对和开发者可取消。

测试还完成封存后链的正常重启、区块日志重放及治理状态一致性检查；LIB 每阶段继续推进，所有测试进程结束后停止。未等待真实 14 天到期，未缩短期限、修改系统时间或放宽权限；精确到期行为仍由已有 WASM/内核测试覆盖。

本次修复了 `cleos push transaction` 收到失败执行 trace 却返回退出码 0 的客户端问题；链上权限拒绝本身正确。独立测试通过 30 项检查，复现方法见 `reference-contracts/tests/SINGLE_CHOICE_VOTING.md`。这不代表自动轮岗、动态扩容和永久黑名单已完成。

### 可选 10% 长期资源／90% PowerUp 配置的启动顺序

启用 `PUREEOS_RESOURCE_SPLIT` 的系统合约必须使用新链，顺序如下：

1. 在封存前创建 `eosio.reserv`，部署现有 `powup.results` 结果合约；创建代币及其他系统账号。
2. 部署系统合约并 `init`，初始化整数投票策略与 `respolicy` 资源总账。
3. 为储备账号购买足够 RAM（本地测试使用 100 SYS），为普通账号准备正常 RAM 和 CPU/NET 资源。储备账号仅保管 PowerUp 权重，不给它委托质押或租入 REX 资源。
4. `eosio` 调用 `cfgpowerup`，设定价格、最低付款和租期。此配置中 CPU/NET 比例固定 10%，质押基数来自总账，不能填写任意假设值。示例价格仅用于测试，不是正式经济参数。
5. 检查 `respolicy`、`powup.state` 及底层实际权重，再调用 `govinit`；漏配 PowerUp 会连同原生权限封存一起回滚。

封存后的资源交易和有界到期清理无需开发者签名或节点批准。此次只有系统合约和测试/构建配置改动，不新增修改底层资源调度或共识。已有封存链若缺少 `eosio.reserv` 或资源总账，不能直接安装这个 WASM；必须另行设计状态迁移，不得在启动脚本中跳过保护检查。

构建、隔离部署及对账测试见 `reference-contracts/tests/SINGLE_CHOICE_VOTING.md` 的资源配置章节；实时测试添加 `--resource-split`，不加时仍是之前未启用 PowerUp 的测试配置。
