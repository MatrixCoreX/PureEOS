# eosio.key 中继执行费升级与实际验证

2026-11-05 当前 TEST 链已完成合约升级、应用适配、公开页面发布和验证。商业中继交易扣除实际 RAM 支出后，执行费与“中继手续费”均计入实际提交 BP 的可领取收入。新旧总费用公式相同；历史网络储备不分配。8 笔商业中继交易累计 0.2261 AIC 已一次完整领取，应用领取手续费为零。

## 精确版本与治理

- Chain ID：`09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532`。
- 新 KEY WASM：`1092b97ede0d06ab7cc7b54ac0fe5c6a47227e8356e3b7a44877d6a47bd6273d`；原代码 `7523bbec364316d06f48283e9cc17cf81fa929bd60b4efd818eadd73e215c788`。
- 原始 ABI 保持 `b6db52cd47fdf811bee83030e524de35d0b6f55aece386392b56d1a085df2f83`；JSON ABI 保持 `f4d64cd7bd84796fab77886fc8308def72b19076a76c2e2bae58857f7bf3b0c5`。表和签名报文布局不变。
- 精确审核材料先发布 Git `522485d6734fbb5f9377dc0156c7d2ff9070f7d4`，审核 SHA-256 `a9560228f74f42684b4b0df73908e8fc3d318330268f79a5604f14bbdab0c414`；原审核包和 MANIFEST 字节未改。
- 原生提案 #1：`2c2d8a7674fb7e961559d0beffbbd28a5eed7b7c0d1fe38074e0f058ccd09ffa`，不可逆区块 31732；公示从 09:43:25.500 UTC 至 10:13:25.500 UTC。24 位有效 BP，16 票否决阈值，观察到 0 票否决。
- 匹配执行：`b5a4334925216a1ceb068fa4a11e9fb694e4dc45d203ee53a7e2d4d4b26e05bb`，不可逆区块 35304；实际链上经过 1869.5 秒。生产网络 7 天公示规则未改。
- 前端功能源码 `097c1ad18fab190bf91e07ea2bf3397450d2952a`，静态发布 `/var/www/aic-test/releases/20261105T105459Z-relay-execution-income-r1`。之前静态版本保留。

## 代码复盘与费用归属

`credit_fee_bucket` 增加内部目的地参数：商业中继的新执行费与百分比费用写入 `relay_earned`，不再把新执行费计入 `network`。`settleram` 在扣除本次原生购 RAM 总额后分账；`charge_key` 覆盖无需新 RAM 的商业中继路径，包括撤销授权。准备金等式和费用上限检查保留。

`total = actual_RAM + execution + relay_fee`；`relay_fee = ceil(base × rate)`，其中 `base = actual_RAM + execution`，最低费不足时包含补足。商业中继可领取 `total − actual_RAM`，不是把 RAM 预算也算作收入。费率仍为 10%–30%，本轮唯一启用的创始中继为 30%。执行费来自公开资源计价，不是逐笔实耗 CPU/NET 报销，中继收入也不等于净利润。

已有 `feereceipt.retained_base` 字段继续表示 RAM 之外的执行费；归属要结合对应 `receipt.relayer` 和代码版本，不能用新规则重写旧交易。历史网络储备 0.2352 AIC（KEY 0.0672、创始 BP 名下 0.1680）保持不可领取。本轮无商业中继充值另外形成 0.0684 AIC 网络储备，领取前后网络储备均为 0.3036 AIC。

## 三种转账的完整流程

### 命名账号 → 公钥

1. 钱包构造同一个原生交易：`eosio.key::deposit` 声明本金、公钥、币种、签名费用上限与请求标识；`eosio.token::transfer` 支付本次 AIC 费用资金 F，附言 `atomic key fee`；最后转入完整代币本金。三个根动作由命名发送人一次签名。
2. 合约核对发送人、动作顺序、实际到账和令牌摘要；需要新记录时，自动从合约发起真实 `eosio::buyram`，payer/receiver 都是 `eosio.key`。实际 AIC 购置总额从这次发送人的费用里承担。
3. 同一交易内创建钱包/余额记录，给收款公钥计入完整本金，并退回 `F−total`。无跨交易手续费预存，也不需要人工补 RAM。发送人须暂时持有 F（SDK 默认 F=max_fee，可明确指定），最终仅扣实际费用。
4. 命名发送人承担原生 CPU/NET；此路径没有商业中继，不收中继手续费，执行费仍归网络储备。接收方无需已有 AIC；发送 USD/CNY 的命名发送人需另外有 AIC 支付费用。

### 公钥 → 公钥

1. 发送方钱包本地签名，绑定完整本金、币种、收款公钥、有效期、授权代次、请求标识、指定中继及费率与 max_fee。中继审核后用自己的命名账号和原生资源提交 `ksend`。
2. 合约验证用户证明和中继资格；需要新钱包/币种余额记录时，在同一交易内实际购 RAM。保守预算是新钱包+余额 1536 字节、新币种余额 512 字节、已有记录 0；实际购入字节可能因整数报价和余量略多。
3. 发送方公钥账本支付完整本金及 AIC 总费用，接收方增加完整本金。USD/CNY 本金不会被折成 AIC 费用；发送方必须已有足够 AIC，接收方无需先有 AIC。
4. 实际 RAM 花费用于原生资源市场，剩余执行费和中继手续费记实际 BP 的 `relay_earned`，随后由 BP 自行免费 claim。已有记录不再次购买；并发后执行者按实际已有状态结算。费用超签名上限、签名/到账/资源不足时整笔回滚。

### 公钥 → 命名账号

1. 公钥持有人签名 `withdraw` 请求，绑定目标命名账号、币种、本金、中继和费用上限；商业中继承担提交交易的原生 CPU/NET。
2. 合约核对授权。目标尚无该代币余额行时，按 512 字节保守预算补充自身 RAM 并由受支持代币合约打开目标余额行；已有余额行时该项为 0。
3. 目标命名账号实际收到完整代币本金，公钥发送方另付 AIC 费用；本次原生 RAM 支出先扣除，执行费和中继手续费记提交 BP 的收入。转其他币也要求发送公钥有 AIC。
4. 此处验证的是商业中继路线。持有人选择 `selfout` 等无商业中继路线时，原执行费归网络储备的规则保持。

## 12 笔实际不可逆账单

费用列单位均为 AIC；最后两项为保守分配字节/实际原生购入字节。SDK 观察到资源倍率 21335/10000；这里是实际账单，不是固定报价。费用随市场、记录状态、备注长度等变化。

| 场景 | 完整本金 | 总费 | RAM | 执行费 | 中继手续费 | 中继收入 | 字节预算/实购 | 原生交易 |
|---|---|---:|---:|---:|---:|---:|---:|---|
| named-to-key first USD | 100.0000 USD | 0.0208 | 0.0037 | 0.0171 | 0.0000 | 0.0000 | 1536/1597 | [432ce4c2e222…](https://aic-test.matrixai.one/#/transaction/432ce4c2e222d6e3045ed79710e5d0b2f5a1bd47a7dac6d1622782e9c7963622?block=35861) |
| named-to-key first AIC | 20.0000 AIC | 0.0185 | 0.0014 | 0.0171 | 0.0000 | 0.0000 | 512/576 | [81c2dae037c9…](https://aic-test.matrixai.one/#/transaction/81c2dae037c9874934b95c7094cc5fb654c4ca94839f6cf32dbe4f677933fe7c?block=35880) |
| named-to-key first CNY | 100.0000 CNY | 0.0185 | 0.0014 | 0.0171 | 0.0000 | 0.0000 | 512/576 | [a4155df20e49…](https://aic-test.matrixai.one/#/transaction/a4155df20e49db9fef6172d5e30fd58b8274c6a880c7a55a2feb4313094b7055?block=35899) |
| key-to-key AIC new | 3.0000 AIC | 0.0299 | 0.0037 | 0.0193 | 0.0069 | 0.0262 | 1536/1597 | [4417881f2a85…](https://aic-test.matrixai.one/#/transaction/4417881f2a85ffd17bb27b5f9fbc2c82840ad9a3b3390956336d10b2d51533cb?block=35922) |
| key-to-key USD new | 2.0000 USD | 0.0270 | 0.0014 | 0.0193 | 0.0063 | 0.0256 | 512/576 | [e6de44b8fb20…](https://aic-test.matrixai.one/#/transaction/e6de44b8fb2080a641eddc891b79d9d36e474170ca0aa34ebfa49447488a26c2?block=35959) |
| key-to-key CNY new | 2.0000 CNY | 0.0270 | 0.0014 | 0.0193 | 0.0063 | 0.0256 | 512/576 | [7d30df90a87f…](https://aic-test.matrixai.one/#/transaction/7d30df90a87fb88a04f3ba465d9ca1b43298cf75ed1ef82eaf0240b54b2614a5?block=35993) |
| key-to-key AIC existing | 1.0000 AIC | 0.0251 | 0.0000 | 0.0193 | 0.0058 | 0.0251 | 0/0 | [5cc52ecdff06…](https://aic-test.matrixai.one/#/transaction/5cc52ecdff0661246ac6933684f1d8aaf96060b3d2cf116024800612b6ab7486?block=36013) |
| key-to-named AIC unopened | 1.0000 AIC | 0.0324 | 0.0014 | 0.0235 | 0.0075 | 0.0310 | 512/576 | [90dfe0b7af8f…](https://aic-test.matrixai.one/#/transaction/90dfe0b7af8f02154f119b487400e9ef98358a6528f411e090c2a2395a1c234e?block=36037) |
| key-to-named USD unopened | 1.0000 USD | 0.0324 | 0.0014 | 0.0235 | 0.0075 | 0.0310 | 512/576 | [0ba4f4cd3b35…](https://aic-test.matrixai.one/#/transaction/0ba4f4cd3b35c7e6fa2554512956c17f2528c347d64e97f1d3cb3e3fe46243ee?block=36068) |
| key-to-named CNY unopened | 1.0000 CNY | 0.0324 | 0.0014 | 0.0235 | 0.0075 | 0.0310 | 512/576 | [5554f57760bb…](https://aic-test.matrixai.one/#/transaction/5554f57760bb5451deadd661e79bd2b68b331c7885fce5c536ee6a639ca140d9?block=36095) |
| key-to-named USD existing | 0.5000 USD | 0.0306 | 0.0000 | 0.0235 | 0.0071 | 0.0306 | 0/0 | [ed790b1c223f…](https://aic-test.matrixai.one/#/transaction/ed790b1c223f71c2234a0f06d10291936684aac0b18367d8fc9e7bafa3383757?block=36124) |
| named-to-USD-only key | 2.0000 USD | 0.0208 | 0.0037 | 0.0171 | 0.0000 | 0.0000 | 1536/1597 | [3ff39cfe3afe…](https://aic-test.matrixai.one/#/transaction/3ff39cfe3afeff40d4e9ecd9fa1e6742d2156500550c7d716d75b28ec6c0f6a0?block=36150) |

12 笔总费用 0.3154 AIC = 实际 RAM 0.0209 + 无中继执行费储备 0.0684 + 商业中继收入 0.2261。每笔原生 `logbuyram` 的金额、payer、receiver、购入字节与实际配额增长核对一致；已有行两笔未购买 RAM。

领取交易 `1eee7ffeb5e7414841fbb732c0e910d01fd6043714fc28981093164bbe9a2881`，不可逆区块 37478。创始 BP 原生余额由 89734.0860 增至 89734.3121 AIC，精确增加 0.2261；`relay_earned` 2261→0，应用领取手续费 0.0000 AIC。原生 CPU/NET 由领取账号承担，未重复领取。

## 验证、失败留存与范围

远端实际 native 290 项、SDK/中继 26 项、前端 268 项/TypeScript/构建通过。公开链 12 笔/79 项验证包括三类方向、三种币、新旧记录、真实 RAM 购买、完整本金、无 AIC 的 USD 发送方拒绝、零费用上限拒绝、旧独立预存模式拒绝、HTTP 同请求同原生 ID，以及完整免费领取。保留初始失败和原始记录，没有重发已成功付款。

native 第一次解析器错误假定 trace 总是 JSON；第二次试用的 ABI 解码 RPC 在当前原生节点不存在，第三次使用已审核离线 ABI 解码后通过，同一 WASM 未因这些辅助问题改变。公开校验曾忽略 `get_table_rows.more/next_key` 导致储备统计截断；已使用完整分页和原始不可逆领取回执只读复核，未重领。浏览器第一次环境缺库，修正既有库路径；随后一个选择器误选折叠明细的隐藏链接，修正为可见链接，保留失败。

四种语言/主题/宽度组合的预览和正常公开 HTTPS Chrome 均通过（zh-dark1440/en-light1440/zh-light390/en-dark390），各核对 16 个公钥视图、12 笔手续费历史、实时最近交易、公式与升级提案/实际执行链接；6 个发布资源经实际公开浏览器逐字节摘要一致。原有 4 个演示公钥的余额及 12 笔历史手续费保持不变。

保留 25 个原节点 PID（24 BP+1 历史）、实际 21/15、24 自投票、90 普通随机投票且覆盖全部 BP/权重各异、全部保证金、AIC 已发行 1 亿/上限 21 亿。唯一创始 HTTP 中继继续启用，其他四个费率条目仍是禁用/空地址。普通低频业务和索引未由本次控制器暂停。最终观察期间，既有普通业务发送器保留了原生 `tx_cpu_usage_exceeded` 拒绝/自动重启记录；浏览器任务结束后观察到其自行处理原批次并推进到至少 353 批。只读观察器未重发付款或修改 CPU 上限；这是共享主机上本轮观察到的运行限制，不能描述成零拒绝或容量合格。KEY/signup 的 owner 封闭；active 实际是封闭的 eosio.code 或自身代码，不能误称只有自身代码。dev/BP 无新增特权，USD/CNY 权限仍委托 dev，分别发行 10000、当前上限 1000000。发行链路是 `usd/cny@active → eosio.dev@active → civicgovlead@active`；仅有提案用 `eosio.dev@upgrade` 的授权不能替代发行权限。代币审核白名单/哈希检查不变；本轮不能据此宣称恶意代币 RAM 风险全部排除。

构建、测试和 Chrome 均仅在保留的 shared Intel Cascadelake 4vCPU/8GB 远端进行。无 TPS、容量/生产安全/并行合格声明，无链重置、BP 停机、新服务器、投票/保证金/总量上限/原生二进制更改。钱包/APK 动态币种发现已按用户指示留给钱包任务，未动其未提交工作。

## 项目目录清理与恢复

51 个历史公开发布包路径（27 种内容）经 SHA-256 校验后移至本机 Windows 分区 `/run/media/guagua/Windows/PureEOS-project-cleanup-20261105/public-artifacts`，原路径保留符号链接；该步净释放 2285469696 字节。此分区与 Linux 同一 NVMe，不是外置备份。

仅删除确认无人使用、Git 不跟踪、归档后逐文件再次校验一致的生成目录 `cdt/build-pureeos`，共 23185 个文件/链接，分配空间 2037272576 字节。恢复归档 `cdt-build-pureeos-20261105.tar.gz` 大小 727965939 字节，SHA-256 `2b2851c9c371d2732e820702bf22422b14b968e80f973e04482be3dad257f392`。两步测量净增加约 4.03 GiB；清理完成时 Linux 分区可用 23.58 GiB（后台其他工作会继续影响数值）。源码、Git、当前工作、签名材料及链/历史数据均保留。清理清单提供精确路径、哈希和恢复位置。
