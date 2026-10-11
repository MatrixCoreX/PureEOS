# 多核交易执行开发分支与回退

更新日期：2026-10-11。

本目录是 `feature/parallel-tx-execution` 分支的实施记录。已建立 PX0 基线/访问审计、PX1 主键读取原型及 Savanna 串行对照，并继续开发有界状态副本和私有更新层；同时修复 SoftFloat 构建中的线程状态隔离问题。**没有开启普通交易并行，未部署新 nodeos 或修改系统合约。PX0 全入口审查、PX1 正式接入及 PX2–PX6 尚未完成。**

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
- [首批测试与实际分支回退验证](VALIDATION_20261011.md)
- [第二批：C++ 主键读取原型与 19 块串行对照](PX1_PRIMARY_READ_PROTOTYPE.md)
- [后续：有界主键状态隔离基础与 SoftFloat 线程状态修复](PX2_ISOLATION_FOUNDATION.md)
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

PX0 剩余工作：逐入口人工分类、补齐作用域外依赖的审查、长期固定区块归档及旧二进制参考。第二批已建立固定业务步骤的双串行控制器对照与测试专用状态读取原型；只有完整门槛满足后，才接入生产 host 路径，不提前添加试算线程。详细边界及 19 个 C++ 用例见第二批记录。

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
- 关闭并行仍须能走原串行路径；本批没有新增 nodeos 并行运行参数。
