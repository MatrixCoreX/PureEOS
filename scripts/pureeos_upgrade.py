#!/usr/bin/env python3
"""Prepare unsigned PureEOS upgrade actions. Never signs or broadcasts."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def pack_abi(cleos, path):
    # Built-in abi_def packing is offline; a JSON file's textual hash is NOT its on-chain ABI hash.
    result = subprocess.run(
        [str(cleos), "convert", "pack_hex", str(path), "--type", "abi_def"],
        check=True, capture_output=True, text=True,
    )
    return bytes.fromhex(json.loads(result.stdout))


def action(name, actor, permission, data):
    return {"account": "eosio", "name": name,
            "authorization": [{"actor": actor, "permission": permission}], "data": data}


def prepare(state, target, code, abi, review, base, execute=False):
    if not state.get("sealed") or not state.get("profile_enabled"):
        raise ValueError("chain is not sealed profile-2 governance")
    developer = state["developer"]
    if target not in state["protected_accounts"] or target in {"eosio.prods", "eosio.msig", "eosio.wrap", "eosio.null"}:
        raise ValueError("target is not an upgradeable protected account")
    if not code.startswith(b"\0asm"):
        raise ValueError("not a WASM binary")
    if execute:
        if not state["pending"] or state["target"] != target:
            raise ValueError("pending proposal target does not match")
        if sha256(code) != state["new_code_hash"] or sha256(abi) != state["new_abi_hash"]:
            raise ValueError("artifacts differ from the published hashes")
        if len(state["vetoes"]) >= (2 * len(state["proposal_voters"]) + 2) // 3:
            raise ValueError("proposal has reached its veto threshold")
        # Deadline and base hashes are checked objectively again by native devexec.
        return {"actions": [action("devexec", developer, "upgrade", {
            "proposal_id": state["proposal_id"], "code": code.hex(), "abi": abi.hex()})]}
    if state["pending"]:
        raise ValueError("cancel or execute the pending proposal first")
    if not review or not base:
        raise ValueError("proposal requires a non-empty review document and current target hashes")
    if base.get("account_name") != target:
        raise ValueError("get_raw_abi account_name differs from target")
    data = {"target": target, "old_code_hash": base["code_hash"], "old_abi_hash": base["abi_hash"],
            "new_code_hash": sha256(code), "new_abi_hash": sha256(abi), "review_document_hash": sha256(review)}
    return {"actions": [action("refreshgov", developer, "upgrade", {}), action("devpropose", developer, "upgrade", data)]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state", type=Path, required=True, help="saved /v1/chain/get_upgrade_state JSON")
    parser.add_argument("--target", required=True)
    parser.add_argument("--wasm", type=Path, required=True)
    parser.add_argument("--abi", type=Path, required=True, help="JSON ABI; packed offline using cleos")
    parser.add_argument("--cleos", type=Path, required=True)
    parser.add_argument("--base", type=Path, help="saved /v1/chain/get_raw_abi JSON for target")
    parser.add_argument("--review", type=Path, help="public review manifest; hash binds these exact bytes")
    parser.add_argument("--execute", action="store_true")
    args = parser.parse_args()
    try:
        result = prepare(json.loads(args.state.read_text()), args.target, args.wasm.read_bytes(),
                         pack_abi(args.cleos, args.abi), args.review.read_bytes() if args.review else None,
                         json.loads(args.base.read_text()) if args.base else None, args.execute)
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
