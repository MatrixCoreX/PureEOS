// SPDX-License-Identifier: LicenseRef-AIC-Noncommercial-3.0
#include "eosio.key.hpp"
#include <eosio/action.hpp>
#include <eosio/crypto_ext.hpp>
#include <eosio/transaction.hpp>
#include <algorithm>
#include <limits>
#include <tuple>

namespace keywallet {
namespace {
constexpr int64_t limit = (int64_t(1) << 62) - 1;
const std::array<uint8_t, 32> field_prime = {
   255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
   255,255,255,255,255,255,255,255,255,255,255,254,255,255,252,47};
bool zero(const checksum256& value) { return value == checksum256{}; }
uint32_t occupancy(const market_resource& r) {
   check(r.version == 0 && r.weight > 0 && r.utilization >= 0 && r.utilization <= r.weight &&
         r.adjusted_utilization >= 0 && r.adjusted_utilization <= r.weight,
         "unsupported or invalid resource-market state");
   // powerup() commits increased current utilization before the smoothed field
   // catches up. This is valid state: price the greater committed demand.
   return uint32_t(__int128(std::max(r.utilization, r.adjusted_utilization)) * 10000 / r.weight);
}
uint32_t outgoing_rows(name token,name recipient,symbol sym) {
   token_balances table(token,recipient.value);
   return table.find(sym.code().raw())==table.end()?1:0;
}
uint64_t parse_cap(const std::string& text) {
   check(!text.empty() && text.size() <= 19, "invalid fee cap");
   uint64_t value = 0;
   for (char ch : text) {
      check(ch >= '0' && ch <= '9', "fee cap must be integer atomic units");
      check(value <= (uint64_t(limit) - uint64_t(ch - '0')) / 10, "fee cap overflow");
      value = value * 10 + uint64_t(ch - '0');
   }
   return value;
}
checksum256 parse_id(const std::string& text) {
   check(text.size() == 64, "request ID must be 32-byte hex");
   std::array<uint8_t,32> bytes{};
   auto nibble = [](char ch) -> uint8_t {
      if(ch >= '0' && ch <= '9') return ch - '0';
      if(ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
      check(false, "request ID must be lowercase hex"); return 0;
   };
   for(uint32_t i=0;i<32;++i) bytes[i] = uint8_t(nibble(text[2*i])*16+nibble(text[2*i+1]));
   checksum256 id(bytes);check(!zero(id),"request ID must not be zero");return id;
}
}

int64_t key_contract::checked(__int128 value) {
   check(value >= 0 && value <= limit, "amount overflow or underflow");
   return int64_t(value);
}
key_contract::configuration key_contract::config() const {
   config_table table(get_self(), get_self().value);
   check(table.exists(), "wallet not initialized");
   auto cfg = table.get();
   check(cfg.version == 2, "unsupported wallet version");
   return cfg;
}
void key_contract::check_system(const configuration& cfg) const {
   check(get_code_hash(system_account) == cfg.system_hash, "system code changed; relay profile needs review");
}
checksum256 key_contract::hash_key(const public_key& key) {
   auto data = pack(key);
   return sha256(data.data(), data.size());
}
checksum256 key_contract::hash_asset(name token, symbol sym) {
   auto data = pack(std::make_tuple(token, sym));
   return sha256(data.data(), data.size());
}
void key_contract::validate_key(const public_key& key) {
   check(key.index() == 0, "K1 public key required");
   const auto& k = std::get<0>(key);
   check(k[0] == 2 || k[0] == 3, "invalid compressed public key");
   std::array<uint8_t, 32> x{};
   for (uint32_t i = 0; i < 32; ++i) x[i] = uint8_t(k[i + 1]);
   check(std::lexicographical_compare(x.begin(), x.end(), field_prime.begin(), field_prime.end()), "public key coordinate out of range");
   // Validate x^3+7 is a square in the secp256k1 field using the native modular
   // exponentiation primitive. No custom signing/verification implementation.
   std::array<uint8_t, 32> rhs{}, root{}, squared{};
   const char exponent3 = 3, exponent2 = 2;
   auto prime = reinterpret_cast<const char*>(field_prime.data());
   check(eosio::mod_exp(reinterpret_cast<const char*>(x.data()), 32, &exponent3, 1, prime, 32,
                       reinterpret_cast<char*>(rhs.data()), 32) == 0, "public-key field operation failed");
   uint16_t carry = 7;
   for (int i = 31; i >= 0; --i) { carry += rhs[i]; rhs[i] = uint8_t(carry); carry >>= 8; }
   if (carry || !std::lexicographical_compare(rhs.begin(), rhs.end(), field_prime.begin(), field_prime.end())) {
      int borrow = 0;
      for (int i = 31; i >= 0; --i) {
         int v = int(rhs[i]) - field_prime[i] - borrow;
         rhs[i] = uint8_t(v); borrow = v < 0;
      }
   }
   auto exponent = field_prime;
   carry = 1;
   for (int i = 31; i >= 0; --i) { carry += exponent[i]; exponent[i] = uint8_t(carry); carry >>= 8; }
   uint8_t previous = 0;
   for (auto& byte : exponent) { uint8_t next = byte & 3; byte = uint8_t((previous << 6) | (byte >> 2)); previous = next; }
   check(eosio::mod_exp(reinterpret_cast<char*>(rhs.data()), 32, reinterpret_cast<char*>(exponent.data()), 32,
                       prime, 32, reinterpret_cast<char*>(root.data()), 32) == 0, "public-key square root failed");
   check(eosio::mod_exp(reinterpret_cast<char*>(root.data()), 32, &exponent2, 1,
                       prime, 32, reinterpret_cast<char*>(squared.data()), 32) == 0 && squared == rhs,
         "public key is not on secp256k1");
   check(k[0] == 2 || std::any_of(root.begin(), root.end(), [](uint8_t x) { return x; }), "invalid public-key parity");
}
public_key key_contract::decode_key(const std::string& text) {
   bool modern = text.rfind("PUB_K1_", 0) == 0;
   check(modern || text.rfind("EOS", 0) == 0, "expected PUB_K1 or legacy EOS public key");
   auto encoded = text.substr(modern ? 7 : 3);
   check(encoded.size() >= 49 && encoded.size() <= 52, "invalid public-key length");
   check(encoded.front() != '1', "noncanonical base58 public key");
   constexpr char alphabet[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
   std::array<uint8_t, 37> decoded{};
   for (char c : encoded) {
      const char* found = std::find(std::begin(alphabet), std::end(alphabet) - 1, c);
      check(found != std::end(alphabet) - 1, "invalid base58 public key");
      uint32_t carry = uint32_t(found - alphabet);
      for (int i = 36; i >= 0; --i) { carry += uint32_t(decoded[i]) * 58; decoded[i] = uint8_t(carry); carry >>= 8; }
      check(carry == 0, "public key base58 overflow");
   }
   std::vector<char> data(decoded.begin(), decoded.begin() + 33);
   if (modern) { data.push_back('K'); data.push_back('1'); }
   auto digest = ripemd160(data.data(), data.size()).extract_as_byte_array();
   for (int i = 0; i < 4; ++i) check(decoded[33 + i] == digest[i], "public key checksum mismatch");
   ecc_public_key bytes{};
   std::copy(decoded.begin(), decoded.begin() + 33, bytes.begin());
   return public_key(std::in_place_index<0>, bytes);
}
int64_t key_contract::raw_balance(name token, name owner, symbol sym) {
   token_balances table(token, owner.value);
   auto it = table.find(sym.code().raw());
   if (it == table.end()) return 0;
   check(it->balance.is_valid() && it->balance.symbol == sym && it->balance.amount >= 0, "unsupported token balance schema");
   return it->balance.amount;
}
void key_contract::init(checksum256 chain_id, checksum256 deployment, name fee_contract,
                        symbol fee_symbol, checksum256 system_hash, fee_policy policy) {
   require_auth(get_self());
   check(get_self() == "eosio.key"_n, "deploy under eosio.key before governance sealing");
   config_table table(get_self(), get_self().value);
   check(!table.exists(), "already initialized");
   check(!zero(chain_id) && !zero(deployment) && !zero(system_hash), "nonzero domain and system code required");
   check(fee_symbol == symbol("AIC", 4) && is_account(fee_contract), "v1 fee asset must be 4,AIC");
   check(policy.minimum_fee > 0 && policy.unit_fee > 0 && policy.new_row_fee > 0,
         "nonzero fee floor, work and storage prices required");
   check(policy.minimum_fee <= 100000000 && policy.unit_fee <= 100000000 && policy.new_row_fee <= 100000000,
         "fee-policy parameter out of bounds");
   check(policy.maximum_multiplier_bps >= 10000 && policy.maximum_multiplier_bps <= 1000000 &&
         policy.epoch_seconds >= 1 && policy.epoch_seconds <= 60, "invalid dynamic-fee policy");
   check(policy.max_wallets > 0 && policy.max_wallets <= 10000000 && policy.max_balances > 0 &&
         policy.max_balances <= 50000000 && policy.max_assets > 0 && policy.max_assets <= 100000 &&
         policy.max_native_payers > 0 && policy.max_native_payers <= 10000000, "invalid storage limits");
   check(get_code_hash(system_account) == system_hash, "system code does not match declared schema");
   token_stats stats(fee_contract, fee_symbol.code().raw());
   auto st = stats.find(fee_symbol.code().raw());
   check(st != stats.end() && st->supply.symbol == fee_symbol && st->max_supply.symbol == fee_symbol,
         "fee token does not exist with expected precision");
   check(raw_balance(fee_contract, get_self(), fee_symbol) == 0, "unattributed initial fee reserve");
   configuration cfg;
   cfg.chain_id = chain_id; cfg.deployment = deployment; cfg.system_hash = system_hash;
   cfg.fee_contract = fee_contract; cfg.fee_symbol = fee_symbol; cfg.policy = policy;
   table.set(cfg, get_self());
   // Fixed escape-path bucket: a new native CPU payer must not allocate an
   // unbounded fee-bucket row or prevent a holder from exiting at capacity.
   fee_buckets buckets(get_self(), get_self().value);
   buckets.emplace(get_self(), [&](auto& row) { row.owner = get_self(); });
   assets registered(get_self(), get_self().value);
   registered.emplace(get_self(), [&](auto& row) { row.id = 1; row.token_contract = fee_contract;
      row.token_symbol = fee_symbol; row.identity = hash_asset(fee_contract, fee_symbol); });
   market_singleton market(system_account, 0);
   check(market.exists(), "PowerUp market must be initialized");
   auto state = market.get();
   check(state.version == 1, "unsupported PowerUp resource-split market version");
   occupancy(state.cpu); occupancy(state.net);
}
void key_contract::check_bp(name bp, const configuration& cfg) const {
   check_system(cfg);
   producers_view producers(system_account, system_account.value);
   auto p = producers.find(bp.value);
   check(p != producers.end() && p->is_active, "relay requires registered active BP");
   candidates_view candidates(system_account, system_account.value);
   auto c = candidates.find(bp.value);
   check(c != candidates.end() && !c->banned, "blacklisted BP cannot relay");
   bonds_view bonds(system_account, system_account.value);
   auto b = bonds.find(bp.value);
   check(b != bonds.end() && b->state == 0 && b->balance == 100000000, "relay requires normal full BP bond");
   maintenance_views maintenance(system_account, system_account.value);
   auto m = maintenance.find(bp.value);
   check(m == maintenance.end() || m->state == 0, "BP in maintenance cannot relay");
}
void key_contract::regrelay(name bp, name permission, std::string endpoint) {
   require_auth(bp);
   auto cfg = config(); check_bp(bp, cfg);
   check(permission.value && permission != "owner"_n && permission != "active"_n && permission != "eosio.code"_n,
         "use a dedicated child relay permission");
   check(endpoint.size() >= 9 && endpoint.size() <= 200 && endpoint.rfind("https://", 0) == 0,
         "relay endpoint must be bounded HTTPS URL");
   relays table(get_self(), get_self().value);
   auto it = table.find(bp.value);
   auto update = [&](auto& row) { row.bp = bp; row.permission = permission; row.endpoint = endpoint; row.enabled = true; };
   if (it == table.end()) table.emplace(bp, update); else table.modify(it, same_payer, update);
   fee_buckets buckets(get_self(), get_self().value);
   if (buckets.find(bp.value) == buckets.end()) buckets.emplace(bp, [&](auto& row) { row.owner = bp; });
}
void key_contract::setrelay(name bp, bool enabled) {
   require_auth(bp);
   if (enabled) check_bp(bp, config());
   relays table(get_self(), get_self().value);
   auto it = table.require_find(bp.value, "relay not registered");
   table.modify(it, same_payer, [&](auto& row) { row.enabled = enabled; });
}
void key_contract::check_relay(name bp, name reward_to, const configuration& cfg) const {
   check(bp.value && reward_to == bp, "relay reward must belong to selected BP");
   check_bp(bp, cfg);
   relays table(get_self(), get_self().value);
   auto it = table.require_find(bp.value, "BP relay not registered");
   check(it->enabled, "relay disabled");
   require_auth(permission_level{bp, it->permission});
}
uint64_t key_contract::find_wallet(const public_key& key) const {
   check(key.index() == 0, "K1 public key required");
   wallets table(get_self(), get_self().value);
   auto index = table.get_index<"bykey"_n>();
   auto it = index.find(hash_key(key));
   if (it == index.end()) return 0;
   check(it->key == key, "public-key identity collision");
   return it->id;
}
uint64_t key_contract::ensure_wallet(const public_key& key) {
   if (auto id = find_wallet(key)) return id;
   validate_key(key);
   auto cfg = config();
   check(cfg.wallet_count < cfg.policy.max_wallets && cfg.next_wallet != uint64_t(-1), "wallet storage capacity reached");
   uint64_t id = cfg.next_wallet++; ++cfg.wallet_count;
   wallets table(get_self(), get_self().value);
   table.emplace(get_self(), [&](auto& row) { row.id = id; row.key = key; row.key_hash = hash_key(key); });
   config_table(get_self(), get_self().value).set(cfg, get_self());
   return id;
}
uint64_t key_contract::find_asset(name token, symbol sym) const {
   check(sym.is_valid(), "invalid symbol");
   assets table(get_self(), get_self().value);
   auto index = table.get_index<"byidentity"_n>();
   auto it = index.find(hash_asset(token, sym));
   if (it == index.end()) return 0;
   check(it->token_contract == token && it->token_symbol == sym, "asset identity collision");
   return it->id;
}
uint64_t key_contract::ensure_asset(name token, symbol sym) {
   if (auto id = find_asset(token, sym)) return id;
   token_stats stats(token, sym.code().raw());
   auto st = stats.find(sym.code().raw());
   check(st != stats.end() && st->supply.is_valid() && st->max_supply.is_valid() &&
         st->supply.symbol == sym && st->max_supply.symbol == sym && st->supply.amount >= 0 &&
         st->max_supply.amount >= st->supply.amount && is_account(st->issuer), "unsupported token stat or precision");
   auto cfg = config();
   check(cfg.asset_count < cfg.policy.max_assets && cfg.next_asset != uint64_t(-1), "asset capacity reached");
   uint64_t id = cfg.next_asset++; ++cfg.asset_count;
   assets table(get_self(), get_self().value);
   table.emplace(get_self(), [&](auto& row) { row.id = id; row.token_contract = token;
      row.token_symbol = sym; row.identity = hash_asset(token, sym); });
   config_table(get_self(), get_self().value).set(cfg, get_self());
   return id;
}
void key_contract::check_asset(uint64_t id) const {
   check(id, "asset not registered");
   assets table(get_self(), get_self().value);
   auto it = table.require_find(id, "asset not registered");
   check(!it->busy, "asset has an outstanding token interaction");
}
int64_t key_contract::balance_of(uint64_t wallet_id, uint64_t asset_id) const {
   if (!wallet_id || !asset_id) return 0;
   balances table(get_self(), wallet_id);
   auto it = table.find(asset_id);
   return it == table.end() ? 0 : it->amount;
}
void key_contract::credit(uint64_t wallet_id, uint64_t asset_id, int64_t amount) {
   check(amount > 0 && wallet_id && asset_id, "invalid credit");
   balances table(get_self(), wallet_id);
   auto it = table.find(asset_id);
   if (it == table.end()) {
      auto cfg = config();
      check(cfg.balance_count < cfg.policy.max_balances, "balance storage capacity reached");
      table.emplace(get_self(), [&](auto& row) { row.asset_id = asset_id; row.amount = amount; });
      ++cfg.balance_count; config_table(get_self(), get_self().value).set(cfg, get_self());
   } else table.modify(it, same_payer, [&](auto& row) { row.amount = checked(__int128(row.amount) + amount); });
}
void key_contract::debit(uint64_t wallet_id, uint64_t asset_id, int64_t amount) {
   check(amount > 0, "invalid debit");
   balances table(get_self(), wallet_id);
   auto it = table.require_find(asset_id, "insufficient token balance");
   check(it->amount >= amount, "insufficient token balance");
   table.modify(it, same_payer, [&](auto& row) { row.amount -= amount; });
}
void key_contract::epoch(uint64_t wallet_id, uint64_t expected, uint64_t next) {
   wallets table(get_self(), get_self().value);
   auto it = table.require_find(wallet_id, "sender wallet missing");
   check(it->auth_epoch == expected, "authorization epoch revoked");
   check(next > expected, "authorization epoch must increase");
   table.modify(it, same_payer, [&](auto& row) { row.auth_epoch = next; });
}
std::vector<char> key_contract::raw_transaction() {
   auto size = transaction_size();
   check(size > 0 && size <= 16384, "wallet transaction size bound exceeded");
   std::vector<char> data(size);
   check(read_transaction(data.data(), data.size()) == size, "incomplete native transaction");
   return data;
}
void key_contract::prove(public_key owner, checksum256 chain_id, checksum256 deployment, uint32_t reference_block_num) {
   // Context-free action: proof bytes are outside transaction::id(). Neither a
   // relay nor signature malleability can manufacture a second native ID.
   check(owner.index() == 0, "K1 owner required");
   int size = get_context_free_data(0, nullptr, 0);
   check(size == 66, "one canonical K1 owner proof required");
   check(get_context_free_data(1, nullptr, 0) == -1, "unexpected extra context-free data");
   std::vector<char> bytes(size);
   check(get_context_free_data(0, bytes.data(), bytes.size()) == size, "incomplete owner proof");
   auto sig = unpack<signature>(bytes); check(sig.index() == 0, "K1 signature required");
   auto data = raw_transaction(); auto id = sha256(data.data(), data.size());
   auto envelope = pack(std::make_tuple(std::string("PureEOS:eosio.key:transaction"), uint32_t(2),
                                       chain_id, get_self(), deployment, id));
   assert_recover_key(sha256(envelope.data(), envelope.size()), sig, owner);
}
void key_contract::verify_envelope(name operation, const public_key& owner, uint64_t auth_epoch,
                                    checksum256 request_id, const std::vector<char>& payload,
                                    const configuration& cfg) const {
   check(get_first_receiver() == get_self() && !get_sender().value, "top-level owner operation required");
   check(owner.index() == 0 && !zero(request_id), "K1 owner and nonzero request ID required");
   auto data = raw_transaction(); auto tx = unpack<transaction>(data);
   check(tx.actions.size() == 1 && tx.context_free_actions.size() == 1,
         "one root operation and one owner proof required");
   check(tx.delay_sec.value == 0 && tx.transaction_extensions.empty(), "unsupported transaction extensions");
   check(uint64_t(tx.expiration.sec_since_epoch()) <= uint64_t(current_time_point().sec_since_epoch()) + 3600,
         "owner transaction validity exceeds one hour");
   const auto& proof = tx.context_free_actions[0];
   check(proof.account == get_self() && proof.name == "prove"_n && proof.authorization.empty() && proof.data.size()==102,
         "owner proof action mismatch");
   auto args=unpack<std::tuple<public_key,checksum256,checksum256,uint32_t>>(proof.data);
   auto reference=std::get<3>(args);
   check(std::get<0>(args)==owner && std::get<1>(args)==cfg.chain_id && std::get<2>(args)==cfg.deployment &&
         reference>0 && (reference&0xffff)==tx.ref_block_num,
         "owner proof domain or reference block mismatch");
   const auto& root = tx.actions[0];
   check(root.account == get_self() && root.name == operation && root.authorization.size()==1 && root.data == payload,
         "owner proof root operation mismatch");
   auto sender = find_wallet(owner); check(sender, "sender wallet missing");
   wallets table(get_self(), get_self().value);
   check(table.get(sender).auth_epoch == auth_epoch, "authorization epoch revoked");
}
uint32_t key_contract::rows_for(const public_key& key, uint64_t asset_id) const {
   auto id = find_wallet(key);
   if (!id) return 2;
   balances table(get_self(), id);
   return !asset_id || table.find(asset_id) == table.end() ? 1 : 0;
}
uint32_t key_contract::multiplier(const configuration& cfg, bool exit_path) {
   if (exit_path && get_code_hash(system_account) != cfg.system_hash) return 10000;
   check_system(cfg);
   const uint64_t epoch = current_time_point().sec_since_epoch() / cfg.policy.epoch_seconds;
   pricing_table table(get_self(), get_self().value);
   auto old = table.get_or_default();
   if (table.exists() && old.epoch == epoch) return old.multiplier_bps;
   market_singleton market(system_account, 0);
   check(market.exists(), "resource market missing");
   auto state = market.get(); check(state.version == 1, "unsupported resource-split market");
   pricing next;
   next.epoch = epoch; next.cpu_occupancy_bps = occupancy(state.cpu); next.net_occupancy_bps = occupancy(state.net);
   const uint64_t use = std::max(next.cpu_occupancy_bps, next.net_occupancy_bps);
   next.multiplier_bps = 10000 + uint32_t(__int128(cfg.policy.maximum_multiplier_bps - 10000) * use * use / 100000000);
   table.set(next, get_self());
   return next.multiplier_bps;
}
key_contract::charge key_contract::quote(const configuration& cfg, uint32_t units, uint32_t rows,
                                         bool relayed, bool exit_path) {
   check(units <= 128 && rows <= 4, "fee work bound exceeded");
   auto value = (__int128(cfg.policy.unit_fee) * units + __int128(cfg.policy.new_row_fee) * rows) * multiplier(cfg, exit_path);
   int64_t base = std::max(cfg.policy.minimum_fee, checked((value + 9999) / 10000));
   int64_t premium = relayed ? checked((__int128(base) * 30 + 99) / 100) : 0;
   return {base, premium, checked(__int128(base) + premium)};
}
void key_contract::check_max(const configuration& cfg, const charge& fee, const asset& maximum) const {
   check(maximum.is_valid() && maximum.symbol == cfg.fee_symbol && maximum.amount >= 0, "invalid AIC fee cap");
   check(fee.total <= maximum.amount, "fee exceeds signed maximum");
}
void key_contract::credit_fee_bucket(name owner, const charge& fee) {
   fee_buckets table(get_self(), get_self().value);
   auto it = table.find(owner.value);
   auto update = [&](auto& row) { row.owner = owner; row.network = checked(__int128(row.network) + fee.base);
      row.relay_earned = checked(__int128(row.relay_earned) + fee.premium); };
   if (it == table.end()) table.emplace(get_self(), update); else table.modify(it, same_payer, update);
}
void key_contract::charge_native(name owner, const charge& fee, const asset& maximum) {
   auto cfg = config(); check_max(cfg, fee, maximum); check_asset(1);
   native_credits table(get_self(), get_self().value);
   auto it = table.require_find(owner.value, "prepay AIC fees separately before principal transfer");
   check(it->amount >= fee.total, "insufficient prepaid AIC fee credit");
   table.modify(it, same_payer, [&](auto& row) { row.amount -= fee.total; });
   credit_fee_bucket(owner, fee);
}
void key_contract::charge_key(uint64_t wallet_id, name bp, const charge& fee, const asset& maximum) {
   auto cfg = config(); check_max(cfg, fee, maximum); check_asset(1);
   debit(wallet_id, 1, fee.total);
   credit_fee_bucket(bp, fee);
}
void key_contract::ontransfer(name from, name to, asset quantity, std::string memo) {
   if (to != get_self() && from != get_self()) return;
   auto cfg = config();
   check(quantity.is_valid() && quantity.amount > 0 && memo.size() <= 256, "invalid token transfer");
   name token = get_first_receiver();
   check(token != get_self(), "wallet is not an issuing token contract");
   require_auth(from); // Never trust a third-party token to authenticate its from field.
   uint64_t id = find_asset(token, quantity.symbol);
   if (from == get_self()) {
      check(to != get_self() && id, "unexpected outgoing token notification");
      assets table(get_self(), get_self().value);
      auto it = table.require_find(id, "unknown outgoing asset");
      check(it->busy && !it->notified && it->out_to == to && it->out_amount == quantity.amount,
            "outgoing notification does not match authorized withdrawal");
      check(raw_balance(token, get_self(), quantity.symbol) == it->reserve &&
            raw_balance(token, to, quantity.symbol) == checked(__int128(it->receiver_before) + quantity.amount),
            "token did not deliver the full principal");
      table.modify(it, same_payer, [&](auto& row) { row.notified = true; });
      return;
   }
   check(to == get_self(), "incorrect deposit receiver");
   if (memo == "fee") {
      check(token == cfg.fee_contract && quantity.symbol == cfg.fee_symbol, "fee funding accepts only configured AIC");
      check_asset(1);
      assets registered(get_self(), get_self().value);
      auto a = registered.require_find(1, "fee asset missing");
      check(raw_balance(token, get_self(), quantity.symbol) == checked(__int128(a->reserve) + quantity.amount), "unbacked fee funding");
      native_credits credits(get_self(), get_self().value);
      auto it = credits.find(from.value);
      if (it == credits.end()) {
         check(quantity.amount >= cfg.policy.minimum_fee && cfg.native_payer_count < cfg.policy.max_native_payers,
               "fee funding minimum or storage capacity not met");
         credits.emplace(get_self(), [&](auto& row) { row.owner = from; row.amount = quantity.amount; });
         ++cfg.native_payer_count; config_table(get_self(), get_self().value).set(cfg, get_self());
      } else credits.modify(it, same_payer, [&](auto& row) { row.amount = checked(__int128(row.amount) + quantity.amount); });
      registered.modify(a, same_payer, [&](auto& row) { row.reserve = checked(__int128(row.reserve) + quantity.amount); });
      record("fundfees"_n, {}, from, {}, {}, token, quantity, 0, {}, 0);
      return;
   }
   const bool modern = memo.rfind("PK2|", 0) == 0;
   check(modern || memo.rfind("PK1|", 0) == 0, "use PK2|PUBLIC_KEY|max_fee_atomic_units|request_id or fee");
   const auto separator = memo.find('|', 4);
   const auto last = separator == std::string::npos ? std::string::npos : memo.find('|', separator+1);
   check(separator != std::string::npos && (modern ? last != std::string::npos && memo.find('|',last+1)==std::string::npos : last==std::string::npos), "invalid deposit memo");
   const auto key = decode_key(memo.substr(4, separator - 4));
   const auto maximum = asset(int64_t(parse_cap(memo.substr(separator + 1, modern ? last-separator-1 : std::string::npos))), cfg.fee_symbol);
   const auto request_id = modern ? parse_id(memo.substr(last+1)) : checksum256{};
   const uint32_t rows = rows_for(key, id) + (id ? 0 : 1);
   auto fee = quote(cfg, 2 + (memo.size() + 63) / 64, rows, false);
   charge_native(from, fee, maximum);
   if (!id) id = ensure_asset(token, quantity.symbol);
   check_asset(id);
   assets table(get_self(), get_self().value);
   auto it = table.require_find(id, "asset missing");
   check(raw_balance(token, get_self(), quantity.symbol) == checked(__int128(it->reserve) + quantity.amount),
         "token deposit does not match exact reserve increase");
   uint64_t wallet_id = ensure_wallet(key);
   credit(wallet_id, id, quantity.amount);
   table.modify(it, same_payer, [&](auto& row) { row.reserve = checked(__int128(row.reserve) + quantity.amount); });
   record("deposit"_n, request_id, from, {}, hash_key(key), token, quantity, fee.total, {}, 0);
}
void key_contract::ksend(transfer_request request) {
   auto cfg = config();
   verify_envelope("ksend"_n, request.from, request.auth_epoch, request.request_id, pack(request), cfg);
   check_relay(request.relayer, request.reward_to, cfg);
   check(request.quantity.is_valid() && request.quantity.amount > 0 && request.memo.size() <= 128,
         "invalid transfer quantity or memo");
   check(request.from != request.to, "cannot transfer to same public key");
   auto id = find_asset(request.token_contract, request.quantity.symbol); check_asset(id); check_asset(1);
   auto sender = find_wallet(request.from); check(sender, "sender wallet missing");
   auto fee = quote(cfg, 4 + (request.memo.size() + 63) / 64, rows_for(request.to, id), true);
   check_max(cfg, fee, request.max_fee);
   check(balance_of(sender, id) >= checked(__int128(request.quantity.amount) + (id == 1 ? fee.total : 0)), "insufficient principal balance");
   check(balance_of(sender, 1) >= fee.total, "insufficient additional AIC fee balance");
   charge_key(sender, request.relayer, fee, request.max_fee);
   debit(sender, id, request.quantity.amount);
   auto recipient = ensure_wallet(request.to);
   credit(recipient, id, request.quantity.amount);
   record("ksend"_n, request.request_id, {}, hash_key(request.from), hash_key(request.to), request.token_contract,
          request.quantity, fee.total, request.relayer, request.auth_epoch);
}
void key_contract::send_out(uint64_t id, name to, int64_t quantity, const std::string& memo) {
   check(is_account(to) && to != get_self() && quantity > 0 && memo.size() <= 256, "invalid withdrawal target");
   assets table(get_self(), get_self().value);
   auto it = table.require_find(id, "asset missing");
   check(!it->busy && it->reserve >= quantity && it->out_ticket != uint64_t(-1), "invalid reserve or outstanding interaction");
   check(raw_balance(it->token_contract, get_self(), it->token_symbol) == it->reserve, "reserve mismatch before withdrawal");
   const auto token = it->token_contract; const auto sym = it->token_symbol;
   const auto receiver_before = raw_balance(token, to, sym);
   if(outgoing_rows(token,to,sym)) {
      auto cfg=config();
      check(cfg.balance_count<cfg.policy.max_balances,"external token-row storage capacity reached");
      ++cfg.balance_count;
      config_table(get_self(),get_self().value).set(cfg,get_self());
   }
   const uint64_t ticket = it->out_ticket + 1;
   table.modify(it, same_payer, [&](auto& row) { row.reserve -= quantity; row.busy = true; row.notified = false;
      row.out_ticket = ticket; row.out_to = to; row.out_amount = quantity; row.receiver_before = receiver_before; });
   action(permission_level{get_self(), "active"_n}, token, "transfer"_n,
          std::make_tuple(get_self(), to, asset(quantity, sym), memo)).send();
   action(permission_level{get_self(), "active"_n}, get_self(), "finishout"_n, std::make_tuple(id, ticket)).send();
}
void key_contract::finishout(uint64_t asset_id, uint64_t ticket) {
   require_auth(get_self()); check(get_sender() == get_self(), "internal settlement only");
   assets table(get_self(), get_self().value);
   auto it = table.require_find(asset_id, "asset missing");
   check(it->busy && it->notified && it->out_ticket == ticket, "missing standard outgoing token notification");
   check(raw_balance(it->token_contract, get_self(), it->token_symbol) == it->reserve, "reserve mismatch after token interaction");
   table.modify(it, same_payer, [&](auto& row) { row.busy = false; row.notified = false;
      row.out_to = {}; row.out_amount = 0; row.receiver_before = 0; });
}
void key_contract::perform_withdraw(name payer, const withdrawal_request& request, bool direct) {
   auto cfg = config();
   verify_envelope(direct ? "selfout"_n : "withdraw"_n, request.from, request.auth_epoch, request.request_id,
                   direct ? pack(std::make_tuple(payer, request)) : pack(request), cfg);
   if (direct) { require_auth(payer); check(!request.relayer.value && !request.reward_to.value, "self withdrawal has no relay reward"); }
   else check_relay(request.relayer, request.reward_to, cfg);
   check(request.quantity.is_valid() && request.quantity.amount > 0 && request.memo.size() <= 128 &&
         is_account(request.to) && request.to != get_self(), "invalid withdrawal");
   auto id = find_asset(request.token_contract, request.quantity.symbol); check_asset(id); check_asset(1);
   auto sender = find_wallet(request.from); check(sender, "sender wallet missing");
   auto fee = quote(cfg, 6 + (request.memo.size() + 63) / 64, outgoing_rows(request.token_contract, request.to, request.quantity.symbol), !direct, direct);
   check_max(cfg, fee, request.max_fee);
   check(balance_of(sender, id) >= checked(__int128(request.quantity.amount) + (id == 1 ? fee.total : 0)), "insufficient principal balance");
   check(balance_of(sender, 1) >= fee.total, "insufficient additional AIC fee balance");
   charge_key(sender, direct ? get_self() : request.relayer, fee, request.max_fee);
   debit(sender, id, request.quantity.amount);
   send_out(id, request.to, request.quantity.amount, request.memo);
   record(direct ? "selfout"_n : "withdraw"_n, request.request_id, {}, hash_key(request.from), {}, request.token_contract,
          request.quantity, fee.total, request.relayer, request.auth_epoch);
}
void key_contract::withdraw(withdrawal_request request) { perform_withdraw({}, request, false); }
void key_contract::selfout(name payer, withdrawal_request request) { perform_withdraw(payer, request, true); }
void key_contract::perform_cancel(name payer, const cancellation_request& request, bool direct) {
   auto cfg = config();
   verify_envelope(direct ? "selfcancel"_n : "cancel"_n, request.from, request.auth_epoch, request.request_id,
                   direct ? pack(std::make_tuple(payer, request)) : pack(request), cfg);
   if (direct) { require_auth(payer); check(!request.relayer.value && !request.reward_to.value, "self cancel has no relay reward"); }
   else check_relay(request.relayer, request.reward_to, cfg);
   auto sender = find_wallet(request.from); check(sender, "sender wallet missing");
   check(request.next_epoch > request.auth_epoch, "authorization epoch must increase");
   auto fee = quote(cfg, 3, 0, !direct, direct); check_max(cfg, fee, request.max_fee);
   check(balance_of(sender, 1) >= fee.total, "insufficient additional AIC fee balance");
   epoch(sender, request.auth_epoch, request.next_epoch);
   charge_key(sender, direct ? get_self() : request.relayer, fee, request.max_fee);
   record(direct ? "selfcancel"_n : "cancel"_n, request.request_id, {}, hash_key(request.from), {}, cfg.fee_contract,
          asset(0, cfg.fee_symbol), fee.total, request.relayer, request.auth_epoch);
}
void key_contract::cancel(cancellation_request request) { perform_cancel({}, request, false); }
void key_contract::selfcancel(name payer, cancellation_request request) { perform_cancel(payer, request, true); }
void key_contract::claim(name bp, name to, asset quantity, asset max_fee) {
   require_auth(bp);
   auto cfg = config(); check_asset(1);
   check(quantity.is_valid() && quantity.symbol == cfg.fee_symbol && quantity.amount > 0, "invalid claim asset");
   auto fee = quote(cfg, 4, outgoing_rows(cfg.fee_contract, to, cfg.fee_symbol), false, true);
   charge_native(bp, fee, max_fee);
   fee_buckets table(get_self(), get_self().value);
   auto it = table.require_find(bp.value, "no relay earnings");
   check(it->relay_earned >= quantity.amount, "insufficient confirmed relay earnings");
   table.modify(it, same_payer, [&](auto& row) { row.relay_earned -= quantity.amount; });
   send_out(1, to, quantity.amount, "eosio.key relay earnings");
   record("claim"_n, {}, bp, {}, {}, cfg.fee_contract, quantity, fee.total, bp, 0);
}
void key_contract::refundfee(name owner, name to, asset quantity, asset max_fee) {
   require_auth(owner);
   auto cfg = config(); check_asset(1);
   check(quantity.is_valid() && quantity.symbol == cfg.fee_symbol && quantity.amount > 0, "invalid fee refund");
   auto fee = quote(cfg, 4, outgoing_rows(cfg.fee_contract, to, cfg.fee_symbol), false, true);
   charge_native(owner, fee, max_fee);
   native_credits table(get_self(), get_self().value);
   auto it = table.require_find(owner.value, "fee credit missing");
   check(it->amount >= quantity.amount, "insufficient fee credit after fee");
   table.modify(it, same_payer, [&](auto& row) { row.amount -= quantity.amount; });
   send_out(1, to, quantity.amount, "eosio.key prepaid fee refund");
   record("refundfee"_n, {}, owner, {}, {}, cfg.fee_contract, quantity, fee.total, {}, 0);
}
void key_contract::record(name kind, checksum256 request_id, name native_from, checksum256 from,
                           checksum256 to, name token, asset quantity, int64_t fee, name relay, uint64_t n) {
   auto raw = raw_transaction(); auto transaction_id = sha256(raw.data(), raw.size());
   action(permission_level{get_self(), "active"_n}, get_self(), "receipt"_n,
          std::make_tuple(kind, request_id, transaction_id, native_from, from, to, token, quantity,
                          asset(fee, config().fee_symbol), relay, n)).send();
}
void key_contract::receipt(name kind, checksum256 request_id, checksum256 transaction_id, name native_from, checksum256 from_key,
                          checksum256 to_key, name token_contract, asset quantity, asset fee,
                          name relayer, uint64_t auth_epoch) {
   require_auth(get_self()); check(get_sender() == get_self(), "internal receipt only");
}
}

// Notifications are dispatched by CDT's generated dispatcher alongside actions.
