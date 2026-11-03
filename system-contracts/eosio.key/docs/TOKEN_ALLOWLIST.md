# 代币白名单与开发者提案 / Token whitelist and developer proposals

2026-11-03 用户选择：本轮测试使用 `eosio.token::4,AIC` 和 `keyusdtoken1::4,USD`。
USD 为隔离测试币，使用已验证的标准 eosio.token WASM；不表示法币储备、兑付或价格承诺。
两者的代码 SHA256 均为 `f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c`。
账号、完整符号/精度、WASM 哈希同时匹配才准入。同一合约的其他币种、同名其他发行合约
及不符精度均不自动获准。每次充值、内部转账和转出授予原生权限前重查代码；发行方升级
后拒绝交互，恢复原审核字节或再经治理审核新版本。

`token_allowlist.json` 编译进受治理保护的 eosio.key WASM。没有开发者即时增删、RAM
出资人放行、中继运营者覆盖或可任意传入的白名单写接口。后续开发者按现有原生治理流程：

1. 审核新币接口、权限、通知、储备及 RAM 行为，更新完整身份及代码哈希清单，远端构建、测试。
2. 发布清单、审核说明、源码与 WASM/ABI 哈希；用 developer@upgrade 授权，原子执行
   `eosio::refreshgov` 后紧接 `eosio::devpropose(target=eosio.key,...)`。
3. 保留原生整整七天（604800 秒）审查与冻结选民 BP 否决；永久黑名单按现有原生规则排除。
4. 审查期结束且否决未达原生阈值，开发者用 `eosio::devexec` 执行完全匹配提案哈希的制品。
   新白名单随受保护代码版本生效，直接 setcode 不能绕过治理。发行合约自身升级同样需要
   重新审查其新哈希，而不是沿用账号许可。

可复用 PureEOS `scripts/pureeos_upgrade.py --target eosio.key` 准备未签名提案；
输入当前 get_upgrade_state/get_raw_abi、已测试制品及公开审核文档。此工具不签名或广播。
新隔离链在治理封存前引导部署本轮两个测试币白名单；不得把引导部署描述成跳过七天审查
执行了新增币提案。实际测试分别记录开发者提案、拒绝越权/提前执行、真实原生否决、取消，
以及已审核引导代码中的 AIC/USD 正反例；未重跑成熟提案执行就明确标注。

白名单只证明审核准入，不是原生 RAM 沙箱。获准的实现仍是信任边界，审核错误仍可能
影响托管账户；不声称可阻止任意已批准恶意代码。未批准代码和升级后的不同字节在转账
授权前被拒绝。保持原生 RAM_RESTRICTIONS，另测恶意收款通知的原子回滚。

首次真实 RAM 登记时，托管账户在审核过的代币合约中以自身作为 RAM payer 执行 open；若他人已经为托管账户预开了零余额行，则在同一原子交易中 close 后重新由托管账户付 RAM open。之后标准转账扣余额不会在首次提币时才转移这行的 RAM 账单。该分配包含在首次登记的保守预算中，追加预算不会关闭已有余额行。

USD 等第三方币提到原生文本账号时，接收账号必须已在对应发行合约由收款人或自愿代付的原生账户支付 RAM 执行
`open(owner, symbol, ram_payer)`。余额为零也可；缺行拒绝，且扣费和本金均回滚。
此规则不要求收款公钥创建原生账号或签名。AIC 保留已选的发送方承担新原生收款行费用规则。

白名单之后仍需真实购买 RAM 并原子 regtoken 登记，白名单不赠送容量，登记不等于批准。
每币预算为保守分配额度，不是执行后的原生精确 RAM 计量。系统 AIC 是唯一手续费资产，
USD 本金和 AIC 手续费分开支付，不自动兑换。

The test profile contains native AIC and standard USD, bound to exact account, full symbol and
reviewed WASM hash. The manifest is compiled into the protected wallet. Future additions use the
existing native seven-day developer upgrade proposal, frozen-electorate veto and hash-bound execution;
there is no direct developer whitelist setter. Funding never approves an issuer. Code changes fail
closed until separately reviewed. Third-party native recipients must pre-open their balance row with
separately authorized RAM payment. Public-key recipients need no opening. This is reviewed-code
admission, not a general native RAM sandbox or exact per-operation RAM metering.
