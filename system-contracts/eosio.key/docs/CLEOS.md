# cleos 公钥转账 / Public-key transfers

这些命令由本分支的原生 C++ cleos 实现。现有 `transfer 文本账号 文本账号` 保持原用法。以下域名、账号、金额均为示例；先在部署了 eosio.key 的链上执行。公钥持有人只把私钥导入自己的本地 keosd，不上传到 BP。

These are native C++ cleos commands in this branch. Existing native-to-native transfers retain their syntax. Endpoints, accounts and amounts below are examples; use a chain with eosio.key deployed. Import the owner's key into the local keosd wallet only; never send it to a BP.

## 代币 RAM 登记 / Token RAM registration

任何原生账户均可真实购买 RAM 并登记币种，AIC 也遵守同一规则。
一个原子交易依次购买与登记，失败一起回滚。相同调用用于追加预算。
把 `keyusdtoken1` / `4,USD` 改为 `eosio.token` / `4,AIC` 即为 AIC 登记。

Any native sponsor can fund admission/top-ups; the receiving public key needs no
opening action. The purchase spends exactly `quant`; `min_bytes` bounds capacity.

```bash
cleos -u https://YOUR-RPC push transaction '{"actions":[
 {"account":"eosio","name":"buyram","authorization":[{"actor":"keyusersolar","permission":"active"}],
  "data":{"payer":"keyusersolar","receiver":"eosio.key","quant":"1.0000 AIC"}},
 {"account":"eosio.key","name":"regtoken","authorization":[{"actor":"keyusersolar","permission":"active"}],
  "data":{"payer":"keyusersolar","token_contract":"keyusdtoken1","token_symbol":"4,USD","min_bytes":4096}}
]}' --expiration 1800
```

登记人预购容量；新增收款记录费由每次发送方另付，收款本金全额到账。
已有行不重复收取新建费用。当前没有 sponsor 退款，正常手续费不自动补充 RAM 字节预算。

## 文本账户转公钥 / Native account to public key

```bash
cleos -u https://YOUR-RPC transfer keyusersolar PUB_K1_RECIPIENT '10.0000 AIC' \
  --max-key-fee '0.5000 AIC' --key-fee-fund '1.0000 AIC'

cleos -u https://YOUR-RPC transfer keyusersolar PUB_K1_RECIPIENT '5.0000 USD' \
  -c keyusdtoken1 --max-key-fee '0.5000 AIC' --key-fee-fund '1.0000 AIC'
```

`--key-fee-fund` 是独立的 AIC 费用预存，与本金转账放在同一个原子交易中；余额足够时可省略。`--max-key-fee` 必须明确填写，是本次最高实际费用，并非预存金额。余额多出的部分可通过 `refundfee` 取回。公钥转账使用合约路由 memo，不能同时指定普通 memo 或 `--pay-ram-to-open`。支持 PUB_K1 和旧式 EOS K1 公钥。

`--key-fee-fund` prepays separate AIC fees atomically with the principal deposit; omit it when credit is sufficient. `--max-key-fee` is mandatory and caps this operation's actual fee, rather than the prepaid amount. Unused credit is refundable with `refundfee`. Public-key deposits use a routing memo, so ordinary memo and `--pay-ram-to-open` cannot be combined with this path. Modern PUB_K1 and legacy EOS K1 encodings are accepted.

## 公钥转公钥或文本账户 / Key to key or native account

```bash
# Use PUB_K1_RECIPIENT for a public-key recipient, or an existing account name.
cleos -u https://YOUR-RPC key prepare PUB_K1_SENDER keyuserlunar '2.0000 AIC' \
  --bp keyrelaymoon --max-fee '0.5000 AIC' > prepared.json

# Review destination, token, amount, maximum fee, BP and expiration first.
cleos -u https://YOUR-RPC key sign prepared.json > signed.json

# BP operator: native transaction is signed with its dedicated child permission.
cleos -u https://YOUR-RPC key push signed.json -p keyrelaymoon@keyrelay

# User: alternatively send the signed public instruction to a BP HTTPS service.
cleos -u https://YOUR-RPC key submit signed.json --relay-url https://YOUR-BP-RELAY
```

`prepare` 读取链 ID、部署域和持久撤销代次，生成随机 request_id，并准备完整原生交易。`sign` 校验可见字段与固定协议编码一致，通过本机 keosd 签名。`push` 只补资源付款人的原生签名，不改交易正文、TAPOS 或过期时间。中继不能修改用户批准的内容。

`prepare` reads the deployment and revocation epoch, generates an independent random request ID and prepares the complete transaction. `sign` checks fixed binary schemas and uses local keosd. `push` only adds the resource payer signature; it never changes the approved transaction, TAPOS or expiration. The owner proof is in CFD, separate from native signatures.

## 自助转出 / Owner-authorized self withdrawal

```bash
cleos -u https://YOUR-RPC key prepare PUB_K1_SENDER keyuserlunar '2.0000 AIC' \
  --self --payer keyusersolar --max-fee '0.5000 AIC' > prepared.json
cleos -u https://YOUR-RPC key sign prepared.json > signed.json
cleos -u https://YOUR-RPC key push signed.json -p keyusersolar@active
```

原生 payer 支付链上 CPU/NET，公钥余额支付基础 AIC 费用，不收 BP 加价。payer 不能替代公钥签名。BP 中继停用、维护或拉黑时仍能使用此路径。`prepare --self` 与商业 `withdraw` 签名域不同，不能把其中一种签名挪用于另一种。

The native payer supplies transaction CPU/NET, while the key pays the base AIC fee with no BP markup. A payer cannot replace the owner's signature. This path remains independent of relay disablement, maintenance and blacklisting. Its signing domain differs from commercial `withdraw`; signatures cannot be reused across them.

`key push` 沿用 cleos 的提交语义；最终到账以不可逆区块为准。HTTP 中继的 `finalized` 状态仅在扫描到实际不可逆交易回执后产生。`--allow-http-for-test` 仅允许显式选择 loopback 测试地址。

`key push` retains normal cleos submission semantics; irreversible inclusion is the settlement criterion. The HTTP relay reports `finalized` only after finding the actual canonical irreversible receipt. `--allow-http-for-test` permits explicit loopback test URLs only.

## 并发、重试和撤销 / Concurrency, retries and revocation

任意设备可使用同一 auth_epoch 各自签名并发转账，无需共同分配 nonce。普通交易不推进代次；余额不足仍可能失败。不要在未知结果时重新 prepare/sign，先用 prepared.json 中已知的 transaction_id 查询不可逆回执。中继只重发同一份交易。明确的 cancel/selfcancel 才提高 auth_epoch，使旧代次尚未执行的证明失效；当前 cleos 快捷命令提供转账，撤销结构可通过 SDK 准备。

Independent devices can authorize concurrent sends at the same epoch. Only cancellation advances it. A timeout is an unknown result: track the transaction_id already present in prepared.json before creating a replacement. Intentionally signing a new ID authorizes another payment. The convenience CLI covers transfers; cancellation envelopes can be prepared with the SDK.

提交外部中继使用独立 HTTP 配置，不转发 RPC Authorization 或其他自定义请求头，且单独校验 HTTPS 证书。loopback 测试豁免只接受严格的本地主机地址。

External relay submission uses a separate HTTP configuration, forwards no RPC credentials/custom headers, and verifies TLS certificates independently. Test HTTP accepts exact loopback hosts only.

## 中继收入领取 / Claim relay earnings

成功商业执行把基础费记入网络费用桶，把基础费的 30% 加价记入指定 BP 的
`feebuckets.relay_earned`。例如基础费 0.0100 AIC、加价 0.0030 AIC，发送方共付
0.0130 AIC；这不是总费用按 70%/30% 分成。失败整笔回滚，不产生可领取收入。
BP 可累积后授权领取到已有原生账户；中继子权限默认不链接 `claim`。
领取需要 BP 自己的原生授权、预存 AIC 支付领取费用，并设置费用上限。
已赚收入不因中继停用、维护或失去资格而被抹去。

Successful commercial execution credits the BP's markup, which is 30% of base
fees, to `relay_earned`. The BP may aggregate and withdraw with its native account
authority, separate prepaid AIC withdrawal fees and an explicit fee cap. Relay
hot permissions are not linked to `claim` by the provisioning procedure.

```bash
# Requires sufficient confirmed earnings and separately prepaid AIC fee credit.
cleos -u https://YOUR-RPC push action eosio.key claim \
  '{"bp":"keyrelaymoon","to":"keyrelaymoon","quantity":"1.0000 AIC","max_fee":"0.1000 AIC"}' \
  -p keyrelaymoon@active --expiration 1800
```

## 原生交易导出 / Native transaction export

`key push signed.json -d` 只签署资源付款部分并导出，不广播；`-d -s` 只导出未附资源签名的原生交易。广播必须继续使用原始正文，不得更新 TAPOS、过期时间或增加强制唯一 action。修改原生字段需要持有人重新复核并签名；不能用通用 push 自动更新未附原生签名的交易头。

`key push signed.json -d` signs the resource envelope and exports without broadcasting. `-d -s` exports without a resource signature. Never refresh the original header or append a uniqueness action. A resource-unsigned export must be signed without mutating its header before submission; generic push may refresh an unsigned transaction and invalidate the owner proof.
