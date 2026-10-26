#pragma once

// Opt-in export of a real-system-contract stress chain. Never measures TPS in
// the tester; the signed transactions are later submitted to isolated nodeos.
BOOST_AUTO_TEST_SUITE(parallel_mixed_stress_fixtures)
using namespace parallel_production_tests;

BOOST_AUTO_TEST_CASE(export_system_application_mixed_fixture) {
   if (std::getenv("PUREEOS_CAPACITY_METADATA_ARRAY_PRECHECK")) {
      fc::variants names;names.reserve(2097166);
      for (unsigned i=0;i<2097166;++i) names.emplace_back(std::to_string(i));
      const auto decoded=fc::json::from_string(fc::json::to_string(names,fc::time_point::maximum())).get_array();
      BOOST_REQUIRE_EQUAL(decoded.size(),names.size());
      for (size_t i=0;i<names.size();++i) BOOST_REQUIRE_EQUAL(decoded[i].as_string(),names[i].as_string());
      return;
   }
   if (const char* measure=std::getenv("PUREEOS_CAPACITY_PACK_BLOCKS_DIR")) {
      const std::filesystem::path dir(measure);
      std::ifstream input(dir/"canonical-blocks.jsonl");BOOST_REQUIRE(input.good());
      std::string line;fc::variants sizes;
      while (std::getline(input,line)) {
         if (line.empty()) continue;
         const auto b=fc::json::from_string(line).as<signed_block>();
         const uint64_t packed=fc::raw::pack_size(b);
         sizes.emplace_back(fc::mutable_variant_object()("block_num",b.block_num())("block_id",b.calculate_id())
            ("transactions",b.transactions.size())("signed_block_bytes",packed)("P2P_payload_bytes",packed+1)("P2P_frame_bytes",packed+5));
      }
      BOOST_REQUIRE(fc::json::save_to_file(sizes,dir/"packed-block-sizes.json"));return;
   }
   // Refresh only test signatures/expiration against the SAME immutable bootstrap.
   if (const char* reuse = std::getenv("PUREEOS_CAPACITY_RESIGN_DIR")) {
      const std::filesystem::path dir(reuse);
      auto entries = fc::json::from_file(dir / "transactions.json").get_array();
      const auto manifest = fc::json::from_file(dir / "manifest.json");
      const auto chain = manifest["chain_id"].as<chain_id_type>();
      std::set<transaction_id_type> refreshed;
      for (auto& entry : entries) {
         auto trx = entry["packed"].as<packed_transaction>().get_signed_transaction();
         // Smallest real transfer payload: from,to,asset,empty memo.
         BOOST_REQUIRE_EQUAL(trx.actions.size(),1u);
         auto& action=trx.actions.front();
         BOOST_REQUIRE(action.account=="eosio.token"_n && action.name=="transfer"_n);
         BOOST_REQUIRE(action.data.size()>=33u);
         BOOST_REQUIRE_EQUAL(action.data.size(),33u+static_cast<unsigned char>(action.data[32]));
         action.data.resize(33);action.data[32]=0;
         trx.expiration = fc::time_point_sec(fc::time_point::now() + fc::seconds(3300));
         trx.signatures.clear();
         trx.sign(base_tester::get_private_key(entry["from"].as<name>(), "active"), chain);
         packed_transaction packed(trx, packed_transaction::compression_type::none);
         BOOST_REQUIRE(refreshed.insert(packed.id()).second);
         auto changed = fc::mutable_variant_object(entry.get_object());
         changed("id", packed.id())("packed", packed); entry = std::move(changed);
      }
      BOOST_REQUIRE(fc::json::save_to_file(entries, dir / "transactions.refreshed.json"));
      std::filesystem::rename(dir / "transactions.refreshed.json", dir / "transactions.json");
      const auto [bls_private,bls_public,bls_proof]=get_bls_key("eosio"_n);
      const std::vector<std::string> keys{
         base_tester::get_public_key("eosio"_n,"active").to_string({})+"=KEY:"+base_tester::get_private_key("eosio"_n,"active").to_string({}),
         bls_public.to_string()+"=KEY:"+bls_private.to_string()};
      BOOST_REQUIRE(fc::json::save_to_file(keys,dir/"TEST-ONLY-keys.json"));
      if (const char* net_text = std::getenv("PUREEOS_CAPACITY_CONTROL_NET")) {
         auto params = fc::json::from_file(dir/"genesis.json").as<genesis_state>().initial_configuration;
         params.max_block_net_usage=std::stoul(net_text);
         if (const char* cpu_text=std::getenv("PUREEOS_CAPACITY_CONTROL_CPU")) params.max_block_cpu_usage=std::stoul(cpu_text);
         params.validate();
         signed_transaction control;
         control.expiration=fc::time_point_sec(fc::time_point::now()+fc::seconds(3300));
         control.set_reference_block(block_id_type(manifest["checkpoints"].get_array().back()["block_id"].as_string()));
         control.actions.emplace_back(std::vector<permission_level>{{"eosio"_n,"active"_n}},"eosio"_n,"setparams"_n,fc::raw::pack(params));
         if (const char* ram_text=std::getenv("PUREEOS_CAPACITY_CONTROL_RAMRATE")) {
            const auto ram_rate=std::stoul(ram_text);BOOST_REQUIRE_EQUAL(ram_rate,1024u);
            control.actions.emplace_back(std::vector<permission_level>{{"eosio"_n,"active"_n}},"eosio"_n,"setramrate"_n,fc::raw::pack(uint16_t(ram_rate)));
            signed_transaction probe=control;probe.signatures.clear();probe.actions.erase(probe.actions.begin());BOOST_REQUIRE_EQUAL(probe.actions.size(),1u);
            probe.sign(base_tester::get_private_key("eosio"_n,"active"),chain);
            BOOST_REQUIRE(fc::json::save_to_file(packed_transaction(probe,packed_transaction::compression_type::none),dir/"control-ramcheck.json"));
         }
         control.sign(base_tester::get_private_key("eosio"_n,"active"),chain);
         BOOST_REQUIRE(fc::json::save_to_file(packed_transaction(control,packed_transaction::compression_type::none),dir/"control-setparams.json"));
      }
      return;
   }
   const char* destination = std::getenv("PUREEOS_PARALLEL_STRESS_EXPORT_DIR");
   if (!destination) { BOOST_TEST_MESSAGE("Mixed stress export not requested"); return; }
   const auto setting = [](const char* key, unsigned fallback, unsigned maximum) {
      const char* text = std::getenv(key);
      if (!text) return fallback;
      char* end = nullptr;
      const auto result = std::strtoul(text, &end, 10);
      BOOST_REQUIRE(end != text && *end == '\0' && result <= maximum);
      return unsigned(result);
   };
   const unsigned count = setting("PUREEOS_PARALLEL_STRESS_COUNT", 1024, 1048576);
   const unsigned work = setting("PUREEOS_PARALLEL_STRESS_WORK", 62500, 1000000);
   const unsigned roots = setting("PUREEOS_PARALLEL_STRESS_ROOTS", 16, 100);
   const unsigned hot = setting("PUREEOS_PARALLEL_STRESS_HOT_PERCENT", 0, 100);
   const bool saturation = setting("PUREEOS_PARALLEL_STRESS_SATURATION", 0, 1);
   const bool unique_rows = setting("PUREEOS_PARALLEL_STRESS_UNIQUE_ROWS", 0, 1);
   const bool global_accounts = setting("PUREEOS_PARALLEL_STRESS_GLOBAL_ACCOUNTS", 0, 1);
   const unsigned transfer_pairs = setting("PUREEOS_PARALLEL_STRESS_TRANSFER_PAIRS", 64, 1048576);
   const unsigned cpu = setting("PUREEOS_PARALLEL_STRESS_TEST_CPU", config::default_max_block_cpu_usage,
                                saturation ? 6000000 : 800000);
   const unsigned net = setting("PUREEOS_PARALLEL_STRESS_TEST_NET", config::default_max_block_net_usage,
                                saturation ? 16 * 1024 * 1024 : config::default_max_block_net_usage);
   // The tester bills each bootstrap transaction at its fixed test cost. The
   // former128/64 setup batches only fit the enlarged diagnostic CPU budget.
   // Keep the requested genesis limits and split setup into valid blocks.
   const unsigned setup_transactions = std::max(2u,
      (cpu - 20000u) / base_tester::DEFAULT_BILLED_CPU_TIME_US);
   const unsigned pair_batch = std::min(global_accounts ? 1024u : 8u, setup_transactions / 2);
   const unsigned funding_batch = std::min(global_accounts ? 1024u : 8u, setup_transactions);
   const unsigned resource_batch = std::min(global_accounts ? 1024u : 4u, setup_transactions / 2);
   const std::string mode = std::getenv("PUREEOS_PARALLEL_STRESS_MODE")
      ? std::getenv("PUREEOS_PARALLEL_STRESS_MODE") : "mixed";
   BOOST_REQUIRE(count >= 64 && roots >= 1 && cpu >= 200000 && uint64_t(count) * roots <= 2000000);
   BOOST_REQUIRE(mode == "token" || mode == "system" || mode == "simple" || mode == "complex" || mode == "mixed" || mode == "balanced");
   BOOST_REQUIRE(mode != "balanced" || (hot == 50 && count % 4 == 0));
   BOOST_REQUIRE(!unique_rows || (mode == "simple" && hot == 0 && work == 0 && roots == 1));
   BOOST_REQUIRE(transfer_pairs == 64 || transfer_pairs == 1024 || (global_accounts && transfer_pairs >= 64 && transfer_pairs <= 1048576));
   BOOST_REQUIRE(global_accounts || transfer_pairs == 64 || mode == "token");
   BOOST_REQUIRE(!global_accounts || (count == transfer_pairs && hot == 0 && roots == 1 && work == 0 &&
      (mode == "token" || (mode == "simple" && unique_rows))));
   const char* contracts_env = std::getenv("PUREEOS_PARALLEL_STRESS_CONTRACTS");
   const char* simple_env = std::getenv("PUREEOS_PARALLEL_STRESS_SIMPLE_WASM");
   const char* complex_env = std::getenv("PUREEOS_PARALLEL_STRESS_COMPLEX_WASM");
   BOOST_REQUIRE(contracts_env && simple_env && complex_env);
   const std::filesystem::path contracts(contracts_env), directory(destination);
   BOOST_REQUIRE(std::filesystem::create_directory(directory));
   std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
   auto genesis = base_tester::default_genesis();
   const auto defaults = genesis.initial_configuration;
   genesis.initial_configuration.max_block_cpu_usage = cpu;
   genesis.initial_configuration.max_block_net_usage = net;
   genesis.initial_configuration.validate();
   genesis.initial_timestamp = block_timestamp_type(fc::time_point::now() -
      fc::seconds(300 + (transfer_pairs > 64 ? 2 * transfer_pairs : 0))).to_time_point();
   const auto [private_key, public_key, proof] = get_bls_key("eosio"_n);
   genesis.initial_savanna.emplace(); genesis.initial_savanna->version = 2;
   genesis.initial_savanna->finalizer_key = public_key;
   genesis.initial_savanna->proof_of_possession = proof;
   fc::temp_directory temporary;
   auto cfg = base_tester::default_config(temporary).first;
   if (unique_rows || global_accounts) cfg.state_size = uint64_t(16384) * 1024 * 1024;
   cfg.wasm_runtime = wasm_interface::vm_type::eos_vm_jit;
   production_chain source(cfg, genesis);
   source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
   fc::variants checkpoints;
   const auto seal = [&] {
      const auto block = source.produce_block(); source.control->abort_block();
      const std::string file = "block-" + std::to_string(block->block_num()) + ".json";
      if (!std::getenv("PUREEOS_CAPACITY_SNAPSHOT_ONLY")) BOOST_REQUIRE(fc::json::save_to_file(*block, directory / file));
      if (checkpoints.size()%1000==0) std::cout << "Bootstrap blocks=" << checkpoints.size() << std::endl;
      checkpoints.emplace_back(fc::mutable_variant_object()("block_num", block->block_num())
         ("block_id", source.head().id())("lib", source.last_irreversible_block_num())
         ("integrity_hash", std::string(64, '0'))("file", file));
   };
   const std::vector<name> system_accounts{"eosio.token"_n, "eosio.stake"_n, "eosio.rex"_n,
      "eosio.ram"_n, "eosio.ramfee"_n, "eosio.bpay"_n, "eosio.vpay"_n,
      "eosio.saving"_n, "eosio.fees"_n, "eosio.reserv"_n};
   source.create_accounts(system_accounts); source.create_accounts({"alice"_n, "test"_n, "simple"_n}); seal();
   std::vector<name> from, to;
   for (unsigned i = 0; i < transfer_pairs; ++i) {
      std::string suffix(5, 'a'); unsigned ordinal = i;
      for (int j = 4; j >= 0; --j) { suffix[j] = char('a' + ordinal % 26); ordinal /= 26; }
      BOOST_REQUIRE_EQUAL(ordinal, 0u);
      from.emplace_back("src" + suffix); to.emplace_back("dst" + suffix);
      source.create_accounts({from.back(), to.back()});
      if ((i + 1) % pair_batch == 0) seal();
   }
   if (transfer_pairs % pair_batch) seal(); // A short setup batch, not measured work.
   const auto file_bytes = [](const std::filesystem::path& path) {
      std::ifstream stream(path, std::ios::binary);
      BOOST_REQUIRE_MESSAGE(stream.good(), "missing stress artifact: " << path);
      std::vector<uint8_t> data{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
      BOOST_REQUIRE(!data.empty() && data.size() <= 4 * 1024 * 1024);
      return data;
   };
   std::vector<fc::sha256> hashes;
   const auto deploy = [&](name account, const std::filesystem::path& base) {
      const auto data = file_bytes(base.string() + ".wasm");
      source.set_code(account, data);
      std::ifstream abi(base.string() + ".abi"); BOOST_REQUIRE(abi.good());
      source.set_abi(account, std::string{std::istreambuf_iterator<char>(abi), std::istreambuf_iterator<char>()});
      hashes.push_back(fc::sha256::hash(reinterpret_cast<const char*>(data.data()), data.size()));
      seal();
   };
   deploy("eosio.token"_n, contracts / "eosio.token/eosio.token");
   const auto asset_text = [](int64_t units) { return asset{units, symbol::from_string("4,SYS")}.to_string(); };
   source.push_action("eosio.token"_n, "create"_n, "eosio.token"_n, fc::mutable_variant_object()
      ("issuer", "eosio")("maximum_supply", "1000000000.0000 SYS"));
   source.push_action("eosio.token"_n, "issue"_n, "eosio"_n, fc::mutable_variant_object()
      ("to", "eosio")("quantity", "1000000000.0000 SYS")("memo", "isolated stress fixture")); seal();
   std::vector<name> users{"alice"_n, "test"_n, "simple"_n};
   users.insert(users.end(), from.begin(), from.end()); users.insert(users.end(), to.begin(), to.end());
   // Activate the real rotation host before installing the current fresh-chain
   // system profile; no native resource-manager mutation or fake system WASM.
   deploy("eosio"_n, contracts / "eosio.bios/eosio.bios");
   source.activate_builtin_protocol_features({builtin_protocol_feature_t::pureeos_rotation_context}); seal();
   deploy("eosio"_n, contracts / "eosio.system/eosio.system");
   source.push_action("eosio"_n, "init"_n, "eosio"_n,
      fc::mutable_variant_object()("version", 0)("core", "4,SYS")); seal();
   deploy("eosio.reserv"_n, contracts / "eosio.system/.powerup/powup.results");
   source.push_action("eosio"_n, "buyram"_n, "eosio"_n, fc::mutable_variant_object()
      ("payer", "eosio")("receiver", "eosio.reserv")("quant", "10.0000 SYS")); seal();
   const auto pricing = [] {
      return fc::mutable_variant_object()("current_weight_ratio", fc::variant())("target_weight_ratio", fc::variant())
         ("assumed_stake_weight", fc::variant())("target_timestamp", fc::variant())("exponent", 2.0)
         ("decay_secs", 86400)("min_price", "1.0000 SYS")("max_price", "100.0000 SYS");
   };
   source.push_action("eosio"_n, "cfgpowerup"_n, "eosio"_n, fc::mutable_variant_object()
      ("args", fc::mutable_variant_object()("net", pricing())("cpu", pricing())
         ("powerup_days", 1)("min_powerup_fee", "0.0001 SYS"))); seal();
   // Bootstrap only: reuse the installed, immutable ABIs instead of reparsing
   // the large system ABI for every account. Transactions still use the normal
   // signature, authorization, WASM and resource-billing path.
   const abi_serializer system_abi(source.control->get_account("eosio"_n).get_abi(),
      abi_serializer::create_yield_function(base_tester::abi_serializer_max_time));
   const abi_serializer token_abi(source.control->get_account("eosio.token"_n).get_abi(),
      abi_serializer::create_yield_function(base_tester::abi_serializer_max_time));
   std::set<std::pair<name,name>> checked_setup_actions;
   const auto setup_action = [&](name code, name action_name, name actor, const fc::variant_object& data) {
      const auto& abi = code == "eosio"_n ? system_abi : token_abi;
      const std::vector<permission_level> auth{{actor, "active"_n}};
      action act(auth, code, action_name, abi.variant_to_binary(abi.get_action_type(action_name), data,
         abi_serializer::create_yield_function(base_tester::abi_serializer_max_time)));
      if (checked_setup_actions.emplace(code,action_name).second)
         BOOST_REQUIRE(act.data == source.get_action(code,action_name,auth,data).data);
      signed_transaction trx;trx.actions.emplace_back(std::move(act));source.set_transaction_headers(trx);
      trx.sign(base_tester::get_private_key(actor,"active"),source.control->get_chain_id());
      return source.push_transaction(trx);
   };
   fc::mutable_variant_object initial_stake_units;
   initial_stake_units.reserve(users.size());
   for (size_t i = 0; i < users.size(); ++i) {
      // Give the shared complex-job signer enough REAL stake for the longer
      // soak. Otherwise account quota, rather than execution, caps this test.
      const int64_t grant = users[i] == "alice"_n ? 100000000 : (global_accounts && transfer_pairs > 262144 ? 100000 : 1000000);
      // A typed variant selects FC's append overload. The generic integer
      // overload calls set(), which linearly scans all prior account names.
      initial_stake_units(users[i].to_string(), fc::variant(grant));
      if (global_accounts && i >= 3)
         setup_action("eosio"_n,"buyrambytes"_n,"eosio"_n,fc::mutable_variant_object()
            ("payer","eosio")("receiver",users[i])("bytes",8192u));
      else
         setup_action("eosio"_n, "buyram"_n, "eosio"_n, fc::mutable_variant_object()
            ("payer", "eosio")("receiver", users[i])
            ("quant", unique_rows && users[i] == "simple"_n ? "50000.0000 SYS" : "10.0000 SYS"));
      setup_action("eosio"_n, "delegatebw"_n, "eosio"_n, fc::mutable_variant_object()
         ("from", "eosio")("receiver", users[i])("stake_net_quantity", asset_text(grant))
         ("stake_cpu_quantity", asset_text(grant))("transfer", false));
      if ((i + 1) % resource_batch == 0) seal();
   }
   seal();
   for (size_t i = 0; i < users.size(); ++i) {
      setup_action("eosio.token"_n, "open"_n, users[i], fc::mutable_variant_object()
         ("owner", users[i])("symbol", "4,SYS")("ram_payer", users[i]));
      setup_action("eosio.token"_n, "transfer"_n, "eosio"_n, fc::mutable_variant_object()
         ("from", "eosio")("to", users[i])("quantity", users[i] == "alice"_n ? "1000000.0000 SYS" :
            global_accounts ? (transfer_pairs > 262144 ? "100.0000 SYS" : "1000.0000 SYS") : "100000.0000 SYS")
         ("memo", "preallocated stress balance"));
      if ((i + 1) % std::max(1u, funding_batch / 2) == 0) seal();
   }
   seal();
   std::vector<name> scopes;
   for (unsigned i = 0; i < 128; ++i) scopes.emplace_back(std::string("job") + char('a' + i / 26) + char('a' + i % 26));
   for (name account : {"test"_n, "simple"_n}) {
      if (unique_rows && account == "simple"_n) {
         const auto wasm = file_bytes(simple_env);
         source.set_code(account, wasm);
         auto abi_path = std::filesystem::path(simple_env); abi_path.replace_extension(".abi");
         std::ifstream abi(abi_path); BOOST_REQUIRE(abi.good());
         source.set_abi(account, std::string{std::istreambuf_iterator<char>(abi), std::istreambuf_iterator<char>()});
         hashes.push_back(fc::sha256::hash(reinterpret_cast<const char*>(wasm.data()), wasm.size()));
         seal();
         for (unsigned first = 1; first <= count; first += 1024) {
            source.push_action(account, "init"_n, "alice"_n, fc::mutable_variant_object()
               ("first", first)("count", std::min(1024u, count - first + 1)));
            if ((first - 1) / 1024 % 4 == 3) seal();
         }
         seal();
         continue;
      }
      source.set_code(account, initialize_code()); seal();
      for (size_t i = 0; i < scopes.size(); ++i) {
         auto trx = make_transaction(source, scopes[i], 10, i + 1); trx.actions.front().account = account;
         trx.signatures.clear(); trx.sign(source.get_private_key("alice"_n, "active"), source.control->get_chain_id());
         BOOST_REQUIRE(!source.push_transaction(trx)->except);
         if (i % 16 == 15) seal();
      }
      const auto wasm = file_bytes(account == "test"_n ? complex_env : simple_env);
      source.set_code(account, wasm);
      hashes.push_back(fc::sha256::hash(reinterpret_cast<const char*>(wasm.data()), wasm.size()));
      source.set_abi(account, R"({"version":"eosio::abi/1.1","types":[],"structs":[{"name":"row","base":"","fields":[{"name":"value","type":"uint64"}]}],"tables":[{"name":"rows","index_type":"i64","key_names":[],"key_types":[],"type":"row"}],"actions":[],"ricardian_clauses":[],"error_messages":[],"abi_extensions":[]})");
      seal();
   }
   source.control->abort_block();
   const auto prefix_hash = source.control->calculate_integrity_hash();
   auto last = fc::mutable_variant_object(checkpoints.back().get_object());
   last("integrity_hash", prefix_hash); checkpoints.back() = std::move(last);
   // Snapshot avoids hashing and replaying every setup prefix for each sample.
   { std::ofstream out(directory / "bootstrap.snapshot", std::ios::binary);
     auto writer = std::make_shared<ostream_snapshot_writer>(out);
     source.control->write_snapshot(writer); writer->finalize(); }
   const auto& table_rows = source.control->db().get_index<key_value_index, by_scope_primary>();
   uint64_t self_paid_balances = 0;
   for (name owner : users) {
      const auto* table = source.find_table("eosio.token"_n, owner, "accounts"_n);
      BOOST_REQUIRE(table);
      const auto row = table_rows.lower_bound(boost::make_tuple(table->id, symbol::from_string("4,SYS").to_symbol_code().value));
      BOOST_REQUIRE(row != table_rows.end() && row->t_id == table->id && row->payer == owner);
      if (global_accounts && owner != "alice"_n && owner != "test"_n && owner != "simple"_n) {
         int64_t ram,net_weight,cpu_weight;
         const auto& limits=source.control->get_resource_limits_manager();
         limits.get_account_limits(owner,ram,net_weight,cpu_weight);
         BOOST_REQUIRE(ram>=4096 && limits.get_account_ram_usage(owner)<=ram);
      }
      ++self_paid_balances;
   }
   source.start();
   std::vector<name> balance_accounts{"eosio"_n};
   balance_accounts.insert(balance_accounts.end(), system_accounts.begin(), system_accounts.end());
   balance_accounts.insert(balance_accounts.end(), users.begin(), users.end());
   fc::mutable_variant_object balances;
   balances.reserve(balance_accounts.size());
   const auto symbol_code = symbol::from_string("4,SYS").to_symbol_code();
   for (name owner : balance_accounts) {
      const auto data = source.get_row_by_account("eosio.token"_n, owner, "accounts"_n, name{symbol_code.value});
      balances(owner.to_string(), fc::variant(data.empty() ? int64_t(0) : fc::raw::unpack<asset>(data).get_amount()));
   }
   fc::variants transactions;
   std::set<transaction_id_type> ids;
   for (unsigned i = 0; i < count; ++i) {
      std::string kind = mode;
      if (mode == "mixed") kind = i % 10 < 4 ? "token" : i % 10 < 6 ? "simple" : i % 10 < 8 ? "system" : "complex";
      // Each group of four has all four combinations. Cold rows exclude the
      // shared row and are disjoint within any supported (<=64 TX) batch.
      // Two contracts have independent tables: shared simple and shared
      // complex are separate conflict chains, not fake label-only conflicts.
      if (mode == "balanced") kind = i % 4 < 2 ? "simple" : "complex";
      const bool shared = mode == "balanced" ? i % 2 == 0 : i % 100 < hot;
      const unsigned slot = i % 100 < hot ? 0 : i % transfer_pairs;
      const auto scope = scopes[mode == "balanced" ? (shared ? 0 : 1 + (i / 4) % (scopes.size() - 1))
                                                     : (shared ? 0 : i % scopes.size())];
      signed_transaction trx; name signer = "alice"_n;
      int64_t units = 0;
      if (kind == "token") {
         signer = from[slot]; units = 1;
         trx.actions.push_back(source.get_action("eosio.token"_n, "transfer"_n, {{signer, "active"_n}},
            fc::mutable_variant_object()("from", signer)("to", to[slot])("quantity", asset_text(units))("memo", std::to_string(i))));
      } else if (kind == "system") {
         signer = from[slot];
         // Quantities make repeated system actions unique without nonstandard
         // action data, fake context-free work or changing transaction limits.
         units = (i % 2 ? 100 : 1) + i;
         kind = i % 2 ? "systemram" : "systemstake";
         trx.actions.push_back(source.get_action("eosio"_n, i % 2 ? "buyram"_n : "delegatebw"_n,
            {{signer, "active"_n}}, i % 2 ? fc::mutable_variant_object()("payer", signer)("receiver", signer)("quant", asset_text(units))
               : fc::mutable_variant_object()("from", signer)("receiver", signer)("stake_net_quantity", asset_text(units))
                  ("stake_cpu_quantity", asset_text(units))("transfer", false)));
      } else if (global_accounts) {
         signer = from.at(i);
         trx.actions.push_back(source.get_action("simple"_n, "step"_n, {{signer, "active"_n}},
            fc::mutable_variant_object()("actor", signer)("key", uint64_t(i + 1))("nonce", uint64_t(1000000 + i))));
      } else {
         trx = make_transaction(source, unique_rows ? name{uint64_t(i + 1)} : scope,
                                1, 1000000 + i, kind == "complex" ? work : 0);
         trx.actions.front().account = kind == "complex" ? "test"_n : "simple"_n;
         if (kind == "complex") trx.actions.resize(roots, trx.actions.front());
      }
      source.set_transaction_headers(trx);
      trx.expiration = fc::time_point_sec(fc::time_point::now() + fc::seconds(global_accounts ? 3300 : 1800));
      if (global_accounts) {
         BOOST_REQUIRE_EQUAL(trx.actions.size(), 1u);
         BOOST_REQUIRE_EQUAL(trx.actions.front().authorization.size(), 1u);
         BOOST_REQUIRE(trx.actions.front().authorization.front().actor == from.at(i));
      }
      trx.signatures.clear(); trx.sign(source.get_private_key(signer, "active"), source.control->get_chain_id());
      packed_transaction packed(trx, packed_transaction::compression_type::none);
      BOOST_REQUIRE(ids.insert(packed.id()).second);
      transactions.emplace_back(fc::mutable_variant_object()("id", packed.id())("packed", packed)
         ("actions", trx.actions.size())("kind", kind)("scope", unique_rows ? "simple"_n : scope)("from", signer)
         ("to", to[slot])("units", units)("contention", shared ? "shared" : "independent")
         ("row_key", unique_rows ? i + 1 : 1)("action", trx.actions.front().name));
   }
   const std::vector<std::string> keys{base_tester::get_public_key("eosio"_n, "active").to_string({}) + "=KEY:" +
      base_tester::get_private_key("eosio"_n, "active").to_string({}), public_key.to_string() + "=KEY:" + private_key.to_string()};
   BOOST_REQUIRE(fc::json::save_to_file(genesis, directory / "genesis.json"));
   BOOST_REQUIRE(fc::json::save_to_file(keys, directory / "TEST-ONLY-keys.json"));
   BOOST_REQUIRE(fc::json::save_to_file(transactions, directory / "transactions.json"));
   // These are local test-manifest arrays larger than FC's generic vector
   // conversion cap. Build JSON variants explicitly; keep global input guards.
   const auto json_names=[](const std::vector<name>& input) {
      fc::variants values;values.reserve(input.size());
      for (name n:input) values.emplace_back(n.to_string());
      return values;
   };
   BOOST_REQUIRE(fc::json::save_to_file(fc::mutable_variant_object()("format", 1)
      ("fixture_kind", "real-system-mixed-stress")("chain_id", genesis.compute_chain_id())("code_hashes", hashes)
      ("checkpoints", checkpoints)("business_transactions", count)("mode", mode)("hot_percent", hot)
      ("loop_iterations", work)("complex_roots", roots)("scopes", scopes)("initial_row_value", 10)
      ("initial_balances", balances)("balance_accounts", json_names(balance_accounts))("users", json_names(users))("initial_stake_units", initial_stake_units)
      ("default_initial_configuration", defaults)
      ("test_capacity_diagnostic", cpu != defaults.max_block_cpu_usage || net != defaults.max_block_net_usage)
      ("saturation_diagnostic", saturation)
      ("unique_business_rows", unique_rows)("unique_row_count", unique_rows ? count : 0)
      ("transfer_pairs", transfer_pairs)
      ("globally_unique_accounts", global_accounts)
      ("bootstrap_business_RAM_minimum_verified",global_accounts ? 4096 : 0)
      ("bootstrap_business_RAM_requested",global_accounts ? 8192 : 0)
      ("bootstrap_ABI_cache_crosschecked",true)
      ("self_paid_balance_rows_verified", self_paid_balances)
      ("bootstrap_snapshot", "bootstrap.snapshot")
      ("tool_overlay", "globally unique five-letter names, own RAM payer, sparse hash, snapshot, resign only")
      ("system_contract", "current integer voting/resource split/rotation profile; no voting load")
      ("resource_policy", "real RAM purchases and staking, PowerUp configured; no native quota overrides"), directory / "manifest.json"));
}
BOOST_AUTO_TEST_SUITE_END()
