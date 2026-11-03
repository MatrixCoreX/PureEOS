# eosio.key v1 development

- User authorized implementation on 2026-11-03, after design-only discussion. Name: eosio.key. Keep all work in this independent directory/branch.
- Multi-token public-key ledger; ordinary native accounts deposit to this contract with the beneficiary public key in memo. No recipient signature/native account prerequisite. Full principal is credited; fees are additional.
- Use on-chain PowerUp CPU/NET market occupancy for v1 dynamic fees, as explicitly selected by the user. Do not describe it as actual block CPU/NET usage or host CPU load. No nodeos change or resource-cap change.
- Only qualified registered/bonded BP accounts may operate commercial relays. Check permanent blacklist on every execution; changing relay keys does not evade parent BP revocation. Keep ordinary deposits and owner-authorized self-withdrawal independent of relay membership.
- Contract independently verifies K1 intent signatures, domain/chain/deployment/op, nonce, expiry, selected BP, reward identity and total fee cap. Thirty percent markup is relay service revenue; no guaranteed net profit.
- Preserve released eosio.system bytes/modes, BP bonds/slashing, voting, rotation/finality, rewards, governance, RAM1024 and disabled deferred execution. eosio.key must be created/deployed before governance sealing on a new isolated test chain; never bypass the sealed eosio.* account restriction.
- Do not deploy to or reset the public low-cost chain. Do not restart its ten stopped BPs. Do not rent a server or run TPS. Remote build/test on the existing cheap server must use separate directories, low priority and bounded compiler resources; test nodes are temporary and isolated from the live network.
- Keep private keys/wallets/configuration out of Git and tool output. Tests may use clearly marked alternate system-table fixtures; never ship a fixture build as the production WASM or describe those cases as native consensus qualification.
