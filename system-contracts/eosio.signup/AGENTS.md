# eosio.signup candidate

- User requested a native developer-proposed, fully reviewed, effective-BP-vetoable system-account creation capability, then creation/deployment of eosio.signup without resetting the current TEST chain.
- This account must stay nonprivileged. Its final system owner/active sealing and code upgrades belong to reviewed native governance; do not introduce a management-key back door.
- Registration accepts public keys only: legacy EOS, PUB_K1 and PUB_R1. Private-key encodings never belong in an on-chain action/memo. This does not implement the separate PQ plan.
- Original signup resource allocation is the initial candidate: 0.1000 AIC NET,0.1000 AIC CPU with transferred ownership; remainder buys at least4KiB RAM. No service fee. Fixed RAM plus liquid surplus was asked as an optional preference but not selected; do not activate changed economics silently.
- Build/test remotely only, in new bounded directories on the retained host. Never stop/restart live BPs or ordinary/history services to free resources. Preserve failed attempts and distinguish compiler/codec checks from native chain qualification.
- Existing proposal3 and its24/24veto evidence stay unchanged. Never rerun old governance, payment or bootstrap controllers. Do not deploy until native creation capability and exact payload are qualified and its full applicable review succeeds.
- No private keys, demo fixture secrets, runtime chain databases or signing material enter source/Git/local evidence.
