# Creation execution failure and correction plan / 创建执行失败与修正计划

Status: **CONTINUATION REPAIR PASSED 38 REMOTE ISOLATED TEST CASES; NOT DEPLOYED OR ACTIVATED.**

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

This was an implementation defect. The retained attempt7 is not evidence that account creation,
signup notification execution, K1/R1 registration, stake ownership, actual RAM
allocation or signup rollback have passed. The separate codec checks and
unsigned native wire comparison do not establish those behaviors.

## Native continuation repair

Retain the actor/permission existence checks in `execute_inline` unchanged.
The repair introduces a native-only internal continuation, `sysstage`, rather
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
   rows and existing object layouts unchanged. No transient capability field is
   added: exact ordinal rebinding plus the required parent action type enforce
   single use. Do not add serialized capability or shorten either review
   profile. Internal action replay remains a no-op before feature activation.

## Repair verification and remaining rollout requirements

On November5, remote build4 linked the repaired node and standalone controller
test binary. Qualification2 passed34 governance cases,3 real-contract cases and
1 unsigned-wire case. The two previous failing successful-execution cases now
pass, including actual pinned signup WASM execution, K1/R1/legacy registration,
separate owner/active signatures, RAM/stake ownership and atomic rollback.
New regressions cover exact action ordering, direct/forwarded staging calls,
missing authorizing actors/permissions, nested/duplicate continuations and
rollback of both governance lanes. Qualification1 already passed34+3 cases but
its helper step failed because staging omitted the unchanged public Python
helper; that failure remains retained. Copying the helper resolved it without
another native source change or rebuild.

This is a mixed O0/O1 incremental **TEST functional build** using retained,
hash-checked dependencies. It is not an optimized production build or a
performance qualification. The approved60-minute maintenance window restored
original programs; no candidate was installed and no live governance action,
feature activation, system-account creation or contract deployment occurred.
See the dated bilingual repair report and public evidence under
`performance-results/machines/intel-xeon-cascadelake--shared-vm--4vCPU-2guestcores--8125768KiB/20261105-system-signup-fix-r2/`.

- Completed in the remote TEST qualification: rebuild the changed native
  implementation and run the governance, snapshot, replay and wire suites.
- Completed in those suites: reject direct/forwarded continuation calls, wrong proposal/parent/target,
  repeated stage/finish, wrong authorities and interposed callbacks. Verify that
  ordinary inline actions still reject missing actors and permissions.
- Completed in the real pinned system/token/signup WASM tests: successful native
  creation, K1, R1, legacy EOS, separate owner/active signatures, real RAM/stake
  ownership, wrong notification source, insufficient RAM and full rollback.
- Still required: the production review profile/build and broader current-chain
  compatibility and permanent-ban/resource-boundary qualification. The mixed
  TEST build is not a production release or performance result.
- Preserve the current chain and obtain separate authorization for any node
  rollout. Activate only after all relevant validators support the qualified
  feature, then publish exact reviewed artifacts and wait the complete native
  review before proposing/executing actual creation.

## 中文结论

缺陷位于内联动作的排队顺序：创建动作尚未执行，后续动作便引用了新账号的
权限。已增加同一交易内的原生续执行阶段，在账号实际存在后排队部署，保留
精确父动作、一次性能力和完整回滚，未放宽通用账号/权限存在性检查。远端
38项隔离测试全部通过，两条原失败路径及真实K1/R1注册、资源与签名已验证。
这是TEST功能验证，生产构建及更广兼容性资格仍待完成。维护仅恢复原程序，
未创建现网账号、未提交现网提案、未激活或部署候选。
