"""Prepare an isolated TEST-only candidate; never signs, proposes or deploys."""
from pathlib import Path
import datetime
import difflib
import hashlib
import json
import shutil

D = Path(__file__).resolve().parent
R = D.parents[1]
BASE = R / "PureEOS-deployed-system-20261102/contracts/eosio.token"
OUT = D / "candidate"
assert not OUT.exists(), "Do not overwrite an existing review candidate"
shutil.copytree(BASE, OUT)
header = OUT / "include/eosio.token/eosio.token.hpp"
source = OUT / "src/eosio.token.cpp"
original_header = header.read_text()
original_source = source.read_text()

declaration = r'''
#ifdef PUREEOS_TEST_TOPUP_20261119
         /** TEST ONLY: fixed, single-use 150000.0000 AIC bank allocation.
          * Requires BOTH developer active and bank authorization. No parameters.
          * This exception must not be shipped in a production token contract.
          */
         [[eosio::action]]
         void testtopup();

         struct [[eosio::table("testmint")]] test_mint_receipt {
            uint64_t id;
            name recipient;
            asset quantity;
            asset supply_before;
            asset supply_after;
            uint64_t primary_key() const { return id; }
         };
         using test_mint_receipts = eosio::multi_index<"testmint"_n, test_mint_receipt>;
#endif
'''
assert original_header.count("      private:\n") == 1
header.write_text(original_header.replace("      private:\n", declaration + "\n      private:\n"))

implementation = r'''
#ifdef PUREEOS_TEST_TOPUP_20261119
void token::testtopup()
{
    check(get_self() == "eosio.token"_n, "TEST topup requires eosio.token");
    require_auth(permission_level{"eosio.dev"_n, "active"_n});
    require_auth("civicbankers"_n);
    check(is_account("civicbankers"_n), "TEST bank does not exist");

    const symbol aic{"AIC", 4};
    const asset quantity{1500000000LL, aic};
    test_mint_receipts receipts(get_self(), get_self().value);
    check(receipts.find(20261119) == receipts.end(), "TEST topup already executed");

    stats statstable(get_self(), aic.code().raw());
    const auto& st = statstable.get(aic.code().raw(), "AIC does not exist");
    check(st.issuer == "eosio"_n, "unexpected AIC issuer");
    check(st.supply.symbol == aic, "unexpected AIC precision");
    check(st.max_supply == asset{21000000000000LL, aic}, "unexpected AIC cap");
    check(st.supply == asset{1000000000000LL, aic}, "unexpected TEST supply");
    check(quantity.amount <= st.max_supply.amount - st.supply.amount,
          "TEST topup exceeds available supply");

    // The receipt is permanent and has no reset/erase action. All writes are
    // atomic, including any failure to bill the bank for the new receipt row.
    receipts.emplace("civicbankers"_n, [&](auto& receipt) {
        receipt.id = 20261119;
        receipt.recipient = "civicbankers"_n;
        receipt.quantity = quantity;
        receipt.supply_before = st.supply;
        receipt.supply_after = st.supply + quantity;
    });
    statstable.modify(st, same_payer, [&](auto& s) { s.supply += quantity; });
    add_balance("civicbankers"_n, quantity, "civicbankers"_n);
}
#endif
'''
assert original_source.count("} /// namespace eosio") == 1
source.write_text(original_source.replace("} /// namespace eosio", implementation + "\n} /// namespace eosio"))

changes = []
for p, old in [(header, original_header), (source, original_source)]:
    rel = str(p.relative_to(OUT))
    changes.extend(difflib.unified_diff(old.splitlines(True), p.read_text().splitlines(True), "a/" + rel, "b/" + rel, n=0))
(D / "TEST_ONLY_TOPUP.patch").write_text("".join(changes))
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
manifest = {
    "status": "UNSIGNED_REVIEW_CANDIDATE_ONLY",
    "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "chain_id": "09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532",
    "target_contract": "eosio.token",
    "recipient": "civicbankers",
    "quantity": "150000.0000 AIC",
    "expected_supply_before": "100000000.0000 AIC",
    "expected_supply_after": "100150000.0000 AIC",
    "unchanged_max_supply": "2100000000.0000 AIC",
    "required_authorizations": ["eosio.dev@active", "civicbankers@active"],
    "one_shot_receipt": {"table": "testmint", "scope": "eosio.token", "primary": 20261119},
    "production_build_excluded_without_macro": "PUREEOS_TEST_TOPUP_20261119",
    "baseline_source": {str(p.relative_to(BASE)): sha(p) for p in [BASE / "src/eosio.token.cpp", BASE / "include/eosio.token/eosio.token.hpp"]},
    "candidate_source": {str(p.relative_to(OUT)): sha(p) for p in [source, header]},
    "baseline_source_build_equivalence_to_live_WASM": "NOT_YET_PROVEN",
    "signatures": 0,
    "onchain_mutations": 0,
    "deployment_plan": [
        "Qualify actual WASM auth, one-shot, cap/issuer/precision, rollback and legacy behavior in an isolated fixture",
        "User must explicitly authorize temporary token contract upgrade after reviewing the concrete impact",
        "Publish exact candidate and full review commitment using the native developer governance lane",
        "Retain the full existing 1800-second review and effective BP veto; no accelerated review",
        "Execute only the exact reviewed code and ABI, then the fixed mint once, reconciling original transaction IDs",
        "Propose exact original token WASM/raw ABI restoration and retain its separate full review/veto",
        "Restore exact original contract code and ABI; keep the permanent one-shot receipt and minted allocation",
        "Verify supply/bank delta, original contract pins and automatic replacement budget/health gates"
    ],
    "material_impact": "Temporary token code/ABI hash change may hold wallet/relay/automatic-replacement trust checks until exact original code and ABI are restored. Two separate reviews require at least 60 minutes. No pin bypass is planned."
}
(D / "ONE_SHOT_TOPUP_PLAN.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
print(json.dumps({"status": manifest["status"], "patch": str(D / "TEST_ONLY_TOPUP.patch"), "onchain_mutations": 0}))
