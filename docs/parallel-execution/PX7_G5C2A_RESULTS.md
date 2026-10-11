# PX7 G5c2a：共享游标的有序轨迹基础

日期：2026-10-12。分支 `feature/parallel-tx-execution`，起点 `e0da47f`。本批已按下述轨迹基础范围验收，属于正式通知采用的前置基础，不开放带代码通知上链。

## 实现与范围

`parallel_wasm_worker.hpp/.cpp` 为私有通知执行新增有类型的事件序列：find/lower/upper/end/next/previous 查询、整笔写入日志索引及本接收者通知索引。查询保存输入和返回句柄，相邻查询同时保存主键参数的原值/结果；store 保存原返回句柄，update/remove 保存原输入句柄。写入仍引用既有日志中的值和 payer，不重复复制值，不隐式添加 find；重复通知不产生事件。纯 db_get 不分配或修改缓存，继续由原始状态读取依赖覆盖。

整笔最多 4,096 个事件，重复读取也计数；不同接收者、不同根动作不刷新额度。触及上限、捕获缺失或失败时整次私有结果丢弃，不能返回前缀；500 ms 绝对预算、目录/步骤/状态预算不变。现有 G5b 顶层模式不创建事件向量，分支检查后直接保留原有路径。

测试新增一个只用于对照的 WASM 生成器：把私有事件转换为真实数据库 host 和 require_recipient 调用，交给原 controller 执行，后续通知仍跑原合约。它验证原缓存能否由轨迹重建，不是正式 C++ adopter，也不是代码/权限证明。当前原工作池和正式采用入口仍不接收此私有结果类型。

关键反例已实际验证：根动作只读某行后通知其他合约，若采用时省略这次读取，后续原通知拿到的迭代器编号会变化。即使表中值不变，返回值及启用 action return value 时的 act_digest 也不同。完整轨迹与原执行一致。这是禁止直接放宽 G5b 根动作门槛的设计证据，不是声称现有已开放路径出现该问题。

## 已执行检查

证据目录：`build/local-tests/parallel-cursor-journal-20261012-cW31D9/`。

- 首次编译成功；审计工具 24 项、进程工具 14 项、源码清单匹配通过。
- `notifications-first.log` 12 项/1,053 条通过。随后加强回执摘要断言，`notifications-receipts.log` 12 项/1,089 条通过。相对 G5c1 新增 4 项/244 条：完整 19 事件交错序列、相邻查询边界/输出不变哨兵、删除/再创建与结束句柄、跨根绝对写入索引、循环通知去重、4,096/4,097 次重复读取边界及整笔预算、原 host 重放/漏读反例/三次写入后的故意失败撤销。摘要检查明确要求对应协议特性已激活。
- `worker-{1,2,3}.log` 初版普通三轮各 73 项/3,698 条；加强摘要断言后 `worker-final-{1,2,3}.log` 再三轮各 73 项/3,734 条通过。该加强只改测试，节点与核心测试哈希保持不变。
- `core-{1,2,3}.log` 各 31 入口/8,028 条，其中 30 个功能用例和一个未请求导出的空入口，容量基准跳过；当前 PureEOS token `pureeos-token-{1,2,3}.log` 各 3 项/1,471 条通过。
- 状态层 `state.log` 57 项/90,530 条、重新链接串行参考 `serial-reference.log` 2 项/5,165 条通过。兼容矩阵 `compatibility.log` 全部 21 组通过，183 项/3,104 条，墙钟 49.99 秒。
- 第一批 ASan/UBSan/LSan `worker-asan-{1,2,3}.log` 各 73 项/3,698 条通过；状态/浮点 `native-asan-{1,2,3}.log` 各 63 项/90,619 条通过，两个注册入口通过。原生目标源代码未改，复用并校验 G5c1 的同一个 ASan 程序，不声称这些原生代码发生了新改造。

加强摘要断言后的最终 ASan/UBSan/LSan `worker-asan-final-{1,2,3}.log` 三轮各 73 项/3,734 条通过；`asan-registered-final.log` 两个注册入口再次通过，墙钟 16.77 秒。这些 sanitizer 覆盖私有执行/状态层与 SoftFloat，不是完整 controller 或节点插桩。

最终 TSan `worker-tsan-{1,2,3}.log` 三轮各 66 项/3,088 条通过，明确排除 7 个涉及未完整插桩 controller 的对照用例；原生状态/浮点 `native-tsan-{1,2,3}.log` 三轮各 63 项/90,619 条通过。`tsan-registered-test.log` 两个注册入口通过，墙钟 16.25 秒。源码审计工具和进程工具最终再次通过，清单匹配，旧 nodeos、主分支/基线及外部合约仓库未改变。

## 真实节点回归与产物

`parallel-producer-ik3e5fdq` 五模式全部通过：真实 PureEOS token 各 16 笔/33 个根动作，首笔创建、花费、关闭新余额行，RAM 收退合计零；启用模式各 2 批、96 次试算、17 次成功试算、6 次正式采用、10 次回退。旧节点强校验重放、三节点 SHiP 摘要、最终确认及 0.5 秒槽间隔一致。试算成功包括随后丢弃的发现轮次结果，不是上链计数。

`parallel-process-gfk2hc69` 完整 55 阶段恢复通过，包含新旧程序和混合版本 P2P。这两组是现有已开放 G5b 功能的回归；恢复语料仍为旧单动作语料，不冒充新通知正式采用或新多动作恢复覆盖。未重跑长业务矩阵、180 轮长驻和饱和容量测试，不据此估算 TPS。

全部节点已退出。`/dev/shm/pureeos-g5c2a-process-Xsead8/` 的完整稀疏归档 `g5c2a-process-evidence.tar.gz` 已逐文件核验，SHA-256 `de0f8940b60b5a100483d6d8abbe1fc14086201cc7f9c3d7fb58f37d3c589747`，权限 600，两份报告也已提取到永久证据目录。本批没有新增环境失败；开始前对上一阶段成功状态的有归档清理见 [G5c1 记录](PX7_G5C1_RESULTS.md)，原失败样本继续保留。

节点哈希 `c13c49f3eb47f1689e926abc1327de66d0c0f91f68284170db339b882ea6bea5`，核心 `babc00aafcf2e5a70ac39061444dccf397632bc36c18080941aaa1e97bc7d34e`。六个普通程序及配置归档 `g5c2a-runtime-c13c49f3.tar.gz` 已核验，SHA-256 `bb77ee89838564b92635fbc4416ff62c0ea79d6e198c8e002190602a7a205bce`，其中 worker 是摘要断言加强前版本（其哈希在 `runtime-first.sha256`）。最终普通 worker `585c000033169ff19df2ae08afaab6acb2fc1a02eff6092fc65dd8cf6cc982c0` 单独保存在已核验的 `g5c2a-final-worker.tar.gz`，归档 SHA-256 `3250a12dc581de56a21817ad42626682c8225d087b65f234eb664d72d0ee925c`。

最终 ASan 两个实际程序与匹配配置、注册测试日志归档 `g5c2a-asan-artifacts.tar.gz` 已逐文件核验，SHA-256 `d933d391284be67a82f926fcba556d65ffce8c675c4c94fc37869b6d2534b4ef`；实际 worker `d89990e255ead96d0a269d82e42715a32ac51d74c2a4c8a4762accd688d90f41`，原生目标 `700169c63c9c4ec989682a50c79751e2e348406adedfcd0975829c4c3a4adca8`。切换到 TSan 后工作目录的同名程序不再代表 ASan，复现须使用对应归档。

TSan 两个实际程序、配置及注册日志归档 `g5c2a-tsan-artifacts.tar.gz` 已逐文件核验，SHA-256 `39856478350838b1f108ff567c9fb81658100104278c000e072fadb4f4c75026`；实际 worker `69fa41ea41bde3949583131556d27fbfd1959adfff4c148c2439ea79ed857b79`，原生目标 `448abd345e9c87eb65b0255fc745be172fbe37fff5ff7d89ecbc7296a46e1ce1`。

## 下一步

G5c2b 后续临时整理：再次核对本批进程归档哈希、全目录比较成功、两份报告均通过且节点全部退出后，只删除其中 58 个生成态 `data/state/shared_memory.bin`，释放 256,126,976 字节。清单及比较日志在 `parallel-notification-preflight-20261012-1IgvVV/g5c2a-archived-state-files.txt`、`g5c2a-archive-compare-before-cleanup.log`；这些状态可从上述完整归档恢复。区块、快照、配置和日志未删。

正式接入前仍要检查事件和变更日志对应同一表/主键/对象代际，核验全树代码与状态依赖，再接工作池、按步骤计费和原通知分派；不能只验证索引连续就使用任意句柄写入。相关约束已更新到 [滚动计划](PX7_GENERAL_EXECUTION_PLAN.md)。当前仍未支持 inline、同步调用或一般二级索引。
