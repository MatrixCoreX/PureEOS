#ifdef PUREEOS_BP_MAINTENANCE
#include <eosio.system/maintenance.hpp>
#endif
#include <eosio.system/eosio.system.hpp>
#include <eosio.token/eosio.token.hpp>

namespace eosiosystem {

   using eosio::current_time_point;
   using eosio::microseconds;
   using eosio::token;

   namespace {
   int64_t integer_reward_share(int64_t bucket, uint64_t share, uint64_t total) {
      check(bucket >= 0 && total > 0 && share <= total, "invalid producer reward ratio");
      // Both operands fit their storage types, but their product need not.
      // Divide in 128 bits and narrow only after checking the payable amount.
      const uint128_t pay = uint128_t(bucket) * share / total;
      check(pay <= uint128_t(bucket) && pay <= uint128_t(asset::max_amount),
            "producer reward amount out of range");
      return int64_t(pay);
   }
   }

   void system_contract::onblock( ignore<block_header> ) {
      using namespace eosio;

      require_auth(get_self());

      // Deserialize needed fields from block header.
      block_timestamp timestamp;
      name            producer;
      uint16_t        confirmed;
      checksum256     previous_block_id;

      _ds >> timestamp >> producer >> confirmed >> previous_block_id;
      (void)confirmed; // Only to suppress warning since confirmed is not used.

      // Add latest block information to blockinfo table.
      add_to_blockinfo_table(previous_block_id, timestamp);

      // _gstate2.last_block_num is not used anywhere in the system contract code anymore.
      // Although this field is deprecated, we will continue updating it for now until the last_block_num field
      // is eventually completely removed, at which point this line can be removed.
      _gstate2.last_block_num = timestamp;

#ifdef PUREEOS_SEQUENTIAL_ROTATION
      // Consensus rotation does not depend on the reward activation clock.
      advance_rotation(timestamp, producer);
#ifdef PUREEOS_BP_MAINTENANCE
      advance_maintenance();
#endif
#endif
#ifdef PUREEOS_SINGLE_CHOICE_VOTING
      if (_rammarket.begin() != _rammarket.end()) sync_reward_policy();
#endif

      /** until activation, no new rewards are paid */
      if( _gstate.thresh_activated_stake_time == time_point() )
         return;

      if( _gstate.last_pervote_bucket_fill == time_point() )  /// start the presses
         _gstate.last_pervote_bucket_fill = current_time_point();


      /**
       * At startup the initial producer may not be one that is registered / elected
       * and therefore there may be no producer object for them.
       */
      auto prod = _producers.find( producer.value );
      if ( prod != _producers.end()
#ifdef PUREEOS_BP_MAINTENANCE
           && !maintenance_paused(get_self(),producer)
#endif
         ) {
         _gstate.total_unpaid_blocks++;
         _producers.modify( prod, same_payer, [&](auto& p ) {
               p.unpaid_blocks++;
         });
      }

      /// only update block producers once every minute, block_timestamp is in half seconds
      if( timestamp.slot - _gstate.last_producer_schedule_update.slot > 120 ) {
#ifndef PUREEOS_SEQUENTIAL_ROTATION
         update_elected_producers( timestamp );
#else
         _gstate.last_producer_schedule_update = timestamp;
#endif

         if( (timestamp.slot - _gstate.last_name_close.slot) > blocks_per_day ) {
            name_bid_table bids(get_self(), get_self().value);
            auto idx = bids.get_index<"highbid"_n>();
            auto highest = idx.lower_bound( std::numeric_limits<uint64_t>::max()/2 );
            if( highest != idx.end() &&
                highest->high_bid > 0 &&
                (current_time_point() - highest->last_bid_time) > microseconds(useconds_per_day) &&
                _gstate.thresh_activated_stake_time > time_point() &&
                (current_time_point() - _gstate.thresh_activated_stake_time) > microseconds(14 * useconds_per_day)
            ) {
               _gstate.last_name_close = timestamp;
               channel_to_system_fees( names_account, asset( highest->high_bid, core_symbol() ) );

               // logging
               system_contract::logsystemfee_action logsystemfee_act{ get_self(), { {get_self(), active_permission} } };
               logsystemfee_act.send( names_account, asset( highest->high_bid, core_symbol() ), "buy name" );

               idx.modify( highest, same_payer, [&]( auto& b ){
                  b.high_bid = -b.high_bid;
               });
            }
         }
      }
   }

   void system_contract::claimrewards( const name& owner ) {
      require_auth( owner );

#ifdef PUREEOS_SINGLE_CHOICE_VOTING
      require_direct_vote_policy();
      // Votes select rotation candidates; they never entitle an account to pay.
      // Accepted on-chain blocks are accounted for by onblock; this individual
      // action is not a standalone proof that a block has become irreversible.
      check(_producers.get(owner.value, "producer not registered").unpaid_blocks > 0,
            "no produced blocks: standby rewards are disabled");
#endif
      execute_next_schedule();
      const auto& prod = _producers.get( owner.value, "producer not registered" );
      check( prod.active(), "producer does not have an active key" );

      check( _gstate.thresh_activated_stake_time != time_point(),
#ifdef PUREEOS_SINGLE_CHOICE_VOTING
                    "cannot claim rewards before the first positive producer vote" );
#else
                    "cannot claim rewards until the chain is activated (at least 15% of all tokens participate in voting)" );
#endif

      const auto ct = current_time_point();

      check( ct - prod.last_claim_time > microseconds(useconds_per_day), "already claimed rewards within past day" );

      const asset token_supply   = token::get_supply(token_account, core_symbol().code() );
      const asset token_max_supply = token::get_max_supply(token_account, core_symbol().code() );
      const asset token_balance = token::get_balance(token_account, get_self(), core_symbol().code() );
      const auto usecs_since_last_fill = (ct - _gstate.last_pervote_bucket_fill).count();

      if( usecs_since_last_fill > 0 && _gstate.last_pervote_bucket_fill > time_point() ) {
#ifdef PUREEOS_SINGLE_CHOICE_VOTING
         const int64_t new_tokens = accrue_halving_rewards(token_supply.amount, _gstate.last_pervote_bucket_fill, ct);
#else
         double additional_inflation = (_gstate4.continuous_rate * double(token_supply.amount) * double(usecs_since_last_fill)) / double(useconds_per_year);
         check( additional_inflation <= double(std::numeric_limits<int64_t>::max() - ((1ll << 10) - 1)),
                "overflow in calculating new tokens to be issued; inflation rate is too high" );
         const int64_t new_tokens = (additional_inflation < 0.0) ? 0 : static_cast<int64_t>(additional_inflation);
#endif
         check(new_tokens <= asset::max_amount, "new reward amount exceeds asset limit");

#ifdef PUREEOS_SINGLE_CHOICE_VOTING
         const int64_t to_savings = 0; // cancel issuance; do not redistribute the former savings portion
         const int64_t to_per_block_pay = new_tokens;
         const int64_t to_per_vote_pay = 0; // independent of all legacy factors/revisions
#else
         int64_t to_producers     = (new_tokens * uint128_t(pay_factor_precision)) / _gstate4.inflation_pay_factor;
         int64_t to_savings       = new_tokens - to_producers;
         int64_t to_per_block_pay = (to_producers * uint128_t(pay_factor_precision)) / _gstate4.votepay_factor;
         int64_t to_per_vote_pay  = to_producers - to_per_block_pay;
#endif

         if( new_tokens > 0 ) {
            // issue new tokens or use existing eosio token balance
            {
               // issue new tokens if circulating supply does not exceed max supply
               if ( token_supply.amount + new_tokens <= token_max_supply.amount ) {
                  token::issue_action issue_act{ token_account, { {get_self(), active_permission} } };
                  issue_act.send( get_self(), asset(new_tokens, core_symbol()), "issue producer rewards" );

               // use existing eosio token balance if circulating supply exceeds max supply
               } else {
                  check( token_balance.amount >= new_tokens, "insufficient system token balance for claiming rewards");
               }
            }
            {
               token::transfer_action transfer_act{ token_account, { {get_self(), active_permission} } };
               if( to_savings > 0 ) {
                  transfer_act.send( get_self(), saving_account, asset(to_savings, core_symbol()), "unallocated bucket" );
               }
               if( to_per_block_pay > 0 ) {
                  transfer_act.send( get_self(), bpay_account, asset(to_per_block_pay, core_symbol()), "fund per-block bucket" );
               }
               if( to_per_vote_pay > 0 ) {
                  transfer_act.send( get_self(), vpay_account, asset(to_per_vote_pay, core_symbol()), "fund per-vote bucket" );
               }
            }
         }

         _gstate.pervote_bucket          += to_per_vote_pay;
         _gstate.perblock_bucket         += to_per_block_pay;
         _gstate.last_pervote_bucket_fill = ct;
      }

#ifndef PUREEOS_SINGLE_CHOICE_VOTING
      auto prod2 = _producers2.find( owner.value );

      /// New metric to be used in pervote pay calculation. Instead of vote weight ratio, we combine vote weight and
      /// time duration the vote weight has been held into one metric.
      const auto last_claim_plus_3days = prod.last_claim_time + microseconds(3 * useconds_per_day);

      bool crossed_threshold       = (last_claim_plus_3days <= ct);
      bool updated_after_threshold = true;
      if ( prod2 != _producers2.end() ) {
         updated_after_threshold = (last_claim_plus_3days <= prod2->last_votepay_share_update);
      } else {
         prod2 = _producers2.emplace( owner, [&]( producer_info2& info  ) {
            info.owner                     = owner;
            info.last_votepay_share_update = ct;
         });
      }

      // Note: updated_after_threshold implies cross_threshold (except if claiming rewards when the producers2 table row did not exist).
      // The exception leads to updated_after_threshold to be treated as true regardless of whether the threshold was crossed.
      // This is okay because in this case the producer will not get paid anything either way.
      // In fact it is desired behavior because the producers votes need to be counted in the global total_producer_votepay_share for the first time.
#endif

      int64_t producer_per_block_pay = 0;
      if( _gstate.total_unpaid_blocks > 0 ) {
         producer_per_block_pay = integer_reward_share(_gstate.perblock_bucket, prod.unpaid_blocks,
                                                       _gstate.total_unpaid_blocks);
      }
#ifdef PUREEOS_SINGLE_CHOICE_VOTING
      // A small pool or share can round below one token unit. Reject the whole
      // claim before consuming block credits or its cooldown. Transaction undo
      // also preserves the fill timestamp and fractional accrual interval.
      check(producer_per_block_pay > 0,
            "no payable block rewards yet; unpaid blocks are preserved");
#endif

#ifndef PUREEOS_SINGLE_CHOICE_VOTING
      double new_votepay_share = update_producer_votepay_share( prod2,
                                    ct,
                                    updated_after_threshold ? 0.0 : prod.total_votes,
                                    true // reset votepay_share to zero after updating
                                 );

      int64_t producer_per_vote_pay = 0;
      if( _gstate2.revision > 0 ) {
         double total_votepay_share = update_total_votepay_share( ct );
         if( total_votepay_share > 0 && !crossed_threshold ) {
            producer_per_vote_pay = int64_t((new_votepay_share * _gstate.pervote_bucket) / total_votepay_share);
            if( producer_per_vote_pay > _gstate.pervote_bucket )
               producer_per_vote_pay = _gstate.pervote_bucket;
         }
      } else {
         if( _gstate.total_producer_vote_weight > 0 ) {
            producer_per_vote_pay = int64_t((_gstate.pervote_bucket * prod.total_votes) / _gstate.total_producer_vote_weight);
         }
      }

      if( producer_per_vote_pay < min_pervote_daily_pay ) {
         producer_per_vote_pay = 0;
      }

      _gstate.pervote_bucket      -= producer_per_vote_pay;
      update_total_votepay_share( ct, -new_votepay_share, (updated_after_threshold ? prod.total_votes : 0.0) );
#endif
      // Do not confiscate, redistribute, or pay out any pre-upgrade vote pool.
      // Its balance/ABI fields remain intact pending an explicitly reviewed
      // historical-funds policy. New claims use only the block-pay bucket.
      _gstate.perblock_bucket     -= producer_per_block_pay;
      _gstate.total_unpaid_blocks -= prod.unpaid_blocks;

      _producers.modify( prod, same_payer, [&](auto& p) {
         p.last_claim_time = ct;
         p.unpaid_blocks   = 0;
      });

      if ( producer_per_block_pay > 0 ) {
         token::transfer_action transfer_act{ token_account, { {bpay_account, active_permission}, {owner, active_permission} } };
         transfer_act.send( bpay_account, owner, asset(producer_per_block_pay, core_symbol()), "producer block pay" );
      }
#ifndef PUREEOS_SINGLE_CHOICE_VOTING
      if ( producer_per_vote_pay > 0 ) {
         token::transfer_action transfer_act{ token_account, { {vpay_account, active_permission}, {owner, active_permission} } };
         transfer_act.send( vpay_account, owner, asset(producer_per_vote_pay, core_symbol()), "producer vote pay" );
      }
#endif
   }

} //namespace eosiosystem
