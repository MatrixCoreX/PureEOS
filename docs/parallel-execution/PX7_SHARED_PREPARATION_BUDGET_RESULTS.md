# N3 批次共享准备预算

日期：2026-10-18。当前状态：固定复核、联合校验和准确程序归档已完成；初始完整回归的一次生产准备截止失败仍保留，不宣布根治。

前一批当前前缀刷新接口的精确程序及 82 项输入已独立冻结至 `/run/media/guagua/新加卷/pureeos-prefix-refresh-artifacts-20261018-qztI9E/`，其固定复核和联合校验已完成，原截止失败仍保留。本批工作区改动不属于前一批测试结果，不能把前一批通过当成本批验收。

## 本批实现范围

- 新增 `parallel_preparation_budget.hpp`：owner 独占、不可复制/移动的批次预算。保存已经截断好的绝对 `isolated_cancel`，保留中断 generation；没有刷新截止时间或重置轮数的接口。
- 同一对象累计扣减 64 轮发现额度、4 MiB environment wire 额度。先比较再相减，超额/整数边界失败不会部分扣减；零字节扣减合法。时限仍由原 checkpoint 和 worker 的 cancellation 检查执行，不用轮数代替时限。
- `controller.cpp::validation_attempts` 持有此对象，初始发现循环和 environment 捕获实际使用它。原局部 `private_cancel` 的 500ms 截断与生产 50ms 上限不变；对象保存的是这个有效私有截止，而非可能无限的外层 validation cancel。
- 下一步当前位置重执行须借用该对象并复用原 batch identity；本批没有启用局部重执行，也不删任何尾部屏障。
- `parallel_module_cache_tests.cpp` 新增三项：跨阶段用尽同一个轮数额度、deadline/generation 不变；wire 上限、零扣减和 `size_t` 极值；异线程五种访问均拒绝，worker 只能获得独立 cancel 副本。

这些额度仅控制本地准备工作，不改变共识、计费、治理、经济模型或 worker 上限 16，不是全进程内存上限。

## 证据与待验收

新证据目录：`build/local-tests/parallel-shared-preparation-budget-20261018-IiI5Jl/`。83 项输入已冻结；静态 **24/24**、访问清单匹配，新增头文件无 DB 访问/host 注册，原 controller 访问顺序及种类未改变。

在前一批测试结束并通过空闲/内存检查后，构建 **10:28.82 / 退出 0**，峰值 RSS **5,856,168 KiB**。本批核心程序 SHA-256 `2605354eb7d9a369b4d56a48d238ed4c839702395aae6d0e5146ba5e843e3fba`，worker 程序 `3521e6590ceedbcbbca355423943aa51ab262b4575c2904ffdf22a93bad44115`。

- 三轮预算专项均 **3 项 / 341 条断言**通过；核心前两轮各 **65 / 13,771**、完整 worker 前两轮各 **411 / 289,563** 通过。
- 核心第三轮 **64/65、13,767/13,771，退出 201**（另有 1 项 benchmark 未选）。唯一失败用例是 `sparse_discovery_uses_global_cap_and_preserves_exact_exhaustion_and_reuse` 的 `queries=64 / workers=2`：生产准备在第 **29** 次尝试触及原 **50ms** 截止，四条关于精确发现次数／耗尽原因的断言失败；同场验证端仍在第 64 轮按发现额度耗尽回退。未报账本/原收据差异，不能将计数失败直接描述成共识错误，也不能将其删去冒充完整通过。
- 本次失败采样 **未观察到编译重叠**，不能归因于另一编译任务。具体调度／成本成因没有定位，不改变原断言、发现轮数或时限。
- 同一准确程序与 83 项输入已快照至 `/run/media/guagua/新加卷/pureeos-shared-budget-artifacts-20261018-AglLaf/`。固定复核的三次稀疏专项各 **1 / 1,048**、一次完整核心 **65 / 13,771**、第三轮完整 worker **411 / 289,563** 通过。初始 driver `tests.status=201` 保留。
- 复核 runner 首次因把不含 WASM/ABI 的源码 manifest 当作夹具列表，预检查退出 **2**，没有运行 C++ 用例；原脚本/日志保留。接续使用显式三个运行夹具的独立 hash 清单，未更改被测程序、断言或预算。

联合校验 `verified-summary.json` 已通过：核对 83 项快照输入、两份程序、原/接续脚本、原 nodeos、三个运行夹具及全部 13 个 C++ 进程的实际结局。准确源码/程序、初始证据、最终证据已归档，六项 archive hash 检查通过。全部运行采样均未观察到编译；这并不证明没有其他 CPU/I/O 竞争。

| 样本 | 完成耗时 | 结局 |
| --- | --- | --- |
| 核心第 1、2 轮与固定复核 | 1:35.39 / 1:34.79 / 1:44.03 | 每次 65 / 13,771 通过 |
| 核心原第 3 轮 | 2:05.17 | 64/65；四条截止相关断言失败，保留 |
| worker 第 1、2、3 轮 | 6:54.90 / 7:24.59 / 7:35.94 | 每次 411 / 289,563 通过 |
| 稀疏专项固定三次 | 3.51 / 3.71 / 3.41 秒 | 每次 1 / 1,048 通过 |

旧 RAM 和各准备截止偶发问题仍未根治。本版没有启用当前前缀重执行，也没有 TPS 结论；工作区后续接线属于 [下一版](PX7_CONTROLLER_PREFIX_REFRESH_RESULTS.md)，不能混用测试结果。

前一批详情见 [当前前缀刷新记录](PX7_PREFIX_REFRESH_RESULTS.md)。旧 RAM 回退计数偶发失败仍未定位，本批不修改相关断言或时限。
