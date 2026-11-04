# Creation execution failure and correction plan / 创建执行失败与修正计划

Status: **OPEN, CANDIDATE MUST NOT BE ACTIVATED OR DEPLOYED**.

## Observed failure

The remote TEST candidate compiled, but qualification attempt7 failed the
successful native creation case and the real signup case before registration.
Both reach `apply_context::execute_inline` while queuing later actions:

```text
inline action's authorizing actor eosio.signup does not exist
```

`apply_eosio_sysexec` queues `newaccount`, sponsor funding, `buyram`, `setcode`,
`setabi`, and `sysfinish`. Queuing is not execution. `execute_inline` validates
the actor and permission immediately; `newaccount` has not run when the root
handler tries to queue an action authorized by the new account. The normal
existence check correctly rejects the transaction and rolls it back.

This is an implementation defect. It is not evidence that account creation,
signup notification execution, K1/R1 registration, stake ownership, actual RAM
allocation or signup rollback have passed. The separate codec checks and
unsigned native wire comparison do not establish those behaviors.

## Planned native continuation

Retain the actor/permission existence checks in `execute_inline` unchanged.
Introduce a native-only internal continuation, tentatively `sysstage`, rather
than adding an authorization exception for an account that does not exist.

1. Keep all public `sysexec` checks: top-level developer upgrade authorization,
   explicit sponsor active authorization, full review, effective frozen vetoes,
   exact target, payload and system/token hashes, and protected-list capacity.
2. Install the same ephemeral, single-target capability. Queue only
   `eosio::newaccount` followed by the internal continuation. Both actions use
   already-existing authorizing accounts; the continuation also carries the
   explicitly selected sponsor authorization. Do not expose a public bypass.
3. The continuation must require the exact native parent ordinal, sender
   `eosio`, matching proposal id, activated feature, pending proposal,
   `account_created=true`, and a previously unused continuation stage. Verify
   that the new account is nonprivileged, has the expected temporary code-only
   authorities, and still has empty code and ABI.
4. Consume that stage once and bind subsequent capability use to the
   continuation's ordinal. Queue sponsor funding before installing signup code,
   then RAM purchase, exact code/ABI installation, and native final sealing.
   Actor and permission existence checks now run after `newaccount` execution.
5. `sysfinish` must accept only that exact continuation parent after both
   single-use payload permissions were consumed. Verify the hashes and
   authorities again, seal owner, and mark execution complete. Any child failure
   must roll back account creation, sponsor debit, protection and proposal state
   in the same transaction.
6. Leave the existing upgrade lane, old proposal3, privileged setter, permission
   rows and existing object layouts unchanged. Append any transient capability
   field; do not add serialized transaction capability or shorten either review
   profile. Internal action replay must remain a no-op before feature activation.

## Required verification before reconsidering rollout

- Rebuild the changed native implementation remotely and rerun all existing
  governance, snapshot, replay and wire checks against the resulting libraries.
- Reject direct/forwarded continuation calls, wrong proposal/parent/target,
  repeated stage/finish, wrong authorities and interposed callbacks. Verify that
  ordinary inline actions still reject missing actors and permissions.
- Pass successful native creation and all real pinned system/token/signup WASM
  checks: K1, R1, legacy EOS, separate owner/active signatures, real RAM/stake
  ownership, wrong notification source, insufficient RAM and full rollback.
- Run the production review profile and required broader compatibility checks;
  the current O1 TEST build is not a production release or performance result.
- Preserve the current chain and obtain separate authorization for any node
  rollout. Activate only after all relevant validators support the qualified
  feature, then publish exact reviewed artifacts and wait the complete native
  review before proposing/executing actual creation.

## 中文结论

当前缺陷位于内联动作的排队顺序：创建动作尚未执行，后续动作便引用了新账号
的权限。应增加同一交易内的原生内部续执行阶段，在账号实际存在后排队部署，
继续使用精确父动作、一次性能力和完整回滚约束。不得放宽通用账号/权限存在性
检查。候选仍未通过完整验证，现网不得启用；本轮维护仅构建和隔离验证，随后
恢复原程序，不创建账号、不提交现网提案、不部署合约。
