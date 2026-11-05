# Native authority description correction

The original reviewmanifest and all listed artifacts remain byte-identical. No native authority_version, authority or source was changed. The human description “own-code active only” was inaccurate. Published core42dda604 and fixed policies1/2 produce ownerthreshold1 with no keys/accounts/waits, and activethreshold1 with no keys/waits and accounts eosio@eosio.code OR the service's own eosio.code. These are code authorities; no dev/BP/external signing key is granted. Ordinary issuer policy3 remains owner→eosio.dev@owner and active→eosio.dev@active OR own code.

The reviewed source creation_active/sysfinish defines this schema: finalization seals owner while retaining service code active. KEY transaction552759846b93ef9637ea74be7d52adaffeb8e145be65667c2d02b1f213da097b is irreversible at15463. Actual code/init/authorities are reconciled separately. The original verification waiter timed out before inclusion; KEY must not be resent.

Do not interpret this as an own-code-only deployment guarantee. eosio itself has sealed owner and code-only active, with no dev/BP private-key authority. Future approved system code still requires exact review and BP veto; arbitrary future code is not mathematically guaranteed harmless. No system-authority rotation feature or actual authority mutation was introduced by this correction.
