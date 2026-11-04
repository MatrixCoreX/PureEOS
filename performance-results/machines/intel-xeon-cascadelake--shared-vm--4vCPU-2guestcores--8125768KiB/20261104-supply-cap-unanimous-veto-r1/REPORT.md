# 发行上限翻倍提案：全体有效 BP 否决实测

已完成真实负向治理测试：提案3要求发行上限从21亿提高至42亿 AIC；24/24个有效BP各自授权否决并不可逆确认，公示满1800秒后的实际执行被原生否决保护拒绝。当前上限仍为21亿，已发行1亿，代币代码和ABI保持原值。

## 实际链与回执

链ID `116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0`，原共享IntelCascadelake4vCPU/2guestcores/8125768KiB设备。native21/15最终确认政策保持，提案治理为24有效选民/16票门槛。已永久拉黑的germanycrest依法排除，未恢复票权；用户的“所有节点反对”在全部24个有效冻结选民上执行，无伪造签名。

- [提案3提交交易](https://aic-test.matrixai.one/#/transaction/6dbeaaff8bbc848c935a9daeba3f3c1fdc1f14eef11e4662e4f6ee8a5b27a850)，不可逆块170344。
- 提交时间2026-11-04T08:53:23.500UTC，截止2026-11-04T09:23:23.500UTC。
- 同一审核payload在2026-11-04T09:23:24.500UTC实际尝试devexec，距提交1801.0秒。
- 原生错误：`upgrade vetoed by at least two thirds of the effective frozen electorate`。失败尝试ID `1040ef871d1fa27bfc7418093bc1afec940197cfaef995bee558bcd793cd5135` 不入链，不能提供虚假的成功交易链接；已对新不可逆区块逐个核对缺席，见VETO_REJECTION_RESULT.json。
- 原代码 `f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c`、原ABI `2d69b533e9ac8e557abfde5c034988dfc8a9d3e7bc7f8656fcf866cea2dda856`、max_supply `2100000000.0000 AIC`、supply `100000000.0000 AIC`保持。提案仍pending且已达否决门槛，未撤回、直接改参、发行或绕过执行。

## 候选和隔离功能验证

候选WASM `bee8320e5e761703f469fb6b0f8c0744479b0bf329f709867bb613855808845a`；原始ABI完全不变。源归档及CPP摘要固定在CAP_REVIEW.json并由真实提案绑定。独立分支的候选只对eosio.token::4,AIC、issuer=eosio及精确旧上限进行一次现有stat行迁移；首次成功AIC转账触发，失败整笔回滚，不增动作/表、mint或权限。

远端私有单节点8项功能检查通过：安装代码本身不改上限、非AIC不触发、透支回滚、错精度拒绝、首次成功翻倍但supply不变、本金完整、重复不再翻倍、同符号其他合约不受影响。临时节点/钱包已正常停止。本候选仅用于真实否决测试，不代表已通过生产并行代码白名单或部署资格，未改公开运行白名单。

原始首轮类型编译错误、长Unix钱包socket路径失败及短路径恢复均保留；后者在临时节点启动前失败。审核文档原文中英文21b/42b缩写不准确，精确资产字段为2100000000.0000/4200000000.0000AIC，即2.1/4.2billion（21亿/42亿）；保留原审核字节和摘要，不事后修改。

## 每个有效 BP 的否决回执

| BP | 不可逆块 | 交易 |
|---|---:|---|
| anthropiclab | 170351 | [查看](https://aic-test.matrixai.one/#/transaction/3a152240906ab02d110eb6964b53238cbbd7a552ecfe84f4ba68c8ea3936fc0c) |
| brazilamazon | 170357 | [查看](https://aic-test.matrixai.one/#/transaction/1cb59f604685fdb527e1cdbee9f66e87fe42e651cb1f9482760e517ae29fba77) |
| canadaboreal | 170364 | [查看](https://aic-test.matrixai.one/#/transaction/4942d4fe8c6f8937898333d33be8e66f8c5a69f0c57ba803a7ff34c4b66f7dfa) |
| cobaltspring | 170373 | [查看](https://aic-test.matrixai.one/#/transaction/bbfa19f9476256c6127c6eaa7689d257a404cdef00d631bd24145897447db111) |
| coherevision | 170384 | [查看](https://aic-test.matrixai.one/#/transaction/f45f621e00b98adb978627be0bcf6204de23fde5af9e2a46d3a037001dac6615) |
| cosmicgarden | 170395 | [查看](https://aic-test.matrixai.one/#/transaction/a7f30d14c6381572d34f1c7e5b5aa0082780f614ef087c91f0bdc78d0b417ea6) |
| deepseekcore | 170407 | [查看](https://aic-test.matrixai.one/#/transaction/46f4d84c9f220d6a52328285793c0b9eeeac64726244c2db24c945cbbdf42a1d) |
| egyptpyramid | 170415 | [查看](https://aic-test.matrixai.one/#/transaction/a3591f44b927a2a509d71ef0769a60c654868c19f2e2057e5412b04b69eae898) |
| finlandhaven | 170421 | [查看](https://aic-test.matrixai.one/#/transaction/29fef65fd1530bd3d26aa478fe8c4a3708f42d88d4ebc47101b528a603ceeaf4) |
| francegarden | 170429 | [查看](https://aic-test.matrixai.one/#/transaction/b85241b1a8d6a9c7d3bbacbb158bf103788467bfe2594f4bf01ef56fc1837de1) |
| glaciercrest | 170437 | [查看](https://aic-test.matrixai.one/#/transaction/e32253beb7094a7503c09ecff52440dbeba8c0159e2cd908bd90d6809d777c51) |
| gptgenesisbp | 170443 | [查看](https://aic-test.matrixai.one/#/transaction/dc0fd36abc7108aeae91f9542df19eb1f64f92395871cd9a16a742463991c684) |
| italyharbors | 170449 | [查看](https://aic-test.matrixai.one/#/transaction/b6d5f20bb7c8a1dceec2c778a8431f284fe067392e39d3e5e4d7d04506472d6b) |
| kenyasavanna | 170458 | [查看](https://aic-test.matrixai.one/#/transaction/40ee595df3ca9f799fff9b435763ab453f9d5fbc8914b8c717612626979672ec) |
| lunarcompass | 170467 | [查看](https://aic-test.matrixai.one/#/transaction/ee65205df99512c4c07f627c3c9374275020ec97c80e8db0201eb51ea13789d5) |
| maplecascade | 170474 | [查看](https://aic-test.matrixai.one/#/transaction/5dd3c16c846b5458ad7a5e6825eacd935c3a04ade7eb989e38ea254b296f1ac9) |
| mistralcloud | 170479 | [查看](https://aic-test.matrixai.one/#/transaction/d53d0d4dbc77cdd900b472e0c898872a53a0c717f52a44c05d6706085b8e13e5) |
| moonlitgrove | 170486 | [查看](https://aic-test.matrixai.one/#/transaction/fb081ab1f3fe7db77aa5c56606572934d09b36062d2237ea2d17705a0479f53f) |
| nebularlight | 170494 | [查看](https://aic-test.matrixai.one/#/transaction/96d294d9283125c4868fec1642e103a27c95896ad57bfb6ba6bb68db8c824d4a) |
| silverstream | 170501 | [查看](https://aic-test.matrixai.one/#/transaction/887b677d63bd23e563e6d679219f57f291db3dfe65e7c4f24dd63ea154f5712e) |
| stellarhaven | 170508 | [查看](https://aic-test.matrixai.one/#/transaction/5ae2c35b73324eeafed9683d8f927c0ff9403b5c2855d700f463ad4b3c5c90c4) |
| velvetforest | 170519 | [查看](https://aic-test.matrixai.one/#/transaction/dd46b7a9cc81ff695f94eca891ab002a11e4556b25119851ba57e9fb56a57dd6) |
| willowgarden | 170527 | [查看](https://aic-test.matrixai.one/#/transaction/0669760bb1237f1602bfdf52f1bc2c7f384da3c2ff87351dd282fcab47f4f7ad) |
| xaineuralnet | 170534 | [查看](https://aic-test.matrixai.one/#/transaction/eb2099a359ff7db5fcdce8ba5efc1c16261e3733fc9d731d4a43de08e7ba584b) |

公示期间仅原普通低频业务运行，没有TPS、链暂停、共识改动、节点增停、新服务器或签名规则例外。后续最终保护审计及私有临时磁盘清理结果随证据追加。English: all24 eligible frozen BP owners vetoed proposal3; the exact reviewed cap payload was rejected by the native two-thirds guard after full review. No cap increase or mint occurred.

私有测试停止并完成证据校验后，仅清理5个私有原始测试/重复源码路径，分配空间5.316MiB、净空闲增加5.469MiB、可用105.427GiB；当前链、26节点、钱包、不可变签名包、源归档及候选制品保留。

最终只读保护审计2026-11-04T09:43:50.538154+00:00：原26节点PID及unit摘要、票权/押金、原Germany禁令、25注册24有效、native21/15、系统/代币/节点字节与CPU/NET/WASM参数全部保持；原普通业务已完成1755批且当次无未决批次，中继/历史/索引器/公钥投影运行。此时链头176195/LIB176193、观察头龄1.038171秒只是一次实测，不构成容量或长期延迟资格。
