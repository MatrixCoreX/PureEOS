# PureEOS node startup, BP registration and maintenance

[简体中文](BP_STARTUP_REGISTRATION.md)

This guide covers the current PureEOS system contract: direct single-choice voting, native proposer/finalizer rotation, signed BP gossip and planned maintenance. Protocol accounts retain their existing `eosio` names. These instructions join an existing chain; creating the genesis chain and its first BP is a separate bootstrap procedure.

Use this order: **prepare the account and keys → start the node → finish synchronization and readiness checks → register the BP and matching public keys → obtain positive direct votes → await normal on-chain rotation.**

Registration does not test whether your process is online, synchronized, reachable or able to sign. Make the node ready before making it eligible for admission.

## 1. Registration, candidacy, native duty and maintenance

| State | Meaning | Operator action |
| --- | --- | --- |
| Ordinary full node | Synchronizes and validates without participating as a BP | Finish synchronization and configuration first |
| Active BP registration | `producersv2.is_active` is true | This alone does not establish positive voting weight or native membership |
| Eligible candidate | Valid registration and proposer/finalizer keys, positive votes and qualifying rank; neither permanently banned nor requesting maintenance | Remain online; normal rotation may select the account |
| Native incumbent | An effective proposer or finalizer policy still assigns duties | Continue producing/finalizing until the native handoff releases all duties |
| Awaiting maintenance | `bpmaint.state=1` | The request exists, but the node must stay online during handoff |
| Effective maintenance | `bpmaint.state=2`, its checkpoint is irreversible and all relevant policies/rotation duties have released the account | Planned shutdown may now proceed |

Waiting as an unassigned standby does not create a personal absence violation. Changes in votes, deregistration or submission of a maintenance request do not immediately end already assigned duties.

## 2. Prepare the host, account and signing roles

Use a verified PureEOS runtime with the existing chain's configuration and genesis file or trusted snapshot. See the [main README](../../README.md#english) for installation, supported systems and source builds.

The hardware floor is 8 physical cores, 16 logical threads and 64 GB of system-visible physical RAM. Every live node must use parallel mode; startup rejects insufficient hardware or explicitly serial settings. Current development caps are CPU16 (3,200,000 μs/block), NET4MiB (4,194,304 bytes/block), with 16,384 MiB of local state capacity. Existing-chain consensus parameters remain authoritative; local configuration cannot change them.

Prepare your own **12-character native account**, RAM and transaction CPU/NET. Use the account's own authorization for its staking and PowerUp. Positive voting weight must actually exist on-chain; a zero-weight selection does not create it. Keep the host clock synchronized and check disk space, signing services, DNS and inbound/outbound P2P connectivity. Producer nodes leave SHIP disabled; a separate non-producing node supplies history.

The example account `bpoperatoraa` has exactly 12 characters. Replace it with your own account.

| Signing role | Purpose | Required match |
| --- | --- | --- |
| Account authority | Authorizes registration, voting and maintenance transactions | The account's current permission; manage separately from node signing roles |
| Block signing | Signs produced blocks | Local provider matches the public key registered by `regproducer` |
| BLS finalizer | Signs finality votes | Local private key matches the active on-chain finalizer key |
| Peer identity | Signs BP gossip identity and endpoint records | Local provider matches the current key registered by `regpeerkey` |

To generate new keys into protected local files, using tools from the runtime directory:

```bash
umask 077
mkdir -p /srv/pureeos/keys
./bin/cleos create key --file /srv/pureeos/keys/block-key.txt
./bin/cleos create key --file /srv/pureeos/keys/peer-key.txt
./bin/helios-util bls create key --file /srv/pureeos/keys/finalizer-key.txt
```

BLS output includes the public/private keys and Proof of Possession. Only the public key and proof are registered. Keep key files and secret configuration outside Git, readable only by the service user and necessary administrators. Set appropriate ownership on these example paths and do not overwrite existing keys.

## 3. Configure the node before starting synchronization

Start with [`config.example.ini`](../../config/config.example.ini), supplying separate data storage, ports and the target chain information. The following adds BP roles and connectivity; it is **not a complete configuration**. Replace every `<...>` placeholder.

```ini
plugin = eosio::producer_plugin
producer-name = bpoperatoraa

http-server-address = 127.0.0.1:8888
p2p-listen-endpoint = 0.0.0.0:9876
p2p-server-address = <YOUR_PUBLIC_P2P_HOST>:9876
p2p-peer-address = <TRUSTED_ORDINARY_SEED_HOST>:9876

signature-provider = <BLOCK_PUBLIC_KEY>=KEY:<BLOCK_PRIVATE_KEY>
signature-provider = <BLS_PUBLIC_KEY>=KEY:<BLS_PRIVATE_KEY>
signature-provider = <PEER_PUBLIC_KEY>=KEY:<PEER_PRIVATE_KEY>
p2p-bp-gossip-endpoint = bpoperatoraa,<YOUR_PUBLIC_P2P_HOST>:9876,<YOUR_OUTBOUND_PUBLIC_IP>
```

Keep `KEY:` entries only in protected local configuration. The current BLS path uses `PUB_BLS...=KEY:PVT_BLS...`; do not assume an ordinary KEOSD provider can substitute for BLS signing. Block and peer signing may use other actually supported, verified providers. The BP gossip private key is loaded through `signature-provider`; generic P2P `peer-private-key` configuration is not its substitute.

Static seeds establish initial connectivity. Signed gossip also needs the registered current peer key, its local signer and reachable endpoints. A static peer list alone is not gossip qualification. Synchronize through ordinary seeds before registration and verify BP discovery after registering the peer key.

Start through the matching runtime wrapper. Manage the production process with systemd or an equivalent supervisor:

```bash
./bin/nodeos \
  --config-dir /srv/pureeos/config \
  --data-dir /srv/pureeos/data \
  --genesis-json /srv/pureeos/genesis.json
```

Do not enable `enable-stale-production` when joining an existing chain. Before admission, verify that `pause-on-startup` or a producer-control API has not paused local production. A local production pause is not on-chain maintenance.

## 4. Pass readiness checks before registration and admission

Compare local and trusted reference RPCs:

```bash
PUREEOS_LOCAL_RPC=http://127.0.0.1:8888
PUREEOS_REFERENCE_RPC=https://YOUR_TRUSTED_RPC
curl -fsS "$PUREEOS_LOCAL_RPC/v1/chain/get_info"
curl -fsS "$PUREEOS_REFERENCE_RPC/v1/chain/get_info"
```

Check the following:

1. Chain IDs match, local head time is near the reference head, and both head and LIB keep advancing with a small gap. A running process or responsive RPC alone is insufficient.
2. Compare block IDs at the same irreversible height on both nodes. A small temporary height difference is not by itself a fork.
3. Local block, BLS and peer signing configurations match the public keys being registered; signers, P2P connections and clock synchronization are working.
4. There are no hardware, serial-mode, validation, disk or signing errors, and local production is not paused.

```bash
curl -fsS "$PUREEOS_LOCAL_RPC/v1/chain/get_block" \
  -H 'Content-Type: application/json' -d '{"block_num_or_id":"<SAME_IRREVERSIBLE_HEIGHT>"}'
curl -fsS "$PUREEOS_REFERENCE_RPC/v1/chain/get_block" \
  -H 'Content-Type: application/json' -d '{"block_num_or_id":"<SAME_IRREVERSIBLE_HEIGHT>"}'
```

The transaction examples assume an existing working wallet/authority and replaced placeholders. Submit through a verified transaction-capable RPC. An accepted RPC response does not establish irreversible inclusion.

```bash
PUREEOS_TX_RPC=https://YOUR_VERIFIED_TRANSACTION_RPC
PUREEOS_WALLET_URL=unix:///YOUR_PRIVATE_WALLET/keosd.sock
CLEOS=(./bin/cleos --no-auto-keosd --url "$PUREEOS_TX_RPC" --wallet-url "$PUREEOS_WALLET_URL")

"${CLEOS[@]}" push action eosio regproducer \
  '["bpoperatoraa","<BLOCK_PUBLIC_KEY>","https://YOUR_BP_SITE",0]' \
  -p bpoperatoraa@active

"${CLEOS[@]}" push action eosio regfinkey \
  '["bpoperatoraa","<BLS_PUBLIC_KEY>","<BLS_PROOF_OF_POSSESSION>"]' \
  -p bpoperatoraa@active

"${CLEOS[@]}" push action eosio regpeerkey \
  '["bpoperatoraa","<PEER_PUBLIC_KEY>"]' \
  -p bpoperatoraa@active
```

The first `regfinkey` activates the first BLS key. Adding later keys does not automatically switch the active key; an existing BP's key rotation requires separate `actfinkey` and native-policy handoff checks.

Verify each transaction in the canonical chain and wait until its block is irreversible. Check the registration, active finalizer key and current peer key before obtaining positive direct votes. A BP with existing positive voting weight may self-vote as follows:

```bash
"${CLEOS[@]}" push action eosio voteproducer \
  '["bpoperatoraa","",["bpoperatoraa"]]' -p bpoperatoraa@active
```

Proxy must be empty and at most one producer may be selected. Other voters must authorize their own votes. Registration, keys and positive votes still remain subject to rank snapshots and rotation. Do not assume a fixed preparation grace period after receiving votes or manually force committee membership.

## 5. Monitor the registration and actual native duties

```bash
"${CLEOS[@]}" get table eosio eosio producersv2 --lower bpoperatoraa --limit 1
"${CLEOS[@]}" get table eosio eosio finalizers --lower bpoperatoraa --limit 1
"${CLEOS[@]}" get table eosio eosio peerkeys --lower bpoperatoraa --limit 1
"${CLEOS[@]}" get table eosio eosio rotcands --lower bpoperatoraa --limit 1
"${CLEOS[@]}" get schedule -j
"${CLEOS[@]}" get finalizer_info
"${CLEOS[@]}" get table eosio eosio rotation
```

A `--lower` query can return the next account: verify row ownership. Inspect registration, integer votes and block key in `producersv2`, active BLS key in `finalizers`, current peer key in `peerkeys`, and violation/ban records in `rotcands`. Follow pagination when reading multiple rows; page length is not a chain-wide total.

Use native `get_producer_schedule` and `get_finalizer_info` for actual duties, not the ranked producer list alone. After admission, verify block output, finalizer votes and an advancing LIB. The explorer overview/governance roster shows native members, latest producer and maintenance counts; the Producers page also shows chain-wide registered/eligible BP totals. These values and maintenance flags are not process-connectivity monitoring.

## 6. Planned shutdown of an existing BP

**Remain online → request maintenance → complete handoff and finalization → verify shutdown conditions → stop the process.**

```bash
"${CLEOS[@]}" push action eosio setmaint '["bpoperatoraa",true]' -p bpoperatoraa@active
"${CLEOS[@]}" get table eosio eosio bpmaint --lower bpoperatoraa --limit 1
"${CLEOS[@]}" get schedule -j
"${CLEOS[@]}" get finalizer_info
"${CLEOS[@]}" get table eosio eosio rotation
"${CLEOS[@]}" get info
```

Stop only after all conditions hold:

- Your maintenance state is **2**, not 1.
- `get_info.last_irreversible_block_num >= bpmaint.effective_block`, reading the same canonical chain.
- Your account has left native active/pending/proposed proposer lists, active/pending finalizer lists and `rotation.active` / `rotation.target`.

An incumbent may request maintenance, but its normal proposer/finalizer handoff must complete first. Without a qualified replacement, ordinary maintenance does not shrink the 21-member committee; continue online or cancel the request. A successful request transaction, an awaiting-maintenance badge or a local production pause does not authorize immediate shutdown.

Effective maintenance creates no new production rights or newly assigned-duty absence penalties. Historical unpaid blocks, valid violations and permanent bans remain. Stop only the service corresponding to your own node; no fixed service-stop command is supplied here.

## 7. Resume after maintenance

**Start the node and signers → synchronize while retaining maintenance → pass section 4 checks → cancel maintenance → await normal rotation.**

```bash
"${CLEOS[@]}" push action eosio setmaint '["bpoperatoraa",false]' -p bpoperatoraa@active
"${CLEOS[@]}" get table eosio eosio bpmaint --lower bpoperatoraa --limit 1
```

Verify irreversible cancellation and your state returning to 0, then stay online. Cancellation restores candidacy rather than forcing immediate committee admission. Do not cancel while the process is stopped, still synchronizing or unable to sign.

## 8. Permanent blacklist rules

On a healthy full committee, **three consecutive finalized personal production windows with zero blocks** permanently ban the BP account. This does not mean three ordinary blocks, arbitrary periods or failed online probes after registration. Unassigned standbys do not accrue absence penalties just for waiting; registration alone does not immediately cause a ban.

An interruption in production/finality observation of252slots (126seconds, inclusive), or a committee below21members, pauses new personal absence evidence. Earlier valid evidence and permanent bans remain. These global safeguards are not a substitute for planned personal maintenance. Re-registration, key changes, votes and maintenance cannot unban the same account. A banned BP may still synchronize as an ordinary full node.

## 9. Unconfirmed transactions and unexpected states

Keep the transaction ID and verify canonical inclusion and LIB, using history when available. RPC `executed` and a temporarily displayed height are not substitutes for finality. If a registration/maintenance transaction is unconfirmed, do not take its dependent next step. Check submission connectivity, resources, signatures and networking; if necessary, use a verified incumbent transaction endpoint after determining the original result.

Duplicate maintenance requests/cancellations are rejected. Permanently banned BPs cannot clear punishment through maintenance, and ordinary unregistered accounts cannot request BP maintenance. Failed transactions follow on-chain atomic rollback semantics; read actual state again afterward.

See [CPU16 / NET4 / SHIP roles](CPU16_NET4_SHIP_ROLES_20261031.md). This guide changes no voting, reward, punishment, finality-quorum or other chain rules.
