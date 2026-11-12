#include <eosio/chain/parallel_pure_float.hpp>
#include <eosio/chain/parallel_worker_pool.hpp>
#include <eosio/chain/parallel_mixed_tree_journal.hpp>
#include <eosio/chain/parallel_canonical_adoption.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <eosio/testing/tester.hpp>
#include "parallel_test_runtime.hpp"
#include <eosio/testing/bls_utils.hpp>
#include <compiler_builtins.hpp>
#include <fc/io/raw.hpp>
#include <fc/filesystem.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <string>

using namespace eosio::chain;
using namespace eosio::chain::experimental;
namespace {
using fbits = unsigned __int128;
using fop = isolated_float_operation;
constexpr name faccount = "test"_n, fchild = "bob"_n;
constexpr fbits fsign = fbits{1} << 127, fall = ~fbits{0};
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(float128_t) == 16 && alignof(float128_t) == 8);
enum class fshape { quad_pair, quad, s128, u128, f32, f64, i32, i64, u32, u64 };
struct float_host { const char* name; fshape argument, result; };
constexpr std::array<float_host, 34> fhosts{{
   {"__addtf3", fshape::quad_pair, fshape::quad},
   {"__subtf3", fshape::quad_pair, fshape::quad},
   {"__multf3", fshape::quad_pair, fshape::quad},
   {"__divtf3", fshape::quad_pair, fshape::quad},
   {"__negtf2", fshape::quad, fshape::quad},
   {"__extendsftf2", fshape::f32, fshape::quad},
   {"__extenddftf2", fshape::f64, fshape::quad},
   {"__trunctfdf2", fshape::quad, fshape::f64},
   {"__trunctfsf2", fshape::quad, fshape::f32},
   {"__fixtfsi", fshape::quad, fshape::i32},
   {"__fixtfdi", fshape::quad, fshape::i64},
   {"__fixtfti", fshape::quad, fshape::s128},
   {"__fixunstfsi", fshape::quad, fshape::u32},
   {"__fixunstfdi", fshape::quad, fshape::u64},
   {"__fixunstfti", fshape::quad, fshape::u128},
   {"__fixsfti", fshape::f32, fshape::s128},
   {"__fixdfti", fshape::f64, fshape::s128},
   {"__fixunssfti", fshape::f32, fshape::u128},
   {"__fixunsdfti", fshape::f64, fshape::u128},
   {"__floatsidf", fshape::i32, fshape::f64},
   {"__floatsitf", fshape::i32, fshape::quad},
   {"__floatditf", fshape::u64, fshape::quad},
   {"__floatunsitf", fshape::u32, fshape::quad},
   {"__floatunditf", fshape::u64, fshape::quad},
   {"__floattidf", fshape::quad, fshape::f64},
   {"__floatuntidf", fshape::quad, fshape::f64},
   {"__eqtf2", fshape::quad_pair, fshape::i32},
   {"__netf2", fshape::quad_pair, fshape::i32},
   {"__getf2", fshape::quad_pair, fshape::i32},
   {"__gttf2", fshape::quad_pair, fshape::i32},
   {"__letf2", fshape::quad_pair, fshape::i32},
   {"__lttf2", fshape::quad_pair, fshape::i32},
   {"__cmptf2", fshape::quad_pair, fshape::i32},
   {"__unordtf2", fshape::quad_pair, fshape::i32},
}};
struct float_environment_restore {
   const uint_fast8_t rounding = softfloat_roundingMode, tininess = softfloat_detectTininess,
                     flags = softfloat_exceptionFlags, precision = extF80_roundingPrecision;
   ~float_environment_restore() {
      softfloat_roundingMode = rounding; softfloat_detectTininess = tininess;
      softfloat_exceptionFlags = flags; extF80_roundingPrecision = precision;
   }
};
template<class T> void fput(std::vector<char>& data, size_t offset, T value) { std::memcpy(data.data() + offset, &value, sizeof(value)); }
template<class T> T fget(const std::vector<char>& data, size_t offset) { T value; std::memcpy(&value, data.data() + offset, sizeof(value)); return value; }
fbits fwords(uint64_t low, uint64_t high) { return (fbits{high} << 64) | low; }
float128_t fquad(fbits value) { return {{uint64_t(value), uint64_t(value >> 64)}}; }
fbits fquadbits(float128_t value) { return fwords(value.v[0], value.v[1]); }
size_t fwidth(unsigned op) {
   switch (fhosts.at(op).result) {
      case fshape::f32: case fshape::i32: case fshape::u32: return 4;
      case fshape::f64: case fshape::i64: case fshape::u64: return 8;
      default: return 16;
   }
}
bool fpointer(unsigned op) { return fwidth(op) == 16; }
bool funsafe(unsigned op, fbits a) {
   // Independent test classification of documented original C UB boundaries.
   return (op == 11 && ((uint64_t(a >> 64) >> 48) & 0x7fff) == 16510) ||
          (op == 15 && ((uint32_t(a) >> 23) & 0xff) == 254) ||
          (op == 16 && ((uint64_t(a) >> 52) & 0x7ff) == 1150) ||
          (op == 24 && a == fsign);
}
// Native ORIGINAL helper comparison, not a replacement for actual original
// WASM below. Do not invoke known-UB original C cases in this native oracle.
std::optional<fbits> fbaseline(unsigned op, fbits a, fbits b = 0) {
   if (funsafe(op, a)) return {};
   const auto x = fquad(a), y = fquad(b);
   const auto i32 = [](int32_t v) -> fbits { return uint32_t(v); };
   const auto i64 = [](int64_t v) -> fbits { return uint64_t(v); };
   switch (op) {
      case 0: return fquadbits(f128_add(x, y));
      case 1: return fquadbits(f128_sub(x, y));
      case 2: return fquadbits(f128_mul(x, y));
      case 3: return fquadbits(f128_div(x, y));
      case 4: return fwords(x.v[0], x.v[1] ^ (uint64_t{1} << 63));
      case 5: return fquadbits(f32_to_f128({uint32_t(a)}));
      case 6: return fquadbits(f64_to_f128({uint64_t(a)}));
      case 7: return f128_to_f64(x).v;
      case 8: return f128_to_f32(x).v;
      case 9: return i32(f128_to_i32(x, 0, false));
      case 10: return i64(f128_to_i64(x, 0, false));
      case 11: return std::bit_cast<fbits>(___fixtfti(x));
      case 12: return uint32_t(f128_to_ui32(x, 0, false));
      case 13: return uint64_t(f128_to_ui64(x, 0, false));
      case 14: return ___fixunstfti(x);
      case 15: return std::bit_cast<fbits>(___fixsfti(uint32_t(a)));
      case 16: return std::bit_cast<fbits>(___fixdfti(uint64_t(a)));
      case 17: return ___fixunssfti(uint32_t(a));
      case 18: return ___fixunsdfti(uint64_t(a));
      case 19: return i32_to_f64(std::bit_cast<int32_t>(uint32_t(a))).v;
      case 20: return fquadbits(i32_to_f128(std::bit_cast<int32_t>(uint32_t(a))));
      case 21: return fquadbits(i64_to_f128(std::bit_cast<int64_t>(uint64_t(a))));
      case 22: return fquadbits(ui32_to_f128(uint32_t(a)));
      case 23: return fquadbits(ui64_to_f128(uint64_t(a)));
      case 24: return std::bit_cast<uint64_t>(___floattidf(std::bit_cast<__int128>(a)));
      case 25: return std::bit_cast<uint64_t>(___floatuntidf(a));
      default: {
         if (op >= fhosts.size()) throw std::logic_error("bad float reference opcode");
         const bool nan = f128_is_nan(x) || f128_is_nan(y);
         if (op == 33) return uint32_t(nan);
         constexpr std::array<int32_t, 7> nan_answers{1, 1, -1, 0, 1, 0, 1};
         return i32(nan ? nan_answers[op - 26] : f128_lt(x, y) ? -1 : f128_eq(x, y) ? 0 : 1);
      }
   }
}
std::vector<char> fbytes(fbits value, size_t size) {
   const auto* begin = reinterpret_cast<const char*>(&value); return {begin, begin + size};
}
isolated_action_input finput(unsigned op, fbits a, fbits b = 0, uint32_t output = 64) {
   isolated_action_input input;
   input.receiver = input.account = faccount; input.action = "run"_n;
   input.authorizations = {{faccount, "active"_n}}; input.data.resize(256);
   fput(input.data, 0, uint32_t(op)); fput(input.data, 4, output);
   fput(input.data, 8, a); fput(input.data, 24, b); fput(input.data, 56, uint32_t(fwidth(op)));
   return input;
}
isolated_action_input fdefault(unsigned op) {
   switch (fhosts[op].argument) {
      case fshape::f32: return finput(op, 0x3fc00000u);
      case fshape::f64: return finput(op, 0x3ff8000000000000ull);
      case fshape::i32: case fshape::u32: case fshape::u64: return finput(op, 7);
      default:
         if (op == 24 || op == 25) return finput(op, (fbits{1} << 54) + 3);
         return finput(op, fbits{0x3fff800000000000ull} << 64, fbits{0x4000400000000000ull} << 64);
   }
}
std::optional<fbits> fexpected(const isolated_action_input& input) {
   return fbaseline(fget<uint32_t>(input.data, 0), fget<fbits>(input.data, 8), fget<fbits>(input.data, 24));
}

std::vector<fbits> fvalues(unsigned op) {
   const fbits infinity = fbits{0x7fff} << 112, one = fbits{0x3fff} << 112;
   if (op == 24 || op == 25)
      return {0, 1, UINT64_MAX, fbits{1} << 64, (fbits{1} << 53) - 1, (fbits{1} << 53) + 1,
         (fbits{1} << 54) + 2, (fbits{1} << 54) + 3, (fbits{1} << 54) + 6,
         (fbits{1} << 55) - 1, fsign - 1, fsign, fsign + 1, fall - 6, fall};
   if (fhosts[op].argument == fshape::f32)
      return {0, 0x80000000u, 1, 0x80000001u, 0x007fffffu, 0x00800000u, 0x3f000000u,
         0x3fc00000u, 0x40200000u, 0xbfc00000u, 0xc0200000u, 0x7e800000u, 0x7f000000u,
         0xff000000u, 0x7f7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00019u, 0xffc00019u, 0x7f800001u};
   if (fhosts[op].argument == fshape::f64)
      return {0, 0x8000000000000000ull, 1, 0x8000000000000001ull, 0x000fffffffffffffull,
         0x0010000000000000ull, 0x3fe0000000000000ull, 0x3ff8000000000000ull, 0x4004000000000000ull,
         0xbff8000000000000ull, 0xc004000000000000ull, 0x47d0000000000000ull, 0x47e0000000000000ull,
         0xc7e0000000000000ull, 0x47f0000000000000ull, 0x7fefffffffffffffull, 0x7ff0000000000000ull,
         0xfff0000000000000ull, 0x7ff8000000000019ull, 0xfff8000000000019ull, 0x7ff0000000000001ull};
   if (fhosts[op].argument == fshape::i32 || fhosts[op].argument == fshape::u32)
      return {0, 1, 2, 0x7fffffffu, 0x80000000u, 0x80000001u, UINT32_MAX};
   if (fhosts[op].argument == fshape::u64)
      return {0, 1, UINT32_MAX, uint64_t{1} << 32, (uint64_t{1} << 63) - 1, uint64_t{1} << 63,
         (uint64_t{1} << 63) + 1, UINT64_MAX};
   return {0, fsign, 1, fsign | 1, (fbits{1} << 112) - 1, fbits{1} << 112,
      one, one | (fbits{1} << 111), fsign | one | (fbits{1} << 111),
      (fbits{0x4000} << 112) | (fbits{1} << 110), infinity, infinity | fsign,
      infinity | (fbits{1} << 111) | 19, infinity | 23, infinity - 1, (infinity - 1) | fsign,
      fbits{0x401e} << 112, fbits{0x403e} << 112, fbits{0x403f} << 112,
      (fbits{0x407d} << 112) | 31, fbits{0x407e} << 112, fsign | (fbits{0x407e} << 112),
      (fbits{0x407e} << 112) | ((fbits{1} << 112) - 1), fbits{0x407f} << 112};
}
std::vector<isolated_action_input> fcorpus(unsigned op) {
   const auto values = fvalues(op); std::vector<isolated_action_input> result;
   if (fhosts[op].argument == fshape::quad_pair) {
      // All sixteen pairwise IEEE classes, plus larger conversion bounds below
      // in unary hosts. Includes distinct NaN payloads, signed zero and infinity.
      for (size_t a = 0; a < 16; ++a) for (size_t b = 0; b < 16; ++b)
         result.push_back(finput(op, values[a], values[b]));
   } else for (auto value : values) result.push_back(finput(op, value));
   for (uint32_t out : {65u, 8u, 9u, 16u, 17u, 24u, uint32_t(65536 - fwidth(op)), uint32_t(65535 - fwidth(op))}) {
      auto input = fdefault(op); fput(input.data, 4, out); result.push_back(std::move(input));
   }
   return result;
}
std::string fc32(uint32_t v) { return "(i32.const " + std::to_string(v) + ")"; }
std::string fscalar(fshape shape) {
   if (shape == fshape::f32) return "f32";
   if (shape == fshape::f64) return "f64";
   if (shape == fshape::i32 || shape == fshape::u32) return "i32";
   return "i64";
}
std::string fimport(unsigned op) {
   const auto& item = fhosts[op];
   std::string params = fpointer(op) ? "i32 " : "";
   if (item.argument == fshape::quad_pair) params += "i64 i64 i64 i64";
   else if (item.argument == fshape::quad) params += "i64 i64";
   else params += fscalar(item.argument);
   return "(import \"env\" \"" + std::string(item.name) + "\" (func $" + item.name + " (param " + params + ")" +
      (fpointer(op) ? "" : " (result " + fscalar(item.result) + ")") + "))";
}
std::string fcall(unsigned op) {
   const auto& item = fhosts[op];
   const std::string output = "(i32.load (i32.const 4))";
   std::string args = fpointer(op) ? output : "";
   if (item.argument == fshape::quad || item.argument == fshape::quad_pair) {
      args += "(i64.load (i32.const 8))(i64.load (i32.const 16))";
      if (item.argument == fshape::quad_pair) args += "(i64.load (i32.const 24))(i64.load (i32.const 32))";
   } else args += "(" + fscalar(item.argument) + ".load (i32.const 8))";
   const std::string call = "(call $" + std::string(item.name) + args + ")";
   return "(i32.store (i32.const 56) " + fc32(fwidth(op)) + ")" +
      (fpointer(op) ? call : "(" + fscalar(item.result) + ".store " + output + call + ")");
}
std::vector<char> fwasm(const std::string& text) { const auto code = wast_to_wasm(text); return {code.begin(), code.end()}; }
std::vector<char> fprogram(unsigned group, bool write_first = false) {
   std::string imports, dispatch, prefix;
   for (unsigned op = group * 17; op < (group + 1) * 17; ++op) {
      imports += fimport(op);
      dispatch += "(if (i32.eq (i32.load (i32.const 0)) " + fc32(op) + ") (then " + fcall(op) + "(return)))";
   }
   if (write_first) {
      imports += R"((import "env" "db_store_i64" (func $store (param i64 i64 i64 i64 i32 i32) (result i32))))";
      prefix = "(drop (call $store (get_local $receiver) (i64.const " + std::to_string("floatrows"_n.to_uint64_t()) +
         ") (get_local $receiver) (i64.extend_u/i32 (i32.load (i32.const 48))) (i32.const 8) (i32.const 16)))";
   }
   return fwasm("(module " + imports + R"(
      (import "env" "read_action_data" (func $read (param i32 i32) (result i32)))
      (import "env" "set_action_return_value" (func $out (param i32 i32)))
      (import "env" "get_call_data" (func $get (param i32 i32) (result i32)))
      (import "env" "set_call_return_value" (func $set (param i32 i32)))
      (import "env" "call" (func $call (param i64 i64 i32 i32) (result i64)))
      (import "env" "get_call_return_value" (func $returned (param i32 i32) (result i32)))
      (import "env" "send_context_free_inline" (func $cfi (param i32 i32)))
      (import "env" "eosio_assert_code" (func $check (param i32 i64)))
      (import "env" "memcmp" (func $compare (param i32 i32 i32) (result i32)))
      (memory 1 1)
      (func $compute )" + dispatch + R"((unreachable))
      (func (export "apply") (param $receiver i64) (param i64) (param $action i64) (local $length i32)
         (set_local $length (call $read (i32.const 0) (i32.const 65536))) )" + prefix + R"(
         (call $compute)
         )" + "(if (i64.eq (get_local $action) (i64.const " + std::to_string("pure"_n.to_uint64_t()) + R"()) (then
            (call $check (i32.eqz (call $compare (i32.load (i32.const 4)) (i32.const 128) (i32.load (i32.const 56)))) (i64.const 921))
            (return)))
         (if (i32.and (i32.load (i32.const 44)) (i32.const 1)) (then
            (call $check (i64.eq (call $call (get_local $receiver) (i64.const 0) (i32.const 0) (get_local $length))
               (i64.extend_u/i32 (i32.load (i32.const 56)))) (i64.const 922))
            (call $check (i32.eq (call $returned (i32.const 48000) (i32.load (i32.const 56)))
               (i32.load (i32.const 56))) (i64.const 923))
            (call $check (i32.eqz (call $compare (i32.load (i32.const 4)) (i32.const 48000) (i32.load (i32.const 56)))) (i64.const 924))))
         (if (i32.and (i32.load (i32.const 44)) (i32.const 2)) (then
            (call $cfi (i32.const 1024) (i32.load (i32.const 52)))))
         (call $out (i32.load (i32.const 4)) (i32.load (i32.const 56))))
      (func (export "sync_call") (param i64 i64 i32) (result i64)
         (drop (call $get (i32.const 0) (i32.const 65536))) (call $compute)
         (call $set (i32.load (i32.const 4)) (i32.load (i32.const 56))) (i64.const 0))))");
}
isolated_action_input fgraph(unsigned op, unsigned route) {
   auto root = fdefault(op); fput(root.data, 44, uint32_t(route));
   if (route & 2) {
      auto child = fdefault(op); child.receiver = child.account = fchild;
      const auto expected = fexpected(child); BOOST_REQUIRE(expected);
      const auto bytes = fbytes(*expected, fwidth(op)); std::copy(bytes.begin(), bytes.end(), child.data.begin() + 128);
      const auto packed = fc::raw::pack(action{{}, fchild, "pure"_n, child.data});
      root.data.resize(1024 + packed.size()); std::copy(packed.begin(), packed.end(), root.data.begin() + 1024);
      fput(root.data, 52, uint32_t(packed.size()));
   }
   return root;
}
std::vector<char> fstart(unsigned op) {
   const auto initial = fdefault(op).data; std::string data;
   constexpr char hex[] = "0123456789abcdef";
   for (unsigned char byte : initial) { data += '\\'; data += hex[byte >> 4]; data += hex[byte & 15]; }
   return fwasm("(module " + fimport(op) + R"(
      (import "env" "set_action_return_value" (func $out (param i32 i32))) (memory 1 1)
      (data (i32.const 0) ")" + data + "\") (func $begin " + fcall(op) + R"() (start $begin)
      (func (export "apply") (param i64 i64 i64)
         (call $out (i32.const 64) )" + fc32(fwidth(op)) + R"()
         (i64.store (i32.const 64) (i64.const 0)) (i64.store (i32.const 72) (i64.const 0)))))");
}
struct float_fixture {
   fc::temp_directory directory;
   chainbase::database db{directory.path(), chainbase::database::read_write, 8 * 1024 * 1024};
   chainbase_transaction_state_view view{db};
   std::shared_ptr<const frozen_primary_state> base;
   const std::array<std::vector<char>, 2> code{fprogram(0), fprogram(1)};
   float_fixture() {
      db.add_index<table_id_multi_index>(); db.add_index<key_value_index>(); db.add_index<account_index>();
      const std::array accounts{faccount, fchild};
      for (name account : accounts) db.create<account_object>([&](auto& a) { a.name = account; });
      base = frozen_primary_state::capture_queries(view, {}, {}, {}, {}, accounts);
   }
};
transaction_trace_ptr fpush(eosio::testing::tester& chain, const isolated_action_input& input, uint32_t nonce) {
   auto bytes = input.data; fput(bytes, 48, nonce);
   signed_transaction trx; trx.actions.push_back(action{{{faccount, "active"_n}}, faccount, input.action, bytes});
   chain.set_transaction_headers(trx); trx.sign(chain.get_private_key(faccount, "active"), chain.control->get_chain_id());
   return chain.push_transaction(trx);
}
} // namespace


BOOST_AUTO_TEST_SUITE(parallel_float_private_tests)

BOOST_AUTO_TEST_CASE(all_float_values_match_original_numeric_helpers_with_explicit_known_bits) {
   float_environment_restore restore;
   BOOST_REQUIRE(isolated_numeric_environment_compatible());
   for (unsigned op = 0; op < fhosts.size(); ++op) BOOST_TEST_CONTEXT(fhosts[op].name) {
      for (const auto& input : fcorpus(op)) {
         const auto a = fget<fbits>(input.data, 8), b = fget<fbits>(input.data, 24);
         const auto expected = fbaseline(op, a, b), actual = isolated_float_compute(static_cast<fop>(op), a, b);
         BOOST_CHECK(actual.has_value() == expected.has_value());
         if (expected) { BOOST_REQUIRE(actual); BOOST_CHECK(*actual == *expected); }
      }
   }
   const fbits one = fbits{0x3fff} << 112, half = fbits{1} << 111, infinity = fbits{0x7fff} << 112;
   BOOST_CHECK(isolated_float_compute(fop::addtf3, one, one) == (fbits{0x4000} << 112));
   BOOST_CHECK(isolated_float_compute(fop::divtf3, one, 0) == infinity);
   BOOST_CHECK(isolated_float_compute(fop::negtf2, infinity | 23) == (fsign | infinity | 23));
   BOOST_CHECK(isolated_float_compute(fop::fixtfsi, one | half) == 2);
   BOOST_CHECK(isolated_float_compute(fop::fixtfsi, (fbits{0x4000} << 112) | (fbits{1} << 110)) == 2);
   BOOST_CHECK(isolated_float_compute(fop::fixtfsi, fsign | one | half) == uint32_t(-2));
   BOOST_CHECK(isolated_float_compute(fop::fixtfti, one | half) == 1);
   BOOST_CHECK(isolated_float_compute(fop::fixsfti, 0x3fc00000u) == 1);
   BOOST_CHECK(isolated_float_compute(fop::fixdfti, 0x3ff8000000000000ull) == 1);
   BOOST_CHECK(isolated_float_compute(fop::floattidf, fbits{7}) == 0x401c000000000000ull);
   BOOST_CHECK(isolated_float_compute(fop::floatuntidf, fall) == 0x47f0000000000000ull);
   const std::array<uint32_t, 8> nan_values{1, 1, uint32_t(-1), 0, 1, 0, 1, 1};
   for (unsigned op = 26; op < 34; ++op)
      BOOST_CHECK(isolated_float_compute(static_cast<fop>(op), infinity | 23, one) == nan_values[op - 26]);
   BOOST_CHECK(!isolated_float_compute(static_cast<fop>(255), one));
}

BOOST_FIXTURE_TEST_CASE(thirty_four_typed_hosts_cover_bits_aliases_legacy_pointers_and_failed_reuse, float_fixture) {
   float_environment_restore restore;
   for (unsigned group = 0; group < 2; ++group) {
      isolated_wasm_module module(code[group]);
      for (unsigned op = group * 17; op < (group + 1) * 17; ++op) BOOST_TEST_CONTEXT(fhosts[op].name) {
         for (const auto& input : fcorpus(op)) {
            const auto expected = fexpected(input);
            if (!expected) { BOOST_CHECK_THROW(module.execute(base, input), primary_state_unsupported); continue; }
            const auto result = module.execute(base, input);
            BOOST_CHECK(result.return_value == fbytes(*expected, fwidth(op)));
            BOOST_CHECK(result.state.writes().empty());
         }
         if (fpointer(op)) for (uint32_t address : {0u, 65521u, UINT32_MAX - 8, UINT32_MAX}) {
            auto bad = fdefault(op); fput(bad.data, 4, address);
            BOOST_CHECK_THROW(module.execute(base, bad), primary_state_unsupported);
            const auto good = fdefault(op);
            BOOST_CHECK(module.execute(base, good).return_value == fbytes(*fexpected(good), fwidth(op)));
         }
         std::stop_source stopped; stopped.request_stop();
         BOOST_CHECK_THROW(module.execute(base, fdefault(op), stopped.get_token()), primary_state_invalid_attempt);
         isolated_cancel expired; expired.deadline = std::chrono::steady_clock::now();
         BOOST_CHECK_THROW(module.execute(base, fdefault(op), {}, {}, expired), isolated_execution_interrupted);
      }
   }
}

BOOST_FIXTURE_TEST_CASE(float_start_functions_reset_every_original_scalar_return_shape, float_fixture) {
   for (unsigned op = 0; op < fhosts.size(); ++op) BOOST_TEST_CONTEXT(fhosts[op].name) {
      const auto input = fdefault(op); isolated_wasm_module module(fstart(op));
      for (unsigned round = 0; round < 3; ++round)
         BOOST_CHECK(module.execute(base, input).return_value == fbytes(*fexpected(input), fwidth(op)));
   }
}

BOOST_FIXTURE_TEST_CASE(float_calls_work_in_context_free_sync_and_mixed_trees, float_fixture) {
   for (unsigned group = 0; group < 2; ++group) {
      const std::vector<isolated_receiver_program> ordinary{{faccount, code[group], {}}, {fchild, code[group], {}}};
      const std::vector<isolated_sync_program> programs{{ordinary[0], true}, {ordinary[1], true}};
      isolated_action_tree_context tree; tree.context_free_data = isolated_context_free_data::capture({});
      isolated_sync_context sync; sync.get_sender_fix = true;
      isolated_action_tree_executor cf_executor; isolated_sync_executor sync_executor; isolated_mixed_tree_executor mixed;
      for (unsigned op = group * 17; op < (group + 1) * 17; ++op) BOOST_TEST_CONTEXT(fhosts[op].name) {
         const auto expected = fbytes(*fexpected(fdefault(op)), fwidth(op));
         const std::array roots{fgraph(op, 2)};
         const auto cf = cf_executor.execute(base, roots, ordinary, tree);
         BOOST_REQUIRE(validate_isolated_action_tree_journal(cf, roots, ordinary, tree));
         BOOST_REQUIRE_EQUAL(cf.steps.size(), 2u); BOOST_CHECK(cf.groups[cf.steps[1].group_index].context_free);
         BOOST_CHECK(cf.steps[0].effects.return_value == expected); BOOST_CHECK(cf.steps[1].effects.return_value.empty());
         const auto sync_root = fgraph(op, 1);
         const auto called = sync_executor.execute(base, sync_root, programs, sync);
         BOOST_REQUIRE(validate_isolated_sync_journal(called, sync_root, programs, sync));
         BOOST_REQUIRE_EQUAL(called.calls.size(), 1u); BOOST_CHECK_EQUAL(called.calls[0].status, 0);
         BOOST_CHECK(called.calls[0].return_value == expected);
         const std::array mixed_roots{fgraph(op, 3)}; const isolated_mixed_tree_context context{tree, sync};
         const auto both = mixed.execute(base, mixed_roots, programs, context);
         BOOST_REQUIRE(validate_isolated_mixed_tree_journal(both, mixed_roots, programs, context));
         BOOST_REQUIRE_EQUAL(both.steps.size(), 2u); BOOST_REQUIRE_EQUAL(both.steps[0].calls.size(), 1u);
         BOOST_CHECK(both.steps[0].calls[0].return_value == expected); BOOST_CHECK(both.steps[1].effects.return_value.empty());
         BOOST_CHECK(both.state.writes().empty());
      }
   }
}

BOOST_FIXTURE_TEST_CASE(float_worker_pools_keep_success_failure_and_numeric_state_isolated, float_fixture) {
   for (unsigned workers : {1u, 2u, 4u}) {
      isolated_worker_pool pool(workers);
      for (unsigned round = 0; round < 3; ++round) {
         std::vector<isolated_action_input> inputs; std::vector<ordered_attempt_batch::request> requests;
         for (unsigned op = 0; op < fhosts.size(); ++op) {
            auto input = fdefault(op);
            if (round == 1 && fpointer(op)) fput(input.data, 4, uint32_t{0});
            const auto hash = fc::sha256::hash(code[op / 17].data(), code[op / 17].size());
            const std::array fingerprints{isolated_execution_fingerprint(hash, {}, input)};
            requests.push_back({fc::sha256::hash(std::to_string(op)), isolated_transaction_fingerprint(fingerprints)});
            inputs.push_back(std::move(input));
         }
         ordered_attempt_batch batch(base, requests); std::vector<isolated_worker_job> jobs;
         for (unsigned op = 0; op < fhosts.size(); ++op)
            jobs.emplace_back(batch.get_ticket(op), code[op / 17], inputs[op], isolated_wasm_limits{});
         const auto results = pool.run(std::move(jobs)); BOOST_REQUIRE_EQUAL(results.size(), fhosts.size());
         for (unsigned op = 0; op < results.size(); ++op) BOOST_TEST_CONTEXT("workers=" << workers << " round=" << round << " " << fhosts[op].name) {
            const auto& result = results[op]; BOOST_CHECK(!result.request && !result.receiver_request);
            if (round == 1 && fpointer(op)) {
               BOOST_CHECK(!result.result); BOOST_CHECK(result.failure == isolated_worker_failure::unsupported);
            } else {
               BOOST_REQUIRE_MESSAGE(result.result, isolated_worker_failure_name(result.failure));
               BOOST_CHECK(result.failure == isolated_worker_failure::none); BOOST_CHECK(result.result->state.writes().empty());
               BOOST_CHECK(result.result->actions[0].return_value == fbytes(*fexpected(inputs[op]), fwidth(op)));
            }
         }
      }
   }
}

BOOST_FIXTURE_TEST_CASE(nondefault_numeric_environments_reject_before_computation_and_restore_cleanly, float_fixture) {
   float_environment_restore restore;
   const auto flags = softfloat_exceptionFlags;
   isolated_wasm_module module(code[0]);
   for (unsigned mode = 0; mode < 3; ++mode) {
      float_environment_restore inner;
      if (mode == 0) softfloat_roundingMode = softfloat_round_max;
      if (mode == 1) softfloat_detectTininess = softfloat_tininess_beforeRounding;
      if (mode == 2) extF80_roundingPrecision = 32;
      BOOST_CHECK(!isolated_numeric_environment_compatible());
      for (unsigned op = 0; op < 34; ++op) {
         const auto input = fdefault(op);
         BOOST_CHECK(!isolated_float_compute(static_cast<fop>(op), fget<fbits>(input.data, 8), fget<fbits>(input.data, 24)));
      }
      BOOST_CHECK_THROW(module.execute(base, fdefault(0)), primary_state_unsupported);
      BOOST_CHECK_EQUAL(softfloat_exceptionFlags, flags);
   }
   BOOST_REQUIRE(isolated_numeric_environment_compatible());
   BOOST_CHECK(module.execute(base, fdefault(0)).return_value == fbytes(*fexpected(fdefault(0)), 16));
}
BOOST_AUTO_TEST_SUITE_END()


namespace {
struct float_live_chain : eosio::testing::tester {
   using tester::tester;
   void start() { _start_block(head().block_time() + fc::microseconds(config::block_interval_us)); }
};
struct float_live_corpus {
   genesis_state genesis = eosio::testing::base_tester::default_genesis();
   std::vector<char> code;
   std::vector<std::vector<char>> blocks;
   controller::config configuration(const fc::temp_directory& dir, uint16_t workers, bool production = false) const {
      auto cfg = eosio::testing::base_tester::default_config(dir).first;
      eosio::testing::configure_parallel_test_runtime(cfg); cfg.contracts_console = true;
      cfg.parallel_validation_threads = workers; cfg.parallel_production_enabled = production && workers != 0;
      cfg.parallel_code_allowlist = {fc::sha256::hash(code.data(), code.size())}; return cfg;
   }
   explicit float_live_corpus(unsigned group) : code(fprogram(group)) {
      auto [key, pub, proof] = eosio::testing::get_bls_key("eosio"_n);
      genesis.initial_savanna.emplace(); genesis.initial_savanna->finalizer_key = pub; genesis.initial_savanna->proof_of_possession = proof;
      fc::temp_directory dir; float_live_chain source(configuration(dir, 0), genesis);
      source.set_node_finalizers(std::array<name, 1>{"eosio"_n});
      const auto seal = [&] { blocks.push_back(fc::raw::pack(*source.produce_block())); };
      source.set_bios_contract(); seal();
      source.activate_builtin_protocol_features({builtin_protocol_feature_t::sync_call, builtin_protocol_feature_t::get_sender_fix});
      seal(); seal(); source.create_accounts({faccount, fchild}); seal();
      for (name account : {faccount, fchild}) source.set_code(account, std::vector<uint8_t>(code.begin(), code.end()));
      seal();
   }
   void prefix(eosio::testing::tester& chain) const {
      for (const auto& block : blocks) {
         chain.push_block(std::make_shared<signed_block>(fc::raw::unpack<signed_block>(block))); chain.control->abort_block();
      }
   }
};
std::vector<char> ftrace(const transaction_trace_ptr& trace) {
   BOOST_REQUIRE_MESSAGE(trace && !trace->except && !trace->failed_dtrx_trace,
      (trace && trace->except ? trace->except->to_detail_string() : "missing float trace"));
   std::vector<std::vector<char>> fields{fc::raw::pack(trace->id), fc::raw::pack(trace->net_usage), fc::raw::pack(trace->receipt),
      fc::raw::pack(trace->account_ram_delta), fc::raw::pack(trace->error_code)};
   for (const auto& action : trace->action_traces) {
      BOOST_REQUIRE(!action.except && !action.error_code);
      fields.push_back(fc::raw::pack(action.action_ordinal)); fields.push_back(fc::raw::pack(action.creator_action_ordinal));
      fields.push_back(fc::raw::pack(action.closest_unnotified_ancestor_action_ordinal)); fields.push_back(fc::raw::pack(action.receiver));
      fields.push_back(fc::raw::pack(action.act)); fields.push_back(fc::raw::pack(action.receipt)); fields.push_back(fc::raw::pack(action.context_free));
      fields.push_back(action.return_value); fields.push_back(fc::raw::pack(action.account_ram_deltas));
      fields.push_back(fc::raw::pack(action.console)); fields.push_back(fc::raw::pack(action.console_markers));
      for (const auto& call : action.call_traces) {
         BOOST_REQUIRE(!call.except && !call.error_code);
         fields.push_back(fc::raw::pack(call.call_ordinal)); fields.push_back(fc::raw::pack(call.sender_ordinal));
         fields.push_back(fc::raw::pack(call.receiver)); fields.push_back(fc::raw::pack(call.read_only)); fields.push_back(call.data);
         fields.push_back(call.return_value); fields.push_back(fc::raw::pack(call.error_id)); fields.push_back(fc::raw::pack(call.console));
         fields.push_back(fc::raw::pack(call.console_markers));
      }
   }
   return fc::raw::pack(fields); // local wall/CPU diagnostics only are excluded
}
transaction_metadata_ptr fmetadata(eosio::testing::tester& chain, const isolated_action_input& input, uint32_t nonce) {
   auto bytes = input.data; fput(bytes, 48, nonce);
   signed_transaction trx;
   trx.actions.push_back(action{{{faccount, "active"_n}}, faccount, input.action, bytes});
   chain.set_transaction_headers(trx); trx.sign(chain.get_private_key(faccount, "active"), chain.control->get_chain_id());
   return transaction_metadata::recover_keys(std::make_shared<packed_transaction>(trx, packed_transaction::compression_type::none),
      chain.control->get_chain_id(), fc::seconds(5), transaction_metadata::trx_type::input);
}
} // namespace

BOOST_AUTO_TEST_SUITE(parallel_float_oracle_tests)

BOOST_AUTO_TEST_CASE(selected_float_profile_configures_canonical_and_private_engines) {
   float_live_corpus corpus(0);
   const bool jit = eosio::testing::base_tester::arguments_contains("--eos-vm-jit");
   for (uint16_t workers : {0, 1, 4}) for (bool production : {false, true}) {
      fc::temp_directory directory;
      const auto cfg = corpus.configuration(directory, workers, production);
      BOOST_CHECK(cfg.wasm_runtime == (jit ? wasm_interface::vm_type::eos_vm_jit : wasm_interface::vm_type::eos_vm));
      BOOST_CHECK(cfg.parallel_engine == (jit ? isolated_vm_engine::jit : isolated_vm_engine::interpreter));
      BOOST_CHECK_EQUAL(cfg.parallel_validation_threads, workers);
      BOOST_CHECK_EQUAL(cfg.parallel_production_enabled, production && workers != 0);
   }
}

BOOST_AUTO_TEST_CASE(actual_original_wasm_matches_all_float_signatures_bits_aliases_and_guarded_cases) {
   eosio::testing::tester original; original.create_accounts({faccount});
   uint32_t nonce = 0;
   for (unsigned group = 0; group < 2; ++group) {
      const auto code = fprogram(group); original.set_code(faccount, std::vector<uint8_t>(code.begin(), code.end())); original.produce_block();
      chainbase_transaction_state_view live(original.control->db()); const auto base = frozen_primary_state::capture(live, {});
      isolated_wasm_module module(code);
      for (unsigned op = group * 17; op < (group + 1) * 17; ++op) BOOST_TEST_CONTEXT(fhosts[op].name) {
         for (auto input : fcorpus(op)) {
            if (nonce && nonce % 32 == 0) original.produce_block(); // retain original per-block CPU allowance
            fput(input.data, 48, ++nonce);
            const auto expected = fexpected(input);
            if (!expected) {
               BOOST_CHECK_THROW(module.execute(base, input), primary_state_unsupported);
               const auto trace = fpush(original, input, nonce); BOOST_REQUIRE(trace && !trace->except);
               BOOST_CHECK_EQUAL(trace->action_traces[0].return_value.size(), fwidth(op));
               continue; // original known-UB C behavior is not redefined or used as private arithmetic
            }
            const auto candidate = module.execute(base, input);
            const auto trace = fpush(original, input, nonce); BOOST_REQUIRE(trace && !trace->except);
            BOOST_REQUIRE_EQUAL(trace->action_traces.size(), 1u);
            BOOST_CHECK(trace->action_traces[0].return_value == fbytes(*expected, fwidth(op)));
            BOOST_CHECK(candidate.return_value == trace->action_traces[0].return_value);
         }
         if (fpointer(op)) for (uint32_t address : {0u, 65521u, UINT32_MAX - 8, UINT32_MAX}) {
            if (nonce && nonce % 32 == 0) original.produce_block();
            auto input = fdefault(op); fput(input.data, 4, address); fput(input.data, 48, ++nonce);
            const auto before = original.control->calculate_integrity_hash();
            BOOST_CHECK_THROW(module.execute(base, input), primary_state_unsupported);
            BOOST_CHECK_THROW(fpush(original, input, nonce), fc::exception);
            BOOST_CHECK(original.control->calculate_integrity_hash() == before);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(original_float_start_functions_match_and_reset_across_reuse) {
   eosio::testing::tester original; original.create_accounts({faccount});
   chainbase_transaction_state_view live(original.control->db()); const auto base = frozen_primary_state::capture(live, {});
   uint32_t nonce = 0;
   for (unsigned op = 0; op < fhosts.size(); ++op) BOOST_TEST_CONTEXT(fhosts[op].name) {
      const auto code = fstart(op); original.set_code(faccount, std::vector<uint8_t>(code.begin(), code.end())); original.produce_block();
      isolated_wasm_module module(code); const auto input = fdefault(op);
      for (unsigned round = 0; round < 3; ++round) {
         const auto candidate = module.execute(base, input);
         const auto expected = fbytes(*fexpected(input), fwidth(op));
         const auto trace = fpush(original, input, ++nonce); BOOST_REQUIRE(trace && !trace->except);
         BOOST_CHECK(trace->action_traces[0].return_value == candidate.return_value);
         BOOST_CHECK(candidate.return_value == expected);
      }
   }
}

BOOST_AUTO_TEST_CASE(float_pointer_failure_undoes_original_prefix_and_private_failure_never_publishes_writes) {
   eosio::testing::tester original; original.create_accounts({faccount});
   chainbase_transaction_state_view live(original.control->db()); const table_key rows{faccount, faccount, "floatrows"_n};
   const std::array tables{rows};
   for (unsigned group = 0; group < 2; ++group) {
      const auto code = fprogram(group, true); original.set_code(faccount, std::vector<uint8_t>(code.begin(), code.end())); original.produce_block();
      isolated_wasm_module module(code);
      for (unsigned op = group * 17; op < (group + 1) * 17; ++op) if (fpointer(op)) BOOST_TEST_CONTEXT(fhosts[op].name) {
         const auto base = frozen_primary_state::capture(live, tables); const auto before = original.control->calculate_integrity_hash();
         auto bad = fdefault(op); fput(bad.data, 4, uint32_t{0}); fput(bad.data, 48, uint32_t(op + 1));
         BOOST_CHECK_THROW(module.execute(base, bad), primary_state_unsupported);
         BOOST_CHECK_THROW(fpush(original, bad, op + 1), fc::exception);
         BOOST_CHECK(original.control->calculate_integrity_hash() == before);
         BOOST_CHECK(!live.read_primary({rows, primary_query_kind::find, op + 1}, 16).row);
         auto good = fdefault(op); fput(good.data, 48, uint32_t(op + 1));
         const auto candidate = module.execute(base, good); BOOST_REQUIRE_EQUAL(candidate.state.writes().size(), 1u);
         const auto trace = fpush(original, good, op + 1); BOOST_REQUIRE(trace && !trace->except);
         BOOST_CHECK(trace->action_traces[0].return_value == candidate.return_value);
         const auto row = live.read_primary({rows, primary_query_kind::find, op + 1}, 16);
         BOOST_REQUIRE(row.row); BOOST_CHECK(row.row->value == candidate.state.primary_write(0).value);
      }
      // A private numeric guard after a real write must discard the WHOLE
      // attempt. The original host still executes unchanged, including its
      // historical C boundary, and alone publishes the transaction's write.
      for (unsigned op : {11u, 15u, 16u, 24u}) if (op / 17 == group) BOOST_TEST_CONTEXT("guarded prefix " << fhosts[op].name) {
         const auto base = frozen_primary_state::capture(live, tables);
         const auto before = original.control->calculate_integrity_hash();
         auto guarded = finput(op, op == 11 ? fbits{0x407e} << 112 : op == 15 ? fbits{0x7f000000u} :
            op == 16 ? fbits{0x47e0000000000000ull} : fsign);
         const uint32_t key = 100 + op; fput(guarded.data, 48, key);
         BOOST_CHECK_THROW(module.execute(base, guarded), primary_state_unsupported);
         BOOST_CHECK(original.control->calculate_integrity_hash() == before);
         BOOST_CHECK(!live.read_primary({rows, primary_query_kind::find, key}, 16).row);
         const auto trace = fpush(original, guarded, key); BOOST_REQUIRE(trace && !trace->except);
         const auto row = live.read_primary({rows, primary_query_kind::find, key}, 16);
         BOOST_REQUIRE(row.row); BOOST_CHECK(row.row->value == std::vector<char>(guarded.data.begin() + 8, guarded.data.begin() + 24));
         const auto fresh = frozen_primary_state::capture(live, tables);
         auto healthy = fdefault(op); fput(healthy.data, 48, key + 100);
         const auto result = module.execute(fresh, healthy); BOOST_REQUIRE_EQUAL(result.state.writes().size(), 1u);
         BOOST_CHECK(result.return_value == fbytes(*fexpected(healthy), fwidth(op)));
      }
   }
}


BOOST_AUTO_TEST_CASE(all_float_hosts_are_adopted_in_production_and_validation_and_unsafe_prefixes_fall_back) {
   for (unsigned group = 0; group < 2; ++group) {
      float_live_corpus corpus(group);
      for (uint16_t workers : {1, 2, 4}) {
         fc::temp_directory pd, sd, vd;
         float_live_chain producer(corpus.configuration(pd, workers, true), corpus.genesis);
         eosio::testing::tester serial(corpus.configuration(sd, 0), corpus.genesis), parallel(corpus.configuration(vd, workers), corpus.genesis);
         corpus.prefix(producer); corpus.prefix(serial); corpus.prefix(parallel);
         producer.set_node_finalizers(std::array<name, 1>{"eosio"_n}); uint32_t nonce = 50000;
         // 4 modes * 6 batches cover all 17 hosts in each module. Then one
         // guarded original-C boundary and one rejected original pointer.
         for (unsigned scenario = 0; scenario < 26; ++scenario)
            BOOST_TEST_CONTEXT("float group=" << group << " workers=" << workers << " scenario=" << scenario) {
            const unsigned route = scenario < 24 ? scenario / 6 : 0;
            producer.start(); std::vector<transaction_metadata_ptr> batch;
            for (unsigned i = 0; i < 3; ++i) {
               auto input = fgraph(group * 17 + ((scenario % 6) * 3 + i) % 17, route);
               if (scenario == 24 && i == 0) input = group == 0 ? finput(11, fbits{0x407e} << 112) : finput(24, fsign);
               if (scenario == 25 && i == 0) { input = fdefault(group * 17); fput(input.data, 4, uint32_t{0}); }
               batch.push_back(fmetadata(producer, input, nonce++));
            }
            std::vector<std::vector<char>> produced, original, candidate;
            const auto collect = [&](auto& dest, const auto& event) {
               const auto trace = std::get<0>(event);
               if (std::any_of(batch.begin(), batch.end(), [&](const auto& tx) { return tx->id() == trace->id; })) dest.push_back(ftrace(trace));
            };
            boost::signals2::scoped_connection s = serial.control->applied_transaction().connect([&](const auto& e) { collect(original, e); });
            boost::signals2::scoped_connection v = parallel.control->applied_transaction().connect([&](const auto& e) { collect(candidate, e); });
            const auto before = producer.control->get_parallel_production_stats(), vb = parallel.control->get_parallel_execution_stats();
            size_t callbacks = 0;
            BOOST_CHECK_EQUAL(producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), [&](const auto& tx) {
               const auto state_before = producer.control->calculate_integrity_hash();
               const auto trace = producer.control->push_transaction(tx, fc::time_point::maximum(), fc::seconds(1), 0, false, 0);
               if (scenario == 25 && callbacks == 0) {
                  BOOST_REQUIRE(trace && trace->except); BOOST_CHECK(!trace->receipt);
                  BOOST_CHECK(producer.control->calculate_integrity_hash() == state_before);
                  BOOST_CHECK(!producer.control->is_known_unexpired_transaction(tx->id()));
               } else {
                  produced.push_back(ftrace(trace));
                  BOOST_REQUIRE_EQUAL(trace->action_traces.size(), route & 2 ? 2u : 1u);
                  BOOST_REQUIRE_EQUAL(trace->action_traces[0].call_traces.size(), route & 1 ? 1u : 0u);
               }
               ++callbacks; return true;
            }), batch.size());
            const auto after = producer.control->get_parallel_production_stats();
            BOOST_TEST_CONTEXT("attempts=" << after.attempts - before.attempts << " succeeded=" << after.succeeded - before.succeeded
               << " preparation_deadlines=" << after.preparation_deadlines - before.preparation_deadlines
               << " preparation_budget=" << after.preparation_budget_fallbacks - before.preparation_budget_fallbacks
               << " preparation_other=" << after.preparation_other_fallbacks - before.preparation_other_fallbacks
               << " worker_deadlines=" << after.worker_deadlines - before.worker_deadlines
               << " vm_timeouts=" << after.worker_vm_timeouts - before.worker_vm_timeouts
               << " worker_other=" << after.worker_other_failures - before.worker_other_failures) {
               BOOST_CHECK_EQUAL(after.adopted - before.adopted, scenario < 24 ? batch.size() : 0u);
               BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, scenario < 24 ? 0u : batch.size());
            }
            const auto block = producer.finish_block();
            serial.push_block(block); serial.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
            BOOST_CHECK(original == produced); BOOST_CHECK(candidate == produced);
            BOOST_CHECK_EQUAL(original.size(), scenario == 25 ? 2u : 3u);
            BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
            BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
            BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().adopted - vb.adopted, scenario == 24 ? 0u : original.size());
            BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().serial_fallbacks - vb.serial_fallbacks, scenario == 24 ? original.size() : 0u);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(owner_numeric_environment_changes_force_original_execution_and_default_reuse_recovers) {
   float_environment_restore restore; float_live_corpus corpus(0);
   const auto change = [](unsigned mode) {
      if (mode == 0) softfloat_roundingMode = softfloat_round_max;
      if (mode == 1) softfloat_detectTininess = softfloat_tininess_beforeRounding;
      if (mode == 2) extF80_roundingPrecision = 32;
   };
   for (uint16_t workers : {1, 2, 4}) {
      fc::temp_directory pd, sd, vd;
      float_live_chain producer(corpus.configuration(pd, workers, true), corpus.genesis);
      eosio::testing::tester serial(corpus.configuration(sd, 0), corpus.genesis), parallel(corpus.configuration(vd, workers), corpus.genesis);
      corpus.prefix(producer); corpus.prefix(serial); corpus.prefix(parallel);
      producer.set_node_finalizers(std::array<name, 1>{"eosio"_n}); uint32_t nonce = 90000;
      for (unsigned mode = 0; mode < 3; ++mode) for (unsigned route = 0; route < 4; ++route) {
         if (mode != 0 && route != 0) continue; // rounding covers every adoption mode; other mode fields use root path
         for (bool before_prepare : {false, true}) for (bool healthy : {false, true})
            BOOST_TEST_CONTEXT("float environment workers=" << workers << " mode=" << mode << " route=" << route
               << " before_prepare=" << before_prepare << " healthy=" << healthy) {
            BOOST_REQUIRE(isolated_numeric_environment_compatible());
            auto input = fgraph(4, route); // exact sign toggle also remains valid in CF self-checks
            if (mode == 0 && route == 0) input = finput(0, fbits{0x3fff} << 112, 1); // round-up differs from nearest-even
            const auto nearest = fbytes(*fexpected(input), fwidth(fget<uint32_t>(input.data, 0)));
            producer.start(); std::vector<transaction_metadata_ptr> batch;
            for (unsigned i = 0; i < 2; ++i) batch.push_back(fmetadata(producer, input, nonce++));
            std::vector<std::vector<char>> produced, original, candidate;
            const auto collect = [&](auto& dest, const auto& event) {
               const auto trace = std::get<0>(event);
               if (std::any_of(batch.begin(), batch.end(), [&](const auto& tx) { return tx->id() == trace->id; })) dest.push_back(ftrace(trace));
            };
            boost::signals2::scoped_connection s = serial.control->applied_transaction().connect([&](const auto& e) { collect(original, e); });
            boost::signals2::scoped_connection v = parallel.control->applied_transaction().connect([&](const auto& e) { collect(candidate, e); });
            const auto before = producer.control->get_parallel_production_stats();
            const auto vb = parallel.control->get_parallel_execution_stats();
            size_t callbacks = 0, consumed = 0;
            const auto push = [&](const auto& tx) {
               float_environment_restore callback_environment;
               if (!healthy) change(mode); // AFTER private preparation in the normal callback case
               // Production adoption deliberately rejects explicit billing.
               // Use the real producer path; both validators later consume its
               // SAME block receipt bill, so non-timing traces remain comparable.
               const auto trace = producer.control->push_transaction(tx, fc::time_point::maximum(), fc::seconds(1), 0, false, 0);
               produced.push_back(ftrace(trace));
               const auto expected = fbytes(*fexpected(input), fwidth(fget<uint32_t>(input.data, 0)));
               BOOST_CHECK(trace->action_traces[0].return_value == expected);
               if (!healthy && mode == 0 && route == 0) BOOST_CHECK(expected != nearest);
               ++callbacks; return true;
            };
            {
               float_environment_restore preparation_environment;
               if (!healthy && before_prepare) change(mode);
               consumed = producer.control->for_each_parallel_production_transaction(batch, fc::time_point::maximum(), push);
               for (size_t i = consumed; i < batch.size(); ++i) push(batch[i]); // only an unconsumed tail belongs to the caller
            }
            BOOST_REQUIRE_EQUAL(callbacks, batch.size()); BOOST_REQUIRE(isolated_numeric_environment_compatible());
            const auto after = producer.control->get_parallel_production_stats();
            BOOST_TEST_CONTEXT("attempts=" << after.attempts - before.attempts << " succeeded=" << after.succeeded - before.succeeded
               << " preparation_deadlines=" << after.preparation_deadlines - before.preparation_deadlines
               << " preparation_budget=" << after.preparation_budget_fallbacks - before.preparation_budget_fallbacks
               << " preparation_other=" << after.preparation_other_fallbacks - before.preparation_other_fallbacks
               << " worker_deadlines=" << after.worker_deadlines - before.worker_deadlines
               << " vm_timeouts=" << after.worker_vm_timeouts - before.worker_vm_timeouts) {
               // Ineligible preparation still invokes the original serial
               // callback; consumed counts queue entries, not private attempts.
               BOOST_CHECK_EQUAL(consumed, batch.size());
               BOOST_CHECK_EQUAL(after.adopted - before.adopted, healthy ? batch.size() : 0u);
               if (!healthy && before_prepare) BOOST_CHECK_EQUAL(after.attempts - before.attempts, 0u);
               else BOOST_CHECK_GE(after.succeeded - before.succeeded, batch.size());
               BOOST_CHECK_EQUAL(after.serial_fallbacks - before.serial_fallbacks, !healthy && !before_prepare ? batch.size() : 0u);
            }
            const auto block = producer.finish_block();
            {
               // Compare original and candidate validators under the SAME
               // deliberately foreign environment. No new network rule is made.
               float_environment_restore validation_environment;
               if (!healthy) change(mode);
               serial.push_block(block); serial.control->abort_block(); parallel.push_block(block); parallel.control->abort_block();
            }
            BOOST_CHECK(original == produced); BOOST_CHECK(candidate == produced); BOOST_CHECK_EQUAL(original.size(), batch.size());
            BOOST_CHECK(producer.control->calculate_integrity_hash() == serial.control->calculate_integrity_hash());
            BOOST_CHECK(serial.control->calculate_integrity_hash() == parallel.control->calculate_integrity_hash());
            BOOST_CHECK_EQUAL(parallel.control->get_parallel_execution_stats().adopted - vb.adopted, healthy ? batch.size() : 0u);
         }
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
