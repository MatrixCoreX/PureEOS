# 按原配置重建高配 PureEOS 测试网

这份资料用于在新的专用服务器上重建同配置的新链。保留了部署方法、
编排脚本、原版程序、已验证押金合约、过程、结果和 TPS 证明。
不包含原链区块、状态数据库、历史索引数据库或原部署私钥。
新建时重新生成密钥、Chain ID 和区块历史；历史测试成绩是旧轮次的证据。

## 已固定的版本与参数

| 项目 | 本轮原始配置 |
|---|---|
| 机器 | Vultr `vbm-8c-64gb-amd`，AMD EPYC 4345P，8 物理核 / 16 线程，64 GiB RAM |
| 操作系统 | Ubuntu 24.04 x86_64；原轮次实际为 24.04.5 |
| 节点源码 | `MatrixCoreX/PureEOS`，v1.0.0，`e62a7ea30632ad5069a9640fc6bd0e1fe819b159` |
| nodeos SHA256 | `05d62c4a92497153e21833be7d2796d1d1fc9eeb09ebeb4dc1034f41c72ecf67` |
| 程序包 SHA256 | `ab311a293ff179b73ec49ba9239a1a8f768e336e8ae039e82004ec48a29690af`，与 GitHub 发布资产一致 |
| 押金合约源码 | `b230c7e12d7cd0bc05ad8269140dc030f8b5fe79`；完整源码亦保存在 PureEOS PR #2 的源码归档中 |
| eosio.system WASM | `b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170` |
| packed ABI | `f19e4c55e11f5029f4a161865ac0b0ea29f730d4f19fbdc6eb30c29c10ef5512` |
| 区块 CPU / NET | CPU16：3,200,000 µs；NET 4,194,304 字节/块 |
| 并行执行 | 8 workers，batch 24，group 8；完整参数以原脚本和 `chain-parameters.json` 为准 |
| 状态容量 / 交易队列 | 16 GiB / 1 GiB |
| RAM | 初始 64 GiB；既有 `setramrate(1024)` 初始化，每块增长 1024 字节 |
| AIC | 初始发行 100,000,000.0000；最大 2,100,000,000.0000 |
| 押金 | 每 BP 10,000 AIC；退出冷却 604800 秒；罚没锁入 eosio.bslash |
| 保留模式 | 单选投票、原出块/最终确认/轮岗、10%/90% 资源分配、奖励减半与治理；两阶段 deferred 禁用 |

这里的 NET4MiB 是本次高配测试网的实际配置；后续开发默认 NET16MiB
不改变这份历史复建配置。不得把 CPU32 或低配 TEST 版本混入这一轮。

## 快速准备与创始节点启动

先解压本机的 `PureEOS-high-spec-rebuild-kit.tar.gz`，把其中的 `rebuild-kit/`
完整上传到新的同型号专用服务器；至少预留 100 GiB 空间。
Git 仓库保存源码、文档与文件指纹；下面的命令在含二进制和合约的本机重建包中执行。
`REBUILD_SHA256.json` 校验重建包内的文件，默认命令只检查文件，不启动服务。

```sh
sudo apt-get update
sudo apt-get install -y python3 ca-certificates curl rsync tar gzip zstd nginx docker.io docker-compose-v2
python3 prepare_new_host.py
sudo python3 prepare_new_host.py --prepare-new-host
sudo python3 -B /opt/pureeos-bp-bond-newchain-20261102-r1/run_bootstrap.py --allow-new-genesis
```

准备工具拒绝已有部署目录或运行中的 nodeos，校验硬件和文件指纹，
放置原程序与合约，并为 45 个 BP 生成全新账户、生产、peer 和 BLS 密钥。
私钥只写入新服务器 0600 文件，不输出到终端。
创始节点是 `gptgenesisbp`。启动后读取新 `NEW_CHAIN_RECEIPT.json` 和实际 API，
检查代码指纹、RAM 增长、原生职责交接、发行量、CPU/NET 和 deferred 激活。

这些是原部署脚本和新增准备工具；本次退役没有另租服务器重跑一次完整新链。
因此这里不宣称完整的一键复建已重新验收。原切换脚本
`archive_and_bootstrap.py` 依赖旧链，不用于空白服务器的首次启动。

## 后续按原顺序完成

1. 创始节点正常推进并最终确认后，按 `pressure/` 的原方法准备账户、RAM、
   质押、PowerUp 和交易集。若另行决定重跑 TPS，先停业务、其他 BP 和历史服务，
   再运行纯 AIC 转账测量；不得用旧报告代替本轮实际结果。
2. 按 `newchain/deploy_history.py` 与归档中的历史部署配置准备独立非出块 SHIP
   节点、PureHistory 和依赖容器。历史源码保留分支为
   `MatrixCoreX/PureHistory:archive/high-spec-deployed-20261102`，其中记录了
   Hyperion 上游版本和实际 loopback 补丁。容器凭据重新生成。
3. 从新的不可逆链状态建立种子；用 `join_controller_v4.py` 逐步加入到
   40 个 BP + 1 个非出块历史节点。每个 BP 的最终确认安全记录独立，
   不复制旧 BP 的 safety 文件。控制器原有资格和历史就绪检查全部保留。
4. `voting/`、`activity/` 保存了自投加普通账号随机单选投票、不同票数、
   90 个普通账号随机业务和实际空块观察的方法。高配业务使用原 1–20 笔批次；
   低配后改为 1–3 笔的设置不覆盖这里。所有签名和资金使用新链实际账户。
5. 若要重演原故障验证，再添加 `koreamoonlit`、`indiasunrise`、
   `icelandcrown`、`aurorashield`、`quantumhaven` 五个 BP；足额押金、自投和普通票，
   等五个都进入实际原生职责并出块，再停它们。等待真实个人零出块窗口和不可逆
   黑名单，验证锁池恰好罚没 50,000 AIC，剩余 40 BP + 历史节点正常。

后续控制器中旧的 Chain ID、检查点和域名必须绑定到本轮新回执和目标地址；
不能复制旧 `PASS` 文件、签名交易、私钥或状态数据库来跳过检查。
完整命名、押金、注销退款、投票和业务规则见同目录的原测试启动手册。

## TPS 证据与页面

旧轮次峰值：不可逆区块 #920，21,348 笔已执行交易 / 0.5 秒 = **42,696 TPS**。
固定 12 秒窗口最终确认 TPS 为 34,507.3333；1,040,000 笔全部最终确认，未确认 0。
峰值、固定窗口和整批口径分开记录，42,696 不代表持续平均吞吐。
加密证据包保存实际发送记录、最终确认 ID、CPU/链头样本、参数、结果与报告，
没有原始区块数据。

`https://aic.matrixai.one/` 是保留的高配页面快照，独立于已退役 API。
`https://aic-test.matrixai.one/` 是继续运行的低配功能演示。
部署新链后，先验证新 HTTPS API/CORS/Chain ID，再明确决定是否替换公开快照；
不要自动覆盖独立的网站服务器或低配演示。
