#include <eosio/chain/parallel_secondary_state.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <type_traits>
#include <tuple>

namespace eosio::chain::experimental {
namespace {
template<class Owned, class Object>
struct integer_traits {
   using object = Object;
   using canonical = typename Object::secondary_key_type;
   static canonical to(const Owned& value) { return value; }
   static Owned from(const canonical& value) { return value; }
   static bool less(const Owned& a, const Owned& b) { return std::less<Owned>{}(a, b); }
   static bool nan(const Owned&) { return false; }
};
template<class Owned> struct traits;
template<> struct traits<uint64_t> : integer_traits<uint64_t, index64_object> {};
template<> struct traits<uint128_t> : integer_traits<uint128_t, index128_object> {};
template<> struct traits<std::array<uint128_t, 2>>
   : integer_traits<std::array<uint128_t, 2>, index256_object> {};
template<> struct traits<secondary_float64> {
   using object = index_double_object;
   using canonical = float64_t;
   static canonical to(const secondary_float64& value) { return {value.bits}; }
   static secondary_float64 from(const canonical& value) { return {value.v}; }
   static bool less(const secondary_float64& a, const secondary_float64& b) {
      return soft_double_less{}(to(a), to(b));
   }
   static bool nan(const secondary_float64& value) { return f64_is_nan(to(value)); }
};
template<> struct traits<secondary_float128> {
   using object = index_long_double_object;
   using canonical = float128_t;
   static canonical to(const secondary_float128& value) {
      canonical out; fc::uint128_to_float128(value.bits, out); return out;
   }
   static secondary_float128 from(const canonical& value) {
      secondary_float128 out; fc::float128_to_uint128(value, out.bits); return out;
   }
   static bool less(const secondary_float128& a, const secondary_float128& b) {
      return soft_long_double_less{}(to(a), to(b));
   }
   static bool nan(const secondary_float128& value) { return f128_is_nan(to(value)); }
};

void validate_query(const secondary_query& query) {
   switch (query.kind) {
      case secondary_query_kind::find_primary:
      case secondary_query_kind::first:
      case secondary_query_kind::last:
         (void)secondary_kind(query.key); // key type only, not an input floating value
         return;
      case secondary_query_kind::find_secondary:
      case secondary_query_kind::lower_bound:
      case secondary_query_kind::upper_bound:
      case secondary_query_kind::next:
      case secondary_query_kind::previous:
         if (secondary_key_is_nan(query.key)) throw secondary_state_invalid_query();
         return;
   }
   throw secondary_state_invalid_query(); // also reject on a missing table
}

template<class Owned>
void locate(const chainbase::database& db, const table_read& table,
      const secondary_query& query, const Owned& key, std::optional<secondary_row_read>& result) {
   using mapping = traits<Owned>;
   using object = typename mapping::object;
   using index_type = typename chainbase::get_index_type<object>::type;
   const table_id id{table.object_id};
   const object* row = nullptr;
   const auto& index = db.get_index<index_type, by_secondary>();
   switch (query.kind) {
      case secondary_query_kind::find_primary:
         row = db.find<object, by_primary>(boost::make_tuple(id, query.primary_key));
         break;
      case secondary_query_kind::find_secondary:
         // Same composite-prefix lookup as generic_index: duplicate secondary
         // values select the lowest primary, regardless of insertion order.
         row = db.find<object, by_secondary>(boost::make_tuple(id, mapping::to(key)));
         break;
      case secondary_query_kind::lower_bound:
      case secondary_query_kind::upper_bound: {
         const auto sought = boost::make_tuple(id, mapping::to(key));
         const auto it = query.kind == secondary_query_kind::lower_bound ?
            index.lower_bound(sought) : index.upper_bound(sought);
         if (it != index.end() && it->t_id == id) row = &*it;
         break;
      }
      case secondary_query_kind::first: {
         const auto it = index.lower_bound(boost::make_tuple(id));
         if (it != index.end() && it->t_id == id) row = &*it;
         break;
      }
      case secondary_query_kind::last: {
         auto it = index.upper_bound(boost::make_tuple(id));
         if (it != index.begin()) { --it; if (it->t_id == id) row = &*it; }
         break;
      }
      case secondary_query_kind::next: {
         const auto it = index.upper_bound(boost::make_tuple(id, mapping::to(key), query.primary_key));
         if (it != index.end() && it->t_id == id) row = &*it;
         break;
      }
      case secondary_query_kind::previous: {
         auto it = index.lower_bound(boost::make_tuple(id, mapping::to(key), query.primary_key));
         if (it != index.begin()) { --it; if (it->t_id == id) row = &*it; }
         break;
      }
   }
   if (!row) { result.reset(); return; }
   auto captured = mapping::from(row->secondary_key);
   if (mapping::nan(captured)) throw secondary_state_invalid_query(); // local fail-closed; no canonical rejection
   // Write the owned row directly into the caller's optional: there is no
   // intermediate optional/variant move through the visitor's return value.
   result.emplace();
   result->object_id = row->id._id;
   result->table_object_id = row->t_id._id;
   result->primary_key = row->primary_key;
   result->payer = row->payer;
   result->key.template emplace<Owned>(captured);
}
} // namespace

secondary_key_kind secondary_kind(const secondary_key& key) {
   if (key.valueless_by_exception()) throw secondary_state_invalid_query();
   return static_cast<secondary_key_kind>(key.index());
}
bool secondary_key_is_nan(const secondary_key& key) {
   (void)secondary_kind(key);
   return std::visit([](const auto& value) { return traits<std::decay_t<decltype(value)>>::nan(value); }, key);
}
bool secondary_key_less(const secondary_key& left, const secondary_key& right) {
   if (secondary_kind(left) != secondary_kind(right) ||
       secondary_key_is_nan(left) || secondary_key_is_nan(right)) throw secondary_state_invalid_query();
   return std::visit([&](const auto& value) {
      using owned = std::decay_t<decltype(value)>;
      return traits<owned>::less(value, std::get<owned>(right));
   }, left);
}
bool secondary_key_equivalent(const secondary_key& left, const secondary_key& right) {
   return !secondary_key_less(left, right) && !secondary_key_less(right, left);
}
size_t secondary_key_bytes(const secondary_key& key) {
   (void)secondary_kind(key);
   return std::visit([](const auto& value) { return sizeof(value); }, key);
}
secondary_query normalize_secondary_query(secondary_query query) {
   validate_query(query);
   switch (query.kind) {
      case secondary_query_kind::first:
      case secondary_query_kind::last:
         query.primary_key = 0;
         [[fallthrough]];
      case secondary_query_kind::find_primary:
         std::visit([](auto& value) { value = {}; }, query.key);
         break;
      case secondary_query_kind::find_secondary:
      case secondary_query_kind::lower_bound:
      case secondary_query_kind::upper_bound:
         query.primary_key = 0;
         break;
      case secondary_query_kind::next:
      case secondary_query_kind::previous:
         break;
   }
   return query;
}
bool secondary_query_less::operator()(const secondary_query& a, const secondary_query& b) const {
   const auto left = std::tie(a.table.code, a.table.scope, a.table.table, a.kind, a.primary_key);
   const auto right = std::tie(b.table.code, b.table.scope, b.table.table, b.kind, b.primary_key);
   if (left != right) return left < right;
   const auto ak = secondary_kind(a.key), bk = secondary_kind(b.key);
   if (ak != bk) return ak < bk;
   return std::visit([&](const auto& value) {
      using owned = std::decay_t<decltype(value)>;
      const auto& other = std::get<owned>(b.key);
      if constexpr (std::is_same_v<owned, secondary_float64> || std::is_same_v<owned, secondary_float128>)
         return value.bits < other.bits;
      else return value < other;
   }, a.key);
}

secondary_read_result transaction_state_view::read_secondary(const secondary_query&) const {
   throw state_view_unsupported_query();
}
secondary_read_result chainbase_transaction_state_view::read_secondary(const secondary_query& query) const {
   // Constructor checks the original owner before touching either database
   // view. The adapter cannot pair unrelated primary and secondary sources.
   return chainbase_secondary_state_view(*this).read_secondary(query);
}

chainbase_secondary_state_view::chainbase_secondary_state_view(const chainbase::database& db)
   : db_(db), primary_(db), owner_(std::this_thread::get_id()) {}
chainbase_secondary_state_view::chainbase_secondary_state_view(const chainbase_transaction_state_view& primary)
   : db_(primary.db_), primary_(primary.db_), owner_(primary.owner_) {
   primary.require_owner_thread();
}
secondary_read_result chainbase_secondary_state_view::read_secondary(const secondary_query& query) const {
   if (std::this_thread::get_id() != owner_) throw state_view_wrong_thread();
   validate_query(query); // NaN for input-taking queries is rejected before any table lookup
   secondary_read_result result;
   result.table = primary_.read_table(query.table);
   if (result.table) std::visit([&](const auto& key) {
      locate(db_, *result.table, query, key, result.row);
   }, query.key);
   return result;
}

} // namespace eosio::chain::experimental
