# PureEOS 系统合约

本仓库维护 PureEOS 新链使用的系统、代币、资源及奖励合约，配套工具链为
AIC CDT 1.0.0。节点和系统合约必须使用同一份经过验证的发行组合。
当前首版仍在验收，不能将开发分支或历史二进制视作已经完成联合发布。

## 本版规则

- 每个投票者只选择一个节点，按代币最小单位整数计票；代理投票关闭，买入REX必须保持唯一投票选择。
- 合格候选初始上限210，按四年翻倍的速度线性增长；从一个节点起链，逐步加入。
- 最多21个节点同时出块，按候选顺序逐次替换一人；轮岗资格和实际出块窗口顺序分别由合约及原生共识规则确定。
- 不由现任21人预检其他节点。满21时连续三个完整本人窗口零产出，永久禁止该账号再次轮岗；不足21时不拉黑。
- 节点仅按实际出块获取奖励，没有按排名或票数分发的备用节点奖励。保留历史收益和账户资产；领奖仍受正常权限和24小时规则约束。
- 节点增发初始约1%年率，四年减半；原储备账户80%增发取消。
- 直接质押及REX租赁合计动态占约10%资源权重，其余由PowerUp提供；保留REX租赁。
- 系统升级由独立开发者账号申报，固定公示14天，合格节点可按原生规则反对；不通过eosio.msig或eosio.wrap授予升级权。

实际执行边界以本仓库代码、匹配的节点原生治理实现和发行验证记录为准。
满席离线安全移除的说明见[SAFE_BLACKLIST_EVICTION.md](SAFE_BLACKLIST_EVICTION.md)。

## 构建

使用新建的独立构建目录及已经核验的AIC CDT 1.0.0 SDK，环境需要CMake、Ninja
和原生C/C++编译器。下列路径应替换成自己的实际SDK和输出目录：

```bash
cmake -S . -B /your/output/pureeos-contracts -G Ninja \
  -DCDT_ROOT=/your/aic-cdt-1.0.0 \
  -Dcdt_DIR=/your/aic-cdt-1.0.0/lib/cmake/cdt \
  -DBUILD_TESTS=OFF
cmake --build /your/output/pureeos-contracts --parallel 2
```

四项新链功能默认开启且必须开启：`PUREEOS_UPGRADE_GOVERNANCE`、
`PUREEOS_SINGLE_CHOICE_VOTING`、`PUREEOS_RESOURCE_SPLIT`、
`PUREEOS_SEQUENTIAL_ROTATION`。旧构建缓存若关闭其中任何一项会明确报错，
不会生成一份仍采用旧投票、奖励或资源规则的系统合约。

也可以直接构建`contracts/`，需要显式选择SDK中的WASM工具链；默认功能要求相同。
AIC CDT源码中的`tests/hardening/build_system_contracts.py`提供隔离重建和逐文件摘要记录。
完整构建还会生成若干继承的工具/示例合约；首版系统部署只使用发行清单指定的合约。
生成一个msig或wrap文件，不代表它获得系统治理或升级权限。

## 部署

此分支用于原生Savanna新创世链。必须先激活匹配的`PUREEOS_ROTATION_CONTEXT`，
部署发行清单中的system/token/bpay及REX、PowerUp辅助合约，完成资源初始化后再
调用`govinit`封存开发者权限与固定14天公示规则。不能使用默认测试私钥或测试资金
作为正式创世配置。准确操作顺序参照节点仓库的候选使用说明和同版本测试脚本。

系统升级、注册矿工、出块签名及账号管理是不同权限；不要把开发者升级私钥装入
普通出块服务器。普通应用可以保留自己的业务权限结构，不因此取得系统升级权。

## 许可

继承代码及第三方声明按[LICENSE](LICENSE)保留；有权授权的自有原创部分适用
[AIC开源许可1.0](AIC_LICENSE.md)。具体范围见[LICENSING.md](LICENSING.md)。
