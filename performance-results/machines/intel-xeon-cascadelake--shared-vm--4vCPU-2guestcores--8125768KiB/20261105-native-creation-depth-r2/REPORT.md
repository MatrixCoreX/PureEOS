# Reviewed creation depth repair / 审核创建深度修复

Remote isolated qualification9 **PASS58cases =46native governance +8actual-contract +4wire**. Core source42dda604af169845b9ea9b9835fe0d25ecc126a8; node SHA256946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397,41330168bytes. Actual pinned systemb591/tokenf8/signup996/KEY7523/issuerEC9; real fees WASM and fundedREX; maximum inline depth4.

The previous57-case qualification omitted the deepest live fee-forwarding path. Publicsysexec attemptc9ceb962c2e6c450ed31b9428eec8abba5a3bd732cad302a51b93d90ac89e7c9 failed atomically with max inline depth reached. No target account/deployment committed. Four mature new-chain proposals1–4 were cancelled irreversibly, not silently edited or replayed. Old-chain vetoed proposal3, bans and raw/history evidence remain archived separately.

Native sysstage is an internal single-use continuation of an exact top-level reviewedsysexec. Its handler validates parent/payer/target/code/ABI/initializer and then rebinds the permit to the stage ordinal. The dispatcher now recognizes that consumed-stage ordinal and omits its bookkeeping hop when dispatching ordinary children. The first attempted fix compared the earlier parent ordinal after rebinding and therefore never applied; qualifications7/8 preserved here document that failure. No WASM intrinsic, ordinary action, CFA child or unreviewed transaction can request the discount. Existing forged/forwarded/stale/multiple capability and authority/state rollback checks remain in the46-case suite.

All wire/state fields, global depth4, CPU/NET caps, sealed system/KEY authorities, ordinary-holder control, BP veto-only scope and TEST1800seconds/production7days remain unchanged. The8real cases include actualKEY7523 initialization with full fees/REX, bad initializer rollback, signupK1/R1/legacy signatures, short-name guards, independent parallel issuers and ordinary legitimate owner delegation.

Retained hardware: shared Intel Cascadelake4guestvCPUs/2visibleguestcores/8125768KiB RAM. Incremental remote O1 TEST build reused142hash-verified objects and rebuilt apply_context plus version and tests. No TPS, production performance, parallel whitelist, stable release or post-quantum claim. Fixtures/raw state/keys remain remote only.

At report creation, one-at-a-time rollout to the current25new-chain nodes is in progress, using the same chain/data/keys and real finality guards. No KEY/signup/USD/CNY public deployment has committed. Fresh exact IDs5–8 must be published, reviewed for the full1800chain seconds and executed independently, followed by actual public transfer/signup/history/browser checks. Rollout and public outcomes are recorded separately.

本轮58项全部通过，补齐真实手续费合约和已注资REX；先前57项不代表覆盖该路径。修复只识别已验证、已消费的原生创建步骤，不放大全局4层上限。第一次修复因比较了重新绑定前的序号而无效，失败证据保留。KEY自身238项/SDK26另有报告。公开部署仍须全节点同版本、新提案完整公示与实际验收，不用隔离结果冒充公开成功。
