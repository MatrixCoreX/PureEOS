# PX7 G5a：交易级私有执行基础

日期：2026-10-12。分支 `feature/parallel-tx-execution`，起点 `fb4d730`（G4b）。本批隔离层基础按下述范围验收完成；正式多 action 接入尚未完成。

## 当前实现

`parallel_wasm_worker.hpp/.cpp` 新增 `isolated_transaction_executor`。一笔交易的最多 16 个顶层 action 在同一线程按原顺序共享一个私有覆盖层；不同交易的执行器可由不同线程独占运行。交易最终只 seal 一次，任何中途失败都不会返回可采用的成功前缀。单步接口是私有方法，外部不能获得一个已发生 WASM 错误但仍可 seal 的覆盖层。

每个 action 重置 WASM 内存/start、游标、通知去重和返回值。输出记录该 action 的连续写入区间、通知的全交易绝对写入位置、执行指纹和单步执行时间；整笔指纹绑定动作数量、顺序和各动作指纹。编译/排队时间不包含在单步私有执行时间内。

整个调用共享原有表/查询/账户/写入次数及字节预算，不为每个 action 重新发放。另限制总代码、总输入各 256 KiB，最多缓存一个编译模块；整笔 500 ms 的绝对上限与外部批次时限取较早者，各 action 原有更短上限仍适用。超限只是未来的串行回退理由，不改变共识有效性。

**此 API 还没有接入 controller 或正式生产/验证工作池。** 原单 action 执行路径复用内部单步函数，因此也需要回归；controller 的单 action 准入门槛没有放开。多 action 的正式采用和累计 CPU 计费是 G5b，带代码通知/inline/sync-call 是后续调用树改造，不计作本批已实现。

## 新增验证设计

- 同一行先写后读、不同主键、每个动作重新从 0 分配迭代器，内存不串用。
- 跨 action 创建/修改/删除/重建、新表身份与最后一行、跨模块切换、无写动作。
- 同名通知可在不同 action 再次通知，但在各自 action 内去重；授权与返回值上限不串用。
- 后续动作非法指针、trap、未支持 host、重复创建、超时、外部取消，不发布前缀；缓存复用不带入先前失败状态。
- 写入次数/字节/查询数的整笔预算、16 个动作边界、总代码/输入预算和输入授权上限。
- 前缀写入后遇到未捕获状态，请求重新捕获并重跑整笔，不把前缀写回数据库。
- 不同合约可读取同笔前序私有写入，不能修改别的合约拥有的表；依赖仍登记在原表/查询上。
- 独立 1/2/4 个执行线程反复运行，不共享覆盖层或 VM；线程归属与整笔截止保护。
- 原串行交易 oracle：连续创建、改值、读取、删表、再创建及无代码通知，对照返回值、迭代器、顶层/通知 ordinal、最终值和正式/私有身份边界；合法前缀后重复创建导致整笔撤销。

## 已完成结果与问题

证据目录：`build/local-tests/parallel-multiaction-20261012-EbSjnS/`。

- 初次新增用例运行 `transaction-first.log`：12 项中 11 项通过；跨合约 reader WAT 夹具多一个右括号，在解析时终止，尚未进入该项执行。修正夹具并重编译后再运行完整套件，失败证据不覆盖。
- 普通 worker `worker-{1,2,3}.log`：三轮每轮 59 用例、2,114 断言通过。新增部分为 11 个私有执行器用例加 1 个原串行 oracle，共 332 断言；其余是原有工作池/单 action/有序协调器检查。
- WASM ASan/UBSan/LSan `worker-asan-{1,2,3}.log`：三轮均 59 用例、2,114 断言通过。只覆盖明确配置的私有层/SoftFloat，并不声称整个 nodeos 及全部依赖都插桩。
- 核心/生产回归 `core-{1,2,3}.log`：三轮每轮 27 个入口、6,808 断言通过（26 个功能用例及 1 个未请求导出的空入口；容量 benchmark 跳过）。这些检查覆盖原单 action 正式采用路径，不是多 action 正式采用的证据。
- 当前 PureEOS token 专项 `pureeos-token-final-{1,2,3}.log`：三轮每轮 2 用例、1,310 断言通过；完整核心默认使用原版 EOS token。另一次 `pureeos-token-1.log` 因 Boost 筛选语法没有匹配用例而退出 200，不算通过，已用正确筛选重新运行。
- 状态层 `state.log`：57 用例、90,530 断言；串行参考 `serial-reference.log`：2 用例、5,165 断言。状态层源码未在 G5a 修改，仍保留本次重跑记录。
- 兼容矩阵 `compatibility.log`：21 组、183 用例、3,104 断言通过，50.62 秒；审计快照一致、审计工具 24 项、进程工具 13 项通过。
- 真实 PureEOS token 首次余额行生产模式三轮：`parallel-producer-2qi3n_o8`、`parallel-producer-2da3qx3v`、`parallel-producer-2ot6z5o8`，旧/新 0/1/2/4 五模式均通过。仍是原单 action 夹具，各启用模式 2 批/80 尝试/16 成功试算/6 采用/10 回退，首笔 RAM `alice +240`、16 笔全部最终确认、旧强校验重放及三节点 SHiP 一致。
- 完整恢复三轮：`parallel-process-bn_ssbuw`、`parallel-process-4hbjciio`、`parallel-process-0ugy0jj0`，每轮 55 阶段通过。进程实际输出在独立 `/dev/shm/pureeos-g5a-process-ItFhsC/`，避免消耗磁盘保护余量；永久归档及报告副本保存在证据目录，不降低磁盘保护阈值。

WASM TSan `worker-tsan-{1,2,3}.log`：三轮每轮 54 用例、1,813 断言通过。排除 5 个混合原 controller 的 oracle（原先 4 个加本批新 oracle）；这 5 项已在普通及 ASan 三轮通过，不误记为 TSan 已测。G5a 未改原生状态/SoftFloat 源码，没有把旧 G4b 原生 sanitizer 报告充作新一轮执行。

没有重跑 14 组长业务回归、180 轮长驻压力或容量基准，也没有 TPS 提升结论。

## 已保存产物

- 新 nodeos-parallel：`c966ae3eb6f3380079ae5f6d99ff035512ffc28f031f21674389222cb6a07aa7`；普通 worker：`bb5aa15501dfbddf21edcdf3b98a3618fc7cd63bb8c4921da65f0e7de1019d76`；核心：`f3518c703cd9ead30e23cd6f1330c08b8b7c617655655cc01b1112ce971bf1d1`。
- `g5a-runtime-c966ae3e.tar.gz`：新 nodeos、worker/核心/状态/串行参考/兼容测试、匹配配置及兼容日志；SHA-256 `3a66c6f26ee451941e5e7caa5c35db9a34ef3ef9993e5b407dc4583dea9ade5a`。
- `g5a-asan-artifacts.tar.gz`：实际运行的 WASM ASan 测试与切换 TSan 前的 CMakeCache；SHA-256 `91cf01bffd094fdda9bef15f53a541148e316c478f28ced439b22ce4b6a7bca1`，二进制指纹在 `runtime-asan.sha256`。
- `g5a-tsan-artifacts.tar.gz`：实际运行的 WASM TSan 测试与匹配 CMakeCache；SHA-256 `418cbb5458f76c89d15ea619ece8a9ef623fc8c1177f169c8df39df89f523686`，二进制指纹在 `tsan-artifacts.sha256`。
- `g5a-process-evidence.tar.gz`：上述 6 个真实进程测试目录的完整稀疏归档，已用 `tar --compare` 核对原件；SHA-256 `babcd8a0d957b1d27f63d1fa83c0d5fcf9728c9511cb3a74c0ecc836517c2ce0`。报告另已解出到证据目录的 `pureeos-g5a-process-ItFhsC/` 下；临时内存盘目录并非永久副本。

归档中的新 nodeos 内容哈希独立核对一致。旧 nodeos 仍为 `e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c`；main/基线仍为 `a4a40a6a1e72c531cd8267758074229ccc0655ce`，reference-contracts 工作区无变更。没有开启现有链、推送或合并分支。
