// Included after production_governance_fixture: real deployed WASM, no native
// resource-manager changes or mocks. Time advances only in the controller tester.
#include <eosio/chain/global_property_object.hpp>
namespace {
std::string resource_json(const fc::variant& value) {
   return fc::json::to_string(value, fc::time_point::maximum());
}
struct resource_split_fixture : production_governance_fixture {
   // Reuse parsed ABI metadata, never table data. Reviewed upgrades change the
   // hash and invalidate this cache before the next decode.
   mutable std::map<name, std::pair<fc::sha256, std::unique_ptr<abi_serializer>>> abi_cache;
   explicit resource_split_fixture(bool configure = true, size_t node_count = 4,
                                   std::filesystem::path root = PUREEOS_RESOURCE_SPLIT_CONTRACTS_DIR,
                                   bool rotation = true, bool reward_headroom = false,
                                   std::string initial_supply = {})
      : production_governance_fixture(std::move(root), false, false, false, true, node_count, rotation,
                                      reward_headroom, std::move(initial_supply)) {
      const auto base = contract_dir / "eosio.system/.powerup/powup.results";
      chain.set_code("eosio.reserv"_n, read_wasm((base.string() + ".wasm").c_str()));
      std::string abi;
      fc::read_file_contents(base.string() + ".abi", abi);
      chain.set_abi("eosio.reserv"_n, abi);
      register_node(nodes[0]);
      call("buyram"_n, fc::mutable_variant_object()("payer", "alice")("receiver", "alice")("quant", "10.0000 SYS"));
      call("buyram"_n, fc::mutable_variant_object()("payer", "eosio")("receiver", "eosio.reserv")("quant", "10.0000 SYS"), "eosio"_n);
      call("buyram"_n, fc::mutable_variant_object()("payer", "eosio")("receiver", nodes[1])("quant", "10.0000 SYS"), "eosio"_n);
      if (configure) cfg();
   }
   transaction_trace_ptr call(name act, const fc::variant_object& args, name signer = "alice"_n) {
      return push({chain.get_action("eosio"_n, act, {{signer, "active"_n}}, args)}, signer);
   }
   void cfg(fc::variant_object net = {}, fc::variant_object cpu = {}) {
      auto config = [](const fc::variant_object& changes) {
         fc::mutable_variant_object r;
         r("current_weight_ratio", fc::variant())("target_weight_ratio", fc::variant())
          ("assumed_stake_weight", fc::variant())("target_timestamp", fc::variant())
          ("exponent", 2.0)("decay_secs", 86400)("min_price", "1.0000 SYS")("max_price", "100.0000 SYS");
         for (const auto& entry : changes) r[entry.key()] = entry.value();
         return r;
      };
      call("cfgpowerup"_n, fc::mutable_variant_object()("args", fc::mutable_variant_object()
         ("net", config(net))("cpu", config(cpu))("powerup_days", 1)("min_powerup_fee", "0.0001 SYS")), "eosio"_n);
   }
   fc::variant row(name table, uint64_t key, name scope = "eosio"_n, name code = "eosio"_n) const {
      const auto data = chain.get_row_by_account(code, scope, table, name{key});
      BOOST_REQUIRE(!data.empty());
      auto yield = abi_serializer::create_yield_function(fc::seconds(5));
      const auto& packed = chain.control->db().get<account_object, by_name>(code).abi;
      const auto hash = fc::sha256::hash(packed.data(), packed.size());
      auto& cached = abi_cache[code];
      if (!cached.second || cached.first != hash) {
         abi_def abi;
         BOOST_REQUIRE(abi_serializer::to_abi(packed, abi));
         cached.second = std::make_unique<abi_serializer>(abi, yield);
         cached.first = hash;
      }
      const auto& serializer = *cached.second;
      return serializer.binary_to_variant(serializer.get_table_type(table), data, yield);
   }
   fc::variant policy() const { return row("respolicy"_n, "respolicy"_n.to_uint64_t()); }
   fc::variant market() const { return row("powup.state"_n, "powup.state"_n.to_uint64_t(), name{}); }
   int64_t resource(name account, const std::string& kind) const {
      if (chain.get_row_by_account("eosio"_n, account, "userres"_n, account).empty()) return 0;
      return asset::from_string(row("userres"_n, account.to_uint64_t(), account)[(kind + "_weight").c_str()].as_string()).get_amount();
   }
   void stake(const std::string& net, const std::string& cpu, name from = "alice"_n,
              name receiver = "alice"_n, bool transfer = false) {
      call("delegatebw"_n, fc::mutable_variant_object()("from", from)("receiver", receiver)
         ("stake_net_quantity", net)("stake_cpu_quantity", cpu)("transfer", transfer), from);
   }
   void vote(name producer = {}) {
      call("voteproducer"_n, fc::mutable_variant_object()("voter", "alice")("proxy", "")
         ("producers", std::vector<name>{producer == name{} ? nodes[0] : producer}));
   }
   void power(int64_t frac = 100000000000000) {
      call("powerup"_n, fc::mutable_variant_object()("payer", "alice")("receiver", "alice")
         ("days", 1)("net_frac", frac)("cpu_frac", frac)("max_payment", "200.0000 SYS"));
   }
   void convert(const std::string& net, const std::string& cpu) {
      call("unstaketorex"_n, fc::mutable_variant_object()("owner", "alice")("receiver", "alice")
         ("from_net", net)("from_cpu", cpu));
   }
   void check_split(bool exact = true) {
      const auto p = policy(), m = market();
      BOOST_CHECK_EQUAL(p["version"].as_uint64(), 1u);
      BOOST_CHECK_EQUAL(m["version"].as_uint64(), 1u);
      chain.produce_blocks(2); // Commit pending native resource-limit updates.
      auto& limits = chain.control->get_resource_limits_manager();
      for (const std::string kind : {"net", "cpu"}) {
         const auto l = p[(kind + "_weight").c_str()].as_int64();
         const auto w = m[kind.c_str()]["weight"].as_int64();
         const auto u = m[kind.c_str()]["utilization"].as_int64();
         BOOST_CHECK_GE(w, 9 * l);
         BOOST_CHECK_GE(w, u);
         BOOST_CHECK_EQUAL(m[kind.c_str()]["assumed_stake_weight"].as_int64(), std::max(int64_t{1}, l));
         if (exact) BOOST_CHECK_EQUAL(w, 9 * std::max(int64_t{1}, l));
         BOOST_CHECK_EQUAL(resource("eosio.reserv"_n, kind), w - u);
         const auto total = kind == "net" ? limits.get_total_net_weight() : limits.get_total_cpu_weight();
         BOOST_CHECK_EQUAL(total, uint64_t(l + w));
      }
   }
   static bool message(const fc::exception& e, const std::string& text) {
      return e.to_detail_string().find(text) != std::string::npos;
   }
};

// Exercise real token transfers and REX liabilities, not just resource weights.
// The system treasury must never subsidize a renter's payment or renewal.
void check_rex_rental_fee_custody(bool forward_fees, bool empty_treasury,
                                 std::filesystem::path contracts = PUREEOS_RESOURCE_SPLIT_CONTRACTS_DIR,
                                 bool rotation = true) {
   resource_split_fixture f(true, 2, std::move(contracts), rotation);
   if (forward_fees) f.deploy("eosio.fees"_n, "eosio.fees");
   f.stake("100.0000 SYS", "100.0000 SYS");
   f.vote();
   f.chain.push_action("eosio.token"_n, "transfer"_n, "eosio"_n, fc::mutable_variant_object()
      ("from", "eosio")("to", "alice")("quantity", "60000.0000 SYS")("memo", "rental fee regression"));
   f.call("deposit"_n, fc::mutable_variant_object()("owner", "alice")("amount", "50010.0000 SYS"));
   f.call("buyrex"_n, fc::mutable_variant_object()("from", "alice")("amount", "50000.0000 SYS"));
   const auto amount = [](const fc::variant& v) { return asset::from_string(v.as_string()).get_amount(); };
   const auto balance = [&](name owner) {
      const auto code = symbol::from_string("4,SYS").to_symbol_code().value;
      if (f.chain.get_row_by_account("eosio.token"_n, owner, "accounts"_n, name{code}).empty()) return int64_t{0};
      return amount(f.row("accounts"_n, code, owner, "eosio.token"_n)["balance"]);
   };
   if (empty_treasury) {
      f.chain.push_action("eosio.token"_n, "transfer"_n, "eosio"_n, fc::mutable_variant_object()
         ("from", "eosio")("to", "alice")("quantity", asset(balance("eosio"_n), symbol(4, "SYS")).to_string())
         ("memo", "empty disposable test treasury"));
      BOOST_REQUIRE_EQUAL(balance("eosio"_n), 0);
   }
   // Keep the native genesis signer while testing 30-day accounting boundaries.
   // Sealing still verifies that eosio.code can debit the protected REX account.
   f.call("unregprod"_n, fc::mutable_variant_object()("producer", f.nodes[0]), f.nodes[0]);
   f.seal();
   const auto treasury = balance("eosio"_n), wallet = balance("alice"_n);
   const auto custody = balance("eosio.rex"_n), fees = balance("eosio.fees"_n);
   const auto pool_amount = [&] { return amount(f.row("rexpool"_n, 0)["total_lendable"]); };
   const auto return_amount = [&] {
      if (f.chain.get_row_by_account("eosio"_n, "eosio"_n, "rexretpool"_n, name{}).empty()) return int64_t{0};
      return f.row("rexretpool"_n, 0)["proceeds"].as_int64();
   };
   const auto fund = [&] { return amount(f.row("rexfund"_n, "alice"_n.to_uint64_t())["balance"]); };
   const auto loan_balance = [&](name table, uint64_t id) {
      if (f.chain.get_row_by_account("eosio"_n, "eosio"_n, table, name{id}).empty()) return int64_t{0};
      return amount(f.row(table, id)["balance"]);
   };
   const auto initial_pool = pool_amount() + return_amount();
   const auto check_accounts = [&](int64_t paid) {
      BOOST_CHECK_EQUAL(balance("eosio"_n), treasury);
      BOOST_CHECK_EQUAL(balance("alice"_n), wallet);
      BOOST_CHECK_EQUAL(balance("eosio.rex"_n), custody - (forward_fees ? 0 : paid));
      BOOST_CHECK_EQUAL(balance("eosio.fees"_n), fees + (forward_fees ? 0 : paid));
      BOOST_CHECK_EQUAL(pool_amount() + return_amount(), initial_pool + (forward_fees ? paid : 0));
      BOOST_CHECK_EQUAL(balance("eosio.rex"_n), pool_amount() + return_amount() + fund() +
         loan_balance("cpuloan"_n, 1) + loan_balance("netloan"_n, 2));
   };
   const auto rent = [&](name act, const char* payment, const char* reserve) {
      return f.call(act, fc::mutable_variant_object()("from", "alice")("receiver", f.nodes[1])
         ("loan_payment", payment)("loan_fund", reserve));
   };
   const auto check_fee_trace = [&](const transaction_trace_ptr& trace, int64_t paid, unsigned expected_transfers) {
      abi_def abi;
      BOOST_REQUIRE(abi_serializer::to_abi(f.chain.control->db().get<account_object, by_name>("eosio.token"_n).abi, abi));
      auto yield = abi_serializer::create_yield_function(fc::seconds(5));
      abi_serializer serializer(abi, yield);
      int64_t outgoing = 0, returned = 0;
      unsigned transfers = 0;
      for (const auto& a : trace->action_traces) {
         if (a.receiver != "eosio.token"_n || a.act.account != "eosio.token"_n || a.act.name != "transfer"_n) continue;
         ++transfers;
         const auto data = serializer.binary_to_variant("transfer", a.act.data, yield);
         const auto from = data["from"].as_string(), to = data["to"].as_string();
         if (from == "eosio.rex" && to == "eosio.fees") outgoing += amount(data["quantity"]);
         else if (from == "eosio.fees" && to == "eosio.rex") returned += amount(data["quantity"]);
         else BOOST_ERROR("unexpected rental token transfer: " + from + " -> " + to);
      }
      BOOST_CHECK_EQUAL(outgoing, paid);
      BOOST_CHECK_EQUAL(returned, forward_fees ? paid : 0);
      BOOST_CHECK_EQUAL(transfers, expected_transfers * (forward_fees ? 2 : 1));
   };
   auto trace = rent("rentcpu"_n, "1.0000 SYS", "1.5000 SYS");
   check_fee_trace(trace, 10000, 1);
   check_accounts(10000);
   BOOST_CHECK_EQUAL(fund(), 75000);
   trace = rent("rentnet"_n, "2.0000 SYS", "2.5000 SYS");
   check_fee_trace(trace, 20000, 1);
   check_accounts(30000);
   BOOST_CHECK_EQUAL(fund(), 30000);
   f.check_split();

   // An unfunded attempt cannot charge any account, allocate resources, or add a loan.
   const auto pool_before = resource_json(f.row("rexpool"_n, 0));
   const auto policy_before = resource_json(f.policy());
   BOOST_CHECK_EXCEPTION(rent("rentcpu"_n, "4.0000 SYS", "0.0000 SYS"), fc::exception,
      [](const auto& e) { return resource_split_fixture::message(e, "insufficient funds"); });
   BOOST_CHECK_EQUAL(resource_json(f.row("rexpool"_n, 0)), pool_before);
   BOOST_CHECK_EQUAL(resource_json(f.policy()), policy_before);
   BOOST_CHECK_EQUAL(fund(), 30000);
   check_accounts(30000);

   f.chain.produce_block(fc::days(31));
   trace = f.call("rexexec"_n, fc::mutable_variant_object()("user", "alice")("max", 10));
   check_fee_trace(trace, 30000, 2);
   check_accounts(60000);
   BOOST_CHECK_EQUAL(loan_balance("cpuloan"_n, 1), 5000);
   BOOST_CHECK_EQUAL(loan_balance("netloan"_n, 2), 5000);
   BOOST_CHECK_EQUAL(fund(), 30000); // renewals debit loan reserves, not the user's free fund again
   f.check_split();
   trace = f.call("rexexec"_n, fc::mutable_variant_object()("user", "alice")("max", 10));
   check_fee_trace(trace, 0, 0);
   check_accounts(60000);

   f.chain.produce_block(); // commit short-lived transactions before advancing time
   f.chain.produce_block(fc::days(31));
   trace = f.call("rexexec"_n, fc::mutable_variant_object()("user", "alice")("max", 10));
   check_fee_trace(trace, 0, 0);
   BOOST_CHECK(f.chain.get_row_by_account("eosio"_n, "eosio"_n, "cpuloan"_n, name{1}).empty());
   BOOST_CHECK(f.chain.get_row_by_account("eosio"_n, "eosio"_n, "netloan"_n, name{2}).empty());
   BOOST_CHECK_EQUAL(fund(), 40000); // unused 0.5 + 0.5 SYS returns exactly once
   BOOST_CHECK_EQUAL(f.resource(f.nodes[1], "cpu"), 0);
   BOOST_CHECK_EQUAL(f.resource(f.nodes[1], "net"), 0);
   check_accounts(60000);
   f.check_split();
}
}

BOOST_AUTO_TEST_SUITE(pureeos_resource_split_tests)

BOOST_AUTO_TEST_CASE(native_configuration_mirror_survives_sealing_and_restart) {
   resource_split_fixture f;
   const auto check_mirror = [&] {
      fc::variant native;
      fc::to_variant(f.chain.control->get_global_properties().configuration.v0(), native);
      const auto global = f.row("globalv2"_n, "globalv2"_n.to_uint64_t());
      for (const auto& field : native.get_object()) {
         BOOST_CHECK_EQUAL(global[field.key().c_str()].as_uint64(), field.value().as_uint64());
      }
   };
   check_mirror();
   f.seal();
   f.chain.produce_blocks(2);
   check_mirror();
   f.chain.close();
   f.chain.open();
   check_mirror();
}

BOOST_AUTO_TEST_CASE(bootstrap_tracks_weights_and_requires_powerup_before_seal) {
   resource_split_fixture f(false);
   f.stake("10.0000 SYS", "20.0000 SYS");
   BOOST_CHECK_EQUAL(f.policy()["net_weight"].as_int64(), 100000);
   BOOST_CHECK_EXCEPTION(f.seal(), fc::exception, [](const auto& e) {
      return resource_split_fixture::message(e, "configure PowerUp before governance sealing"); });
   BOOST_CHECK(f.state().developer == name{}); // native permission changes also roll back
   f.cfg();
   f.check_split();
   BOOST_REQUIRE_NO_THROW(f.seal());
   f.stake("1.0000 SYS", "2.0000 SYS"); // ordinary resources still work after sealing
   f.check_split();
}

BOOST_AUTO_TEST_CASE(fixed_ratio_and_manual_limits_cannot_bypass_policy) {
   resource_split_fixture f;
   const auto before = resource_json(f.market());
   BOOST_CHECK_EXCEPTION(f.cfg(fc::mutable_variant_object()("current_weight_ratio", 200000000000000ll)), fc::exception,
      [](const auto& e) { return resource_split_fixture::message(e, "fixed 10%"); });
   BOOST_CHECK_EXCEPTION(f.cfg({}, fc::mutable_variant_object()("assumed_stake_weight", 1000)), fc::exception,
      [](const auto& e) { return resource_split_fixture::message(e, "actual staking and REX weights"); });
   BOOST_CHECK_EQUAL(resource_json(f.market()), before);
   for (auto act : {"setacctcpu"_n, "setacctnet"_n}) {
      const auto field = act == "setacctcpu"_n ? "cpu_weight" : "net_weight";
      BOOST_CHECK_THROW(f.call(act, fc::mutable_variant_object()("account", "alice")(field, -1), "eosio"_n), fc::exception);
   }
   BOOST_CHECK_THROW(f.call("setalimits"_n, fc::mutable_variant_object()("account", "eosio.rex")
      ("ram", -1)("net", 100)("cpu", 100), "eosio"_n), fc::exception);
   BOOST_CHECK_THROW(f.stake("1.0000 SYS", "1.0000 SYS", "alice"_n, "eosio.reserv"_n), fc::exception);
   f.check_split();
}

BOOST_AUTO_TEST_CASE(staking_transfer_and_powerup_do_not_multiply_votes) {
   resource_split_fixture f;
   f.stake("100.0000 SYS", "200.0000 SYS");
   f.vote();
   const auto votes = f.row("votersv2"_n, "alice"_n.to_uint64_t())["last_vote_amount"].as_int64();
   f.stake("10.0000 SYS", "20.0000 SYS", "eosio"_n, f.nodes[1], true);
   f.check_split();
   const auto policy = resource_json(f.policy());
   f.power();
   BOOST_CHECK_EQUAL(resource_json(f.policy()), policy);
   BOOST_CHECK_EQUAL(f.row("votersv2"_n, "alice"_n.to_uint64_t())["last_vote_amount"].as_int64(), votes);
   BOOST_CHECK_GT(asset::from_string(f.row("accounts"_n, symbol::from_string("4,SYS").to_symbol_code().value,
      "eosio.fees"_n, "eosio.token"_n)["balance"].as_string()).get_amount(), 0);
   f.check_split();
}

BOOST_AUTO_TEST_CASE(shrink_preserves_paid_orders_then_expiry_restores_ratio) {
   resource_split_fixture f;
   f.stake("100.0000 SYS", "100.0000 SYS");
   f.vote();
   f.power(900000000000000);
   const auto order = resource_json(f.row("powup.order"_n, 0, name{}));
   f.convert("95.0000 SYS", "95.0000 SYS");
   BOOST_CHECK_EQUAL(f.policy()["net_weight"].as_int64(), 50000);
   BOOST_CHECK_EQUAL(resource_json(f.row("powup.order"_n, 0, name{})), order);
   BOOST_CHECK_EQUAL(f.market()["net"]["weight"].as_int64(), 8100000);
   BOOST_CHECK_EQUAL(f.resource("alice"_n, "net"), 8150000);
   f.check_split(false);
   f.chain.produce_block(fc::days(2));
   f.call("powerupexec"_n, fc::mutable_variant_object()("user", "alice")("max", 1));
   BOOST_CHECK_EQUAL(f.market()["net"]["utilization"].as_int64(), 0);
   BOOST_CHECK_EQUAL(f.resource("alice"_n, "net"), 50000);
   f.check_split();
}

BOOST_AUTO_TEST_CASE(bounded_expiry_releases_each_order_once_after_shrink) {
   resource_split_fixture f;
   f.stake("100.0000 SYS", "100.0000 SYS");
   f.vote();
   f.power(400000000000000);
   f.power(400000000000000);
   f.convert("95.0000 SYS", "95.0000 SYS");
   f.chain.produce_block(); // Commit short-lived transactions before skipping time.
   f.chain.produce_block(fc::days(2));
   f.call("powerupexec"_n, fc::mutable_variant_object()("user", "alice")("max", 1));
   BOOST_CHECK_EQUAL(f.market()["net"]["utilization"].as_int64(), 3600000);
   f.check_split(false);
   f.call("powerupexec"_n, fc::mutable_variant_object()("user", "alice")("max", 1));
   BOOST_CHECK_EQUAL(f.market()["net"]["utilization"].as_int64(), 0);
   f.check_split();
   f.call("powerupexec"_n, fc::mutable_variant_object()("user", "alice")("max", 100));
   BOOST_CHECK_EQUAL(f.resource("alice"_n, "net"), 50000);
   f.check_split();
}

BOOST_AUTO_TEST_CASE(rex_rent_renew_and_expiry_track_only_outstanding_resources) {
   resource_split_fixture f;
   f.stake("100.0000 SYS", "100.0000 SYS");
   f.vote();
   f.convert("90.0000 SYS", "90.0000 SYS");
   f.chain.push_action("eosio.token"_n, "transfer"_n, "eosio"_n,
      fc::mutable_variant_object()("from", "eosio")("to", "alice")("quantity", "50000.0000 SYS")("memo", "REX liquidity"));
   f.call("deposit"_n, fc::mutable_variant_object()("owner", "alice")("amount", "50010.0000 SYS"));
   f.call("buyrex"_n, fc::mutable_variant_object()("from", "alice")("amount", "50000.0000 SYS"));
   for (auto act : {"rentcpu"_n, "rentnet"_n})
      f.call(act, fc::mutable_variant_object()("from", "alice")("receiver", f.nodes[1])
         ("loan_payment", "1.0000 SYS")("loan_fund", "1.0000 SYS"));
   const auto voter = f.row("votersv2"_n, "alice"_n.to_uint64_t())["last_vote_amount"].as_int64();
   f.check_split();
   f.chain.produce_block(fc::days(31));
   f.call("rexexec"_n, fc::mutable_variant_object()("user", "alice")("max", 10));
   BOOST_CHECK_EQUAL(f.policy()["cpu_weight"].as_int64(), 100000 + f.resource(f.nodes[1], "cpu"));
   BOOST_CHECK_EQUAL(f.policy()["net_weight"].as_int64(), 100000 + f.resource(f.nodes[1], "net"));
   BOOST_CHECK_GT(f.resource(f.nodes[1], "cpu"), 0);
   f.check_split();
   f.chain.produce_block(fc::days(31));
   f.call("rexexec"_n, fc::mutable_variant_object()("user", "alice")("max", 10));
   BOOST_CHECK_EQUAL(f.policy()["cpu_weight"].as_int64(), 100000);
   BOOST_CHECK_EQUAL(f.policy()["net_weight"].as_int64(), 100000);
   BOOST_CHECK_EQUAL(f.row("votersv2"_n, "alice"_n.to_uint64_t())["last_vote_amount"].as_int64(), voter);
   f.check_split();
}

BOOST_AUTO_TEST_CASE(rex_rental_fee_custody_with_forwarder) {
   check_rex_rental_fee_custody(true, false);
}

BOOST_AUTO_TEST_CASE(rex_rental_fee_custody_empty_treasury) {
   check_rex_rental_fee_custody(true, true);
}

BOOST_AUTO_TEST_CASE(rex_rental_fee_custody_without_forwarder) {
   check_rex_rental_fee_custody(false, true);
}

#ifdef PUREEOS_ROTATION_CONTRACTS_DIR
BOOST_AUTO_TEST_CASE(rex_rental_fee_custody_rotation_profile) {
   check_rex_rental_fee_custody(true, true, PUREEOS_ROTATION_CONTRACTS_DIR, true);
}
#endif

BOOST_AUTO_TEST_CASE(deployed_fees_contract_routes_ram_and_powerup_income_after_sealing) {
   resource_split_fixture f;
   f.deploy("eosio.fees"_n, "eosio.fees");
   f.stake("100.0000 SYS", "100.0000 SYS");
   f.vote();
   f.seal();
   const auto balance = [&](name n) {
      const auto code = symbol::from_string("4,SYS").to_symbol_code().value;
      if (f.chain.get_row_by_account("eosio.token"_n, n, "accounts"_n, name{code}).empty()) return int64_t{0};
      return asset::from_string(f.row("accounts"_n, code, n, "eosio.token"_n)["balance"].as_string()).get_amount();
   };
   const auto initial_fees = balance("eosio.fees"_n);
   f.chain.push_action("eosio.token"_n, "transfer"_n, "alice"_n, fc::mutable_variant_object()
      ("from", "alice")("to", "eosio.fees")("quantity", "1.0000 SYS")("memo", "before REX"));
   BOOST_CHECK_EQUAL(balance("eosio.fees"_n), initial_fees + 10000);
   f.call("deposit"_n, fc::mutable_variant_object()("owner", "alice")("amount", "100.0000 SYS"));
   f.call("buyrex"_n, fc::mutable_variant_object()("from", "alice")("amount", "100.0000 SYS"));
   const auto held_fees = balance("eosio.fees"_n);
   auto rex_before = balance("eosio.rex"_n);
   const auto payer_before = balance("alice"_n);
   f.power();
   const auto powerup_fee = payer_before - balance("alice"_n);
   BOOST_REQUIRE_GT(powerup_fee, 0);
   BOOST_CHECK_EQUAL(balance("eosio.rex"_n) - rex_before, powerup_fee);
   BOOST_CHECK_EQUAL(balance("eosio.fees"_n), held_fees);
   rex_before = balance("eosio.rex"_n);
   f.call("buyram"_n, fc::mutable_variant_object()("payer", "alice")("receiver", "alice")("quant", "10.0000 SYS"));
   BOOST_CHECK_EQUAL(balance("eosio.rex"_n) - rex_before, 500); // existing RAM fee = 0.5%
   BOOST_CHECK_EQUAL(balance("eosio.fees"_n), held_fees);
   // Pre-REX accumulated fees are not silently swept by a later notification.
   BOOST_CHECK_EQUAL(held_fees, initial_fees + 10000);
   f.check_split();
}

BOOST_AUTO_TEST_CASE(undelegation_refund_and_zero_longterm_floor) {
   resource_split_fixture f;
   f.register_node("eosio"_n);
   f.stake("80000000.0000 SYS", "80000000.0000 SYS", "eosio"_n, "alice"_n, true);
   f.vote("eosio"_n); // activate through a real positive vote, not a mocked global-state flag
   f.call("undelegatebw"_n, fc::mutable_variant_object()("from", "alice")("receiver", "alice")
      ("unstake_net_quantity", "80000000.0000 SYS")("unstake_cpu_quantity", "80000000.0000 SYS"));
   BOOST_CHECK_EQUAL(f.policy()["net_weight"].as_int64(), 0);
   f.check_split();
   f.chain.produce_block(fc::days(4));
   // A fully unstaked user has no bandwidth: sponsor real PowerUp, do not bypass limits.
   f.call("powerup"_n, fc::mutable_variant_object()("payer", "eosio")("receiver", "alice")
      ("days", 1)("net_frac", 1000000000000000ll)("cpu_frac", 1000000000000000ll)("max_payment", "200.0000 SYS"), "eosio"_n);
   f.call("refund"_n, fc::mutable_variant_object()("owner", "alice"));
   f.check_split();
}

BOOST_AUTO_TEST_CASE(failed_payment_rolls_back_resources_orders_and_counters) {
   resource_split_fixture f;
   f.stake("10.0000 SYS", "20.0000 SYS");
   const auto before = resource_json(f.market());
   const auto policy = resource_json(f.policy());
   BOOST_CHECK_THROW(f.call("powerup"_n, fc::mutable_variant_object()("payer", "alice")("receiver", "alice")
      ("days", 1)("net_frac", 100000000000000ll)("cpu_frac", 100000000000000ll)("max_payment", "0.0000 SYS")), fc::exception);
   BOOST_CHECK_THROW(f.stake("5000.0000 SYS", "5000.0000 SYS"), fc::exception);
   BOOST_CHECK_EXCEPTION(f.stake("100000000000000.0000 SYS", "0.0000 SYS"), fc::exception,
      [](const auto& e) { return resource_split_fixture::message(e, "long-term resource weight out of range"); });
   BOOST_CHECK_EQUAL(resource_json(f.market()), before);
   BOOST_CHECK_EQUAL(resource_json(f.policy()), policy);
   f.check_split();
}
BOOST_AUTO_TEST_SUITE_END()
