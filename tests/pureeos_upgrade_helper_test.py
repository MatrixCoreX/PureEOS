#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("upgrade", Path(__file__).resolve().parents[1] / "scripts/pureeos_upgrade.py")
upgrade = importlib.util.module_from_spec(spec)
spec.loader.exec_module(upgrade)


class UpgradeHelperTests(unittest.TestCase):
    def setUp(self):
        self.code = b"\0asm\x01\0\0\0"
        self.abi = b"packed abi fixture"
        self.state = {"sealed": True, "profile_enabled": True, "developer": "developer", "pending": False,
                      "protected_accounts": ["eosio", "eosio.token", "eosio.msig"]}
        self.base = {"account_name": "eosio", "code_hash": "1" * 64, "abi_hash": "2" * 64}

    def proposal(self):
        return upgrade.prepare(self.state, "eosio", self.code, self.abi, b"review", self.base)

    def test_order_and_permission_and_packed_hash(self):
        actions = self.proposal()["actions"]
        self.assertEqual([a["name"] for a in actions], ["refreshgov", "devpropose"])
        self.assertEqual(actions[1]["authorization"][0]["permission"], "upgrade")
        self.assertEqual(actions[1]["data"]["new_abi_hash"], upgrade.sha256(self.abi))

    def test_execution_and_hash_mismatch(self):
        self.state.update(self.proposal()["actions"][1]["data"])
        self.state.update(pending=True, proposal_id=5, proposal_voters=["a", "b", "c"], vetoes=["a"])
        result = upgrade.prepare(self.state, "eosio", self.code, self.abi, None, None, True)
        self.assertEqual(result["actions"][0]["data"]["proposal_id"], 5)
        with self.assertRaisesRegex(ValueError, "artifacts differ"):
            upgrade.prepare(self.state, "eosio", self.code + b"x", self.abi, None, None, True)
        self.state["vetoes"].append("b")
        with self.assertRaisesRegex(ValueError, "veto threshold"):
            upgrade.prepare(self.state, "eosio", self.code, self.abi, None, None, True)

    def test_reject_unsealed_disabled_and_wrong_target(self):
        self.state["sealed"] = False
        with self.assertRaises(ValueError): self.proposal()
        self.state["sealed"] = True
        with self.assertRaises(ValueError):
            upgrade.prepare(self.state, "eosio.msig", self.code, self.abi, b"review", self.base)
        self.base["account_name"] = "eosio.token"
        with self.assertRaises(ValueError): self.proposal()


if __name__ == "__main__":
    unittest.main()
