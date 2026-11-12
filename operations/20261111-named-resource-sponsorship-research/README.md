# Named-account resource sponsorship / 命名账号资源代付研究

Research recorded on 2026-11-11, published on 2026-11-12. This remains a design study: no sponsorship implementation, contract upgrade, simulation execution or live sponsored native transfer was performed.

协议层面，具备所需资源和授权的普通命名账号也可以做 CPU/NET 代付，不需要 BP 身份或系统特权。当前 eosio.key 商业中继登记的 BP 资格限制属于现有业务规则，不能等同于协议限制。原研究中的“沿用合格 BP 登记”是当时的一个建议，并非已经决定所有未来原生代付服务必须由 BP 提供。

- 当前链已在第 1 块激活 `ONLY_BILL_FIRST_AUTHORIZER`。承担 CPU/NET 的账号必须是首个普通 action 的首个授权者；仅调整签名数组顺序无效。
- RAM 仍按具体合约动作的 payer 规则分配。可在完整交易中预先按报价购买发送账号缺少的 RAM，再由发送账号承担数据行 RAM；不能假设代签会自动改变 RAM payer。
- 先组装完整交易，明确收款、币种、本金、资源购买及服务费，由用户核对签署，再由资源方对相同字节共同签名。固定 AIC RAM 支出可以限制现金成本，实际获得字节不足则整笔交易失败。
- 命名转命名可研究使用现有原生动作组合；命名转公钥当前直接充值入口严格要求两个根动作，不接受任意代付前缀，需要另行设计受治理的专用入口，保留现有直接零手续费路径。
- `compute_transaction` 可帮助发现执行、资源及 RAM 问题；它不能证明签名授权成立，也不能保证未来状态、执行耗时或 AIC 成本相同。接口存在已通过只读元数据确认，本研究没有执行该接口。
- 当前钱包与中继没有实现这套原生代付服务。是否开放普通服务商、是否在链上强制资格，应在后续方案中明确。

At protocol level an ordinary named account with sufficient resources and the required authorization may sponsor CPU/NET; it does not need BP status or privileged status. Existing KEY relay BP eligibility is a separate application rule. The earlier BP-registry recommendation is an option, not an implemented universal restriction. RAM payer rules, immutable joint signatures, explicit fees, atomic rollback and the current strict KEY two-root deposit boundary must be addressed separately. `compute_transaction` is useful preflight evidence, not proof of authorization or a cost guarantee. See the original source hashes and historical read-only capability snapshot in the JSON files.
