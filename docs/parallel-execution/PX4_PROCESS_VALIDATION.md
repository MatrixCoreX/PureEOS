# PX4：受限并行验证的真实进程验证

本阶段完成默认关闭的 nodeos 参数、独立旧二进制对照、真实 P2P、恢复及历史输出验证。**这不是生产者并行打包或 TPS 验收**；PX5/PX6 和完整兼容矩阵的其余部分仍需推进。

## 已实测结果

当前脚本连续三次独立运行，每次 54 个阶段全部通过；每次重新创建目录、进程和端口，不复用上一轮数据库。

| 场景 | 实际比较与结果 |
|---|---|
| 旧串行与新 0/1/2/4 worker | 同一 14 块业务语料，逐块核对 chain ID、block ID、LIB、完整性摘要 |
| 真正采用 | 1/2/4 worker 均 19 次试算、18 次成功、11 次采用、8 次串行回退；不是全部重跑的影子模式 |
| 正常退出与重启 | 保持区块、LIB、状态，旧程序通过原有 P2P 输入，新程序另外覆盖 HTTP 输入 |
| SIGKILL 与重放 | 脏数据库拒绝直接启动；`force-all-checks` + `disable-replay-opts` 完整恢复到原可逆 head，并再次实际采用 |
| 损坏恢复记录 | 每种模式在新副本截断记录、删除可逆父块；均拒绝启动，原记录 SHA-256 不变 |
| 快照 | 各模式生成相同 SHA-256 的快照；独立数据库恢复后，通过 P2P 接收尾部 2 块，状态一致 |
| 关闭实验开关 | 1/2/4 worker 的数据目录均可停机后以 0 worker 重启，无资产迁移或重新创世 |
| 混合版本 P2P | 1 个新串行发送节点，加旧串行和新 0/1/2/4 共 5 个观察节点，逐块执行同一语料 |
| P2P 竞争分叉 | 先采用旧分支写入，再接收较晚 slot 的竞争分支；新并行节点实际撤销后重新执行，状态与旧程序一致 |
| trace/state-history | 普通流与分叉流分别比较 `trace_history`、`chain_state_history` 的 log/index；6 个节点的四个文件分别字节摘要完全相同 |
| 配置拒绝 | 超线程/批次边界、非法/缺失代码哈希、light、JIT、trusted producer、Deep Mind 共 9 种组合明确拒绝 |

历史插件使用默认非 debug 输出；原实现将本机耗时/调试控制台字段置零或省略，因此这里可以对比完整文件摘要。不是把本来允许变化的运行时耗时强行要求相同。快照/不可逆日志建立的 LIB 下限不会因尾部区块携带较旧 QC 而回退，脚本显式校验这一点。

JSON 区块持久化缺陷是本次真实进程测试发现的原有问题，已修复；详见 [独立缺陷记录](HTTP_BLOCK_CACHE_FIX.md)。失败证据保留，不计入通过结果。

## 二进制、语料与证据

证据根目录：`build/local-tests/parallel-overlay-20261011-q42k0b/`。

- 旧 nodeos：`e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c`。
- 新 `nodeos-parallel`：`1e71dfc8c57582cd7ef009e1a986514a927d27df43ae3f86adb0d790c69f1ff2`。
- 进程脚本：`d28e3c9adfed6d783823bc1767461355e910fd37ad21449e6bd21fe40e3f8722`。
- 固定业务 manifest：`abdf4548b94eb0c2fa0d5f906d9565e882afd9ea9c8c964b02e95908b5efb169`。
- 二进制归档 `px4-nodeos-1e71dfc8-artifacts.tar.gz`：`5034e9ee6a07b2d4cfde44217c4fa0a29e7429132c80729168dee489c66e58f1`。
- 语料归档 `px4-business-corpus-v1.tar.gz`：`c165aed8579ae5cfe02c460d1c7e7d095a97797f7cfca1c099b015a2bedef868`。
- 三次完成报告：`parallel-process-ibe08xvo/report.json`、`parallel-process-exattbfq/report.json`、`parallel-process-_97n5_wr/report.json`。
- 新增 JSON 原生回归后，验证套件为 6 用例 / 780 断言，三次独立通过：`validation-json-cache-repeat3.log`。
- 进程工具自身 10 个离线用例通过；源码审计 24 个用例通过，791 文件指纹匹配。

固定语料由原串行路径生成并包含原签名，旧程序独立执行；没有将新执行结果复制成旧程序的数据库。导出只含公开创世数据、区块、代码哈希及检查点，不含私钥/钱包。

## 重现与默认关闭

构建时使用 `-DNODE_EXECUTABLE_NAME=nodeos-parallel`，目标也是 `nodeos-parallel`。这使旧 `programs/nodeos/nodeos` 不被覆盖；可执行文件名只允许简单 basename。

在新建、独立配置目录中，受限验证参数为：

```ini
wasm-runtime = eos-vm
validation-mode = full
experimental-parallel-validation-threads = 2
experimental-parallel-validation-batch-size = 8
experimental-parallel-code-hash = <已经审计的64位WASM哈希>
```

线程默认 0，范围 0–16；批次默认 8，范围 1–64。只支持已审计的受限主键操作；未配置代码和不支持能力仍串行。OC、profiling、Deep Mind、trusted producer 等与实验入口互斥。参数不改变链上合约准入或共识。

测试运行：

```bash
python3 -B tests/parallel_execution_test.py \
  --old-nodeos /path/to/saved/nodeos \
  --new-nodeos /path/to/nodeos-parallel \
  --old-sha256 e28d831303fc6de5fe5bf62f1b165eb26128bf984e7532c75dd0a7edc5deb02c \
  --corpus /path/to/business-corpus-v1 \
  --output-parent /existing/test-output-directory
```

仅对自己的子进程发信号，只损坏新副本，所有端口为 loopback，历史插件网络监听关闭。不连接外部链，不读取原 `config-dir/`、手动测试链或钱包。恢复旧程序时使用不含新实验参数的配置副本。

## 仍不能据此声称

- 尚未有多线程生产者；这些区块由原串行执行产生，T25 的“新并行生产者 → 旧验证者”仍待 PX5。
- 未测试物理断电、真实多机容量、生产环境长跑或 TPS 提升。
- 故障进程没有签名私钥；finalizer 签名安全继续依赖既有专项回归，不把无签名观察者的强杀当作全部签名故障覆盖。
- 通用代币、增删行、二级索引、通知/inline、sync-call 等尚未并行化；它们的完整串行回归和混合边界仍在补测。

下一步按 [生产端计费与批次设计](PX5_PRODUCER_DESIGN.md) 实现默认关闭的生产实验，并补齐限额、回调、截止和旧程序互验。
