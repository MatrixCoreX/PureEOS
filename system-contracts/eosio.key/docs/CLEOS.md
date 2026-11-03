# cleos 公钥转账 / Public-key transfers

这些命令由本分支的原生 C++ cleos 实现。现有 `transfer 文本账号 文本账号` 保持原用法。以下域名、账号、金额均为示例；先在部署了 eosio.key 的链上执行。公钥持有人只把私钥导入自己的本地 keosd，不上传到 BP。

These are native C++ cleos commands in this branch. Existing native-to-native transfers retain their syntax. Endpoints, accounts and amounts below are examples; use a chain with eosio.key deployed. Import the owner's key into the local keosd wallet only; never send it to a BP.

## 文本账户转公钥 / Native account to public key

```bash
cleos -u https://YOUR-RPC transfer keyusersolar PUB_K1_RECIPIENT '10.0000 AIC' \
  --max-key-fee '0.5000 AIC' --key-fee-fund '1.0000 AIC'

cleos -u https://YOUR-RPC transfer keyusersolar PUB_K1_RECIPIENT '5.000000 USDT' \
  -c keytesttoken --max-key-fee '0.5000 AIC' --key-fee-fund '1.0000 AIC'
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

`prepare` 自动读取链 ID、部署域和持久 nonce，区分公钥收款和文本收款；文本收款账户必须存在。`sign` 通过本机 keosd 的 `sign_digest` 签名并校验返回签名。费用来自发送公钥的额外 AIC 余额；本金是其他币时仍需提前给该公钥存入 AIC。中继无权改变金额、收款人、手续费上限或收取 30% 加价的 BP 身份。

`prepare` reads chain ID, deployment domain and persistent nonce, and distinguishes public-key from native-account recipients. Native recipients must exist. `sign` calls local keosd `sign_digest` and verifies the returned signature. Fees come from the sender key's additional AIC balance; a key holding another token also needs AIC. The relay cannot change principal, destination, fee cap or the BP earning the 30% markup.

## 自助转出 / Owner-authorized self withdrawal

```bash
cleos -u https://YOUR-RPC key prepare PUB_K1_SENDER keyuserlunar '2.0000 AIC' \
  --self --max-fee '0.5000 AIC' > prepared.json
cleos -u https://YOUR-RPC key sign prepared.json > signed.json
cleos -u https://YOUR-RPC key push signed.json --payer keyusersolar -p keyusersolar@active
```

原生 payer 支付链上 CPU/NET，公钥余额支付基础 AIC 费用，不收 BP 加价。payer 不能替代公钥签名。BP 中继停用、维护或拉黑时仍能使用此路径。`prepare --self` 与商业 `withdraw` 签名域不同，不能把其中一种签名挪用于另一种。

The native payer supplies transaction CPU/NET, while the key pays the base AIC fee with no BP markup. A payer cannot replace the owner's signature. This path remains independent of relay disablement, maintenance and blacklisting. Its signing domain differs from commercial `withdraw`; signatures cannot be reused across them.

`key push` 沿用 cleos 的提交语义；最终到账以不可逆区块为准。HTTP 中继的 `finalized` 状态仅在扫描到实际不可逆交易回执后产生。`--allow-http-for-test` 仅允许显式选择 loopback 测试地址。

`key push` retains normal cleos submission semantics; irreversible inclusion is the settlement criterion. The HTTP relay reports `finalized` only after finding the actual canonical irreversible receipt. `--allow-http-for-test` permits explicit loopback test URLs only.

## nonce 与并发 / Nonce and concurrency

nonce 保存在合约的钱包行中。`key prepare` 查询当前值，但不会预占；当前没有跨 cleos 进程、跨设备的分号器。两个进程同时准备不同转账，可能都读到 N；链上原子校验只允许其中一笔成功，另一笔需要重新读取并签名。中继收到的是已签指令，不能擅自换号。需要多笔都成功时，应由持有人客户端在签名前协调连续 nonce，并按序提交、处理失败恢复；逐笔确认后再准备下一笔是当前直接用法。

The contract wallet row owns the persistent nonce. `key prepare` reads it without reserving it; V1 has no cross-process or cross-device allocator. Concurrent prepares may both read N. Atomic execution admits one; the loser needs a fresh read/signature. A relay cannot renumber an already signed instruction. To execute all requests, an owner-side client must coordinate consecutive nonces before signing and submit in order with failure recovery. Preparing the next request after confirmation is the current direct CLI flow.
