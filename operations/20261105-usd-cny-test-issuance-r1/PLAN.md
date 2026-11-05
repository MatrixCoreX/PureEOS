# USD/CNY test allocation

Latest user instruction supersedes the earlier 100-million cap: set BOTH caps to **1,000,000,000.0000**. Additionally issue **100,000,000.0000** of EACH token for transfer testing, retaining the existing 10,000.0000. Expected issued supply: **100,010,000.0000** each.

Use the existing issuer active delegation through eosio.dev/civicgovlead. Issue to the issuer as required by the deployed contract and transfer the full new allocation to the existing owned test funder `gptgenesisbp`. Six root actions in one atomic native transaction. No governance proposal, contract/authority change, AIC issuance/cap change, testnet reset, node interruption, new server, benchmark or replay of earlier payments.

Sign once in the existing remote wallet. Retain the immutable signed packet remotely only. Verify canonical irreversible execution, both cap/supply changes, exact recipient balance increments, unchanged issuer balances, code/ABI/authorities, original node PIDs and AIC state. Close only the temporary founder signer afterward. Any ambiguous submission must be reconciled using its original ID before any retry.
