# eosio.signup 注册服务候选

**状态：开发中；远端CDT编译、1760项独立解析/曲线检查及WASM模块静态检查
通过，真实链上注册尚未验证，未创建或部署。**

该服务参考原EOS转账附言注册方式：已有命名账号向 `eosio.signup` 转入
`eosio.token` 的4位精度AIC，附言指定新账号和公钥。全部步骤在同一交易内
完成；任何一步失败，转账和创建一并回滚。它不保管用户私钥，不需要
`privileged`，没有管理员提款或修改注册政策的动作。

## 密钥与附言

支持旧 `EOS...`、`PUB_K1_...` 和 `PUB_R1_...` 公钥。校验Base58规范编码、
完整长度范围、类型绑定RIPEMD160校验和、压缩点格式和K1/P-256曲线点。
这不包含另行规划的抗量子原生密钥类型。只提交公钥，不能提交私钥。

附言格式（占位文字应替换为真实公钥）：

```text
abcdefghijkl-PUB_K1_<public-key>
abcdefghijkl-PUB_K1_<owner-public-key>-PUB_R1_<active-public-key>
abcdefghijkl EOS<legacy-public-key>
```

新账号名必须正好12位，只含小写a-z或1-5，无点且尚未注册。单公钥格式将
owner/active设为同一公钥；双公钥格式分别指定。兼容单空格旧分隔符，
可去除附言首尾ASCII空白，其他多余分隔符或无效密钥导致整笔回滚。
K1与R1可分别用于owner和active。钱包是否能生成/导入/签署这些类型需
单独验证，合约接受R1不代表所有外部钱包均支持。

## 初始资源政策

保持参考服务的分配方式，尚未激活任何经济政策：

- 0.1000AIC用于NET，0.1000AIC用于CPU；`delegatebw(...,true)`将质押所有权
  转给新账号；这部分是资源质押，不是服务费。
- 转账余款全部购买新账号RAM；系统0.5%向上取整购买费包含在余款中。
- 新账号实际RAM购买量至少4096字节。买前用当前曲线做整数预检，实际
  `logbuyram` 通知再次核对字节数及费用；不足则全部回滚。
- 无注册服务加价、没有余款流动余额返还。使用当前市场价格，不能仅凭
  大于0.2000AIC就保证注册成功；账号原生开销与资源表开销也必须容纳。

本方案尚需真实系统合约验证CPU/NET质押在当前资源分离模式下的实际效果；
不能把质押额直接宣称为可用交易次数或持续PowerUp资源。每个市场报价
均可能随状态变化，4096字节门槛也不代表所有后续合约业务都够用。

若以后选择“固定RAM，余款转入新账号流动余额”，需要另行定义发送方付
新token余额行RAM的预算。当前代码没有默默引入这个服务补贴。

## 代码与权限约束

候选固定eosio系统和eosio.token的已审核代码摘要；代码发生变化即拒绝
注册，必须经审核更新候选。这防止在未审阅的资源或转账语义下继续运行。
这些摘要不证明编译结果已获批准，也不替代源代码/ABI/配置核验。

拒绝其他代币/伪装转账来源；收到转账要求真实发送者授权；自身转出通知
直接返回。创建普通账号、购买RAM、转移质押均由本合约的 `eosio.code`
权限完成。`eosio.signup` 自身的创建和部署依赖另行开发的新原生提案能力。
现有 `devexec` 不能创建缺失账号，不能用临时privileged或恢复创世密钥绕过。

## 参考来源与差异

原Dappub/signupeoseos仓库查询为404，不能宣称取得该原仓库当前内容。
审阅的保留公开实现：

- [tokenbankteam/signupeoseos](https://github.com/tokenbankteam/signupeoseos/tree/27a30465d8176668a8d0824b0dd8ab662e2e10b7)：旧EOS公钥、同owner/active、CPU/NET各0.1、其余RAM。
- [eosrio/eosriosignup](https://github.com/eosrio/eosriosignup/tree/7400cc499fd005b5b5cb32672fc9465d9dce2473)：另行对比的双公钥/固定RAM方案。

本候选增加K1/R1类型、校验和/曲线点检查、代码摘要约束和实际RAM结果检查；
未沿用参考实现中不完整的校验或近似RAM费用处理。

## 构建与验证边界

仅在保留远端主机的隔离目录构建，指定已核验的CDT：

```sh
CDT_ROOT=/path/to/verified/cdt bash scripts/build.sh /isolated/output
```

`tests/codec_tests.cpp`将解析器与独立OpenSSL的曲线/Base58/RIPEMD实现比较。
测试密钥仅在远端测试进程内临时生成，不输出、不落盘。该程序即使通过，
也只证明独立解析/算术检查，不证明合约dispatcher、通知授权、RAM/质押
或原生治理执行通过。全部测试必须远端运行。

本次候选结果见 [remote-compile-r4.json](qualification/remote-compile-r4.json)。
通知合约没有直接调用动作或业务表，因此提供显式通知结构ABI，`actions`
和`tables`保持空数组。WASM已检查存在apply及预期导入；尚未执行其apply。

后续原生隔离验证见 [remote-native-r7.json](qualification/remote-native-r7.json)：
节点O1 TEST候选已编译；治理31/32、真实合约2/3、无签名原生线格式1/1通过。
创建成功路径在入队后续动作时失败，因为新账号尚不存在。真实注册流程未到达，
不能宣称K1/R1注册、实际RAM/质押和签名通过。原始ABI171字节，已由原生
序列化与无签名工具交叉比较；完整候选仍未验证通过，也未部署。

必须完成实际K1/R1/旧EOS注册、分离owner/active签名、资金与质押归属、
重复账号、非法附言/校验和/曲线、错误token来源、资源不足、通知注入、
部署RAM不足和原子回滚验证，随后才能提交精确代码/原始ABI/审核文档摘要。
当前链已有提案3及24/24否决应继续保留；原生新能力未核验激活之前，
不得提交必然失败的创建交易或直接部署。
