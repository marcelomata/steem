/*
 * Copyright (c) 2017, Respective Authors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <steemit/plugins/witness/witness_plugin.hpp>
#include <steemit/plugins/witness/witness_objects.hpp>
#include <steemit/plugins/witness/witness_operations.hpp>

#include <steemit/chain/database_exceptions.hpp>
#include <steemit/chain/account_object.hpp>
#include <steemit/chain/comment_object.hpp>
#include <steemit/chain/witness_objects.hpp>
#include <steemit/chain/generic_custom_operation_interpreter.hpp>
#include <steemit/chain/index.hpp>

#include <graphene/utilities/key_conversion.hpp>

#include <fc/io/json.hpp>
#include <fc/smart_ref_impl.hpp>

#include <boost/asio.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <iostream>


#define DISTANCE_CALC_PRECISION (10000)


namespace steemit { namespace plugins { namespace witness {

using steemit::chain::plugin_exception;
using std::string;
using std::vector;

namespace bpo = boost::program_options;


void new_chain_banner( const steemit::chain::database& db )
{
   std::cerr << "\n"
      "********************************\n"
      "*                              *\n"
      "*   ------- NEW CHAIN ------   *\n"
      "*   -   Welcome to Steem!  -   *\n"
      "*   ------------------------   *\n"
      "*                              *\n"
      "********************************\n"
      "\n";
   return;
}

template<typename T>
T dejsonify(const string& s) {
   return fc::json::from_string(s).as<T>();
}

// TODO: Move this somewhere else. Also exists in app/plugin.hpp, which will be removed.
#ifndef LOAD_VALUE_SET
#define LOAD_VALUE_SET(options, name, container, type) \
if( options.count(name) ) { \
   const std::vector<std::string>& ops = options[name].as<std::vector<std::string>>(); \
   std::transform(ops.begin(), ops.end(), std::inserter(container, container.end()), &dejsonify<type>); \
}
#endif

class witness_plugin_impl {
public:
   witness_plugin_impl( boost::asio::io_service& io ) :
      _timer(io),
      _db( appbase::app().get_plugin< steemit::plugins::chain::chain_plugin >().db() ) {}

   void pre_transaction( const steemit::protocol::signed_transaction& trx );
   void pre_operation( const steemit::chain::operation_notification& note );
   void on_block( const signed_block& b );

   void update_account_bandwidth( const steemit::chain::account_object& a, uint32_t trx_size, const bandwidth_type type );

   void schedule_production_loop();
   block_production_condition::block_production_condition_enum block_production_loop();
   block_production_condition::block_production_condition_enum maybe_produce_block(fc::mutable_variant_object& capture);

   bool _production_enabled = false;
   uint32_t _required_witness_participation = 33 * STEEMIT_1_PERCENT;
   uint32_t _production_skip_flags = steemit::chain::database::skip_nothing;

   std::map< steemit::protocol::public_key_type, fc::ecc::private_key > _private_keys;
   std::set< steemit::protocol::account_name_type >                     _witnesses;
   boost::asio::deadline_timer                                          _timer;

   std::shared_ptr< generic_custom_operation_interpreter< witness_plugin_operation > > _custom_operation_interpreter;
   steemit::chain::database& _db;
};

struct comment_options_extension_visitor
{
   comment_options_extension_visitor( const comment_object& c, const database& db ) : _c( c ), _db( db ) {}

   typedef void result_type;

   const comment_object& _c;
   const database& _db;

   void operator()( const comment_payout_beneficiaries& cpb )const
   {
      STEEMIT_ASSERT( cpb.beneficiaries.size() <= 8,
          plugin_exception,
         "Cannot specify more than 8 beneficiaries." );
   }
};

void check_memo( const string& memo, const steemit::chain::account_object& account, const account_authority_object& auth )
{
   vector< public_key_type > keys;

   try
   {
      // Check if memo is a private key
      keys.push_back( fc::ecc::extended_private_key::from_base58( memo ).get_public_key() );
   }
   catch( fc::parse_error_exception& ) {}
   catch( fc::assert_exception& ) {}

   // Get possible keys if memo was an account password
   string owner_seed = account.name + "owner" + memo;
   auto owner_secret = fc::sha256::hash( owner_seed.c_str(), owner_seed.size() );
   keys.push_back( fc::ecc::private_key::regenerate( owner_secret ).get_public_key() );

   string active_seed = account.name + "active" + memo;
   auto active_secret = fc::sha256::hash( active_seed.c_str(), active_seed.size() );
   keys.push_back( fc::ecc::private_key::regenerate( active_secret ).get_public_key() );

   string posting_seed = account.name + "posting" + memo;
   auto posting_secret = fc::sha256::hash( posting_seed.c_str(), posting_seed.size() );
   keys.push_back( fc::ecc::private_key::regenerate( posting_secret ).get_public_key() );

   // Check keys against public keys in authorites
   for( auto& key_weight_pair : auth.owner.key_auths )
   {
      for( auto& key : keys )
         STEEMIT_ASSERT( key_weight_pair.first != key,  plugin_exception,
            "Detected private owner key in memo field. You should change your owner keys." );
   }

   for( auto& key_weight_pair : auth.active.key_auths )
   {
      for( auto& key : keys )
         STEEMIT_ASSERT( key_weight_pair.first != key,  plugin_exception,
            "Detected private active key in memo field. You should change your active keys." );
   }

   for( auto& key_weight_pair : auth.posting.key_auths )
   {
      for( auto& key : keys )
         STEEMIT_ASSERT( key_weight_pair.first != key,  plugin_exception,
            "Detected private posting key in memo field. You should change your posting keys." );
   }

   const auto& memo_key = account.memo_key;
   for( auto& key : keys )
      STEEMIT_ASSERT( memo_key != key,  plugin_exception,
         "Detected private memo key in memo field. You should change your memo key." );
}

struct operation_visitor
{
   operation_visitor( const chain::database& db ) : _db( db ) {}

   const chain::database& _db;

   typedef void result_type;

   template< typename T >
   void operator()( const T& )const {}

   void operator()( const comment_options_operation& o )const
   {
      const auto& comment = _db.get_comment( o.author, o.permlink );

      comment_options_extension_visitor v( comment, _db );

      for( auto& e : o.extensions )
      {
         e.visit( v );
      }
   }

   void operator()( const comment_operation& o )const
   {
      if( o.parent_author != STEEMIT_ROOT_POST_PARENT )
      {
         const auto& parent = _db.find_comment( o.parent_author, o.parent_permlink );

         if( parent != nullptr )
         STEEMIT_ASSERT( parent->depth < STEEMIT_SOFT_MAX_COMMENT_DEPTH,
             plugin_exception,
            "Comment is nested ${x} posts deep, maximum depth is ${y}.", ("x",parent->depth)("y",STEEMIT_SOFT_MAX_COMMENT_DEPTH) );
      }

      auto itr = _db.find< comment_object, steemit::chain::by_permlink >( boost::make_tuple( o.author, o.permlink ) );

      if( itr != nullptr && itr->cashout_time == fc::time_point_sec::maximum() )
      {
         auto edit_lock = _db.find< content_edit_lock_object, by_account >( o.author );

         STEEMIT_ASSERT( edit_lock != nullptr && _db.head_block_time() < edit_lock->lock_time,
             plugin_exception,
            "The comment is archived" );
      }
   }

   void operator()( const transfer_operation& o )const
   {
      if( o.memo.length() > 0 )
         check_memo( o.memo,
                     _db.get< steemit::chain::account_object, steemit::chain::by_name >( o.from ),
                     _db.get< account_authority_object, steemit::chain::by_account >( o.from ) );
   }

   void operator()( const transfer_to_savings_operation& o )const
   {
      if( o.memo.length() > 0 )
         check_memo( o.memo,
                     _db.get< steemit::chain::account_object, steemit::chain::by_name >( o.from ),
                     _db.get< account_authority_object, steemit::chain::by_account >( o.from ) );
   }

   void operator()( const transfer_from_savings_operation& o )const
   {
      if( o.memo.length() > 0 )
         check_memo( o.memo,
                     _db.get< steemit::chain::account_object, steemit::chain::by_name >( o.from ),
                     _db.get< account_authority_object, steemit::chain::by_account >( o.from ) );
   }
};

void witness_plugin_impl::pre_transaction( const steemit::protocol::signed_transaction& trx )
{
   flat_set< account_name_type > required; vector<authority> other;
   trx.get_required_authorities( required, required, required, other );

   auto trx_size = fc::raw::pack_size(trx);

   for( const auto& auth : required )
   {
      const auto& acnt = _db.get_account( auth );

      update_account_bandwidth( acnt, trx_size, bandwidth_type::forum );

      for( const auto& op : trx.operations )
      {
         if( is_market_operation( op ) )
         {
            update_account_bandwidth( acnt, trx_size * 10, bandwidth_type::market );
            break;
         }
      }
   }
}

void witness_plugin_impl::pre_operation( const steemit::chain::operation_notification& note )
{
   if( _db.is_producing() )
   {
      note.op.visit( operation_visitor( _db ) );
   }
}

void witness_plugin_impl::on_block( const signed_block& b )
{
   int64_t max_block_size = _db.get_dynamic_global_properties().maximum_block_size;

   auto reserve_ratio_ptr = _db.find( reserve_ratio_id_type() );

   if( BOOST_UNLIKELY( reserve_ratio_ptr == nullptr ) )
   {
      _db.create< reserve_ratio_object >( [&]( reserve_ratio_object& r )
      {
         r.average_block_size = 0;
         r.current_reserve_ratio = STEEMIT_MAX_RESERVE_RATIO * RESERVE_RATIO_PRECISION;
         r.max_virtual_bandwidth = ( uint128_t( STEEMIT_MAX_BLOCK_SIZE * STEEMIT_MAX_RESERVE_RATIO )
                                    * STEEMIT_BANDWIDTH_PRECISION * STEEMIT_BANDWIDTH_AVERAGE_WINDOW_SECONDS )
                                    / STEEMIT_BLOCK_INTERVAL;
      });
   }
   else
   {
      _db.modify( *reserve_ratio_ptr, [&]( reserve_ratio_object& r )
      {
         r.average_block_size = ( 99 * r.average_block_size + fc::raw::pack_size( b ) ) / 100;

         /**
         * About once per minute the average network use is consulted and used to
         * adjust the reserve ratio. Anything above 25% usage reduces the reserve
         * ratio proportional to the distance from 25%. If usage is at 50% then
         * the reserve ratio will half. Likewise, if it is at 12% it will increase by 50%.
         *
         * If the reserve ratio is consistently low, then it is probably time to increase
         * the capcacity of the network.
         *
         * This algorithm is designed to react quickly to observations significantly
         * different from past observed behavior and make small adjustments when
         * behavior is within expected norms.
         */
         if( _db.head_block_num() % 20 == 0 )
         {
            int64_t distance = ( ( r.average_block_size - ( max_block_size / 4 ) ) * DISTANCE_CALC_PRECISION )
               / ( max_block_size / 4 );
            auto old_reserve_ratio = r.current_reserve_ratio;

            if( distance > 0 )
            {
               r.current_reserve_ratio -= ( r.current_reserve_ratio * distance ) / ( distance + DISTANCE_CALC_PRECISION );

               // We do not want the reserve ratio to drop below 1
               if( r.current_reserve_ratio < RESERVE_RATIO_PRECISION )
                  r.current_reserve_ratio = RESERVE_RATIO_PRECISION;
            }
            else
            {
               // By default, we should always slowly increase the reserve ratio.
               r.current_reserve_ratio += std::max( RESERVE_RATIO_MIN_INCREMENT, ( r.current_reserve_ratio * distance ) / ( distance - DISTANCE_CALC_PRECISION ) );

               if( r.current_reserve_ratio > STEEMIT_MAX_RESERVE_RATIO * RESERVE_RATIO_PRECISION )
                  r.current_reserve_ratio = STEEMIT_MAX_RESERVE_RATIO * RESERVE_RATIO_PRECISION;
            }

            if( old_reserve_ratio != r.current_reserve_ratio )
            {
               ilog( "Reserve ratio updated from ${old} to ${new}. Block: ${blocknum}",
                  ("old", old_reserve_ratio)
                  ("new", r.current_reserve_ratio)
                  ("blocknum", _db.head_block_num()) );
            }

            r.max_virtual_bandwidth = ( uint128_t( max_block_size ) * uint128_t( r.current_reserve_ratio )
                                       * uint128_t( STEEMIT_BANDWIDTH_PRECISION * STEEMIT_BANDWIDTH_AVERAGE_WINDOW_SECONDS ) )
                                       / ( STEEMIT_BLOCK_INTERVAL * RESERVE_RATIO_PRECISION );
         }
      });
   }
}

void witness_plugin_impl::update_account_bandwidth( const steemit::chain::account_object& a, uint32_t trx_size, const bandwidth_type type )
{
   const auto& props = _db.get_dynamic_global_properties();
   bool has_bandwidth = true;

   if( props.total_vesting_shares.amount > 0 )
   {
      auto band = _db.find< account_bandwidth_object, by_account_bandwidth_type >( boost::make_tuple( a.name, type ) );

      if( band == nullptr )
      {
         band = &_db.create< account_bandwidth_object >( [&]( account_bandwidth_object& b )
         {
            b.account = a.name;
            b.type = type;
         });
      }

      share_type new_bandwidth;
      share_type trx_bandwidth = trx_size * STEEMIT_BANDWIDTH_PRECISION;
      auto delta_time = ( _db.head_block_time() - band->last_bandwidth_update ).to_seconds();

      if( delta_time > STEEMIT_BANDWIDTH_AVERAGE_WINDOW_SECONDS )
         new_bandwidth = 0;
      else
         new_bandwidth = ( ( ( STEEMIT_BANDWIDTH_AVERAGE_WINDOW_SECONDS - delta_time ) * fc::uint128( band->average_bandwidth.value ) )
            / STEEMIT_BANDWIDTH_AVERAGE_WINDOW_SECONDS ).to_uint64();

      new_bandwidth += trx_bandwidth;

      _db.modify( *band, [&]( account_bandwidth_object& b )
      {
         b.average_bandwidth = new_bandwidth;
         b.lifetime_bandwidth += trx_bandwidth;
         b.last_bandwidth_update = _db.head_block_time();
      });

      fc::uint128 account_vshares( a.effective_vesting_shares().amount.value );
      fc::uint128 total_vshares( props.total_vesting_shares.amount.value );
      fc::uint128 account_average_bandwidth( band->average_bandwidth.value );
      fc::uint128 max_virtual_bandwidth( _db.get( reserve_ratio_id_type() ).max_virtual_bandwidth );

      has_bandwidth = ( account_vshares * max_virtual_bandwidth ) > ( account_average_bandwidth * total_vshares );

      if( _db.is_producing() )
         STEEMIT_ASSERT( has_bandwidth,  plugin_exception,
            "Account: ${account} bandwidth limit exeeded. Please wait to transact or power up STEEM.",
            ("account", a.name)
            ("account_vshares", account_vshares)
            ("account_average_bandwidth", account_average_bandwidth)
            ("max_virtual_bandwidth", max_virtual_bandwidth)
            ("total_vesting_shares", total_vshares) );
   }
}

void witness_plugin_impl::schedule_production_loop() {
   //Schedule for the next second's tick regardless of chain state
   // If we would wait less than 50ms, wait for the whole second.
   fc::time_point now = fc::time_point::now();
   int64_t time_to_next_second = 1000000 - (now.time_since_epoch().count() % 1000000);
   if(time_to_next_second < 50000)      // we must sleep for at least 50ms
       time_to_next_second += 1000000;

   _timer.expires_from_now( boost::posix_time::microseconds( time_to_next_second ) );
   _timer.async_wait( boost::bind( &witness_plugin_impl::block_production_loop, this ) );
}

block_production_condition::block_production_condition_enum witness_plugin_impl::block_production_loop()
{
   if( fc::time_point::now() < fc::time_point(STEEMIT_GENESIS_TIME) )
   {
      wlog( "waiting until genesis time to produce block: ${t}", ("t",STEEMIT_GENESIS_TIME) );
      schedule_production_loop();
      return block_production_condition::wait_for_genesis;
   }

   block_production_condition::block_production_condition_enum result;
   fc::mutable_variant_object capture;
   try
   {
      result = maybe_produce_block(capture);
   }
   catch( const fc::canceled_exception& )
   {
      //We're trying to exit. Go ahead and let this one out.
      throw;
   }
   catch( const steemit::chain::unknown_hardfork_exception& e )
   {
      // Hit a hardfork that the current node know nothing about, stop production and inform user
      elog( "${e}\nNode may be out of date...", ("e", e.to_detail_string()) );
      throw;
   }
   catch( const fc::exception& e )
   {
      elog("Got exception while generating block:\n${e}", ("e", e.to_detail_string()));
      result = block_production_condition::exception_producing_block;
   }

   switch(result)
   {
      case block_production_condition::produced:
         ilog("Generated block #${n} with timestamp ${t} at time ${c}", (capture));
         break;
      case block_production_condition::not_synced:
//         ilog("Not producing block because production is disabled until we receive a recent block (see: --enable-stale-production)");
         break;
      case block_production_condition::not_my_turn:
//         ilog("Not producing block because it isn't my turn");
         break;
      case block_production_condition::not_time_yet:
//         ilog("Not producing block because slot has not yet arrived");
         break;
      case block_production_condition::no_private_key:
         ilog("Not producing block because I don't have the private key for ${scheduled_key}", (capture) );
         break;
      case block_production_condition::low_participation:
         elog("Not producing block because node appears to be on a minority fork with only ${pct}% witness participation", (capture) );
         break;
      case block_production_condition::lag:
         elog("Not producing block because node didn't wake up within 500ms of the slot time.");
         break;
      case block_production_condition::consecutive:
         elog("Not producing block because the last block was generated by the same witness.\nThis node is probably disconnected from the network so block production has been disabled.\nDisable this check with --allow-consecutive option.");
         break;
      case block_production_condition::exception_producing_block:
         elog( "exception prodcing block" );
         break;
      case block_production_condition::wait_for_genesis:
         break;
   }

   schedule_production_loop();
   return result;
}

block_production_condition::block_production_condition_enum witness_plugin_impl::maybe_produce_block(fc::mutable_variant_object& capture)
{
   steemit::chain::database& db = appbase::app().get_plugin< steemit::plugins::chain::chain_plugin >().db();
   fc::time_point now_fine = fc::time_point::now();
   fc::time_point_sec now = now_fine + fc::microseconds( 500000 );

   // If the next block production opportunity is in the present or future, we're synced.
   if( !_production_enabled )
   {
      if( db.get_slot_time(1) >= now )
         _production_enabled = true;
      else
         return block_production_condition::not_synced;
   }

   // is anyone scheduled to produce now or one second in the future?
   uint32_t slot = db.get_slot_at_time( now );
   if( slot == 0 )
   {
      capture("next_time", db.get_slot_time(1));
      return block_production_condition::not_time_yet;
   }

   //
   // this assert should not fail, because now <= db.head_block_time()
   // should have resulted in slot == 0.
   //
   // if this assert triggers, there is a serious bug in get_slot_at_time()
   // which would result in allowing a later block to have a timestamp
   // less than or equal to the previous block
   //
   assert( now > db.head_block_time() );

   steemit::chain::account_name_type scheduled_witness = db.get_scheduled_witness( slot );
   // we must control the witness scheduled to produce the next block.
   if( _witnesses.find( scheduled_witness ) == _witnesses.end() )
   {
      capture("scheduled_witness", scheduled_witness);
      return block_production_condition::not_my_turn;
   }

   fc::time_point_sec scheduled_time = db.get_slot_time( slot );
   steemit::chain::public_key_type scheduled_key = db.get< steemit::chain::witness_object, steemit::chain::by_name >(scheduled_witness).signing_key;
   auto private_key_itr = _private_keys.find( scheduled_key );

   if( private_key_itr == _private_keys.end() )
   {
      capture("scheduled_witness", scheduled_witness);
      capture("scheduled_key", scheduled_key);
      return block_production_condition::no_private_key;
   }

   uint32_t prate = db.witness_participation_rate();
   if( prate < _required_witness_participation )
   {
      capture("pct", uint32_t(100*uint64_t(prate) / STEEMIT_1_PERCENT));
      return block_production_condition::low_participation;
   }

   if( llabs((scheduled_time - now).count()) > fc::milliseconds( 500 ).count() )
   {
      capture("scheduled_time", scheduled_time)("now", now);
      return block_production_condition::lag;
   }

   auto block = db.generate_block(
      scheduled_time,
      scheduled_witness,
      private_key_itr->second,
      _production_skip_flags
      );
   capture("n", block.block_num())("t", block.timestamp)("c", now);

   appbase::app().get_plugin< steemit::plugins::p2p::p2p_plugin >().broadcast_block( block );
   return block_production_condition::produced;
}


witness_plugin::witness_plugin() :
   appbase::plugin< witness_plugin >( WITNESS_PLUGIN_NAME ),
   my( new witness_plugin_impl( appbase::app().get_io_service() ) ) {}

witness_plugin::~witness_plugin() {}

void witness_plugin::set_program_options(
   boost::program_options::options_description& command_line_options,
   boost::program_options::options_description& config_file_options)
{
   string witness_id_example = "initwitness";
   command_line_options.add_options()
         ("enable-stale-production", bpo::bool_switch()->notifier([this](bool e){my->_production_enabled = e;}), "Enable block production, even if the chain is stale.")
         ("required-participation", bpo::bool_switch()->notifier([this](int e){my->_required_witness_participation = uint32_t(e*STEEMIT_1_PERCENT);}), "Percent of witnesses (0-99) that must be participating in order to produce blocks")
         ("witness,w", bpo::value<vector<string>>()->composing()->multitoken(),
            ("name of witness controlled by this node (e.g. " + witness_id_example + " )" ).c_str() )
         ("private-key", bpo::value<vector<string>>()->composing()->multitoken(), "WIF PRIVATE KEY to be used by one or more witnesses or miners" )
         ;
   config_file_options.add(command_line_options);
}

void witness_plugin::plugin_initialize(const boost::program_options::variables_map& options)
{ try {
   LOAD_VALUE_SET( options, "witness", my->_witnesses, steemit::protocol::account_name_type )

   if( options.count("private-key") )
   {
      const std::vector<std::string> keys = options["private-key"].as<std::vector<std::string>>();
      for (const std::string& wif_key : keys )
      {
         fc::optional<fc::ecc::private_key> private_key = graphene::utilities::wif_to_key(wif_key);
         FC_ASSERT( private_key.valid(), "unable to parse private key" );
         my->_private_keys[private_key->get_public_key()] = *private_key;
      }
   }

   my->_custom_operation_interpreter = std::make_shared< generic_custom_operation_interpreter< witness_plugin_operation > >( my->_db );
   my->_custom_operation_interpreter->register_evaluator< enable_content_editing_evaluator >( this );
   appbase::app().get_plugin< steemit::plugins::chain::chain_plugin >().db().set_custom_operation_interpreter( name(), my->_custom_operation_interpreter );

   my->_db.on_pre_apply_transaction.connect( [&]( const signed_transaction& tx ){ my->pre_transaction( tx ); } );
   my->_db.pre_apply_operation.connect( [&]( const operation_notification& note ){ my->pre_operation( note ); } );
   my->_db.applied_block.connect( [&]( const signed_block& b ){ my->on_block( b ); } );

   add_plugin_index< account_bandwidth_index >( my->_db );
   add_plugin_index< content_edit_lock_index >( my->_db );
   add_plugin_index< reserve_ratio_index     >( my->_db );
} FC_LOG_AND_RETHROW() }

void witness_plugin::plugin_startup()
{ try {
   ilog("witness plugin:  plugin_startup() begin");
   steemit::chain::database& d = appbase::app().get_plugin< steemit::plugins::chain::chain_plugin >().db();

   if( !my->_witnesses.empty() )
   {
      ilog( "Launching block production for ${n} witnesses.", ("n", my->_witnesses.size()) );
      if( my->_production_enabled )
      {
         if( d.head_block_num() == 0 )
            new_chain_banner( d );
         my->_production_skip_flags |= steemit::chain::database::skip_undo_history_check;
      }
      my->schedule_production_loop();
   } else
      elog("No witnesses configured! Please add witness IDs and private keys to configuration.");
   ilog("witness plugin:  plugin_startup() end");
   } FC_CAPTURE_AND_RETHROW() }

void witness_plugin::plugin_shutdown()
{
   try
   {
      my->_timer.cancel();
   }
   catch(fc::exception& e)
   {
      edump( (e.to_detail_string()) );
   }
}

} } } // steemit::plugins::witness