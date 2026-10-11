# PX7 G3b：真实 token 转账与有序通知验收

日期：2026-10-11 至 12 日。分支 `feature/parallel-tx-execution`，本批起点 `3637260`。本页只记录本批新代码，不复用 G3a/PX6 的测试结果冒充重测。实验仍默认关闭，不改已有链、系统合约经济或治理。

## 实际支持边界

新增 `is_account` 的有界正/负账户副本及依赖。owner 读取 `account_object/by_name`，worker 未知账户只返回带类型的取数请求，不持有 controller/Chainbase 指针。账户身份/存在性重新核对，但不绑定通知会改变的接收序列、授权序列或无关 ABI；最多复制/读取 256 个账户，仍沿用最多 8 次探测及原本地总预算。

`require_recipient` 产生最多 64 个去重且保留首次出现顺序的接收者；root 自身已视为通知过。正式采用前，owner 检查全部接收者存在、无 WASM、无对应原生 handler；不支持则在任何正式写入/调度前返回原执行。准入后通过原 `require_recipient/exec_one` 生成通知、ordinal、receipt、sequence、RAM/CPU 检查和撤销，**没有省略无代码通知**。

当前 25 个受限 host、9 个仅调用回退桩、228 个拒绝链接导入。新增 `send_inline` 仅回退桩是因为仓库原版 token 的发行逻辑引用它；实际执行内联调用仍立即丢弃整个尝试，没有空实现。其余桩和全部拒绝接口仍逐项测试，262 项生产注册没有自动批准。

只覆盖单 action、已有余额行、同 payer/同长度、无代码通知的转账。新收款余额行、首次付款导致 RAM payer 改变、带代码通知（即使 no-op）、inline/sync-call、二级索引等仍整笔回退。第一次回退仍丢弃本批余下旧试算。G4–G7 没有因此完成。

## 真实合约与差分检查

没有改写 token 业务来迎合并行：

- 仓库 `unittests/contracts/eosio.token/eosio.token.wasm`：`4c76fdcd411cafbf3af1c0987bbb093bac2bfd2e8a669f12feae785ffd43a7c3`。
- 只读 `reference-contracts/build/pureeos-resource-split/eosio.token/eosio.token.wasm`：`ea1bd149e28f21420450b6c7b2c5be600ef9922e44c383fa4489b025aac0eb46`。用 `PUREEOS_PARALLEL_TOKEN_WASM` 显式选择并核对固定哈希，不引入任意外部 fixture。

已有余额行由所有者支付 RAM。两个 WASM 均在 0/1/2/4 worker 下检查独立转账、热点、先入后出、新余额行、payer 转移、转移后重新可采用、通知代码安装/移除、透支、不存在接收者及通知合约拒绝。失败交易核对原异常码、全量相关 trace、完整性哈希恢复、未进入去重表，后续合法交易继续成功。

`consensus_trace` 比较增加 action/creator/ancestor ordinal、receiver、原 action、context-free 标志，原 receipt、返回值、控制台、RAM 和事务资源字段保留。独立 WAT 检查 root/self/重复通知去重、首次顺序及连续两笔读取同一通知账户不会被 receipt 序列变化误冲突。worker 另测 start 通知、未知账户请求、负读取、预算、跨线程拒绝、私有写后失败及模块复用。

每个启用模式，token 验证检查点序列实际采用 9 笔、回退 9 笔（不计初始化）；生产专项 6 轮采用 5 笔、回退 7 笔，含 3 笔正式拒绝及后续合法交易。每轮直接断言采用数，不是只看后台执行过或余额相等。

## 本批证据

根目录：`build/local-tests/parallel-token-20261011-tte5Vn/`。

| 检查 | 已完成记录 |
|---|---|
| 状态/账户副本 | 53 用例 / 36,779 断言；三轮通过，另有一次先行测试 |
| WASM/工作池/顺序协调 | 44 用例 / 1,719 断言；三轮通过 |
| 核心验证与生产 | 22 个入口 / 4,222 断言；三轮通过；其中一个是未请求导出的空入口，实际 21 个功能用例 |
| 当前 PureEOS token 专项 | 2 用例 / 1,194 断言；三轮通过，不含在默认原版 token 的统计中 |
| 审计与工具 | 24 项审计单测、12 项夹具自检通过；791 文件 / 262 host 的清单同步，0 自动批准 |
| 当前 token 真实生产进程 | 三轮五模式通过：新 0/1/2/4、旧程序；每模式 16 笔，旧程序完整重放和 SHiP 对照 |
| 仓库原版 EOS token 真实生产进程 | 一轮五模式通过；每模式 16 笔，同样核对实际采用、旧程序完整重放和 SHiP |
| 原 WAT 生产夹具回归 | 一轮五模式通过，确认新增 token 选项没有改变默认夹具路径 |
| 私有 WASM TSan | 40 用例 / 1,480 断言，三轮通过；排除 4 个未插桩 controller 对照，不增加抑制 |
| 原生隔离/SoftFloat TSan | 59 用例 / 36,868 断言，三轮通过 |
| 新旧验证/恢复/分叉/SHiP 进程 | 每轮 55 阶段，三轮通过；沿用固定 WAT 语料，不冒充 token 专项 |
| ASan/UBSan/LSan | WASM 44 用例 / 1,719 断言，原生隔离 59 用例 / 36,868 断言，各三轮通过 |
| 原路径兼容 | 21 组 / 183 用例 / 3,104 断言通过，包含通知/ordinal、权限、资源、RAM、只读和 sync-call |

当前 token 的三轮真实生产报告：`parallel-producer-h52jje2o/report.json`、`parallel-producer-7ljc7vd1/report.json`、`parallel-producer-s7bbj5c2/report.json`；原版 token：`parallel-producer-_hiusfg0/report.json`。每个启用模式 2 个批次、80 次试算（含取数重跑）、16 次完整私有成功、6 笔正式采用、10 笔原序回退。全部 16 笔最终确认；原 500ms 连续出块保持。14 秒限量负载不是饱和 TPS，不据此推算性能倍数。

恢复报告：`parallel-process-2ayrllen/report.json`、`parallel-process-4ktctsq_/report.json`、`parallel-process-vru1711k/report.json`。三个报告均为 `passed` 且各 55 阶段。恢复试验沿用固定既存行 WAT；新的 token 通知另有上面的完整 controller 与真实生产互验，不能混淆语料范围。

原 WAT 默认生产路径报告：`parallel-producer-9odyo2by/report.json`，五模式通过，负载明确标记为 `existing-row-wat`；没有将其统计为真实 token 测试。

## 构建与运行记录

当前 `nodeos-parallel` SHA-256：`39df12c76dca66368341acad7bcf55cf6ba48598a290bb60b268699a5a148f5b`。原 nodeos 仍为 `e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c`，没有替换。代码复审后移动了一段“仅回退桩”的注释，重新构建确认运行程序哈希不变。

普通产物归档 `g3b-runtime-39df12c7.tar.gz` 保存新程序、四个普通测试程序、两个固定 token WASM/ABI、仓库原版 token 源码、CMakeCache 和完整兼容日志，SHA-256：`72d978a5f1eff2137ae0cb2f3749823bd2fa58d27e23e76a9a767d3f3ecc5e75`。核心测试程序为 `244b0fdfb0ddccfcc85f9da13de7cd1631cd3b344f200f944b5e52a5e03eda1f`，普通 worker 为 `fecb559e1ee940295b3c263bdb8eee1997858115c729a346b59e14167274df8a`。

TSan 产物 `g3b-tsan-artifacts.tar.gz` 保存两个检测程序及当时的 `CMakeCache.txt`，SHA-256：`9e763ae7579044984e84d7d10dfefb8d484e7dab4344eca40a4be360ef587a7a`。重新配置 ASan 前已归档，不把同路径后来覆盖的程序混作 TSan 检测产物。

ASan/UBSan/LSan 产物 `g3b-asan-artifacts.tar.gz` 同样保存两个检测程序及匹配配置，SHA-256：`921ba60fdebfb2764341b85c5a0d3a20df419b82fe64bc5f1ffe282a2c4968f7`。WASM 检测程序：`2c03948a7dcc8140619abea6c09d83a38f5e1fe8eb58d0e2d296045a7441ef3a`；原生检测程序：`25284cf9463aef16da2a0ad85dae2d640c65fb109dc37ead715dc7a397c7ac92`。构建目录最终保留 `PUREEOS_PARALLEL_SANITIZERS=address,undefined`，不是 TSan 程序；检测覆盖私有执行器/VM 模板/状态层与 SoftFloat，不是整个 controller、Chainbase、FC 的全量插桩。

`build-chain-1.log`、`build-core-1.log` 保存编译；首次误用不存在的 `chain` 目标后改用 `eosio_chain`。`worker-new-1/2.log` 是用错测试 suite 的筛选错误，没有用例执行，不计作通过；正确 suite 的 `worker-new-3.log` 为 2 用例 / 25 断言通过。上述工具调用错误没有通过删除断言或放宽协议限额处理。

本批 G3b 的上述功能、安全检测与互操作检查完成，G3 可在本页明确边界内验收。没有重跑原业务 14 组长回归或 180 轮长驻实验，没有云端饱和 TPS、物理断电/磁盘损坏或主网上线结论。G4 生命周期/RAM、G5 多 action 与调用树、G6 二级索引/一般接口、G7 性能与推广仍未完成；不得把 G3 的局部采用称为全面通用并行。
