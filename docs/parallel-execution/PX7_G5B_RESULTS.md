# PX7 G5b：多顶层动作的正式并行采用

日期：2026-10-12。分支 `feature/parallel-tx-execution`，起点 `88ca410`。本阶段已按下述受限多顶层动作范围验收完成，不代表完整调用树或全部合约已能并行。

## 实现边界

工作池以整笔交易为单位执行，最多 16 个受限顶层动作。同笔动作在一个线程上按原顺序共享私有覆盖层，不同交易可以同时执行。整笔指纹绑定根动作数量、顺序、代码与输入；协调器使用整笔读写集合，包括第二个及以后动作的依赖。

首次正式写入前检查全部基础读依赖、所有动作的代码/输入、连续写入区间和无代码通知。每一步回到原 WASM 执行位置，通过原数据库 host 按原顺序写入、调度通知，正式 ID、权限、RAM、回执及整笔 undo 仍由原路径产生。后续动作不拿私有临时 ID 与正式分配 ID 重新比较；无代码通知不消费下一顶层结果。

私有执行或初始预检失败，整笔串行。已采用正式前缀后若防御检查不匹配，只执行当前与后续动作的原串行路径，并丢弃该批次剩余试算；不重跑已写入的前缀。任何原 host 或最终资源检查异常仍撤销整笔。采用计数不是成功交易计数，最终 RAM 检查失败也可能发生在所有动作已采用之后。

非显式 CPU 计费绑定顶层动作序号，逐动作仅计入成功私有执行耗时一次，不计编译、排队或失败探测。累计耗时从原 CPU 截止扣除，保留 block wall deadline、主观 CPU、中断与暂停恢复；通知不重复计费。动作 trace 记录本步增量，而非整笔累计值。

按需捕获最多 `min(64, 8 × 本批最大动作数)` 轮，全批均为单动作时仍为 8 轮，整批绝对时限不变；并不按轮次重新发放时间或资源预算。超出 16 个动作、总代码/输入各 256 KiB，或遇到原生、特权、未列入名单代码、带代码通知、inline/sync-call 等未支持能力，走原串行。默认实验开关仍关闭。

## 当前已完成检查

证据目录：`build/local-tests/parallel-multiaction-adoption-20261012-tHTfot/`。

- `build.log` 首次编译在旧协调器测试失败：模板化后不能再隐式推导 `optional`。旧测试改为明确结果类型，保留全部断言。`build-2.log` 在新增测试消息表达式失败：Boost 消息宏中的三目表达式缺少括号。修正后 `build-3.log` 完成核心测试与新节点编译；两个编译失败日志保留。
- 工作池三轮 `worker-first.log`、`worker-{2,3}.log`：每轮 61 用例、2,645 断言。新增 2 项共 531 断言，检查真正的逐动作 WASM 执行区间重叠、线程隔离、同笔前缀依赖、第二个动作冲突、失败无前缀输出和输入总预算。
- 定向核心 `core-targeted-first.log`：5 项、1,336 断言。四个新增用例含跨动作余额、16/17 动作边界、通知/表生命周期、第二步权限/付款人/游标失败、最终 RAM 撤销、真实 token 创建/消费/关闭余额，以及逐次 CPU、重复/跳序拒绝、累计超限、外部中断和暂停恢复。已有生命周期分叉/快照/重启用例扩展为单动作与双动作两种模式。
- 完整核心 `core-{1,2,3}.log`：三轮每轮 31 个入口、8,028 断言；其中 30 个功能用例加 1 个未请求导出的空入口，容量基准跳过。默认 token 是仓库原版 EOS token。
- 当前 PureEOS token `pureeos-token-{1,2,3}.log`：三轮每轮 3 用例、1,471 断言，包含新的多动作正式采用。
- 状态层 `state.log`：57 用例、90,530 断言；重新链接的串行参考 `serial-reference.log`：2 用例、5,165 断言。
- 重新链接的原路径兼容矩阵 `compatibility.log`：21 组、183 用例、3,104 断言全部通过，墙钟 50.87 秒。
- 审计工具 24 项通过，进程工具新增多动作 trace/RAM 检查后 14 项通过；审计快照检查通过。CTest 的私有 TSan 筛选同步排除新增交易原串行 oracle，避免把未插桩的完整 controller 混入私有线程隔离声明。

ASan 测试负载校准：初次完整第一轮通过，但 `worker-asan-2.log` 的新增重叠用例未得到成功私有结果；单独重跑也在 `multi-asan-isolated-2.log` 复现。临时为工作池捕获异常增加耗时诊断后，`multi-asan-diagnostic-4.log` 记录两个失败尝试分别为 500,411 与 500,380 微秒，异常为 `primary attempt is failed or already sealed`，对应整笔 500 ms 绝对截止保护；没有 sanitizer 内存/未定义行为诊断。原因是新增测试在两个动作中各放入原单动作的 150,000 次循环，将整笔探针负载加倍。现改为每步 75,000 次、总量与原单动作一致，仍要求所有结果成功且实际 WASM 区间重叠，独立超时/取消用例与正式 500 ms 上限不变。临时诊断代码已撤回，失败记录不覆盖。

负载校准后的普通 `worker-final-{1,2,3}.log` 和 ASan/UBSan/LSan `worker-asan-final-{1,2,3}.log` 均三轮通过，每轮仍为 61 用例、2,645 断言。重新构建的 nodeos-parallel 与核心测试哈希完全未变，校准没有改变通过真实进程验证的生产逻辑。Sanitizer 只覆盖目标中明确插桩的私有执行/状态层及 SoftFloat，不是整个 nodeos 或正式 controller 的全量插桩。

私有 TSan `worker-tsan-{1,2,3}.log` 三轮均 56 用例、2,344 断言通过，排除 5 个使用原 controller 的串行 oracle。`tsan-registered-test.log` 另外验证修正后的 CTest 注册入口，实际筛选同样 56 项并通过，不把跳过的 5 项算作线程检查覆盖。

## 真实进程与覆盖边界

多动作夹具是真实 PureEOS token，每轮 16 笔交易、33 个顶层动作。首笔在同一交易内完成首次收款、花掉刚收到的余额、关闭新余额行，后续双转账包含净值不变但确实发生写入的冲突。测试工具检查动作数量/顺序及先收后退 RAM，不能把零变化空夹具算作通过。

三轮 `parallel-producer-xc2hnjxb`、`parallel-producer-iweino7d`、`parallel-producer-_xr2nzoh` 已完成五模式（旧节点、新节点 0/1/2/4 worker）：启用模式各 2 批、96 次试算、17 次成功试算、6 次正式采用、10 次回退。成功试算包含会被后续捕获轮次丢弃的结果，不是成功上链笔数。16 笔均最终确认，33 个动作完成，首笔合计 RAM 为零，0.5 秒出块间隔、旧节点强校验重放及三个节点 SHiP 摘要一致。

原恢复矩阵三轮 `parallel-process-oryusgjs`、`parallel-process-qt86hlrc`、`parallel-process-r0pcm412` 各 55 阶段全部通过。这是原恢复语料的回归；新多动作分叉/快照/重启证据来自上面的核心用例，不把旧单动作进程语料改名为新多动作覆盖。

进程目录位于 `/dev/shm/pureeos-g5b-process-coBiTr/`。永久稀疏归档 `g5b-process-evidence.tar.gz` 已逐文件 `tar --compare` 成功，SHA-256 `91644a2a2ee64b122678fbd632d3d5668008abb1850b5e1d34f7048ab5a53482`；六份报告也已解出到证据目录中相同根目录下，归档权限仅当前账号可读写。

本阶段验收完成，后续继续 G5c 的带代码通知、inline 与同步调用。所有有限负载都不是饱和容量测试，不给出 TPS 提升结论；未重跑 14 组长业务矩阵和 180 轮长驻压力，也没有把旧原生状态/SoftFloat sanitizer 报告记作本轮新执行。

## 当前普通构建指纹

- nodeos-parallel：`c36b77bfb518cfc0249edc77d3f11810f82a558f8b9451d7ee53e6207d40fe2c`。
- 最终普通 worker：`c59fccc2222c06328e9205a192d9cc2940c1f590b1a06e6a8598d2be2d40a737`。校准前哈希另见 `runtime-first.sha256`。
- 核心测试：`aa388040b4b0387bebe7ed0c32c6f8abce3167570741c13d5d6b7e02175af708`。

`g5b-runtime-c36b77bf.tar.gz` 保存新节点、普通 worker/核心/状态/串行参考/兼容测试及配置、兼容运行日志；SHA-256 `8e717a332a947f3f204bf02760f05ef924748f99963657765c854fdbe3efbe76`，已逐文件核对，归档内节点哈希另行核对一致。

ASan 二进制 `818c153b834d3e72a81de2ff017bfec8ae371b5255f2e2c27ae00a56004193a6`；匹配配置保存在 `g5b-asan-artifacts.tar.gz`，归档 SHA-256 `a92795f32b149a4d3bd1e69709b75ecd7909c4fe29d255deb082910c50796fe7`，已逐文件核对。切换 TSan 后同名工作目录二进制不再是 ASan 版本，需使用该归档复现。

TSan 二进制及匹配配置、注册入口运行日志保存在 `g5b-tsan-artifacts.tar.gz`，归档 SHA-256 `535e719d1b6d87d2ca96656303995fb57720fc3db7d33b5d46c6339271727cc3`，已逐文件核对，二进制哈希另见 `tsan-artifacts.sha256`。四个归档的汇总指纹保存在 `archives.sha256`。

旧节点仍为 `e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c`，主分支及精确基线标签未移动。上一轮临时状态的有归档清理见 [G5a 记录](PX7_G5A_RESULTS.md)，不涉及用户链、钱包或配置。

G5c1 后续临时空间整理：重新验证上述进程归档哈希及全目录 `tar --compare` 成功，六份报告均通过且全部测试节点已退出后，仅删除 `/dev/shm/pureeos-g5b-process-coBiTr/` 下清单列出的 174 个生成态 `data/state/shared_memory.bin`，释放 768,380,928 字节。清单和校验记录位于 `parallel-notifications-20261012-k9wrWe/g5b-archived-state-files.txt`、`g5b-archive-compare-before-cleanup.log`。区块、快照、配置和日志仍保留原文件；状态文件可由 `g5b-process-evidence.tar.gz` 恢复。本轮新失败样本及用户数据未删除。
