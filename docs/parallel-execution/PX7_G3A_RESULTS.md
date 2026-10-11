# PX7 G3a：CDT 基础兼容与有界内存增长

日期：2026-10-11。开发分支 `feature/parallel-tx-execution`，起点 `a00c7d2`。这是 G3 的前置子阶段，**不是通用 token 转账或全面合约并行完成**。不改系统合约经济、治理、资源模型、链格式或共识规则；默认关闭。

## 代码改造和安全边界

`parallel_wasm_worker.cpp` 新增 4 个内存操作和 3 个断言入口、`abort`。私有 host 合计 23 个受限入口；另外明确注册 10 个仅回退的导入桩，其余 229 个生产接口仍不能链接。`parallel_wasm_worker.hpp` 补充两类导入的区别，生产 controller/adopter 流程没有新增旁路。

- 内存增长：允许 CDT 常见的“不声明最大页数”，但每次 `memory.grow`（含 start）必须先检查当前页数和请求量。超过本地上限整笔回退，不能把本地限制改造成合约可见的 `-1` 成功分支。协议上限仍通过原准备/采用阶段绑定；局部上限内保留模块自身上限的原 `-1` 语义。负数/极大增长保守回退；初始页数和代码预算不放宽。未修改 EOS VM 子模块。
- `memcpy` 拒绝重叠，`memmove` 允许重叠，返回原 WASM 偏移；零长度仍检查原转换器要求的目标字节。`memcmp` 规范化为 `-1/0/1`，`memset` 使用低字节。所有范围先检查，不能访问正式数据库或别的实例内存。
- 即使断言条件为真，也检查消息指针/范围或完整 NUL 终止。断言失败及 `abort` 丢弃尝试，由原路径产生正式异常和撤销，不把本地异常当作共识拒绝。
- 仅回退导入为 `is_account`、`require_recipient`、`prints_l`、`db_store_i64`、`db_remove_i64`、`set_blockchain_parameters_packed`、`get_blockchain_parameters_packed`、`set_proposed_producers`、`current_time`、`get_active_producers`。它们可以存在于模块中，但只要调用就立即丢弃整次尝试，**不返回默认值、不吞通知、不模拟创建记录或特权行为**。
- 注册审计与拒绝测试分别核对上述三类；每个仅回退入口都有正确签名的调用、未调用、start 调用、先写后调用、错误签名测试。原生产 262 项审计仍为 pending，未自动批准。

## 真实 CDT 夹具

新增 `unittests/test-contracts/parallel_cdt/`，源码、ABI、WASM 与重建命令一并保留。CDT 4.1.1 编译的最终 WASM 为 `0365f466b059f6985baa7a1aa1415e59139e58bdb10344a9c457b59909fe8de8`，20 个导入、24 个本地函数、初始 1 页且未声明最大页数。测试固定核对该哈希，不替换成简化 WAT。

它使用原 CDT dispatcher、`multi_index`、动态分配、授权、断言和返回值。创建行仍走原串行；既存行保持 payer/长度更新。大分配放在私有写之后，验证整次丢弃并重新串行不会重复记账。分配 2MiB、只触碰/比较前 4KiB，确保该场景测试的是内存能力限制，而不是人为耗尽原 CPU 限额。

验证端和生产端分别使用 0/1/2/4 worker，对照原串行整链状态、返回字节、receipt、RAM、出块顺序和 CPU 计费。验证端另覆盖热点和“旧状态余额不足、前序入账后可成功”；生产端覆盖独立采用、内存超限整批尾部回退、下一批重新采用。普通 CDT 夹具不是 `eosio.token`。

## 本轮测试记录

证据根目录：`build/local-tests/parallel-cdt-20261011-DopCfC/`。

| 检查 | 当前结果 |
|---|---|
| 状态/依赖/稀疏捕获 | 51 用例 / 36,751 断言，3 次通过 |
| WASM、工作池、顺序协调器 | 42 用例 / 1,702 断言，3 次通过 |
| 私有 WASM TSan | 38 用例 / 1,463 断言，3 次通过；排除 4 个未插桩 controller 对照用例，无新增抑制 |
| 原生隔离/SoftFloat TSan | 57 用例 / 36,840 断言，3 次通过 |
| 核心验证与非显式计费生产 | 验证 11 用例 / 1,900 断言；生产 7 功能用例 / 976 断言，均 3 次通过；另有未启用导出入口，不计功能用例 |
| ASan/UBSan/LSan | WASM 42 用例 / 1,702 断言、原生隔离 57 用例 / 36,840 断言，均 3 次通过，无新增抑制 |
| 原路径兼容矩阵 | 21 组 / 183 用例 / 3,104 断言，1 次通过 |
| 新旧 nodeos 验证/恢复/分叉/SHiP | 每轮 55 阶段，3 次通过 |
| 真实生产者五模式 | 新 0/1/2/4 + 旧程序，每模式 16 笔，3 次通过；包含旧程序完整重放和 SHiP 对照 |
| 审计与工具 | 24 项审计单测、12 项夹具自检通过；791 文件 / 262 host 快照匹配 |

32 组内存/断言输入直接对照原 host 转换器；内存增长测试覆盖无上限、较大上限、较小模块上限，start/apply、零增长、成功、模块失败、本地超限、极大/负值及多次复用。4-worker 混合批次同时包含成功、扩容和失败操作。

真实 CDT 专项最终为 2 个用例 / 789 项断言；对每个启用模式，验证序列实际采用 8 笔、原序回退 8 笔，生产序列实际采用 6 笔、原序回退 3 笔。均逐块/逐轮断言，不是仅确认后台线程执行过。

真实验证报告：`parallel-process-0blsyvl7/report.json`、`parallel-process-k03k_l_n/report.json`、`parallel-process-u6snetod/report.json`。这些真实进程复测使用已固定的既存行业务语料；新 CDT 合约的新增覆盖来自完整 controller 测试，**不能把这 55 阶段称为真实 token 或 CDT 进程验收**。

真实生产报告：`parallel-producer-s0mk781p/report.json`、`parallel-producer-xxgy636j/report.json`、`parallel-producer-utmerw88/report.json`。14 秒限量发送窗口不是节点容量 TPS，不能用 16 笔除以窗口推算极限性能。

## 保留的失败与修正

1. `build-core-1.log`：新测试的 Boost 消息宏中三元表达式缺括号，编译失败；补括号后继续，没有改链逻辑或删除断言。
2. `cdt-core-1.log`：初版夹具对多 MiB 缓冲区逐字节比较，关闭并行也达到原 150ms CPU 上限。旧夹具保存为 `first-cdt-fixture.tar.gz`；保持 2MiB 分配，只处理前 4KiB。没有放宽 CPU/内存协议限制。
3. `cdt-core-2.log`：生产测试每轮创建普通 signal connection，离开作用域未断开，后续轮次重复收集回执；关闭并行同样出现。改为 `scoped_connection`，另加每轮恰好 3 个回执的断言。账本、采用数量检查没有被降低或移除。
4. `worker-asan-1.log`、`worker-asan-serial-1.log`：ASan/UBSan 下重负载重叠测试触及本地 500ms，停止其他编译/节点后仍有 1 个重叠用例超时，未报告内存错误。只将该功能用例显式设置为既有允许范围的 1 秒尝试预算，仍执行同样的 150,000 次 WASM 循环并检查真实重叠；加入逐 worker 用时日志。生产默认 500ms、私有批次限制、工作池测试及原有短时限/取消测试均不改变。最终 `worker-asan-final-1/2/3.log` 全部通过；普通版也用最终夹具重跑 3 次。失败日志不删除，不把插桩版耗时当作性能测量。

## 产物与未完成范围

`nodeos-parallel` SHA-256：`6c3cd8686f4cf87eb44f8d3526e44b1c20a6836b6fce4c96ce7de6f1c28d6665`。

普通产物归档 `g3a-runtime-6c3cd868.tar.gz` 保存新节点、4 个普通测试程序、CDT 源码/ABI/WASM、CMakeCache 和兼容矩阵完整日志；SHA-256：`a278a906e32bdc0ef5e3f112bd88fcfbaa749e52abbc31e900c764312d670c68`。

ASan/UBSan/LSan 归档 `g3a-asan-artifacts.tar.gz` 保存两个最终检测程序与对应 CMakeCache；SHA-256：`5adbf6b81793bee901af8531eaa154d26d15485960cf824aa93d7ee7e17869ab`。其中 WASM 检测程序 SHA-256 为 `370add56780ff2cab077ce84d32d66289f41af0df44c1ba67f224e90caedccd8`。

最终 TSan 归档 `g3a-tsan-final-artifacts.tar.gz` 保存重新编译并各复测 3 次的两个检测程序与对应 CMakeCache；SHA-256：`d0398896e5a7bd69b742b9325334740bc5eca728366dd89aa5c34b18a7c60304`。其中 WASM 检测程序为 `ec98cd8fa953b5414956776e55eabb104ab430fa014c488ecb37202fce2bf63b`。构建目录最后恢复为 `PUREEOS_PARALLEL_SANITIZERS=thread`。

初次 TSan 归档 `g3a-tsan-artifacts.tar.gz`（SHA-256：`c2cac7495c000eeb825a960372bbb4a0068c50bc47da416248351e87a3efa1f8`）仍保留预算夹具调整前的程序，不混作最终产物。

最终日志为 `core-final-*`、`worker-final-*`、`worker-asan-final-*`、`native-asan-*`、`worker-tsan-final-*`、`native-tsan-final-*`、`state-*`、`compatibility.log`。本批 **G3a 已验收**。检测插桩覆盖私有执行器/VM 模板/状态层与 SoftFloat，不是整个 controller/Chainbase/FC 的全量插桩证明。

仍未实现 G3b 的账户存在性跟踪及有序通知，因此真实 token 转账仍不能采用并行结果。新增/删除/resize/payer/RAM（G4）、多 action/通知合约/inline/sync-call（G5）、二级索引和一般 host（G6）、新的性能与推广门槛（G7）均未完成。owner 独占正式账本、单 action 限制、首个回退丢弃批次尾部继续保留。

原 nodeos、`main`/基线标签、合约仓库、个人配置/钱包和手动链未改变；没有合并或推送。本轮未重跑原业务 14 组长回归或 180 轮长驻，没有新的 TPS 倍数结论，也不将历史 PX6 结果当作新版本性能证明。
