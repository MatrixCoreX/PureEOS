# PX7 G6a：二级索引拥有值与原序读取基础

日期：2026-10-15。分支 `feature/parallel-tx-execution`，HEAD `3856ab5b0723da1a1c41d60751109af789403945`；本批为未提交改动，不覆盖此前混合树、插桩失败或回退基线。证据目录 `build/local-tests/parallel-mixed-lru-20261015-TlEO9d/`。

## 当前范围

新增 `parallel_secondary_state.hpp/.cpp`，提供五类精确键、八类查询、拥有完整值的行与表元数据结果，以及仅允许创建线程读取的 Chainbase 适配器。当前只连接独立测试和总单测目标，**没有连接 worker/controller，没有开放二级索引 WASM host，也没有修改当前 transaction_state_view ABI**。

- idx64 / idx128 / idx256 保留原无符号数值顺序；idx256 比较两个 uint128 数值，不比较内存字节序。
- double / long double 存原始位，排序用原 SoftFloat comparator。精确相等与排序等价分开，±0 排序等价但记录位不同；NaN 输入在查表之前拒绝，find-primary 的输出值不当成输入 NaN 检查。
- 重复二级键按主键决定次序；覆盖 find-primary、find-secondary、lower/upper、first/last、完整 `(secondary, primary)` tuple 的 next/previous。后两者只是查询，不负责证明未来游标锚点的身份/代际。
- 主键与全部二级索引共享原 table object、payer 和 count。结果不携带 Chainbase 行指针；同一数字 object ID 在不同索引族中不是同一个对象。
- 任意外线程读取在数据库访问前失败；适配器不加锁，也不授权其他线程并发修改数据库。非法查询/局部不支持只能让未来并行调用方回退，不能变成新的链上拒绝规则。

## 已做的普通验证

`parallel_secondary_state_tests.cpp` 共十组测试，参考实现独立遍历物理 `by_id` 索引并用原 comparator 在 vector 中选取结果，不重用被测 composite-index 查询。这里的数据库夹具不是原 WASM 指针、迭代器或 RAM 语义对照，后者仍待 G6c/d。

覆盖精确位/排序差别；不存在表与空索引；全部五族、重复键和极端主键；共享表计数及退役重建；键/payer 更新、对象删除重建、两层 undo；正负 quiet/signaling NaN；±0/无穷/次正规数比较；无符号高位/进位边界；固定种子 800 次交错更新/删除、每次三个跨表查询；外线程拒绝。

初版构建成功，程序 `73e68381a292d261335762e9d3e16cc1778b49079f0836c33cf163c99598ba89`，首轮 **10 / 7,604** 全通过，日志 `build-g6a-normal.log`、`g6a-normal-first.log`。GCC 对 optional/variant 临时移动的未初始化诊断保留；先尝试局部原位构造仍有两条警告，`build-g6a-inplace-normal.log` 保留，未关闭警告选项。

随后改为在调用方的 optional 内直接构造当前 variant 分支，消除中间 optional/variant 返回移动。最终普通构建 `build-g6a-direct-normal.log` 退出 0，无 warning/error；程序 `abb995a4183f30e9889d2c9c9f184ce06bd47bb2233e3ca0d5fe3fdc4f996921`，`g6a-normal-three.log` 固定三轮各 **10 / 7,604** 全通过。五份源码 `g6a-direct-sources.sha256` 测试后复核一致，程序哈希、编译和链接参数另存。

访问清单仅增加两份生产源文件、更新 chain CMake 哈希与候选统计：增加四处数据库句柄和九处读取候选，没有 host 注册、写操作或运行时准入变化。`g6a-inventory-before.log` 保存预期差异，更新后 `g6a-inventory-after.log` 精确匹配。该词法清单不是线程安全证明。

## 尚未完成

独立 ASan/UBSan/LSan 构建与三轮验证已通过，`build-g6a-asan.log` 无 warning/error；实际程序 `aa8e25a95a22577238ce27e9ac02aa9509f58a9f7a1e180bbe273d2b872107ee`，运行库检查 `g6a-asan-runtime.json`，编译/链接与身份另存。`g6a-asan-three.log` 三轮各 **10 / 7,604**，退出 0，未报告内存、未定义行为或泄漏错误。这里插桩的是新适配器、原主键读取适配器、测试和 SoftFloat；预编译 FC/Chainbase 并非全量插桩，不能声称全链内存检查。

私有线程检查尚未完成。G6b 的共同预算/稀疏冻结请求与共享表生命周期，G6c 的私有写入/游标/依赖/事件，G6d 的原 host 正式重放及真实 WASM，G6e 一般接口与最终真实节点/稳定性验收均不因此完成。

混合 LRU 版本 `997122aa…` 的五项采用计数失败单独记录于 [混合正式接入记录](PX7_MIXED_ADOPTION_RESULTS.md)，不由本状态层十项通过抵消。默认并行开关、原共识、治理和经济规则均未改变。
