# PX7 G4b：主键与表生命周期

日期：2026-10-12。分支 `feature/parallel-tx-execution`，起点 `2d03650`（G4a）。本批按以下边界完成验收；实验仍默认关闭，G5–G7 未完成。

## 实现与安全边界

- 私有状态支持主键创建、更新、删除及表创建/最后一行删除/重建。每次操作保留类型、顺序、payer 和值；新增对象使用尝试内负数身份，绝不把私有 ID 写入正式数据库。
- 正式提交调用原 `db_store_i64/db_update_i64/db_remove_i64`，由原路径分配正式 ID、结算表费/行费、检查 RAM/授权并撤销失败交易。通知与写入仍交错重放，不能丢失失败前已经调度的通知。
- 原迭代器不能因同键重建而复活；新 action 仍沿用原动作级游标范围。私有执行碰到退役表的旧结束句柄时回退原串行，不自行发明原缓存的失效规则。
- 稀疏查询跳过私有删除项时，继续向 owner 请求原邻居证明，再与私有插入键合并；未知记录不当作不存在。结构变化使该表后续旧试算保守失效，包括插入后删除的净零变化。既存行更新保留精确主键冲突检查。
- 当前 262 个生产接口分为 27 个受限支持、7 个仅链接且调用必整笔回退、228 个拒绝链接。新增 store/remove 不等于任意合约都已支持：多 action、带代码通知、inline、sync-call、二级索引及系统原生操作仍不在本批范围。

没有修改系统合约、经济/治理规则、正式节点配置或旧 nodeos，没有开启现有链的实验。

## 本批修正与失败证据

审计测试发现 store/remove 方法已实现，但导入注册仍标为 `ISOLATED_FALLBACK`。已改为受支持注册，保持原有注册顺序；两个宏的运行时展开一致，修正前后 nodeos-parallel 的二进制 SHA 相同。原失败 `audit-tests.log` 保留，修正后的审计 24 项与快照一致检查通过。

新增生命周期恢复测试首次编译时将传入原 tester 的交易声明成 const，而原接口需要可变引用。已修正测试变量并完整重编译；原 `build-registry-final.log` 保留，不把编译失败当作运行时检查通过。

## 已完成测试

证据根目录：`build/local-tests/parallel-lifecycle-20261012-JTMWTP/`。所有失败和初步运行日志保留；下述三轮结果均来自本批代码，不引用旧版本作为替代。

- 状态层 `state-final-{1,2,3}.log`：每轮 57 用例、90,530 断言。包含三个固定随机种子的混合增删改，与独立 `std::map` 模型比较五类查询、主键 0/最大值、表计数/代际、值/payer/长度及旧句柄失效。
- 核心验证/生产 `core-recovery-{1,2,3}.log`：每轮 27 个入口、6,808 断言；其中 26 个功能用例、1 个未请求导出的空 exporter 入口，容量 benchmark 跳过。
- 当前 PureEOS token 专项 `pureeos-token-recovery-{1,2,3}.log`：每轮 2 用例、1,310 断言。完整核心默认使用仓库原版 EOS token；覆盖首次收款、open/close、最后一行删除与再建、同块结构冲突。
- 普通 worker `worker-registry-{1,2,3}.log`：每轮 47 用例、1,782 断言。包含私有创建/删除、同键重建、通知位置、旧迭代器与错误 host 签名。
- 生命周期生产差分覆盖 0/1/2/4 worker、9 类场景：删表重建、保留表的增删重建、只删除、旧迭代器错误、写后 trap、付款人缺少授权/合法授权/不存在、最终 RAM 超限。原 trace（含异常码/通知位置）、状态、RAM、对象 ID、返回的迭代器数值及整笔撤销均与串行核对。
- 生命周期分叉/undo/快照/重启专项 `lifecycle-recovery-core.log`：1 用例、116 断言；1/2/4 worker 在竞争分支切换、快照恢复、数据库重开后继续增删重建并实际采用，核对全状态与 trace。该用例也已纳入上述三轮完整核心运行。
- 串行参考 `serial-reference.log`：2 用例、5,165 断言。
- 兼容矩阵 `compatibility-final.log`：21 组、183 用例、3,104 断言，49.23 秒。
- 原生状态/SoftFloat ASan/UBSan/LSan `native-asan-{1,2,3}.log` 与 TSan `native-tsan-{1,2,3}.log`：每轮 63 用例、90,619 断言。
- WASM 工作线程 TSan `worker-tsan-{1,2,3}.log`：每轮 43 用例、1,543 断言。仍排除 4 个混合原 controller 的 oracle，用例没有被误算为 TSan 已覆盖；sanitizer 只插桩明确配置的隔离层及 SoftFloat，不是全节点/所有库全量插桩。
- 审计工具 24 项、进程工具 13 项离线检查通过；新 CLI 检查会拒绝生命周期/RAM 夹具混用，或不带 token 的生命周期模式，不创建子测试链。
- 真实 PureEOS token 首次余额行创建三轮：`parallel-producer-ergwvqyd`、`parallel-producer-dcr7vb0b`、`parallel-producer-c0z6s9k4`；原版 EOS token 一轮：`parallel-producer-y2ow10kk`。各轮旧生产者/新 0/1/2/4 worker 五模式通过；每个启用模式 2 批次、80 次实际探测/尝试、16 次成功试算、6 次正式采用、10 次串行回退。首笔 alice 新增 240 字节 RAM，16 笔全部最终确认；新旧 P2P、旧程序强校验重放、三个节点 SHiP trace/state 字节一致，正常 slot 间隔 0.5 秒。

另保留初步首次收款运行 `parallel-producer-app0nffs`，nodeos 与最终三轮相同，但 exporter 是加入新恢复测试前的版本，不将它替代最终三轮。

## 补充验收与磁盘保护事件

- WASM ASan/UBSan/LSan `worker-asan-{1,2,3}.log`：每轮 47 用例、1,782 断言通过，包括 TSan 明确排除的 4 个原 controller oracle。
- 当前 PureEOS token RAM 模式回归 `parallel-producer-3b9bn06a`：五模式通过，首笔 alice `-128`、sponsor `+128`、净值 0；正式采用/回退、最终确认与新旧历史互验检查仍通过。
- 完整恢复通过三轮：`parallel-process-tzuxkb5t`、`parallel-process-00dsmuwy`、`parallel-process-o3m4nud9`，各 55 阶段，含新旧 P2P、重放、快照/重启、分叉、历史输出对照。
- 另一次 `parallel-process-x4vx43tj` 在完成 49 阶段后失败：磁盘空闲低于 1 GiB，旧 nodeos 原磁盘保护正常关闭进程，日志明确为 `exceeded file system configured threshold`。没有修改/降低保护阈值，也不把该次计为通过。归档已结束测试的数据、释放空间后，重新完整运行得到第三个 55 阶段通过报告 `o3m4nud9`；失败目录完整保留。

为释放空间，对本批 8 个已结束进程测试目录做完整稀疏归档，并用 `tar --compare` 核对全部成员成功后，仅移除其中 176 个生成的 `data/state/shared_memory.bin`，释放 785,403,904 字节分配空间。精确清单是 `g4b-archived-state-files.txt`；所有配置、日志、报告、区块和快照仍在原位。数据库文件可从下面的完整进程归档恢复；失败那一轮、原程序和用户配置均未清理。恢复应解压至独立目录，不覆盖正在运行的节点。

## 可核对产物

旧 nodeos SHA-256 仍为 `e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c`。本批 nodeos-parallel 为 `5de386b101424239cc1848aaebef4467106a83d2732a3aa42bad717355266271`，核心测试 `f57371178e5293f2473905cc7f1f09228a0ae0d77984337cac0075bd788c4bed`，普通 worker `0ac267809a2d59f3227431f9f6b7212e0d0ca474fb5bf978f2bd930d36b47bf2`。

- `g4b-runtime-5de386b1.tar.gz`：新 nodeos、状态/worker/核心/串行参考测试、原生 ASan 测试、匹配构建配置及兼容运行日志；SHA-256 `40cee22983c35a57b2b266522126620c758bbb1b262f2d4a462c28cfb6a6f65c`。归档中新 nodeos 内容的哈希已独立核对一致。
- `g4b-asan-artifacts.tar.gz`：两类 ASan 测试与匹配 CMakeCache，SHA-256 `1754f93c2a6af8839dd9074c66e9df8f50e7783816581d954ebdc507299215bc`；二进制指纹在 `asan-artifacts.sha256`。
- `g4b-tsan-artifacts.tar.gz`：两类 TSan 测试与切换配置前的 CMakeCache，SHA-256 `2b4c7879e39ec3cdaad56f54b8b87302ae883a26d9e1dd3f14e2b76a37da5e64`；二进制指纹在 `tsan-artifacts.sha256`。不会把之后同名 ASan 文件当作 TSan 产物。
- `g4b-completed-process-data.tar.gz`：上述 8 个已结束进程目录的全部内容（6 个 producer，包括初步样本；2 个恢复目录），SHA-256 `3a43fe6460eaa28cbde37b78d9763c410ef65f4ec482aa0cbba81900c20dc72d`。不包括之后的恢复重跑，也不包括失败目录；两者仍完整保留。

`main` 与基线标签仍指向 `a4a40a6a1e72c531cd8267758074229ccc0655ce`；reference-contracts 仍为 `c454378bf2efc4e15fa2c55169ba4749795f3534` 且无工作区变动。

本批未重跑 14 组长业务回归、180 轮长驻压力或容量基准。16 笔固定进程负载只证明兼容、采用与最终确认，不是 TPS 上限或性能提升证明。G5–G7 仍需继续。
