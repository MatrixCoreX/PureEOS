# 原生 Savanna 单节点创世（v1）

> 本页描述兼容保留的 profile 1。取消 BP 管理权限、开发者公示升级和节点 2/3 否决使用新链 profile 2，见 [独立说明](developer-upgrades.md)，不能只按本页 version 1 初始化后期待自动生效。

本入口用于**新链**：从创世锚点起直接使用 Savanna，而不是先运行旧 DPoS 再执行 `switchtosvnn`。v1 固定由 `eosio` 出块，使用 `initial_key` 签名；同一个 `eosio` 作为唯一 finalizer，BLS 权重及确认门槛都是 1。后继区块仍执行正常签名和 QC 校验。

## 创世配置

在标准 genesis JSON 中增加以下字段；示例里的占位符必须换成有效公钥和持有证明：

```json
{
  "initial_savanna": {
    "version": 1,
    "finalizer_key": "PUB_BLS_...",
    "proof_of_possession": "SIG_BLS_..."
  }
}
```

保留原来的 `initial_timestamp`、`initial_key`、`initial_configuration`。首次创建新链时统一确定时间和密钥，所有节点使用相同文件；不得修改已有链的创世文件来切换模式。

`initial_savanna.protocol_features` 可以省略，此时填入 v1 的规范清单：`PREACTIVATE_FEATURE`、`SAVANNA` 及其递归依赖，按依赖在前排列。完整清单、公钥、PoP、版本都参与 chain ID 计算。显式提供不同清单、错误版本、无效 PoP，或者本地禁用了必需协议特性，均拒绝启动，不降级为旧模式。

BLS 公私钥和持有证明可用本项目工具生成：

```bash
umask 077
build/programs/helios-util/helios-util bls create key --file /安全目录/finalizer.keys
```

此文件包含私钥。只将 `Public key` 和 `Proof of Possession` 写入创世 JSON，不要发布私钥，不要使用测试密钥上线。

## 单节点启动

使用新的空数据目录。节点配置需要两份本地签名能力：

```ini
plugin = eosio::producer_plugin
plugin = eosio::chain_api_plugin
producer-name = eosio
enable-stale-production = true
signature-provider = <initial_key对应的出块公钥>=KEY:<出块私钥>
signature-provider = <finalizer_key对应的BLS公钥>=KEY:<BLS私钥>
```

```bash
build/programs/nodeos/nodeos \
  --data-dir /测试或正式环境独立目录/data \
  --config-dir /测试或正式环境独立目录/config \
  --genesis-json /绝对路径/genesis.json
```

按项目既有编号，创世锚点为区块 1，第一个实际生产的后继块为区块 2。这里的“原生”指锚点和后继区块一开始就是 Savanna 状态，不代表必须把区块编号改为 0。

必须同时观察 head 和 LIB（最后不可逆块）持续增长。仅有出块私钥、缺少 BLS 私钥时，不能形成新的最终确认。单节点没有容错冗余，该唯一节点停机或丢失私钥会使链停顿；这不是可由轮换算法消除的风险。

## 存储及旧链兼容

- 新增配置不存在时，继续走旧创世路径；旧 chain ID 仍按原三个字段编码计算。
- 原生创世完整日志使用版本 4；版本 1～3 仍使用原有创世编码读取和写入。
- 裁剪日志保留完整创世头；压缩后若已删除区块 1，则只保存 chain ID，需状态数据库或快照恢复，不能仅凭该日志从创世重放。
- 原生状态使用已有 Savanna 快照结构；旧版本 2 快照中的创世字段按旧格式读取。
- 旧版 nodeos 不支持新的原生创世入口及完整日志版本。不要用旧二进制打开原生链的数据目录。
- 重启或重放时，出块插件先应用磁盘 fork database 中尚未恢复的可逆区块，再允许生产新块；恢复过程可中断并续跑。这避免从 LIB 另起分支而与持久化 finalizer 投票锁冲突，不关闭投票锁或无近期投票时的暂停保护。

## 验证入口

在 `PureEOS` 目录中运行：

```bash
cmake -S . -B build
cmake --build build --target native_savanna_test --parallel 2
ctest --test-dir build -R '^(native_savanna_genesis|native_genesis_blocklog_compat)$' --output-on-failure

cmake --build build --target nodeos cleos helios-util --parallel 2
python3 tests/native_savanna_genesis_test.py --build-dir build
```

该测试目标复用真实 controller 和 tester，覆盖创世身份/PoP、首块、最终确认、缺钥行为、外部验证、日志重放、重启、快照、裁剪和旧日志兼容；不是纯规则模拟器。

CLI 测试另行生成临时测试密钥、独立数据目录和回环地址端口，实际启动 nodeos，验证创世启动、重启及 `--replay-blockchain` 后 LIB 持续增长。各阶段至少新增 16 个不可逆块，覆盖默认 6 秒投票暂停窗口，并检查上一阶段的可逆链头没有被替换。结束后停止自己启动的进程并清理临时测试数据，不操作已有链。

## 本阶段边界

本实现是内核单节点创世入口，不等于整套 210 候选轮换已经上线：

- 还需把实际 proposer/finalizer 状态通过确定性只读接口交给系统合约。
- 系统合约仍用历史 `last_proposed_finalizers` 判断共识模式；原生初始化适配尚未接入，不能直接把旧 `switchtosvnn` 启动流程用于原生链。
- 系统合约旧的投票激活门槛、真实前 210 名筛选、轮换持久化、证据/冷却、按出块奖励、独立升级权限是后续阶段。
- 本入口没有授权 BP 治理，也没有更改费用、REX 或奖励资金路径；现有权限不会因为原生创世而自动完成治理改造。

目前应在隔离测试环境使用，不作为完整生产上线批准。
