# PX1 主键读取原型与串行对照基础

日期：2026-10-11。开发分支：`feature/parallel-tx-execution`。回退基线仍为 `main` 的 `a4a40a6`，合约基线仍为 `c454378`。

## 1. 本批交付及边界

本批新增真实 C++ 读取接口、Chainbase 适配器、独立索引对照测试，以及原生 Savanna/WASM 区块流的双控制器参考测试。不再只是源码扫描工具。

**这是 PX1 的测试原型，不是已接入交易执行器的并行功能。** PX0 全入口人工审查尚未完成，因此本批库只作为测试依赖，不连接 `controller`、`host_context` 或 producer 的实际执行路径；不启动试算 worker，不允许共享 Chainbase 多线程写入。

当前仍没有 overlay、读写集、范围版本、epoch 取消、按序提交或生产者并行打包。这些完成前不得添加可启用普通交易并行的 nodeos 开关。

## 2. 新增文件与接口

| 文件 | 实际内容 |
|---|---|
| `libraries/chain/include/eosio/chain/transaction_state_view.hpp` | `experimental` 命名空间中的逻辑表键、独立拥有数据的返回值、读取接口及串行适配器声明 |
| `libraries/chain/transaction_state_view.cpp` | 真实 Chainbase 表/主键查找、上下界、末行读取、大小限制和线程归属检查 |
| `unittests/parallel_state_view_tests.cpp` | 真实 Chainbase 单元测试；独立按物理 ID 扫描的参考算法 |
| `unittests/parallel_serial_reference_tests.cpp` | 真实 controller/WASM、19 块语料、双独立状态库和收据/状态对照 |
| `libraries/chain/CMakeLists.txt` | 新增 `pureeos_state_view_experimental` 测试依赖库，不加入 `eosio_chain` 执行源文件或 nodeos 链接依赖 |
| `unittests/CMakeLists.txt` | 新增两个独立测试目标和 CTest 注册；完整 `unit_test` 也能链接这些新用例的实现 |

两个独立测试目标为 `parallel_state_view_test`、`parallel_serial_reference_test`，CTest 标签为 `parallel-px1`。

## 3. 主键读取契约与代码审查

| 操作 | 对照的原 host 行为 | 本批接口行为 |
|---|---|---|
| `read_table` | `host_context::find_table` | 返回逻辑键、对象 ID、payer、count 的独立副本，或明确的表不存在 |
| `find` | `db_find_i64` 的表/行定位 | 只匹配完全相同主键，不把下一行当作命中 |
| `lower_bound` | `db_lowerbound_i64` | 返回本表内第一个大于等于给定主键的记录 |
| `upper_bound` | `db_upperbound_i64` | 返回本表内第一个严格大于给定主键的记录，不做主键加一，避免最大值溢出 |
| `last` | `db_previous_i64(end)` 中的末行定位步骤 | 在本表范围内选末行；不新增 WASM intrinsic，也不替代完整 iterator 校验 |

表不存在与“表存在但没有匹配行”通过两个 optional 分开表示。表 count 可能包含二级索引记录，因此不能通过 count 非零推断一定存在主键行。

结果使用普通 `std::vector<char>` 保存数据，不返回 Chainbase 对象指针、迭代器或共享 mmap blob。原记录更新、删除、撤销，甚至数据库关闭后，已经返回的数据仍由调用者独立持有。

每次完整行复制显式指定本地字节预算。超限抛出独立的本地原型异常，不是共识层非法交易结论；未来接入试算时必须按本地回退处理。未知查询类型也显式拒绝，不能在表不存在时静默返回成功。

适配器只在构造它的线程读取。跨线程调用会在读取数据库前被拒绝；不可复制或移动适配器。**这不是数据库锁，也不能阻止其他数据库所有者并发写入**：数据库必须比视图活得久，索引必须在读取前注册，并且调用方保证不存在并发修改。未来真正的冻结基础视图仍需独立的生命周期/epoch 设计。

适配器是实时串行读取，不是 MVCC 快照。同一视图在主线程顺序修改或 undo 后会读到当前值；不会将数据库 revision 假装成已实现的逐键冲突版本。

本批所有原有生产源码指纹保持不变。审计快照只新增这两个源文件：数据库类型/引用候选增加 3 项，可能读取增加 8 项，没有增加可变访问候选或 host 注册；并行运行时批准入口仍为 0。

## 4. 18 项 Chainbase 测试

使用真实 Chainbase 数据库与生产表索引，不是只用内存 map 模拟生产实现。参考算法按物理 `by_id` 索引全量遍历、使用普通比较选行，不复用适配器的复合索引 `find/lower_bound/upper_bound` 实现。

覆盖：

1. 表不存在与行不存在。
2. 二进制内容、零字节、payer、count、对象 ID 与逻辑表键。
3. 空值记录与零复制预算。
4. 精确查找不能替代为范围查找。
5. 0、最大 uint64 主键及上下界。
6. code、scope、table 任一不同都必须隔离。
7. 空表末行/边界不能串到相邻表。
8. 只有二级索引记录、没有主键行的表。
9. 更新、删除记录和删除表后，旧返回值仍有效。
10. 修改返回副本不能反向修改数据库。
11. 大小限制不写状态，缺失行不因预算零而报错。
12. 未知查询枚举在存在/不存在表时都拒绝。
13. 跨线程误用被拒绝，原线程仍可使用。
14. 实时读取和嵌套 undo。
15. undo 后重建表/行、物理 ID 复用时不返回旧缓存。
16. 数据库只读模式可查询，不改变修订和数据库可用内存。
17. 数据库关闭后的返回值存活。
18. 固定种子 `0x5058305f52454144` 下，600 轮增删改、3 个逻辑表、4 种查询，共 7,200 次独立差分。

最终本目标共 18 个用例、7,284 个断言通过。这里的跨线程测试验证的是“拒绝误用”，不计作多线程交易执行成功。

## 5. 19 块原生 Savanna 对照

另一个测试使用真实 controller、代币 WASM 和原生 Savanna 初始 finalizer，生成固定业务步骤的 19 个块。每块序列化后，分别独立解码交给参考控制器和观察控制器；两边使用不同数据库，不复制已执行状态。

业务步骤包括：

- 创建 `token/alice/bob/carol`，部署代币合约及 ABI。
- 创建、发行代币，给 Alice/Bob 转入余额。
- 同块依次 Alice→Bob、Bob→Carol，形成前后交易依赖。
- Bob 转空余额后 `close`，删除余额表；下一块转回 Bob，重建记录。
- 超额转账必须报 `overdrawn balance`，并保持失败前的待执行状态完整性摘要不变；该失败输入不伪装成已收录交易。
- 再产生空块推动最终确认。

每个区块比较：

1. head 区块 ID 和 LIB 高度。
2. 交易 ID/receipt CPU/NET/status；各 action receipt 的完整序列化字节（含全局、接收、授权、代码/ABI 序号）、返回值与 RAM delta。
3. 完整链状态 integrity hash。
4. 新读取接口与原 tester/数据库接口取得的余额、表存在性、表 ID/payer/count。
5. 查询前后观察控制器的 integrity hash 不变。

最终还核对四个账户的预期资产金额和币种，并防止两个空的回调收集器意外互相“相等”导致假通过。

本目标最终为 1 个用例、569 个断言通过。加上 Chainbase 用例，本批新增 C++ 测试共 **19 个用例、7,853 个断言**。

边界：两边仍用当前未修改的串行执行核心；观察侧只是额外调用新读取接口。这不是旧/新 nodeos 二进制互验，不是并行区块验证，也不是独立 P2P 多节点测试。语料由固定测试业务步骤生成、在每轮测试内固定区块字节，尚未交付用于跨版本长期对照的持久化黄金区块归档。

## 6. 构建与回归

使用既有 Release、非 OC 配置，单任务编译两个新测试目标；没有编译/替换 nodeos，没有升级系统合约。首次编译发现 `shared_blob` 不提供 `empty()`，已按实际接口改用 `size() != 0`，随后构建与测试通过。

```bash
cmake -S /home/guagua/AntelopeIO/PureEOS -B /dev/shm/pureeos-release-ESOoZv
cmake --build /dev/shm/pureeos-release-ESOoZv --target parallel_state_view_test parallel_serial_reference_test -j1
ctest --test-dir /dev/shm/pureeos-release-ESOoZv -L 'parallel-px[01]' --repeat until-fail:3 --output-on-failure
```

最后两项 PX0 工具检查与两项新 C++ 检查合计 4 个 CTest，各连续 3 遍通过；重复不累计为更多不同用例。原 PX0 工具中的 16 项 Python 用例也保留通过。

测试证据保存在本机独立目录 `build/local-tests/parallel-read-20261011-3YE6z7/`（不提交 Git）。其中包含重复测试日志、两个目标的详细断言报告，以及测试程序、原型静态库和构建参数归档，避免 `/dev/shm` 清空后丢失本批证据。

本批最终二进制 SHA-256：

| 目标 | SHA-256 |
|---|---|
| `parallel_state_view_test` | `b8d54277d131329aa159d140c05048c0c1e79cbed72abbd54c05219cdb2152a4` |
| `parallel_serial_reference_test` | `fca05f11de9da9a4aed64de4c39342de57b6d7db8309309c6c0b551fec0f6dfb` |
| 原 `nodeos`（未替换） | `e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c` |

本批未运行全仓库 CTest、ASan/TSan、完整既有业务回归、多节点 P2P、生产者并行计费或 TPS 测量，不能据此宣称全计划完成或吞吐提高。

## 7. 下一批必须补齐

- 完成 PX0 的逐入口分类、范围外依赖审查及长期可复用的串行基线/黄金语料。
- 接入 host 前，补足 iterator 的创建、失效、删除再建和 sync-call 父级失效语义，不能用主键重新查找悄悄修复本应无效的 iterator。
- 将元数据定位和按需读取内容分开。目前原型按预算复制完整行，仅用于验证值语义；不能直接替代 `db_get_i64` 的查长度/部分复制行为，也不能引入普通 `find` 必须复制大记录的生产性能退化。
- 继续主键写入的私有状态设计：表 count、RAM payer、ID 分配/撤销和读取自己写入的语义必须一起处理。
- 进入 worker 阶段前完成冻结基础状态、点/不存在/范围依赖、epoch 取消及副作用隔离；保留未知入口整笔串行回退。

本批只保存到开发分支，不合并回 `main`、不推送远端。既有手动链和原始计划文件保持不变。
