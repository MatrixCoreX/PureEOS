# 全网公钥账户按需增长：实际升级与费用

本次已通过 eosio.dev 提案 #4 公示并不可逆升级 eosio.key。实际链上公示至执行经过 3803.0 秒，要求至少1800秒；执行区块 #267777，交易 [25009c181bfa](https://aic-test.matrixai.one/#/transaction/25009c181bfade71bbcdb47c8b8d42b4b6217f542a2201a23742da83d9e265df?block=267777)。完整审查文档已在提案前发布，SHA256为1152e6be7cfbf341647c6cc6578d083fb03677dddce55ff916662a095b93b430。

新代码：`7db295ea153410eed1a1810e6122c594d6a904117db232381cf713d1ab8d8416`；新raw ABI：`83b0c49b82492d50ca72fd34e66cc879ba150fc7630ac780e372dcfaf2f4244c`。源码4b9627ea2及精确审查包7d95fc419先发布；既有config/动作/账本布局保持，仅增加growth表。没有重新初始化、全表迁移或重置测试网。旧1000/4000字段保留为兼容字段，不再挡住新增。64位计数/ID/RAM溢出与真实物理资源仍有限，不声称物理无限或TPS容量。

## 三种转账与费用

RAM实购金额包含在额外手续费中，本金完整到账。新增钱包和币种余额预算1536字节，已有公钥新增币种余额512字节，首次建立受支持资产元数据另加2048字节；已有记录不重复购买。这是保守分配预算，不是逐行实际内存用量计量。合约按链上市场真实购买，校验收到的配额；RAM项包括原生买入费和舍入余量，以执行回执为准。

### 命名账号 → 公钥

钱包将deposit声明、AIC费用预算转入、代币本金转入组成同一笔原生交易。命名发送账号签名并承担原生CPU/NET；KEY在同笔交易内买RAM并给收款公钥增加完整本金，未用费用预算立即退回，不形成多次复用的预存余额。收款方无需命名账号或AIC。费用从命名发送方额外提供的AIC支付；这种直接路径没有商业中继加价，非RAM执行项仍进入KEY网络费用桶。

### 公钥 → 公钥

发送方在本地签完整意图、收款人、本金、币种、费用上限和所选BP中继费率。中继以自己的命名账号资源和受限权限提交。KEY验证授权，在发送方公钥AIC余额另扣实际手续费，必要时同笔购RAM，再给收款公钥记入完整本金。接收方无需先准备AIC；发送USD/CNY者仍须有AIC付费。

### 公钥 → 命名账号

发送方签名、中继提交与上一种相同，命名收款账号须存在。若它尚无该币原生余额行，KEY同笔购512字节预算并替它open，然后正常代币transfer完整本金；已有行不重复购买。费用仍额外扣发送公钥的AIC，不扣收款本金。注册命名账号另有原子Signup桥：本轮0.2500 AIC本金用于新账号0.1000 NET、0.1000 CPU和其余RAM，无Signup加价，另收正常KEY手续费；创建与付款任一步失败均回滚。

商业中继路径总费用＝RAM实购＋非RAM执行项＋中继手续费。中继手续费按基础费（含RAM项及适用最低补足）乘所选10%–30%费率向上取整；本轮唯一HTTP中继gptgenesisbp为30%。实际RAM支付外的金额计入该BP已赚余额，之后领取；执行项是现有资源计费报价，不是逐笔CPU/NET实际成本报销。领取应用手续费为0，原生CPU/NET仍由BP承担，收款AIC行须已开立，缺行时由领取者先自付RAM开立。历史网络余额不转为中继收入。

任一路径超过发送方签名费用上限，或购RAM/记账/转账/注册任一步失败，整笔原生交易回滚。不会默默提高上限或少给本金。实际执行可能与较早报价不同。

## 本轮实际回执

全部金额列为AIC；字节列为分配预算/实际买入配额。每笔本金和原生RAM购买均已独立检查。

| 场景 | 总费用 | RAM支付 | 执行项 | 中继费 | 字节 | 不可逆交易 |
| --- | --- | --- | --- | --- | --- | --- |
| named-to-key AIC | 0.0217 AIC | 0.0038 AIC | 0.0179 AIC | 0.0000 AIC | 1536 / 1608 | [9f94d4d03dfa](https://aic-test.matrixai.one/#/transaction/9f94d4d03dfa611baa6c3abaf7822a739dc48dbb7a7147780f0e15744b448d40?block=268332) |
| named-to-key USD | 0.0193 AIC | 0.0014 AIC | 0.0179 AIC | 0.0000 AIC | 512 / 565 | [dd5e66618fc6](https://aic-test.matrixai.one/#/transaction/dd5e66618fc62f237f43a7df679518dec1391b15645fc71ca73f8ef25295577c?block=268858) |
| named-to-key CNY | 0.0193 AIC | 0.0014 AIC | 0.0179 AIC | 0.0000 AIC | 512 / 565 | [de8d6f755a49](https://aic-test.matrixai.one/#/transaction/de8d6f755a49e484a269e630a0232bf4b0b2db1d315dbe3d09c3db5905c73352?block=268881) |
| key-to-key AIC | 0.0311 AIC | 0.0038 AIC | 0.0201 AIC | 0.0072 AIC | 1536 / 1608 | [58fa742ad924](https://aic-test.matrixai.one/#/transaction/58fa742ad9241f535435c9e98b3d8a537c4828c49b9cecb239733680b590f937?block=268906) |
| key-to-key USD | 0.0280 AIC | 0.0014 AIC | 0.0201 AIC | 0.0065 AIC | 512 / 565 | [6eadf184191c](https://aic-test.matrixai.one/#/transaction/6eadf184191c4e1688fe8e8730a4bbb19da74099ae5051193de0d5cf2ddd1650?block=268938) |
| key-to-key CNY | 0.0280 AIC | 0.0014 AIC | 0.0201 AIC | 0.0065 AIC | 512 / 565 | [1702ada9ad63](https://aic-test.matrixai.one/#/transaction/1702ada9ad633900adfbe7d73fad2b3e70abaf9ca21d34c1b8af4c154ddb19f2?block=268967) |
| atomic KEY-to-Signup | 0.0349 AIC | 0.0000 AIC | 0.0268 AIC | 0.0081 AIC | 0 / 0 | [c943ca6d5de3](https://aic-test.matrixai.one/#/transaction/c943ca6d5de317dc587117df7670c5cd3de5ff4a82e9e8b7de7711117bc5ce54?block=268997) |
| key-to-named AIC | 0.0338 AIC | 0.0014 AIC | 0.0246 AIC | 0.0078 AIC | 512 / 565 | [2324dfd40619](https://aic-test.matrixai.one/#/transaction/2324dfd406199ff4a2bd9f2397a1c3e9cb9b5471a0e4ad66c165891b184d3f83?block=269027) |
| key-to-named USD | 0.0338 AIC | 0.0014 AIC | 0.0246 AIC | 0.0078 AIC | 512 / 565 | [d28d386dd200](https://aic-test.matrixai.one/#/transaction/d28d386dd2008ee33c2648ba60df188d23732d0479ba3bd39902d2b34b90e1a2?block=269055) |
| key-to-named CNY | 0.0338 AIC | 0.0014 AIC | 0.0246 AIC | 0.0078 AIC | 512 / 565 | [48f85abf0bd6](https://aic-test.matrixai.one/#/transaction/48f85abf0bd6f34d50b61e01be9c8a995536bb3997a0e9068837cb4244651812?block=269085) |
| named-to-USD-only key | 0.0217 AIC | 0.0038 AIC | 0.0179 AIC | 0.0000 AIC | 1536 / 1608 | [d80339c8c4d6](https://aic-test.matrixai.one/#/transaction/d80339c8c4d6f1e97ce2994b0257de46b5d14cc9ad10c45ff4a1001370bbdc92?block=269111) |

本轮只领取本轮新增商业收入 0.2126 AIC，交易 [4611dd71c1ee](https://aic-test.matrixai.one/#/transaction/4611dd71c1ee280a56ce6b26da0cbf3b89f45de763c39fb4410e11171e5823b3?block=269124)；旧已赚余额与网络储备保留，没有重放旧领取或原有付款。新建账号为`keygrowcheck`，Owner/Active由新测试密钥控制，没有dev引用。新公钥账本与同一密钥的命名账号余额分别保存。

## 验证与范围

远端实际旧WASM存款/原地升级及三币种三路线完整回归355项；另21项明确TEST ONLY种子边界检查，28项SDK检查。种子验证32位迁移、64位/ID/RAM溢出及旧1TiB门槛，不表示真实创建了数十亿账户或购买物理TiB。前端原269项全量运行中268项通过、1项在15秒超时；该项按原断言45秒预算单独复核通过，合计269个用例通过；保留原失败，不声称一次全量269绿灯。TypeScript/生产构建通过；原256MiB堆失败与构建资源调整记录保留。四种实际预览、普通公开HTTPS升级前/后界面及全部发布文件SHA256均通过，截图逐张检查。更广的历史get_block400记录保留，不宣称整个浏览器健康测试全部完成。

钱包test.16 SHA256 `db6d8f1f6b47f14814dd52dbfe56172564603e03c56b46d0df5e0cb1d3ca2c10`，原签名证书，732项核心检查、32个API26辅助类编译及r58所有其他类字节一致检查通过。它保留已安装QR test.15/r58功能，仅增加明确新/旧KEY校验与包装。本轮未覆盖两台正在另项实际验收的手机；该兼容APK单独提供，旧手机版本遇到新代码会安全要求更新。不是完整Android/硬件Keystore/相机/IME验收声明，既有待修问题不在本轮修复。

第一笔曾被原生CPU上限拒绝，原包在418个连续不可逆区块确认缺席及分配状态不变后仅重试同一ID，最终在268332不可逆执行一次；原等待器超时和回包区块提示268331均保留。后续恢复跳过已成功付款。首笔原临时报价和费用桶前值未持久化，不声称独立首笔历史桶差额/报价相等检查；其本金、实际费用、RAM和储备已核对。

升级后新密钥实际11笔小额付款及1次新收入领取通过，三币种、三路线、原子注册、实际RAM/手续费/全额本金及储备等式核实。26个现有原生节点进程、13组监控权限、原生资源/21席规则、其他合约、三币种发行量/上限保持。观察到原普通业务发送器自行重启，未修复或重放；没有零拒绝或外部安全审计声明。Signup、原生、白名单与费用公式均未升级，没有新增服务器、TPS、BP停机、重置或改用户权限。

本轮清理4份已停止任务的未签名/对齐APK中间产物，删除分配字节10,280,960（约9.80MiB）。签名APK、源码、测试证据、密钥和链数据保留；文件系统同期净增包含其他后台活动，未全部归因本轮。
