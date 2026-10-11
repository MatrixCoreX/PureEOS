#include <eosio/chain/genesis_state.hpp>
#include <eosio/chain/protocol_feature_manager.hpp>
#include <eosio/chain/exceptions.hpp>
#include <map>

// these are required to serialize a genesis_state

namespace eosio { namespace chain {

native_savanna_genesis::native_savanna_genesis() {
   std::map<builtin_protocol_feature_t, digest_type> visited;
   std::function<digest_type(builtin_protocol_feature_t)> add = [&](auto feature) {
      if (auto it = visited.find(feature); it != visited.end())
         return it->second;
      auto digest = protocol_feature_set::make_default_builtin_protocol_feature(feature, add).digest();
      visited.emplace(feature, digest);
      protocol_features.push_back(digest);
      return digest;
   };
   add(builtin_protocol_feature_t::preactivate_feature);
   add(builtin_protocol_feature_t::savanna);
}

genesis_state::genesis_state() {
   initial_timestamp = fc::time_point::from_iso_string( "2018-06-01T12:00:00" );
   initial_key = fc::variant(eosio_root_key).as<public_key_type>();
}

genesis_state::genesis_state(const genesis_state_v0& old)
   : initial_configuration(old.initial_configuration), initial_timestamp(old.initial_timestamp), initial_key(old.initial_key) {}

void genesis_state::validate() const {
   initial_configuration.validate();
   if (!initial_savanna)
      return;
   EOS_ASSERT(initial_key != public_key_type{}, chain_exception, "native Savanna requires a valid initial signing key");
   const auto& cfg = *initial_savanna;
   EOS_ASSERT(cfg.version == 1 || cfg.version == 2, chain_exception, "unsupported native Savanna genesis profile");
   EOS_ASSERT(cfg.protocol_features == native_savanna_genesis{}.protocol_features, chain_exception,
              "native Savanna genesis protocol feature manifest does not match profile v1");
   EOS_ASSERT(!cfg.finalizer_key.jacobian_montgomery_le().isZero() &&
              !cfg.proof_of_possession.jacobian_montgomery_le().isZero(),
              chain_exception, "native Savanna finalizer key and proof must not be points at infinity");
   EOS_ASSERT(bls12_381::pop_verify(cfg.finalizer_key.jacobian_montgomery_le(),
                                  cfg.proof_of_possession.jacobian_montgomery_le()),
              chain_exception, "invalid native Savanna finalizer proof of possession");
}

chain::chain_id_type genesis_state::compute_chain_id() const {
   digest_type::encoder enc;
   if (initial_savanna)
      fc::raw::pack(enc, *this);
   else
      fc::raw::pack(enc, legacy_state()); // Existing chain IDs must not change.
   return chain_id_type{enc.result()};
}

} } // namespace eosio::chain
