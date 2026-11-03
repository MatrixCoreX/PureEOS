本轮已在保留的低配 TEST 设备完成新链部署：免费领取中继收入，领取 BP 承担原生 CPU/NET 和缺失收款行的 RAM；公钥转账、充值和提币继续原 PowerUp CPU/NET 市场占用动态费用及适用的商业中继加价。

Chain ID：116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0。测试网原生提案公示期为1800秒，以链上的 execute_after 为准；到期且满足原有权限、否决、哈希等条件后由开发者发送 devexec，执行并非自动。正式版默认604800秒、旧14天重放常量及现有发布资产保持。

25个原健康 BP 名称与90普通账号沿用当前规则，所有BP本人10000 AIC押金、自投，90普通账号随机单选投票，实际总权重两两不同。在线25 BP加1非出块 SHIP 历史节点，共26 nodeos。押金托管250000.0000 AIC、罚没池0。原十个停机身份继续排除。

验收时实际原生最终确认委员会为3人、阈值3票；保留原生正常准入和交接时序。节点进程数不代表所有成员都已进入原生政策。

普通业务每批1–3笔，目标90%转账/10%tick-note，整批不可逆并排空后等待30–60秒及实际1–6个空块，显式过期1800秒。本链使用新密钥和新状态。

| 实际场景 | 本金／领取量 | AIC 应用费 | 不可逆交易 | 区块 |
| --- | --- | --- | --- | --- |
| 文本账号充值 AIC 到公钥 | 50.0000 AIC | 0.2205 AIC | [22c7121372c2](https://aic-test.matrixai.one/#/transaction/22c7121372c2a501f6143fa45f76e9500112044f5c547bd6647eb1d295c9e576) | 1249 |
| 文本账号充值 USD 到公钥 | 50.0000 USD | 0.1155 AIC | [261e02f85f15](https://aic-test.matrixai.one/#/transaction/261e02f85f15ea43b50b57c707060e59631c323cdac6ce07702b2a65bd6bad79) | 1252 |
| 公钥内部转账 USD | 1.2500 USD | 0.2840 AIC | [93100589173f](https://aic-test.matrixai.one/#/transaction/93100589173f25ae6e40e02129886d493863cf2fd3eee3502ce2e0f28c5c111f) | 1258 |
| 公钥内部转账 AIC | 2.0000 AIC | 0.1475 AIC | [9f37c55a85a7](https://aic-test.matrixai.one/#/transaction/9f37c55a85a73c108242a2340613b037de0a51600e16010b50b641e806831238) | 1265 |
| 公钥提 USD 到已 open 的文本账号 | 0.5000 USD | 0.0126 AIC | [2c1706d57b28](https://aic-test.matrixai.one/#/transaction/2c1706d57b2856eb3a30ff5468874c77aaf35c120bf931e11c90882990264cba) | 1275 |
| 全额领取中继收入 | 0.0997 AIC | 0.0000 AIC | [f57b5623cccf](https://aic-test.matrixai.one/#/transaction/f57b5623cccf898098b20bae8032b951b3197a678cc1fb6ebb4ad0aaa8f8fb2a) | 1278 |
| 领取人付 RAM open 后原子领取 | 0.0030 AIC | 0.0000 AIC | [0f872dc1b995](https://aic-test.matrixai.one/#/transaction/0f872dc1b995aedf80fc928e46ded280192513ace0955159eded4762d64b9c3e) | 1288 |
| 公网 cleos 通过独立 HTTPS 中继 | 0.0001 USD | 0.0130 AIC | [a344763b456d](https://aic-test.matrixai.one/#/transaction/a344763b456d6e15ada249ba9f0556984667a08f4eab1c7f7bde38f6a266699f) | 1282 |

领取人付RAM的公开交易中，领取BP gptgenesisbp 新增 240 字节，eosio.key 新增 0 字节；收款 0.0030 AIC 全额到账，预存、网络费桶、价格缓存和币种RAM预算未被领取操作扣减或增长。缺收款行的单独claim已验证原子拒绝。

正常系统95项、明确标记的替代系统表夹具95项、15项SDK/中继测试通过。覆盖1800秒期限、错误七天参数拒绝、提前执行拒绝、原生否决/取消、最小1原子领取、无预存领取、open失败回滚、权限和超领拒绝、代币代码白名单、重复/并发交易及精确储备。夹具不代表真实原生黑名单或多机器共识资格；本轮没有等待到期后执行一次成熟升级。

本轮只做功能部署和低频业务，没有TPS或容量资格测试。实际设备为共享 Intel Cascadelake 4 vCPU、2 guest cores、8125768 KiB RAM；CPU4/NET1MiB、4 workers/state4GiB、RAM64GiB/setramrate1024及原系统模式保持。

旧0/1代低配测试链原始区块、状态、SHIP和索引卷已按要求删除，未创建原始历史备份。源码、构建/测试报告及无关的高配退役不可变证据保留。当前新链数据保留运行。

| 来源 | 固定版本／哈希 |
| --- | --- |
| EOSkey源码 | 81a09942c0e8531c020a4891d928bc7485979971 |
| EOSkey WASM | b6509dfb14fe2f23c97e8019a355cc0354a3de2d919f135bce20ddb51cdcf7f2 |
| TEST节点源码 | 057e1731660d3b8423c960cb81fe1758ae9c29f6 |
| nodeos | 0be5282d1d5a5eb72081e1d1374931d5e79cff139de728f8d230e29df50534e7 |

原生链、插件、appbase、chainbase、状态历史、版本及辅助对象共134个编译单元在本轮远端重建，随后仅增量重编治理激活实现和版本元数据。记录保留初次GMP开发库路径链接失败、修复后的实际库哈希，以及引导账号查询包装异常；后者在核对尚未创建钱包/发行账号、未支付钱包资金后于同一新Chain ID续跑，没有重新生成链或重放支付。

| 测试公钥 | 关联文本账号 | 公钥余额 | 文本账号余额 |
| --- | --- | --- | --- |
| [PUB_K1_7y1ttFMwRegZvahUzmJ7ykHKR2jmxfLkr3Xrx7uoJmXz2EhDEQ](https://aic-test.matrixai.one/#/key/PUB_K1_7y1ttFMwRegZvahUzmJ7ykHKR2jmxfLkr3Xrx7uoJmXz2EhDEQ) | keymapleseed | 10.0000 AIC / 20.0000 USD | 15.0000 AIC / 5.0000 USD |
| [PUB_K1_6W8FU4k6nypnX4cedeeoEkUaYAVzj2jJTszg8hgTS45EwPszzq](https://aic-test.matrixai.one/#/key/PUB_K1_6W8FU4k6nypnX4cedeeoEkUaYAVzj2jJTszg8hgTS45EwPszzq) | keyriverbank | 10.0000 AIC / 20.0000 USD | 15.0000 AIC / 5.0000 USD |
| [PUB_K1_8eZk9Cy8y79NqMVBotLVftHZihJegeXjVDrpE915KZ6HLPSY7Z](https://aic-test.matrixai.one/#/key/PUB_K1_8eZk9Cy8y79NqMVBotLVftHZihJegeXjVDrpE915KZ6HLPSY7Z) | keystarlight | 10.0000 AIC / 20.0000 USD | 15.0000 AIC / 5.0000 USD |
| [PUB_K1_5gfuS4139ESsBsh8fS4GsEu1PitGzHjaTa3knTpxFtxsUzAydm](https://aic-test.matrixai.one/#/key/PUB_K1_5gfuS4139ESsBsh8fS4GsEu1PitGzHjaTa3knTpxFtxsUzAydm) | 无 | 10.0000 AIC / 20.0000 USD | 无 |
| [PUB_K1_7SGTuWbkuTtmPyeDRCgRox1kVsYaYWydvogiKsyaWTtPkfmawY](https://aic-test.matrixai.one/#/key/PUB_K1_7SGTuWbkuTtmPyeDRCgRox1kVsYaYWydvogiKsyaWTtPkfmawY) | 无 | 10.0000 AIC / 20.0000 USD | 无 |
| [PUB_K1_8SSd8pP8hCABQf5htfLsMgLkAAB6BgHqAv87QHPQ2skLrraRtt](https://aic-test.matrixai.one/#/key/PUB_K1_8SSd8pP8hCABQf5htfLsMgLkAAB6BgHqAv87QHPQ2skLrraRtt) | 无 | 10.0000 AIC / 20.0000 USD | 无 |

关联表示文本账号owner/active使用同一公钥，两套余额独立。私钥及钱包仅保留在服务器。

公开入口：[浏览器](https://aic-test.matrixai.one/)、[中继健康](https://aic-test.matrixai.one/relay/health)。交易RPC为 https://aic-test.matrixai.one/rpc，独立中继为 https://aic-test.matrixai.one/relay。
