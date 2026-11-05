# New ordinary issuer ownership — candidate, not activated

2026-11-05 latest selection: USD/CNY are ordinary issuer accounts initially
delegated to the `eosio.dev` account. This supersedes the earlier sealed-system
custody and independent copied-private-key plans.

Creation policy `authority_version=3` is restricted to an absent, permitted,
dotless short account. It cannot create an externally controlled `eosio.*`
service, replace an existing owner, or change `eosio.key`. It uses the same
bounded four-proposal lane, exact code/ABI/RAM funding, complete native review
and effective BP veto as other creation policies.

The domain `PureEOS developer-owned issuer initializer v1\0` binds the human
review hash and exact target `init` bytes. Deployment and initialization occur
under temporary native protection. Finalization atomically removes that
temporary protection and installs:

- owner: threshold 1, account `eosio.dev@owner`, no direct keys or waits;
- active: threshold 1, `eosio.dev@active` or the issuer's own `eosio.code`, no
  `eosio@eosio.code` management delegation and no direct keys or waits;
- privileged: false.

The actual designated developer replaces `eosio.dev` in disposable test fixtures.
Policy 1/2 system services remain sealed and protected. No ordinary issuer key
is generated or copied into source, wallets, reports or artifacts.

After creation, ordinary account ownership governs deployment, issuance and
voluntary ownership handover. Developer `@upgrade` is the chain-proposal role;
issuer `owner/active` delegation is ordinary holder authorization. These roles
must not be conflated. A later issuer code change also changes its reviewed
WASM identity and does not automatically update the KEY whitelist.

Remote qualification must demonstrate both new issuers, exact final authorities,
early/altered-payload rejection, initialization rollback, no BP/foreign takeover,
ordinary owner-authorized issuance/handover and rejection of any attempt to
reuse creation for an existing account. Existing sealed-policy tests remain as
regressions. Public activation and fresh-chain evidence are still pending.

## 中文

最新选择是把 `usd`、`cny` 作为普通发行账号，初期委托 `eosio.dev`，不复制私钥。
新建策略版本3只适用于尚不存在、无拍卖占用的普通短号；不能用于 `eosio.*`、
`eosio.key` 或既有账号。仍须完整公示、BP否决、精确代码/ABI/初始化/资源预算审核。

初始化期间临时保护，全部成功后原子交付：owner 委托 dev 的 owner；active 委托
dev 的 active 或本账号代码；不保留 eosio 代码管理委托，不授予特权。后续部署、
发行和交接由合法账号持有人授权，不能与 dev 的链级提案权限混为一谈。KEY 的
审核哈希不会因为发行方自行升级而自动更新。

系统服务版本1/2保持封印。此候选尚未完成远端验证或公开激活；原来的通过结果
不能替代新增账号所有权策略的资格验证。
