# PureEOS 节点启动、BP 注册与维护流程

[English](BP_STARTUP_REGISTRATION.en.md)

适用于当前 PureEOS 系统合约：单选直接投票、原生 proposer/finalizer 轮岗、签名 BP gossip 和计划维护。系统账户仍为 `eosio` 等现有名称。本流程用于加入已有链；首个 BP 创建创世链另按链初始化流程操作。

推荐顺序：**准备账户和密钥 → 启动节点 → 完成同步与就绪检查 → 注册 BP 和配套公钥 → 正权重直接投票 → 等待链上轮岗。**

注册动作不会替操作者检查服务器是否在线、同步是否完成、私钥是否可用或端点是否可达。新 BP 应先准备好节点，再让它具备参选资格。

## 1. 区分注册、候选、在岗与维护

| 状态 | 含义 | 操作要求 |
| --- | --- | --- |
| 普通全节点 | 同步和验证区块，尚未作为 BP 参选 | 可以先完成同步和配置 |
| 已激活注册 BP | `producersv2.is_active` 为真 | 不代表有有效票权，也不代表已进入原生排班 |
| 可轮岗候选 | 注册和出块/确认密钥有效，有正票权，满足排名规则，且未永久封禁、未申请维护 | 保持在线，可能被正常轮岗选中；注册或得票不保证立即入岗 |
| 当前在岗 | 原生生效出块或最终确认政策仍分配了职责 | 持续出块/签署最终确认，不能因自己认为已经退出而停机 |
| 等待维护 | `bpmaint.state=1` | 已申请，仍须在线完成交接；尚不能停机 |
| 维护已生效 | `bpmaint.state=2`，检查点不可逆，且已脱离全部相关政策/轮换职责 | 此后才可以按计划停机 |

普通候补没有被分配个人出块窗口时，不因等待而累计缺岗。已经承担的职责，不会仅因票数变化、注销或刚提交维护申请就立即消失。

## 2. 准备运行环境、账户与密钥

- 使用经过校验的 PureEOS 运行包和目标链的配置、创世文件或可信快照；源码构建及系统支持见[主 README](../../README.md)。
- 最低为 8 个物理核心、16 个逻辑线程、64 GB 系统可见物理内存。所有节点必须并行运行，启动会检查并拒绝不符合要求的配置。
- 当前开发配置为 CPU16（3,200,000 μs/块）、NET4MiB（4,194,304 字节/块），本地状态库容量为 16,384 MiB。加入已有链时遵守其实际共识参数，本地配置不能改变链限额。
- 准备自己控制的 **12 位原生账户**、RAM 和足够的交易 CPU/NET。按当前规则由账户自己授权、自己质押和自己 PowerUp。正票权取决于实际链上投票权重，零权重选择不会制造有效票权。
- 保持系统时间同步，确认磁盘空间、P2P 入站/出站、域名和签名服务正常。出块节点不启用 SHIP；历史服务由独立非出块节点提供。

示例使用 `bpoperatoraa`，恰好 12 位。它是示例账户，实际操作必须改为自己的账户。

| 密钥角色 | 用途 | 节点与链上如何匹配 |
| --- | --- | --- |
| 账户权限密钥 | 授权注册、投票、维护等交易 | 由账户当前权限控制；与节点签名角色区分管理 |
| 出块密钥 | 签署区块 | 本地 provider 与 `regproducer` 的公钥一致 |
| BLS 最终确认密钥 | 签署最终确认投票 | 本地 BLS 私钥与链上已激活 finalizer 公钥一致 |
| peer 身份密钥 | 签名 BP gossip 身份和端点记录 | 本地 provider 与 `regpeerkey` 登记的当前公钥一致 |

如需生成新密钥，可使用运行包内工具。以下操作只生成本地文件，不注册 BP：

```bash
umask 077
mkdir -p /srv/pureeos/keys
./bin/cleos create key --file /srv/pureeos/keys/block-key.txt
./bin/cleos create key --file /srv/pureeos/keys/peer-key.txt
./bin/helios-util bls create key --file /srv/pureeos/keys/finalizer-key.txt
```

BLS 输出包含公钥、私钥和 Proof of Possession；注册只提交公钥和证明。密钥目录和配置应仅对节点服务用户及必要的管理员可读，不上传 Git。示例路径须预先赋予服务用户正确权限，已有密钥不要用生成命令覆盖。

## 3. 配好节点，再启动同步

从 [`config.example.ini`](../../config/config.example.ini) 准备完整配置，填写独立的数据目录、端口和目标链信息。下面是需要补充的角色配置，**不是完整配置文件**；`<...>` 均须替换。

```ini
plugin = eosio::producer_plugin
producer-name = bpoperatoraa

http-server-address = 127.0.0.1:8888
p2p-listen-endpoint = 0.0.0.0:9876
p2p-server-address = <YOUR_PUBLIC_P2P_HOST>:9876
p2p-peer-address = <TRUSTED_ORDINARY_SEED_HOST>:9876

signature-provider = <BLOCK_PUBLIC_KEY>=KEY:<BLOCK_PRIVATE_KEY>
signature-provider = <BLS_PUBLIC_KEY>=KEY:<BLS_PRIVATE_KEY>
signature-provider = <PEER_PUBLIC_KEY>=KEY:<PEER_PRIVATE_KEY>
p2p-bp-gossip-endpoint = bpoperatoraa,<YOUR_PUBLIC_P2P_HOST>:9876,<YOUR_OUTBOUND_PUBLIC_IP>
```

`KEY:` 示例只能放在受保护的本地配置中。当前 BLS 路径使用 `PUB_BLS...=KEY:PVT_BLS...`；不要假设普通密钥的 KEOSD provider 能替代 BLS provider。出块和 peer 密钥也可使用代码已支持、实际可用的签名 provider。BP gossip 的 peer 私钥通过 `signature-provider` 配置，普通 P2P 的 `peer-private-key` 不是其替代品。

静态种子用于初始连接，签名 gossip 还需要注册后的链上 peer key、本地对应私钥和可达端点。仅填写种子列表，不等于已验证 gossip。未注册前先通过普通种子同步；注册后再核对 BP 身份和发现连接是否正常。

以匹配的运行包启动，正式运行交给 systemd 等进程管理器：

```bash
./bin/nodeos \
  --config-dir /srv/pureeos/config \
  --data-dir /srv/pureeos/data \
  --genesis-json /srv/pureeos/genesis.json
```

为加入已有链，不要启用 `enable-stale-production`。准备参选前还应确认未通过 `pause-on-startup` 或生产控制 API 暂停本地出块；本地暂停不等于链上维护。

## 4. 通过就绪检查后，才注册和参选

先比较本地节点与可信参考节点，以下均为只读查询：

```bash
PUREEOS_LOCAL_RPC=http://127.0.0.1:8888
PUREEOS_REFERENCE_RPC=https://YOUR_TRUSTED_RPC
curl -fsS "$PUREEOS_LOCAL_RPC/v1/chain/get_info"
curl -fsS "$PUREEOS_REFERENCE_RPC/v1/chain/get_info"
```

就绪条件：

1. 两端 `chain_id` 一致，本地 head 时间接近当前链头；head 和 LIB 持续前进，差距保持很小。只看到进程存在或 RPC 能响应不够。
2. 用两端 `get_block` 核对同一个已不可逆高度的区块 ID，确认在同一条链上。对刚从其他端同步的区块，短时间高度相差少量不等于分叉。
3. 本地出块、BLS 和 peer 签名配置与准备提交的公钥相符，服务可持续使用；P2P 连接和时间同步正常。
4. 没有硬件、串行模式、验证、磁盘或签名错误，节点没有处于本地暂停出块状态。

区块 ID 查询示例：

```bash
curl -fsS "$PUREEOS_LOCAL_RPC/v1/chain/get_block" \
  -H 'Content-Type: application/json' -d '{"block_num_or_id":"<SAME_IRREVERSIBLE_HEIGHT>"}'
curl -fsS "$PUREEOS_REFERENCE_RPC/v1/chain/get_block" \
  -H 'Content-Type: application/json' -d '{"block_num_or_id":"<SAME_IRREVERSIBLE_HEIGHT>"}'
```

下面交易示例假设操作者已有可用签名钱包/权限，路径及公钥占位符已替换。注册动作提交到已验证可接收交易的节点；不要把一次 RPC 接受当作已经最终确认。

```bash
PUREEOS_TX_RPC=https://YOUR_VERIFIED_TRANSACTION_RPC
PUREEOS_WALLET_URL=unix:///YOUR_PRIVATE_WALLET/keosd.sock
CLEOS=(./bin/cleos --no-auto-keosd --url "$PUREEOS_TX_RPC" --wallet-url "$PUREEOS_WALLET_URL")

"${CLEOS[@]}" push action eosio regproducer \
  '["bpoperatoraa","<BLOCK_PUBLIC_KEY>","https://YOUR_BP_SITE",0]' \
  -p bpoperatoraa@active

"${CLEOS[@]}" push action eosio regfinkey \
  '["bpoperatoraa","<BLS_PUBLIC_KEY>","<BLS_PROOF_OF_POSSESSION>"]' \
  -p bpoperatoraa@active

"${CLEOS[@]}" push action eosio regpeerkey \
  '["bpoperatoraa","<PEER_PUBLIC_KEY>"]' \
  -p bpoperatoraa@active
```

首次 `regfinkey` 会激活首个 BLS 密钥；后续增加密钥不自动切换当前 active key。已有 BP 换钥须单独核对 `actfinkey` 与原生政策交接，不能照搬首次注册假设。

逐笔确认交易已进入规范链，且其区块已不可逆，再核对账户注册、finalizer active key 和 peer key。之后再发起单选直接投票；下面是有正票权的 BP 自投示例：

```bash
"${CLEOS[@]}" push action eosio voteproducer \
  '["bpoperatoraa","",["bpoperatoraa"]]' -p bpoperatoraa@active
```

`proxy` 必须为空，最多一个候选；其他投票人也必须自己授权投票。正票权、注册及密钥就绪后，仍须满足排名快照和轮岗规则。不要预设从得票到入岗有固定准备时间，也不要人为设置委员会名单。

## 5. 注册后持续检查

```bash
"${CLEOS[@]}" get table eosio eosio producersv2 --lower bpoperatoraa --limit 1
"${CLEOS[@]}" get table eosio eosio finalizers --lower bpoperatoraa --limit 1
"${CLEOS[@]}" get table eosio eosio peerkeys --lower bpoperatoraa --limit 1
"${CLEOS[@]}" get table eosio eosio rotcands --lower bpoperatoraa --limit 1
"${CLEOS[@]}" get schedule -j
"${CLEOS[@]}" get finalizer_info
"${CLEOS[@]}" get table eosio eosio rotation
```

表的 `--lower` 查询可能返回下一个账户；必须核对返回行确实属于自己。`producersv2` 看激活注册、精确整数票数和出块公钥，`finalizers` 看 active key，`peerkeys` 看当前 peer 公钥，`rotcands` 看候选及违规/封禁记录。查询多行时按接口游标翻页，不能拿当前页数量充当全链人数。

以原生 `get_producer_schedule` 和 `get_finalizer_info` 判断实际职责，不能只看 `get_producers` 排名。入岗后核对实际区块生产、BLS 投票及持续推进的 LIB。浏览器总览/治理页显示当前原生在岗名单、最新出块者和维护数量；“节点”页还显示注册/有效 BP 等全链统计。列表和链上维护状态不等于进程在线监控。

## 6. 已有 BP 停机维护

操作顺序：**保持在线 → 申请维护 → 等待交接和最终确认 → 检查停机条件 → 停机。**

```bash
"${CLEOS[@]}" push action eosio setmaint '["bpoperatoraa",true]' -p bpoperatoraa@active
"${CLEOS[@]}" get table eosio eosio bpmaint --lower bpoperatoraa --limit 1
"${CLEOS[@]}" get schedule -j
"${CLEOS[@]}" get finalizer_info
"${CLEOS[@]}" get table eosio eosio rotation
"${CLEOS[@]}" get info
```

只有同时满足以下条件，才可以停止本地进程：

- 自己的 `bpmaint.state` 为 **2**；状态 1 仍须在线。
- `get_info.last_irreversible_block_num >= bpmaint.effective_block`，并确认读取的是同一条规范链。
- 自己已经离开原生 active/pending/proposed 出块名单、active/pending 最终确认名单，以及 `rotation.active` / `rotation.target`。

当前在岗允许申请维护，系统先完成正常 proposer/finalizer 交接。没有合格候补时，普通维护不会自动缩小 21 席，申请者必须继续在线等待或取消申请。不要把“维护请求交易成功”“浏览器显示等待维护”或“本地停止出块”当作可以关机。

维护已生效后，不新增生产奖励权益或新分配职责的缺岗处罚；既有 unpaid blocks、有效违规及永久封禁记录仍保留。停机操作使用自己实际的服务名，本文不提供可直接误停其他节点的固定 service 命令。

## 7. 维护后恢复

**先恢复节点和签名服务 → 保持维护状态同步 → 完成第 4 节就绪检查 → 关闭维护 → 等待正常轮岗。**

```bash
"${CLEOS[@]}" push action eosio setmaint '["bpoperatoraa",false]' -p bpoperatoraa@active
"${CLEOS[@]}" get table eosio eosio bpmaint --lower bpoperatoraa --limit 1
```

确认取消交易不可逆、自己的状态为 0，并持续保持在线。关闭维护恢复候选资格，不强制立即回到委员会。若节点仍在同步、密钥错误或进程未启动，不要提前关闭维护。

## 8. 哪些情况会进入永久黑名单

在健康的完整委员会下，BP **连续三个已最终确认的个人出块窗口完全没有出块**，构成永久账户封禁。它不是连续三个普通区块、三个任意时间段，也不是“注册后三次探测不在线”。候补未被分配窗口不因等待受罚；单纯注册不会立刻被拉黑。

全链出块/最终确认观察中断达到252 slot（126秒，含边界）或委员会不足21人时，暂停新增个人缺岗证据；已有有效违规和永久封禁不清除。这是全链异常保护，不应依赖它安排个人停机。已被永久封禁的同一 BP 账户，注销重注册、换钥、改票或申请维护都不能解除封禁；它仍可作为普通全节点同步。

## 9. 交易未确认或状态不符时

保存交易 ID，先通过规范区块、LIB 或可用历史服务核对。RPC 返回 `executed` 或浏览器暂时显示某个高度，都不能替代最终确认。注册/维护交易未确认时，不执行依赖该状态的下一步；检查交易入口连通性、资源、签名与网络，必要时换用已验证的在岗节点交易入口，并先查清原交易结果。

重复开启或重复关闭维护会被拒绝。永久封禁 BP 不能用维护解除处罚；未注册普通账户也不能调用 BP 维护。任何失败交易均按链上的原子回滚语义处理，操作后重新读取实际状态。

配置和节点角色参见 [CPU16 / NET4 / SHIP 分工](CPU16_NET4_SHIP_ROLES_20261031.md)。本说明不修改投票、奖励、处罚、最终确认门槛或其他链规则。
