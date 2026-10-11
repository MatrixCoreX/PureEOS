# PX7 G4a：既存行 resize、RAM payer 与交错通知

日期：2026-10-12。分支 `feature/parallel-tx-execution`，起点 `0c1f7ab`（G3b）。本批按下述边界完成验收；未开启实验或上线现有链，G4 整体尚未完成。

## 实现边界

私有 WASM 可修改既有主键行的长度与 RAM payer；重复更新按原序保存，`same_payer` 取最新私有行的 payer，旧迭代器读取当前私有长度与内容。正式提交仍用原 `db_update_i64`，由原权限、RAM、事务撤销逻辑判定接受或拒绝；没有让 worker 操作 Chainbase 或正式资源管理器。

通知日志增加“前面已有几次写入”，owner 在任何变更前校验全部接收者与位置，然后按原顺序交错调度通知和更新数据。通知接收者带代码/原生处理器仍回退。返回值只在 root 成功结束后发布。

本批不包括创建/删除行、新表/删表、二级索引、多 action、带代码通知、inline、sync-call；这些仍整笔走原串行路径。没有修改系统合约、经济/治理规则、正式配置或旧二进制。

## 本批发现并修复的问题

1. 扩展 resize 后，旧游标缓存的行长度已不可靠。`db_get_i64(iterator, ..., 0)` 改为读取当前私有元数据，包含中途清空再扩容。
2. 不能把多次更新压成最终值：付款人 A→B→A 中间仍可能发生权限/账户错误。日志保留全部中间值和 payer，由原 host 逐项执行。
3. 初版将所有通知放在所有写入之后。新失败测试在 1/2/4 worker 下均发现：RAM 付款人不存在时账本虽已撤销，失败 trace 却少了写入前调度的通知。保留失败证据 `ram-pre-order-check-2.log`，3 个断言失败；已改为按写入位置交错重放，后续复测结果另列。
4. 测试初版在原生创世夹具上调用 `setalimits`，但该夹具只有原生 ABI，没有 BIOS 的该 action，导致序列化失败。改为在测试链上先安装仓库真实 BIOS，再通过已签名交易设置配额，并让独立验证链重放；没有用直接改数据库或放宽 RAM 限制绕过测试。

## 验收证据

证据根目录：`build/local-tests/parallel-ram-20261012-eBmAKx/`。所有失败日志保留，旧报告仅作为历史证据，不用于替代本批验证。

- 状态测试 `state-ordered-{1,2,3}.log`：每轮 53 用例、36,882 断言通过。
- 审计快照一致；审计工具 24 项、进程工具 12 项离线测试通过。
- 核心/生产差分 `core-{1,2,3}.log`：每轮 24 个入口、5,507 断言通过；其中 23 个功能用例，1 个未请求导出的空 exporter 入口，性能 benchmark 另行跳过。
- 当前 PureEOS token `pureeos-token-{1,2,3}.log`：每轮 2 用例、1,194 断言通过。默认完整核心用例使用仓库原版 EOS token；两者固定哈希与 G3b 一致，未修改合约。
- 普通 worker `worker-{1,2,3}.log`：每轮 45 用例、1,748 断言通过。
- 独立 RAM 差分 `ram-ordered-early.log` 和 `ram-ordered-1.log`：每次 2 用例、1,285 断言通过，包含先前失败的通知交错场景及第 2/3 次写入失败。
- 串行参考 `serial-reference.log`：2 用例、5,165 断言通过。
- 兼容回归 `compatibility.log`：21 组、183 用例、3,104 断言通过，耗时 50.56 秒。
- 原生状态/SoftFloat 的 ASan/UBSan/LSan `native-asan-{1,2,3}.log`：每轮 59 用例、36,971 断言通过。
- WASM 工作线程 ASan/UBSan/LSan `worker-asan-{1,2,3}.log`：每轮 45 用例、1,748 断言通过。
- 新旧真实生产进程：PureEOS token RAM 模式三轮、原版 EOS token RAM 模式一轮，均五模式通过。每个启用模式有 2 批次、80 次有界探测/尝试、16 次成功试算、6 次正式采用、10 次串行回退；16 笔均最终确认。三个节点 SHiP trace/state 字节一致，旧程序强校验重放通过，正常 slot 间隔 0.5 秒。
- 恢复三轮：每轮 55 阶段通过，报告目录 `parallel-process-ktsy4tv9`、`parallel-process-y_c0py2j`、`parallel-process-_pwyyuvi`，含新旧 P2P、重放、快照/重启、分叉及历史输出对照。
- TSan `native-tsan-{1,2,3}.log`：每轮 59 用例、36,971 断言通过；`worker-tsan-{1,2,3}.log`：每轮 41 用例、1,509 断言通过。与 G3b 一样明确排除 4 个混合原 controller 的 oracle 用例；它们在普通和 ASan 运行中通过，不以跳过充当 TSan 已测。sanitizer 只覆盖明确配置的隔离层及 SoftFloat，不是整节点/所有依赖全量插桩。

生产差分覆盖 0/1/2/4 worker，使用原非显式 CPU 计费；用例包括改长/改短/清空、payer 切换、缺少授权、第一次/第二次/第三次写入时付款人不存在、最终 RAM 超额、以及中间超额但最终净值恢复的合法交易。区分“root 采用私有结果”与“整笔最终成功”：后续原 RAM 配额检查可以拒绝已采用的 root，并撤销整笔交易及停止采用批次尾部。

真实进程新增 `--token-fixture --ram-fixture`：固定 16 笔真实 token 交易中，第一笔使用原来由 alice 支付余额行 RAM 的 sponsor 发送，检查 alice 退款、sponsor 付费、净额为零，并要求实验生产确实采用该交易。新旧节点相互验证、旧程序强校验重放、SHiP trace/state 对照仍为必要条件；此有限负载不是容量/TPS 测试。

PureEOS 三轮报告目录分别为 `parallel-producer-l1mc9i_x`、`parallel-producer-v5mdf43_`、`parallel-producer-p7fpk99q`；原版 EOS 为 `parallel-producer-4dbipike`。每组首笔均观测到 alice 的 `-128` 与 sponsor 的 `+128` RAM 变化。

## 可核对产物

保留旧 nodeos 的 SHA-256 仍为 `e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c`。本批 `nodeos-parallel` 为 `d78602872f08d5d27abf0c1efc5a43c27922101fa640bb5ba15ce9a4205fbeab`，核心测试为 `8b2ac4d51e1194ef069a69882af28caee1aaaf2e0485d244477f65d75aa605d5`，普通 worker 测试为 `33fd075225536610a4023fd33cdee1b32a961a49c81794d4dfe7d3617c00a923`。

- `g4a-runtime-d7860287.tar.gz`：新程序、5 类普通测试、匹配 CMakeCache 和兼容日志；SHA-256 `ced39b7af5afaf3eff9bb792294e85e0d045f396b22b8ff64749a10e2b69cb69`。
- `g4a-asan-artifacts.tar.gz`：两个实际通过测试的 ASan/UBSan 二进制及匹配 CMakeCache，在切换 TSan 前保存；SHA-256 `ca1589b58d7400a5e5396d93a4c8c3604bc8992827a839d2ea957a2e2b913af9`。各二进制完整指纹在 `asan-artifacts.sha256`。
- `g4a-tsan-artifacts.tar.gz`：两个实际通过测试的 TSan 二进制及匹配 CMakeCache；SHA-256 `050f3606dc97374bbe650d55ff6469063de079e404bf30b55cde726e857b1206`，各二进制指纹在 `tsan-artifacts.sha256`。构建树暂保留 TSan 配置，不把之后同名文件当作 ASan 产物。

本批没有重新运行 14 组长业务回归、180 轮长驻负载或新容量基准；此前记录不算作本批已测。这一阶段不作 TPS 提升或默认启用结论，G4b–G7 仍需继续。
