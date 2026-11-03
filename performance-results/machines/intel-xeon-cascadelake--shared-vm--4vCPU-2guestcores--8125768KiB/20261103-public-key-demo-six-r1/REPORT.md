# 六个公钥与三个文本账号（2026-11-03）

用户要求增加若干测试公钥，并将一部分绑定文本账号。已在原公开 TEST 链38391d73bbcaac3c51af7009d0d11b09ca263895c1bf55a96494c3194f7f4d65新增6个随机独立K1公钥，其中3个作为全新12字符文本账号的owner/active权限密钥。未替换任何旧账号权限；其他3个无原生账号关联。

每个公钥托管余额10.0000 AIC、20.0000 USD，发送方额外付新行/执行费，收款本金完整。每个文本账号自身余额15.0000 AIC、5.0000 USD，另有自有10AIC NET/100AIC CPU抵押与自付RAM的USD open行。公钥托管余额和文本账号自身余额独立记账。所有资金来自gptgenesisbp已发行测试币，不增发、不动原90普通账号、BP票数/押金/罚没池。

绑定语义为同一K1公钥是文本账号的原生owner/active权限；没有新增或修改eosio.key绑定表/代码。PureHistory真实get_key_accounts返回三组对应关系，对其余三组返回空。18笔不可变签名交易分别写入日志后发送，均核对规范不可逆区块ID和executed回执；重复运行重用原包/ID及既有回执，不自动签出替代付款。

实际浏览器所用公网只读RPC和历史查询接口另核对6组余额/绑定。此轮无前端代码修改，没有重跑Chrome，也未创建服务器或启动已删除的安全VM。所有私钥只在保留TEST服务器/opt/pureeos-keynet-20261103-r1/private及其本地签名钱包，未下载/输出/Git。源码编辑与证据接收在本机，实际生成/交易/验证均在远端，无TPS。

|序号|文本账号|公钥|
|---|---|---|
|1|keymapleseed|PUB_K1_7KkBtkkFhkK3CJ751zzghVVQKbXvN4x9kcCSH4ZoNgXDHaNrp7|
|2|keyriverbank|PUB_K1_5NiBtf3pshecov8Dc9jNXw2BFyGRk7mGqdFTm8BpfoLaKG2cmT|
|3|keystarlight|PUB_K1_7g4xWPhmt48rjUB6uZCNF36zY1XgmrUR77RNuwwt27iSeEhTFx|
|4|无关联|PUB_K1_78awimr8VkVzBHw3xXWK77VQP7rwVXTXNg42WuBfFMG9hHhpZd|
|5|无关联|PUB_K1_51WWk5G31dS3XXEVEnU2kAG8zPRrUNP5tPd8XoSEozF2t2i4h3|
|6|无关联|PUB_K1_6Rf6SG444QchLiHPxaDZkQEnak53TtXMjPkV3qLPdwrTEuqJzw|

浏览器入口：https://aic-test.matrixai.one/#/key/<公钥>。详细余额、权限验证、索引、18笔不可逆位置见RESULT.json；公网端点验证见PUBLIC_API_VERIFICATION.json。
