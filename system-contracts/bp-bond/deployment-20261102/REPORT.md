# BP 押金新链部署与单 BP TPS 记录

状态：**PASS**。本轮日期为北京时间 2026-11-02；原始凭证使用 UTC。

## 部署结果

通过：40 个 BP＋1 个非出块历史节点，押金合计 400,000 AIC，实际生效确认策略 21 人、门槛 15。

新链 ID：`9924d9d6405276c9e4cfee86a5b497a19b5c3fa4d576508689884b8be47a0bac`。创始 BP：`gptgenesisbp`。其他 39 个使用完整 12 字符 AI、国家意象、自然星空名称；这些都是同一保留服务器上的模拟身份，不代表有关企业或国家的实际节点。

普通 BP 本人缴纳 **10,000.0000 AIC** 才能注册；安全职责交接、旧窗口结算后冷却 **604800 秒**，退出检查点不可逆后退款。托管账户为 `eosio.bond`；永久黑名单押金在实际决定块不可逆后结算至 `eosio.bslash`，本版没有转出、分配或销毁接口。`eosio.token` 仅为 AIC 代币合约。封存后的托管 owner 无可满足授权，active 仅为协议代码权限；开发者没有直接提款私钥，未来代码升级仍受既有七天审查及 BP 否决机制约束。

## 硬件和固定参数

- 保留的 Vultr `vbm-8c-64gb-amd` 裸金属，AMD EPYC 4345P，8 物理核／16 线程，标称 64 GiB RAM，实际可见 64,880,889,856 字节；Ubuntu 24.04.5 LTS＋已发布打包运行时。没有新增服务器。
- CPU16：3,200,000 µs／块；NET：4,194,304 字节／块；本地状态容量 16,384 MiB；每笔交易限制及计费保持既定预设。
- 8 并行 worker，batch 24，early-adoption group 8，chain threads 6；P2P 单消息 64 MiB，每连接在途 100 MiB，生产队列 1 GiB。
- 初始链上 RAM 64 GiB，显式 `setramrate(1024)`；保持两个 deferred 禁用特性。保留单选整数投票、原生轮岗与最终确认、维护、10%／90% 资源、精确 1% 奖励及减半、七天升级治理。
- 初始发行 100,000,000.0000 AIC，最大供应 2,100,000,000.0000 AIC；未预发剩余额度。

## 一次单 BP 纯转账测量

8 条发送连接，81,920 对账户，80,000 offered TPS，1 秒预热、12 秒固定窗口、90 秒排空。账户、RAM、自有质押、自付 PowerUp、资金和签名均在计时前准备。计时期间只有创始 BP nodeos；生产者 SHIP 关闭，其他 BP、历史节点和全部历史后台停止。`ONLY_BP_SCOPE.json` 和 `SENDER_READY.json` 保留了真实进程及连接证据。

| 口径 | 本轮实测 |
|---|---:|
| 发送／最终确认 | 1,040,000／1,040,000 |
| 未确认 | 0 |
| 固定窗口入块 TPS | 34614.75 |
| 固定窗口最终确认 TPS | 34507.33 |
| 全交易集入块 TPS | 37645.13 |
| 全交易集最终确认 TPS | 36450.89 |
| 节点 CPU（逻辑核等效） | 4.681 |
| 主线程 CPU | 95.88% |
| 窗口内节点峰值 RSS | 1,881,681,920 字节 |
| 实际规范块内重复账户对 | 0 |

全部接收交易的规范不可逆凭证、完整整数余额账本、供应量和 CPU/NET 上限检查通过。81,920 对工作集在本轮规范块中未出现同块重复；这是业务数据及顺序分组审计，不是调度器或回退路径的插桩证明。

80,000 是提交目标，40,000 是历史比较目标；本轮固定窗口最终确认 TPS 未达到 40,000。上述数字不是持续性能、多机性能或七天真实运行资格证明，也不能据此对历史不同代码版本作因果性能比较。

## 历史与链上留证

压测完成后才启动独立非出块 SHIP 节点及 PureHistory。历史逐个核对了 **1,040,000** 个交易 ID 和规范区块位置，缺失 **0**、重复 **0**；区块头从 2 连续覆盖至 2796。公开 HTTPS 交易详情及 CORS 检查通过。

摘要文件：`execution/pressure/REPORT_PRECOMMIT.json`，SHA256：`60aa4b710de58d8fa68bb12a4ff6931d879a54e60ddc5aa9301f403820fcf8d8`。链上摘要交易：`fb2c095e67cbfbc2f5d0fccfabf6f9d69239756fe0a16c4137035eecda7c64f9`，高度 **2812**，规范块 ID：`00000afc3e35f63639060e89f575cd2865c775982c69461865d3bef1326428e0`。

浏览器：<https://aic.matrixai.one/>；节点 API：<https://aic-api.matrixai.one/>；历史 API：<https://purehistory-api.matrixai.one/>。静态 HTML、默认 API 域名及页面脚本返回 200；本环境未提供可用视觉浏览器，未进行渲染后的视觉验收。

## 源码和资格

- 节点源提交：`e62a7ea30632ad5069a9640fc6bd0e1fe819b159`；ELF SHA256：`05d62c4a92497153e21833be7d2796d1d1fc9eeb09ebeb4dc1034f41c72ecf67`。
- 合约源提交：`b230c7e12d7cd0bc05ad8269140dc030f8b5fe79`，父提交：`15dea1aa8a4b158eb771c5b13947ddeaafd28910`。
- WASM SHA256：`b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170`；ABI JSON SHA256：`9dab82410be6e25b184c2b7e4d834709d9bb556d3e114ea9504080f6b059f358`。
- 已部署 packed ABI SHA256：`f19e4c55e11f5029f4a161865ac0b0ea29f730d4f19fbdc6eb30c29c10ef5512`；所有序列化字段与构建文件一致，CDT 文件注释 `____comment` 不进入序列化 ABI。
- 实际 WASM：10 项资金功能＋12 项既有模式回归，共 22 项；ABI 原有项目全部保留。`PUREEOS_BP_BOND=OFF` 字节一致复现发布 WASM `373b66b2994c3131ce6c9c3d4526fdf836c0ac2c36d8265b2239d61e75ca2ceb`。
- 原生控制器推进七天验证边界，不等于真实在线运行七天。完整候选源码归档已进行本地 SHA256 和 Git blob 内容哈希校验，代码见草稿 PR <https://github.com/MatrixCoreX/PureEOS/pull/2>。

## 保留的失败与恢复

所有原生构建、测试辅助程序修复和拒绝的托管原型均保留在远程资格目录。本轮部署还保留以下准备／恢复失败：

1. 归档预检误用了 ABI 中不存在的 `rotstate` 表，已按实际 ABI 改为 `rotation`；失败发生在停止任何旧链服务之前。
2. 钱包命令行空参数的等号写法被 Boost 拒绝；改为独立空参数，未启动计时压测。
3. 历史目录 0750 被 UMask0077 收窄为 0700，同时旧服务别名的 Requires/Wants 仍指向旧节点。停止旧依赖，修复规范 unit 及有效 alias drop-in，明确目录权限，保留该次新后台数据后重新回填；在索引前核对新历史源链 ID 和规范不可逆块。此故障发生在已经完成的有效单 BP 测量之后。

4. `brazilamazon` 的链上密钥与押金正确，但注册前建立的普通引导连接未在 peer-key 缓存生效后进入签名发现状态。本人签名广播就绪后，重连同一个引导地址，成功学到其他 BP 的签名广播。编排增加有条件、一次的引导重连，仍要求真实开放的签名连接通过；没有添加全量静态 BP 列表或修改节点／合约二进制。

5. `japanblossom` 已有不可逆注册及足额押金，但核对脚本在一次未定位到账户的取表响应中退出。失败原始响应未保存，不能确定当时是限时截断还是短暂视图差异。后续主键查询确认足额余额；改为按账户定界查询、有界重试及完整表分页，保留失败检查点并复用原注册，不再次扣款。

没有因上述故障重建已经成功的新链或追加 TPS 测量。旧链数据库保留原目录，历史索引和配置另行归档；旧链黑名单及违规证据留在旧链，未捏造新链违规。

远程完整证据：`/opt/pureeos-bp-bond-20261102-r1` 和 `/opt/pureeos-bp-bond-newchain-20261102-r1`。本地只接收公共源码、报告及摘要证据并逐字节核验；钱包、部署私钥、数据库密码、含私钥配置和大型签名交易集未下载或提交 Git。

## English summary

Status: **PASS**. The single-founder pure-AIC round sent and finalized 1,040,000 transfers with zero unresolved transactions. Fixed-window inclusion/finality: **34614.75 / 34507.33 TPS**; whole-corpus inclusion/finality: **37645.13 / 36450.89 TPS**. Offered load was 80,000 TPS using eight connections, 81,920 account pairs, 1-second warmup, 12-second window and 90-second drain. Producer SHIP, all other nodes and history backends were off during timing; they were restored afterward. Full canonical receipts, integer balances, supply and caps passed. All 1,040,000 accepted transaction IDs and block positions were independently checked in the history index with zero missing or duplicate IDs.

Hardware: retained AMD EPYC 4345P bare metal, eight physical cores / sixteen threads, advertised 64 GiB and 64,880,889,856 visible bytes. CPU16 / NET4 MiB / state16 GiB, parallel execution, RAM1024 and disabled deferred stages were retained. The 40-BP deployment uses this same physical host; it is a functional demonstration, not multi-machine or sustained throughput qualification. Seven-day native clock tests do not establish seven days of wall-clock stability. No extra server or pressure round was created. Ordinary BPs bond exactly 10,000 AIC; safe-exit cooldown is 604800 seconds; forfeitures are locked in eosio.bslash under the current code and upgrade-governance rules.
