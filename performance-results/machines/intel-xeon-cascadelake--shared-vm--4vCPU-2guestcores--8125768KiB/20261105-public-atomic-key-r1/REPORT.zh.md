# 测试网实际部署与验收完成记录

状态：**全部四项部署不可逆，服务/公网/费用/权限/90余额验收通过**。

Chain ID：`09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532`。运行目录`/opt/pureeos-devnet-20261105-r1`。24个测试BP与1个非出块历史节点共25进程；AI/地区机构名称为测试身份，不表示真实企业运营。创始保持gptgenesisbp。实际原生21成员/15票；治理四提案各冻结24有效BP、16票否决门槛，不混为同一门槛。

## 四项并行审核与执行

原1–4已取消。精确新5–8先在Git7665192df公开，manifestSHA256`295c33bd6f88620d0015586b2214bfba43a5cd246d759defaffe1d7811e2733a`，再由交易09f0eb17fd5be203b25f4b0b80c5552dc46178780096ff51385cbad9e092fec4在11818不可逆提交。每项完整1800链秒，截止2026-11-05T07:19:55UTC，均未达到否决门槛。分别执行并确认：

| 账号 / Account | WASM SHA256 | 不可逆块 / LIB block | 规范交易 / Canonical transaction |
|---|---|---:|---|
| eosio.key | `7523bbec364316d06f48283e9cc17cf81fa929bd60b4efd818eadd73e215c788` | 15463 | [552759846b93…](https://aic-test.matrixai.one/#/transaction/552759846b93ef9637ea74be7d52adaffeb8e145be65667c2d02b1f213da097b?block=15463) |
| usd | `ec9e8326c1fef0b561b2b5c45ad5c05628bd172c2afdedfd6f24fa34178354bb` | 16729 | [13b07919eaf0…](https://aic-test.matrixai.one/#/transaction/13b07919eaf0a4458e29ace41b85c2206cbc3dfb59c0ec69c6c8f9b5ca9fa590?block=16729) |
| cny | `ec9e8326c1fef0b561b2b5c45ad5c05628bd172c2afdedfd6f24fa34178354bb` | 16736 | [197b53594303…](https://aic-test.matrixai.one/#/transaction/197b5359430314955245bcd80e535eada8fba37884b096a976f22837d778feec?block=16736) |
| eosio.signup | `996d520ce32c01da431334fc7bf1f03755f4c82f7ae73ddf8b9d931054ae35f3` | 16754 | [0a73444a84ca…](https://aic-test.matrixai.one/#/transaction/0a73444a84caa03bf7fecc4bc32f1ffa42c6fbd26fb4ba1179ce849b2bea492f?block=16754) |

当前创建计数8、待执行0；浏览器按当前新链展示4撤回/4实际执行，旧链提案不混入。

## 权限与资产

| 账号/角色 | 实际权限与边界 |
|---|---|
| eosio.dev | 非privileged。owner→civicgovlead@owner，active→civicgovlead@active，upgrade→civicgovauth@active；无直接公钥或wait。未来多签须同时保持上级权限不更弱。 |
| eosio.key / eosio.signup | 非privileged、owner空权威封印。active为eosio@eosio.code或自身@eosio.code，无dev/BP/外部密钥。原审核“仅自身code”文字不准确，更正单独公开，原审核字节不改。 |
| usd / cny | 普通、非privileged发行账号，不加入系统保护表。owner→eosio.dev@owner；active→eosio.dev@active或自身code。合法owner可管理这两个自身账号，不扩展到KEY或别人账号。 |
| BP | 自身签名/生产/投票/奖励及治理否决。全部非privileged，没有系统托管owner/active密钥或强改他人授权/资产的管理接口。 |
| eosio.prods / wrap / msig / null | owner和active均空权威封印，不能用旧BP多签入口接管系统。 |

代码权限不是可导入钱包的私钥。eosio本身owner封印、active仅其代码；审查后的系统代码仍有系统能力。可见代码与BP否决是治理保障，不能据此证明任意未来获批代码绝不会影响资产。未新增系统权限轮换或通用超级执行器；普通合法持有人仍可自愿更改自己的授权。USD/CNY为模拟第三方代币，没有真实法币储备承诺。

AIC最大2100000000.0000、已发行100000000.0000不变。USD/CNY各最大1000000.0000、初始发行10000.0000，均在普通发行账号上。没有额外增发AIC或更改用户资产。

## 三种转账和实际费用

完整本金到账，发送方额外付AIC。原生RAM买入由KEY在同一原子交易内完成，既有行不重复买；没有正常业务预存fee、人工regtoken/open或人工水位补充。下表为这些具体交易的实际账单，并非永久报价。

| 场景 | 完整本金 | 实扣AIC | 实际RAM支出 | 留存执行费 | 中继费 | 预算/实购字节 | 不可逆收据 |
|---|---|---:|---:|---:|---:|---:|---|
| 命名→公钥 / USD首次准入 | 100.0000 USD | 0.0251 | 0.0083 | 0.0168 | 0.0000 | 3584/3637 | [c381c9f1b1e8…](https://aic-test.matrixai.one/#/transaction/c381c9f1b1e8da6694727a94a063620c42b28e4eb629d2f0521fee7d8d4e99c4?block=17038) |
| 命名→公钥 / 新AIC余额 | 20.0000 AIC | 0.0182 | 0.0014 | 0.0168 | 0.0000 | 512/576 | [ee1ef967d8b1…](https://aic-test.matrixai.one/#/transaction/ee1ef967d8b14c92c929416e0188b7540d60412fcacd131186837035df093361?block=17058) |
| 命名→公钥 / CNY首次准入 | 100.0000 CNY | 0.0228 | 0.0060 | 0.0168 | 0.0000 | 2560/2617 | [cbd04baa5ebd…](https://aic-test.matrixai.one/#/transaction/cbd04baa5ebd4416ddc43e1a2f16d81a4055882149fc20bd6327d4441f8cf1ea?block=17086) |
| 公钥→公钥 / 新公钥AIC | 3.0000 AIC | 0.0294 | 0.0037 | 0.0189 | 0.0068 | 1536/1596 | [82495ef2b3bf…](https://aic-test.matrixai.one/#/transaction/82495ef2b3bfbc7832d9a9289eb679119631670f0350eee5285f7d690261d2c5?block=17099) |
| 公钥→公钥 / 新USD余额 | 2.0000 USD | 0.0264 | 0.0014 | 0.0189 | 0.0061 | 512/576 | [c6403622508b…](https://aic-test.matrixai.one/#/transaction/c6403622508b3ef8c3c5224cc5d1c5a693366f34fbb2701a2f8cfce23ee33d55?block=17124) |
| 公钥→公钥 / 新CNY余额 | 2.0000 CNY | 0.0264 | 0.0014 | 0.0189 | 0.0061 | 512/576 | [e2d91953fc14…](https://aic-test.matrixai.one/#/transaction/e2d91953fc146231a54f524425b2e06b20f9b4e249969c0459a971324bbe062c?block=17148) |
| 公钥→公钥 / 已有AIC余额 | 1.0000 AIC | 0.0246 | 0.0000 | 0.0189 | 0.0057 | 0/0 | [891aee2216b1…](https://aic-test.matrixai.one/#/transaction/891aee2216b1bcef12f2c2ad6a2327fc5c8b54f13db86576a6d5ec9d00ee80c2?block=17197) |
| 公钥→命名 / 首次AIC余额 | 1.0000 AIC | 0.0319 | 0.0014 | 0.0231 | 0.0074 | 512/576 | [ec052f92feb1…](https://aic-test.matrixai.one/#/transaction/ec052f92feb16356fac2faca0c3ba004e000f252a355050a32e655d1a2d00d3a?block=17229) |
| 公钥→命名 / 首次USD余额 | 1.0000 USD | 0.0319 | 0.0014 | 0.0231 | 0.0074 | 512/576 | [3fdb6932b173…](https://aic-test.matrixai.one/#/transaction/3fdb6932b1737da209fc2940af414ea02e86af3e7c97894645155e1bcaf7f29c?block=17247) |
| 公钥→命名 / 首次CNY余额 | 1.0000 CNY | 0.0319 | 0.0014 | 0.0231 | 0.0074 | 512/576 | [7072af411aad…](https://aic-test.matrixai.one/#/transaction/7072af411aade0fba37900052a71208238725392cd12bb5167fe2ccd7f250b38?block=17271) |
| 公钥→命名 / 已有USD余额 | 0.5000 USD | 0.0301 | 0.0000 | 0.0231 | 0.0070 | 0/0 | [6e977f65d39b…](https://aic-test.matrixai.one/#/transaction/6e977f65d39b51864fee96f9516e8be403d93ec8e283d730eedcc3402af188c6?block=17294) |
| 命名→仅持USD的新公钥 | 2.0000 USD | 0.0205 | 0.0037 | 0.0168 | 0.0000 | 1536/1596 | [2fc70567bd11…](https://aic-test.matrixai.one/#/transaction/2fc70567bd11fd8b24bdd96823acfc178314aa1f630eeeb3f363f9089bfd795b?block=17320) |

12笔合计应用费0.3192AIC＝实际RAM0.0301＋留存执行费0.2352＋中继0.0539；预算12288字节，实际买到12902字节。收款本金不因此减少。

- 命名→key：钱包一次签三个根动作（deposit声明、AIC费用资金F、完整本金transfer）；同笔退回F−实际费用。100USD例F=1AIC、实际0.0251、退0.9749AIC；无持久预存、无中继服务费，原生CPU/NET由发送命名账号承担。
- key→key：用户私钥本地签owner证明和完整交易；唯一创始中继用专用子权限提交并付原生CPU/NET；发送key账本额外扣AIC。3AIC到新key例：扣本金3加费用0.0294，B完整得到3；其中实际RAM0.0037、留存0.0189、中继0.0068。
- key→命名：同样由中继提交；命名账号须已存在，首次该币余额行自动以KEY为RAM payer打开。1AIC例：扣本金1加费用0.0319，命名账号完整得1，其中RAM0.0014/留存0.0231/中继0.0074。首次开币种行无需收款人手动open。

RAM金额精确等于这笔原生buyram实际总支出，含按最小单位向上取整的原生0.5%买入费和一个净单位余量；余量也实际买成RAM。1536/512/0为保守分配预算，不是精确逐行实际用量。非AIC币种首次准入另加2048字节。CPU/NET倍率只乘工作费，基础费最低0.0100，再加中继；本轮报价随市场/备注/时期变化。转USD/CNY的发送方没有AIC会原子失败，收款方没有AIC仍可接收。

只启用gptgenesisbp的30%HTTP中继。其余10/15/20/25%为空端点、禁用比较条目，无新增中继热钥匙/权限/HTTP服务。费率范围10–30%，以签名锁定率和总费用上限约束。领取本次已赚中继收入0.0539AIC完整到账、应用费0；领取方承担其原生资源及必要收款开表。

## 验证范围

- 远端native58（46治理+8真实合约+4报文）、KEY238原生及最终SDK26通过；真实手续费WASM、注资REX和全局4层环境。
- 后端完整33项、前端266项/TypeScript/生产构建通过；最终6个静态文件与公网Chrome SHA256完全相符。
- 公开转账55检查/12笔成功交易，注册19检查/3组实际K1、legacy EOS、K1 owner/R1 active注册及签名；免费全额领取中继收入、缺AIC、签名费用上限、旧fee转账、重复HTTP提交/回滚均验证。
- 预览及公网各4组实际Chrome，每组4个自有公钥页面，每个阶段合计16视图，核对12笔规范付款及真实余额/费用；另有桌面/手机三方向共6个交易收据页，逐项核对实际RAM/执行/中继/合计。
- 当前25节点、24自投保证金、90随机普通票覆盖所有BP且总票两两不同；实际21/15最终性持续。90个原生余额与业务账本精确相符，包含已证明的额外0.0001AIC入账。
- 本轮是共享Cascadelake4guest-vCPU/2可见guest核心/8125768KiB上的功能与安全边界检查；没有TPS、生产容量、并行白名单、物理手机或抗量子算法上线资格声明。

新SDK/ABI已适配。旧APK及旧cleos命名充值快捷封装没有本轮兼容资格，应使用新SDK的三动作构造。公钥钱包目前K1；注册公开实测K1/legacy及分离K1owner-R1active。资料不代表整链抗量子算法已上线。

## 故障复盘与恢复

1. 旧新链提案1–4的首次KEY执行因sysstage→RAM费用→REX路径超过4层原子失败；四项已不可逆取消，未篡改审核内容。原57/237夹具遗漏真实费用/REX路径，原失败和结果保留，不能作为完整路径覆盖。修复原生已消费stage序号判断和KEY多余self-payram跳转后，保持全局4层重验通过。
2. 新5–8全部重新完整公示1800链秒。KEY曾4次明确CPU拒绝，同一不可变ID随后入块15463，CPU9758us；初始收据等待器先超时。按正向规范收据恢复，未重发KEY、未改150000us上限。其余三项分别执行；客户端验证窗口扩大为600秒，协议公示/资源限制不变。
3. 权限审计纠正了“仅自身code”的错误文字，真实策略1/2保留封印eosio.code或自身code；没有外部私钥、dev/BP接管或实际权限更改。Git另附中英更正，原manifest与审核文件哈希不变。
4. 浏览器第一次复验命中了折叠明细中的隐藏链接，修正测试选择器为可见链接后通过；原失败日志保留，没有为此改动产品数据。
5. 提案资源标记给90普通账号之一civicuserone入账0.0001AIC，闭合业务账本此前未计入，导致定期对账失败。验证11818规范收据后只补记这一已有入账，保留原初始总额并单列external credit；暂停/恢复仅该自有业务控制器，随后真实批次与90余额通过。没有再转账、改原生余额/投票/保证金。未来模板把标记收款方移出90业务组。

## 固定流程、留档与清理

流程为固定输入/缓存资格→旧链排空留档→新创世核心→角色/创始交接→BP自投及90随机票→历史查询→4精确审核先Git公开→完整公示→4独立执行/规范恢复→三方向/三币种/注册/公网验收→脱敏证据与Git。复用合格工件可省重编，公示、准入、最终性和封禁窗口不能缩短；下一轮必须生成新目录/密钥/ChainID/审核包，不能机械重跑当前付款控制器。通用任意计划尚无端到端资格声明。

旧116ea0c3链原始blocks/state/SHIP/索引、德国封禁和旧全票否决提案3仍单独留存，原26服务停用；此次撤回的是新09链创建1–4。没有恢复已排除身份、增加服务器或TPS轮次。所有构建/测试/Chrome远端执行。

89份公开证据先在本机逐字节校验，之后仅清除已停止的私有PASS夹具/opt/ka-attempt-15，移除已分配9859072字节；保留所有源/合格对象缓存/工件/失败证据/旧链原始数据。临时预览已停、创始keosd已停，普通签名钱包、唯一中继、历史和25节点保持运行，节点PID未变。私钥、口令、签名包、原始状态不进入本公开证据。

## 代码与来源

Native核心42dda604af169845b9ea9b9835fe0d25ecc126a8，ELF946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397。KEY代码e1847d0d9、signup源码aeb53ba、custody.token源码随KEY分支；精确WASM/ABI/初始化在审核包。前端源c3d9809/源码档案67259a06665729929537042343fbc162e26b3478a4b0105ec4e5373d2ade0af9，实际静态发布/var/www/aic-test/releases/20261105T075357Z-atomic-ram-r3。后端功能源a4f11a8及构建摘要保留。稳定发行标签/资产未更改。

原生复盘重点：apply_context::exec识别已消费的sysstage序号，system_account_proposal.cpp逐ID审核/能力/最终封印；KEY begin_payment/payment_notification直接排入schedule_ram，settleram核对实际配额/资金/上限后再结算。每项实测与SHA256在附带证据和MANIFEST中。
