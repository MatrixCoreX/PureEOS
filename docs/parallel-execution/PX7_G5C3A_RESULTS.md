# PX7 G5c3a0–a2：普通 inline 私有调用树

日期：2026-10-12；分支 `feature/parallel-tx-execution`，起点 `9ef30aa`。本阶段完成有界 action 解包、私有普通 inline 调用树、整树重建预检；**没有把普通 inline 结果接入正式账本**，接入属于下一步 a3。实验默认关闭，不宣称全面并行或 TPS 提升比例。

证据：`build/local-tests/parallel-inline-action-20261012-1paBn0/`。原 nodeos、主分支、基线、外部系统合约和用户配置不变。

## 实现边界

- `parallel_inline_action`：检查长度后再分配，限制授权数与字节数；保留完整发送字节（含尾部）及解码长度，保持原 `< max_inline_action_size` 限制。非标准编码/本地能力不足只选择串行回退，不添加共识拒绝条件。
- `parallel_action_tree`：独立组、步骤、混合事件及域分离指纹；绑定 creator、ancestor、sender、ordinal、组、深度和协议配置。全交易共享覆盖层，每个 inline 组重新建立游标与通知去重集。先完成组内通知，再按发送顺序执行 inline 子组，不按 ordinal 数值排序执行。
- `parallel_wasm_worker`：`send_inline` 和 `get_sender` 仅在私有树栈内能力存在时可用；旧单步、多根、通知入口实际调用仍回退。最多 256 步/程序、4,096 事件、256 KiB 根参数加 inline 原始字节、本地深度 16，并同时遵守协议深度；500 ms 整笔预算不刷新。深层失败不发布成功前缀。
- 整树预检从拥有所有权的冻结副本重建事件、查询、写入与队列，重新解包所有 inline 并绑定子输入；逐字段比较变更，重放读依赖须由原证明覆盖。owner checkpoint 异常传播。它不是授权证明、当前代码/账本证明，也不重新解释 WASM 业务语义。
- host 清单目前为 27 个原受限接口、6 个调用回退桩、2 个仅私有树接口、227 个拒绝链接接口。`send_context_free_inline`、sync-call、一般二级索引未开放。

## 测试结果

| 范围 | 本阶段实际通过 |
| --- | --- |
| 普通 worker | 三轮，每轮 102 项 / 7,158 断言 |
| ASan / UBSan / LSan worker | 三轮，每轮 102 / 7,158 |
| 私有 TSan worker | 三轮，每轮 94 / 6,201；排除 8 项未完整插桩 controller 对照 |
| TSan 原生状态 / 浮点 | 三轮，每轮 63 / 90,619 |
| ASan 注册入口 | worker 与原生两个入口通过，22.65 秒；本阶段原生只重跑一次 |
| TSan 注册入口 | 两个入口通过，16.80 秒 |
| 普通核心 | 三轮，每轮 36 入口 / 9,614；含一个未请求的空导出入口，排除容量基准 |
| 当前 PureEOS token | 三轮，每轮 3 / 1,471 |
| 状态 / 串行参考 | 57 / 90,530；2 / 5,165 |
| 兼容检查 | 21 组通过，48.77 秒 |
| 审计 / 进程工具 | 24 / 15 项通过；源码清单再次匹配 |

新增 codec 7 项 / 1,932；树私有执行 8 / 257；树 journal 3 / 78；原串行 oracle 1 / 311。覆盖恶意长度、截断和尾字节、固定种子差分、多根与 22 步树、非连续 ordinal、跨通知/inline 的 sender、组内共享而组间重建游标、深度/步骤/事件/字节预算、取消与复用、1/2/4 独占线程、43 类 journal 篡改和缺失状态/代码。

原 controller 对照验证根通知先于 inline 的真实回执顺序、发送当场权限拒绝及缺失 permission、显式 `eosio.code` 授权后成功、孙动作 trap 撤销。私有预检能成功但原授权失败是刻意覆盖的边界，不能把私有成功误认作已授权。sanitizer 覆盖私有执行/状态及 SoftFloat，不是完整节点插桩。

## 真实进程回归

本批进程位于 `/dev/shm/pureeos-g5c3a-process-sVavaW/`，全部使用固定的最终产物，运行期间不重链接：

- `parallel-producer-30fowasr`：代码通知五模式（旧程序、新程序关闭并行及 1/2/4 线程）通过；每模式 16 笔 / 32 根动作，启用模式各 2 批、128 次试算、16 成功、4 采用、12 回退，旧程序强校验重放通过。
- `parallel-producer-40a76z0t`：当前 token 五模式通过；每模式 16 笔 / 33 根动作，启用模式各 2 / 128 / 16 / 6 / 10，旧程序强校验重放通过。
- `parallel-process-rhe6sc5w`：代码通知恢复语料 55 阶段通过。语料 `parallel-notification-preflight-20261012-1IgvVV/coded-recovery-corpus-v1`，manifest SHA-256 `76165ddb81dd0930b2e8bfb76a9e6e62943529962de7f67cd9382b9a022adb9b`。包含逐块检查、重启、SIGKILL 后恢复、损坏/截断拒绝、快照、混合版本 P2P 和竞争分叉。

这是既有正式通知/token 路径的回归，**不是普通 inline 正式采用测试**。未重跑旧单动作 55 阶段语料、全业务长矩阵、长驻或饱和容量实验；ENABLE_OC=OFF。修正了上一阶段报告中两种恢复夹具名称写反的问题，实际报告和文件未变，见 [G5c2b 更正](PX7_G5C2B_RESULTS.md)。

## 失败与修复记录

首次树夹具拼接多一个右括号，`tree-first.log` 在执行器前失败，修复后通过。扩展 sender/oracle 后 `tree-sender-first.log` 因测试向 carol 重复部署完全相同代码被原检查拒绝；仅重部署实际改变的 test/dave 后通过。保留所有失败及中间日志，不将它们计作最终通过，也未放宽生产保护。

## 产物与可恢复空间处理

最终 nodeos-parallel：`089e033f1054a9bf317d03463c62481a9186631c51b9e72d88c676e595e4bf91`；核心：`00f7868da55665339a46531a860f5c561b7a6a53280cc1b5d1c7a9203094e65a`；普通 worker：`22b94812227fecc1128252d936bac84286a7800bb888671c4a1584cdf1ea67b5`。原 nodeos 保持 `e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c`。

四份归档均权限 600，生成后逐文件比较通过：

- `g5c3a-private-asan-artifacts.tar.gz`：`5fcd974aa7f4294dfa98e2e4865296d7ed4b3525bb1a10b6a474d20f4cf66af8`。worker `97d64016b60b022bce72b227e64fdf38a89b1828f271789b1dd27f0313a6b261`；原生 `700169c63c9c4ec989682a50c79751e2e348406adedfcd0975829c4c3a4adca8`。
- `g5c3a-private-tsan-artifacts.tar.gz`：`cee529dd9c0b8b76e19555892ed692af26c8fb3a75270d205947111187a6766c`。worker `94ce2b269c688c0306ebe23191d4cc0886348b458949cd1d275b2fdd57117217`；原生 `448abd345e9c87eb65b0255fc745be172fbe37fff5ff7d89ecbc7296a46e1ce1`。
- `g5c3a-runtime-089e033f.tar.gz`：`4411309c7ce9abe3f165a7cfab45ba73ec967a64d2eaf43d3c8360ff9d3d0ea7`。六个未插桩 Release 程序；所附 CMakeCache 的额外 sanitizer 模式为 thread，不表示这六个目标插桩。
- `g5c3a-process-evidence.tar.gz`：`2e130ffcb1f5a8d934925f000198bd3e2501842105d74aa3fc69a0c1ec2799af`。包含三个完整进程目录及实际代码通知恢复语料。

再次核对进程归档哈希、逐文件比较和三份通过报告，确认无运行 nodeos 后，只删除这三份测试中的 73 个 `data/state/shared_memory.bin`，释放 322,650,112 字节，可从进程归档恢复。区块、快照、日志、配置和用户目录保留。继续 a3，不把此检查点当作全面并行完成。
