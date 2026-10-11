# 多核交易执行开发分支与回退

更新日期：2026-10-12。

本目录是 `feature/parallel-tx-execution` 分支的实施记录。已建立基线/访问审计、主键状态隔离、私有 WASM 工作池，并实现默认关闭的受限区块并行验证及生产：成功试算可在原事务中按序采用，不再全部串行重跑。独立 `nodeos-parallel` 已完成旧新验证/生产、恢复与历史输出对照。**PX0–PX6 受限首版实现、本地测试与性能评估已收尾；性能推广门槛并非全部通过，结论是保持实验默认关闭，不上线现有链。**

正在按用户要求继续推进 [PX7 通用合约并行扩展](PX7_GENERAL_EXECUTION_PLAN.md)，边研究、开发和修订计划。首批扩展是精确主键/查询依赖及有界稀疏状态捕获；这些改动的测试证据与下述 PX6 历史记录分开保存，不能用旧测试冒充新代码验收。

G1/G2 已完成本批验收：同表不同记录真实采用、按查询复制、双重扣款拒绝及回滚、有界退回原路径；单元/核心/TSan/真实进程均重复三次，21 组原路径兼容检查通过。具体计数、归档和当时未完成边界见 [G1/G2 记录](PX7_G1_G2_RESULTS.md)。该阶段尚未涵盖真实资产合约、调用树、RAM 生命周期及二级索引，后续进度如下。

G3a 已完成：补齐内存/断言基础接口与逐次内存增长保护，真实 CDT dispatcher/`multi_index`/动态分配合约的既存行更新在 1/2/4 worker 下被正式采用。核心、普通 worker、ASan/UBSan/LSan、TSan 和新旧验证/生产进程均复测 3 次；保留测试中发现的夹具问题与修正，详见 [G3a 验收](PX7_G3A_RESULTS.md)。

G3b 已验收：账户存在性依赖及原序无代码通知接入后，原版 EOS token 与当前 PureEOS token 的既存余额转账已在 1/2/4 worker 下实际采用；通知回执、RAM、失败撤销与原串行核对。核心/工作池/两类 sanitizer 三轮、真实 token 生产互验、恢复和兼容检查的计数及产物见 [G3b 验收](PX7_G3B_RESULTS.md)。该阶段尚未覆盖带代码通知、创建/删除行、payer/长度变化、inline/sync-call。

G4a 已验收：既存行可改变长度及 RAM payer；所有中间更新与通知按原调用顺序重放，修复了 RAM 失败时通知 trace 不一致的问题。核心/工作池/两类 sanitizer 三轮、真实 EOS/PureEOS token RAM 转移、新旧重放及兼容回归见 [G4a 验收](PX7_G4A_RESULTS.md)。

G4b 已验收：主键创建/删除、表生命周期、私有临时身份与正式原序分配接入；实际 token 首次收款及 open/close、RAM/权限失败、迭代器、分叉/快照/重启差分通过。三轮核心每轮 6,808 断言，其他测试、磁盘保护失败及归档恢复说明见 [G4b 验收](PX7_G4B_RESULTS.md)。多 action、带代码调用树和二级索引继续推进 G5–G7。**这不是全面合约并行完成，也不是默认开启或上线许可。**

G5a 隔离层基础已验收：多个顶层 action 可共享同一私有覆盖层，保留动作级输出/游标、全交易预算与全失败丢弃。普通/ASan/TSan 三轮及原路径回归见 [G5a](PX7_G5A_RESULTS.md)。它尚未接入正式多 action 工作池和采用入口，不把私有执行器通过当作多 action 已能并行上链；继续推进 G5b 的正式采用与 CPU 累计计费。

PX6 收尾版本（`0068f79`）的核心验证 8 用例/851 断言、生产端 6 个功能用例/542 断言、21 组兼容矩阵均独立重复三次通过。该版真实验证进程每次 55 阶段、生产者旧/新 0/1/2/4 五模式也各三次通过，包含旧程序完整重放新产块。14 组原业务与基础回归通过；180 轮/2,880 笔长驻进程测试通过。普通 worker、ASan/UBSan/LSan 与私有模块 TSan 均三次通过。**并非所有 EOS 合约都支持并行，没有生产容量或云服务器 TPS 提升承诺。**

正式性能对照保留 160 样本、每组 8 次：独立已有行负载 4 worker 相对旧版完整验证约 1.354×，热点约 0.730×；关闭模式也未在所有负载证实达到 5% 退化门槛。详细样本、波动和部署决定见 [最终评估](PX6_RESULTS.md)，实际支持/回退边界见 [验收范围](ACCEPTANCE_SCOPE.md)。

## 1. 原分支已经保存到哪里

工作区根目录 `AntelopeIO/` 不是 Git 仓库，原生程序和合约是两个独立仓库。创建开发分支前，两者均在 `main`，此前的业务改造还未提交。

本次将原有源码内容原样保存为本地 checkpoint；没有修改它们的业务语义，也没有推送远端。

| 仓库 | 原分支及基线提交 | 当前用途 |
|---|---|---|
| `PureEOS` | `main`：`a4a40a6a1e72c531cd8267758074229ccc0655ce` | 原生回退基线；后续开发在 `feature/parallel-tx-execution` |
| `reference-contracts` | `main`：`c454378bf2efc4e15fa2c55169ba4749795f3534` | 与原生基线配套的合约；本专项暂不修改 |

两个仓库均有本地标签 `baseline/pre-parallel-20261011`，分别指向本表提交。即使以后 `main` 前进，标签仍可用于定位今天保存的代码；不移动或覆盖这个标签。

原生 checkpoint 包含此前 113 个变更文件，合约 checkpoint 包含 31 个变更文件。`PureEOS/config-dir/config.ini` 保留原状且未纳入 Git；构建产物、私钥/钱包、运行数据库和手动链不在这次源码提交范围中。仓库中原有测试夹具仍按原状保存。

没有创建与这些仓库竞争的父级 Git 仓库，也没有自动合并、重写或推送远程历史。

## 2. 计划与审计资料

- [完整多核实施计划快照](plan-snapshot/PureEOS_多核并行交易执行_实施计划.md)
- [性能总计划快照](plan-snapshot/PureEOS_性能优化与TPS提升_实施计划.md)
- [PX0 首批访问审查记录](PX0_ACCESS_REVIEW.md)
- [受限并行路径逐入口边界复审](PX0_RESTRICTED_BOUNDARY_REVIEW.md)
- [首批测试与实际分支回退验证](VALIDATION_20261011.md)
- [第二批：C++ 主键读取原型与 19 块串行对照](PX1_PRIMARY_READ_PROTOTYPE.md)
- [后续：有界主键状态隔离基础与 SoftFloat 线程状态修复](PX2_ISOLATION_FOUNDATION.md)
- [继续推进：私有迭代器、独立 WASM 执行器与实测边界](PX2_WASM_WORKER.md)
- [最新：有界工作池、顺序校验与正式采用接入](PX2_ORDERED_BATCH.md)
- [区块验证真实采用、错误块撤销与恢复差分](PX3_VALIDATION.md)
- [真实 nodeos、新旧 P2P、恢复与历史输出](PX4_PROCESS_VALIDATION.md)
- [真实进程发现并修复的 HTTP 区块缓存问题](HTTP_BLOCK_CACHE_FIX.md)
- [生产端批次与计费设计](PX5_PRODUCER_DESIGN.md)
- [生产端核心采用、计费与进程验收](PX5_ADOPTION.md)
- [指定高度暂停后恢复调度修复](PAUSE_RESUME_FIX.md)
- [PX6 测量口径与启用门槛](PX6_MEASUREMENT_PROTOCOL.md)
- [PX6 最终测量、稳定性和默认关闭决定](PX6_RESULTS.md)
- [PX0–PX6、T01–T26 的首版覆盖与限制](ACCEPTANCE_SCOPE.md)
- [PX7 通用合约并行扩展滚动计划](PX7_GENERAL_EXECUTION_PLAN.md)
- [PX7 G3a：真实 CDT 基础兼容与检测记录](PX7_G3A_RESULTS.md)
- [PX7 G3b：真实 token 转账与无代码通知](PX7_G3B_RESULTS.md)
- [PX7 G4a：既存行长度、RAM payer 与失败顺序](PX7_G4A_RESULTS.md)
- [PX7 G4b：主键/表生命周期、正式 ID 与恢复](PX7_G4B_RESULTS.md)
- [PX7 G5a：多 action 私有交易基础与隔离检查](PX7_G5A_RESULTS.md)
- [独立实验、关闭与回退操作](OPERATIONS.md)
- [源码指纹和候选访问清单](access-inventory.json)
- 审计工具：`scripts/parallel_access_audit.py`
- 工具测试：`tests/parallel_access_audit_test.py`

`plan-snapshot/` 保存的是开始本分支时原计划的字节一致副本。原文中的路径以 `AntelopeIO/` 工作区为参照，原文日期、状态和测试记录是当时事实，不代表本分支后续进度；进度以本 README 与阶段记录为准。

两份原始文件仍留在工作区根目录，没有改写。Git 中的新副本使计划不再仅依赖仓库之外的文件。

## 3. 首批实施范围

已经建立源码扫描与变化检测：

1. 扫描 chain、chaindb 和四类相关插件的生产源文件。
2. 记录数据库引用、访问器、可能的读写操作、索引/undo/模式操作及 EOS VM host 注册位置。
3. 对扫描范围内所有文件保存 SHA-256；即使某次间接调用没有命中词法规则，文件变动仍会触发快照不一致。
4. 检查新增/删除文件、扫描工具本身变化、登记内容变化；`--check` 只读，不自动更新或批准快照。
5. 所有 host 注册的并行安全审查仍为 `pending`，实际批准的并行入口数为 0。

工具是防遗漏的审查辅助，不是完整 C++ 静态分析器。别名、宏展开、模板、回调和动态调用关系仍需人工核实。扫描快照一致不等于这些接口已完成隔离或线程安全验证。

受限首版的逐入口边界已有独立复审：当时私有 15 个入口可试算，其余原生 host 一律不能链接到 worker。PX7 G3a 将这一区分扩展为 23 个受限入口、10 个“调用必整笔回退”的显式导入桩和 229 个拒绝链接入口；扫描工具中的 pending 仍不是运行许可。固定业务区块已归档并由保留的旧二进制独立执行。将来扩展主键增删、二级索引及跨合约能力仍需逐项审查，不能沿用这一小范围结论直接开放。

## 4. 如何运行本批检查

在 `PureEOS` 仓库内：

```bash
python3 -B tests/parallel_access_audit_test.py -v
python3 -B scripts/parallel_access_audit.py
python3 -B scripts/parallel_access_audit.py --check docs/parallel-execution/access-inventory.json
```

`--check` 退出 0 表示与快照相同，1 表示源码/工具/范围发生变化需要复审，2 表示输入或读取错误。它不会写文件、修改节点配置、联网或自动启用任何能力。

新增两个 CTest 名称：`pureeos_parallel_access_audit_unit`、`pureeos_parallel_access_audit_snapshot`，标签 `parallel-px0`。重新配置构建目录后可运行 `ctest --test-dir <构建目录> -L parallel-px0 --output-on-failure`。这些工具检查不能代替原生执行、WASM、分叉或 TPS 测试。

更新快照前必须审查差异和测试；不要用自动重新生成快照来消除一个尚未理解的告警。

## 5. 回到现在保存的代码

当前开发分支的阶段变更会保存为独立本地提交。回退前先检查工作区；若出现尚未提交的新工作，先保存到开发分支，不使用 `reset --hard` 或强制 checkout 丢弃它们。

在 `AntelopeIO/` 工作区执行：

```bash
git -C PureEOS status --short
git -C PureEOS switch main
git -C PureEOS log -1 --oneline
```

本次保存后，`main` 的预期提交是 `a4a40a6`。未跟踪的原有 `config-dir/` 会保留，不属于需要删除的开发残留。合约仓库一直停留在 `main` 的 `c454378`，本批不需要切换它。

如果以后 `main` 又有新提交，但希望取回 **2026-10-11 的精确源码基线**，在保存好当前工作后，从标签建立一个新的恢复分支：

```bash
git -C PureEOS switch -c recovery/pre-parallel-20261011 baseline/pre-parallel-20261011
```

若该恢复分支名已存在，先检查它，不强制覆盖。只有合约仓库以后也发生改造时，才需要在该仓库用同名标签建立对应恢复分支。

回到开发工作：

```bash
git -C PureEOS switch feature/parallel-tx-execution
```

**切 Git 分支只恢复源码，不会替换正在运行的 nodeos 二进制，也不会回滚链数据库或链上合约。** 今后实验必须继续使用独立构建/数据目录。若实验状态被错误执行污染，应从可信快照/区块串行恢复，不能仅切分支继续使用该状态。

此前已验证的 nodeos SHA-256 为 `e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c`。持久化归档仍位于 `PureEOS/build/local-tests/single-thread-opt-20261010-AnX1IE/release-artifacts-e28d8313.tar.gz`，原验证 manifest 在同目录；本批没有覆盖该归档或部署新程序。

## 6. 实施约束

- 后续源码变更只进入开发分支，按 PX 阶段保存本地提交；未经确认不合并回 `main`、不推送远端。
- 原分支保留完整旧功能，不因开发需要删除既有测试或修改旧用例预期。
- 系统合约暂不开发并行功能；若以后确需改造，先在该仓库另建开发分支，不能直接改基线 `main`。
- 手动测试链、钱包、运行配置和已有链数据保持不动。
- 关闭并行仍走原串行路径；运行参数默认 0 worker，实际参数及互斥门槛见操作说明。生产端需另加显式开关；本轮性能门槛未全部通过，不推荐部署生产网络。
