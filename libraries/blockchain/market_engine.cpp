#include <bts/blockchain/market_engine.hpp>
#include <fc/real128.hpp>

#include <bts/blockchain/fork_blocks.hpp>

namespace bts { namespace blockchain { namespace detail {

  market_engine::market_engine( pending_chain_state_ptr ps, chain_database_impl& cdi )
  :_pending_state(ps),_db_impl(cdi)
  {
      _pending_state = std::make_shared<pending_chain_state>( ps );
      _prior_state = ps;
  }

  void market_engine::cancel_all_shorts()
  {
      for( auto short_itr = _db_impl._short_db.begin(); short_itr.valid(); ++short_itr )
      {
          const market_index_key market_idx = short_itr.key();
          const order_record order_rec = short_itr.value();
          _current_bid = market_order( short_order, market_idx, order_rec );

          // Initialize the market transaction
          market_transaction mtrx;
          mtrx.bid_owner = _current_bid->get_owner();
          mtrx.bid_type = short_order;

          cancel_current_short( mtrx, market_idx.order_price.quote_asset_id );
          push_market_transaction( mtrx );
      }

      _pending_state->apply_changes();
  }

  bool market_engine::execute( asset_id_type quote_id, asset_id_type base_id, const fc::time_point_sec& timestamp )
  {
      try
      {
          _quote_id = quote_id;
          _base_id = base_id;

          oasset_record quote_asset = _pending_state->get_asset_record( _quote_id );
          oasset_record base_asset = _pending_state->get_asset_record( _base_id );
          FC_ASSERT( quote_asset.valid() && base_asset.valid() );

          // The order book is sorted from low to high price. So to get the last item (highest bid),
          // we need to go to the first item in the next market class and then back up one
          const price next_pair = (base_id+1 == quote_id) ? price( 0, quote_id+1, 0 ) : price( 0, quote_id, base_id+1 );
          _bid_itr        = _db_impl._bid_db.lower_bound( market_index_key( next_pair ) );
          _ask_itr        = _db_impl._ask_db.lower_bound( market_index_key( price( 0, quote_id, base_id) ) );
          _short_itr      = _db_impl._short_db.lower_bound( market_index_key( next_pair ) );
          _collateral_itr = _db_impl._collateral_db.lower_bound( market_index_key( next_pair ) );

          int last_orders_filled = -1;
          asset trading_volume(0, base_id);
          price opening_price, closing_price;

          if( !_ask_itr.valid() )
          {
            wlog( "ask iter invalid..." );
            _ask_itr = _db_impl._ask_db.begin();
          }

          if( _bid_itr.valid() )   --_bid_itr;
          else _bid_itr = _db_impl._bid_db.last();

          if( _collateral_itr.valid() )   --_collateral_itr;
          else _collateral_itr = _db_impl._collateral_db.last();

          if( _short_itr.valid() )   --_short_itr;
          else _short_itr = _db_impl._short_db.last();

          _feed_price = _db_impl.self->get_median_delegate_price( _quote_id, _base_id );
          // Market issued assets cannot match until the first time there is a median feed
          if( quote_asset->is_market_issued() )
          {
              const omarket_status market_stat = _pending_state->get_market_status( _quote_id, _base_id );
              if( (!market_stat.valid() || !market_stat->last_valid_feed_price.valid()) && !_feed_price.valid() )
                  FC_CAPTURE_AND_THROW( insufficient_feeds, (quote_id) );
          }

          // prime the pump, to make sure that margin calls (asks) have a bid to check against.
          get_next_bid(); get_next_ask();
          idump( (_current_bid)(_current_ask) );
          while( get_next_bid() && get_next_ask() )
          {
            idump( (_current_bid)(_current_ask) );

            // Make sure that at least one order was matched every time we enter the loop
            FC_ASSERT( _orders_filled != last_orders_filled, "We appear caught in an order matching loop!" );
            last_orders_filled = _orders_filled;

            // Initialize the market transaction
            market_transaction mtrx;
            mtrx.bid_owner = _current_bid->get_owner();
            mtrx.ask_owner = _current_ask->get_owner();
            mtrx.bid_price = _current_bid->get_price();
            mtrx.ask_price = _current_ask->get_price();
            mtrx.bid_type  = _current_bid->type;
            mtrx.ask_type  = _current_ask->type;

            if( _current_bid->type == short_order )
            {
                FC_ASSERT( quote_asset->is_market_issued() );
                if( !_feed_price.valid() ) { _current_bid.reset(); continue; }

                // Always execute shorts at the feed price
                mtrx.bid_price = *_feed_price;

                // Skip shorts that are over the price limit.
                if( _current_bid->state.short_price_limit.valid() )
                {
                  if( *_current_bid->state.short_price_limit < mtrx.ask_price )
                  {
                      _current_bid.reset(); continue;
                  }
                  mtrx.bid_price = std::min( *_current_bid->state.short_price_limit, mtrx.bid_price );
                }
            }

            if( _current_ask->type == cover_order )
            {
                FC_ASSERT( quote_asset->is_market_issued() );
                if( !_feed_price.valid() ) { _current_ask.reset(); continue; }

                /**
                *  If call price is not reached AND cover has not expired, he lives to fight another day.
                *  Also don't allow margin calls to be executed too far below
                *  the minimum ask, this could lead to an attack where someone
                *  walks the whole book to steal the collateral.
                */
                if( (mtrx.ask_price < mtrx.bid_price && _current_collat_record.expiration > _pending_state->now()) ||
                    mtrx.bid_price < minimum_ask() )
                {
                   _current_ask.reset(); continue;
                }
                //This is a forced cover. He's gonna sell at whatever price a buyer wants. No choice.
                mtrx.ask_price = mtrx.bid_price;
            }
            // get_next_ask() will return all covers first after checking expiration... which means
            // if it is not a cover then we can stop matching orders as soon as there exists a spread
            //// The ask price hasn't been reached
            else if( mtrx.bid_price < mtrx.ask_price ) break;

            if( _current_ask->type == cover_order && _current_bid->type == short_order )
            {
                price collateral_rate                = *_feed_price; // Asserted valid above
                collateral_rate.ratio               /= 2; // 2x from short, 1 x from long == 3x default collateral
                const asset cover_collateral         = asset( *_current_ask->collateral, _base_id );
                const asset max_usd_cover_can_afford = cover_collateral * mtrx.bid_price;
                const asset cover_debt               = get_current_cover_debt();
                const asset usd_for_short_sale       = _current_bid->get_balance() * collateral_rate; //_current_bid->get_quote_quantity();

                //Actual quote to purchase is the minimum of what's for sale, what can I possibly buy, and what I owe
                const asset usd_exchanged = std::min( {usd_for_short_sale, max_usd_cover_can_afford, cover_debt} );

                mtrx.ask_received   = usd_exchanged;

                /** handle rounding errors */
                // if cover collateral was completely consumed without paying off all USD
                if( usd_exchanged == max_usd_cover_can_afford )
                   mtrx.ask_paid       = cover_collateral;
                else  // the short was completely consumed
                   mtrx.ask_paid       = mtrx.ask_received * mtrx.ask_price;


                mtrx.bid_received   = mtrx.ask_paid;
                mtrx.bid_paid       = mtrx.ask_received;

                /** handle rounding errors */
                if( usd_exchanged == usd_for_short_sale ) // filled full short, consume all collateral
                   mtrx.short_collateral = _current_bid->get_balance();
                else
                   mtrx.short_collateral = mtrx.bid_paid * collateral_rate; /** note rounding errors handled in pay_current_short */

                pay_current_short( mtrx, *quote_asset, *base_asset );
                pay_current_cover( mtrx, *quote_asset );
            }
            else if( _current_ask->type == cover_order && _current_bid->type == bid_order )
            {
                const asset cover_collateral          = asset( *_current_ask->collateral, _base_id );
                const asset max_usd_cover_can_afford  = cover_collateral * mtrx.bid_price;
                const asset cover_debt                = get_current_cover_debt();
                const asset usd_for_sale              = _current_bid->get_balance();

                asset usd_exchanged = std::min( {usd_for_sale, max_usd_cover_can_afford, cover_debt} );

                mtrx.ask_received = usd_exchanged;

                /** handle rounding errors */
                // if cover collateral was completely consumed without paying off all USD
                if( mtrx.ask_received == max_usd_cover_can_afford )
                   mtrx.ask_paid = cover_collateral;
                else // the bid was completely consumed
                   mtrx.ask_paid = mtrx.ask_received * mtrx.ask_price;

                mtrx.bid_received = mtrx.ask_paid;
                mtrx.bid_paid     = mtrx.ask_received;

                pay_current_bid( mtrx, *quote_asset );
                pay_current_cover( mtrx, *quote_asset );
            }
            else if( _current_ask->type == ask_order && _current_bid->type == short_order )
            {
                // Bound collateral ratio (maximizes collateral of new margin position)
                price collateral_rate          = *_feed_price; // Asserted valid above
                collateral_rate.ratio          /= 2; // 2x from short, 1 x from long == 3x default collateral
                const asset ask_quantity_usd   = _current_ask->get_quote_quantity();
                const asset short_quantity_usd = _current_bid->get_balance() * collateral_rate;
                const asset usd_exchanged      = std::min( short_quantity_usd, ask_quantity_usd );

                mtrx.ask_received   = usd_exchanged;

                /** handle rounding errors */
                if( usd_exchanged == short_quantity_usd )
                {
                   mtrx.ask_paid       = mtrx.ask_received * mtrx.ask_price;
                   mtrx.short_collateral = _current_bid->get_balance();
                }
                else // filled the complete ask
                {
                   mtrx.ask_paid       = _current_ask->get_balance();
                   mtrx.short_collateral = usd_exchanged * collateral_rate;
                }

                mtrx.bid_received   = mtrx.ask_paid;
                mtrx.bid_paid       = mtrx.ask_received;

                pay_current_short( mtrx, *quote_asset, *base_asset );
                pay_current_ask( mtrx, *quote_asset );
            }
            else if( _current_ask->type == ask_order && _current_bid->type == bid_order )
            {
                const asset bid_quantity_xts = _current_bid->get_quantity();
                const asset ask_quantity_xts = _current_ask->get_quantity();
                const asset quantity_xts = std::min( bid_quantity_xts, ask_quantity_xts );

                // Everyone gets the price they asked for
                mtrx.ask_received   = quantity_xts * mtrx.ask_price;
                mtrx.bid_paid       = quantity_xts * mtrx.bid_price;

                mtrx.ask_paid       = quantity_xts;
                mtrx.bid_received   = quantity_xts;

                // Handle rounding errors
                if( quantity_xts == bid_quantity_xts )
                   mtrx.bid_paid = _current_bid->get_balance();

                if( quantity_xts == ask_quantity_xts )
                   mtrx.ask_paid = _current_ask->get_balance();

                mtrx.fees_collected = mtrx.bid_paid - mtrx.ask_received;

                pay_current_bid( mtrx, *quote_asset );
                pay_current_ask( mtrx, *base_asset );
            }

            push_market_transaction( mtrx );

            if( mtrx.ask_received.asset_id == 0 )
              trading_volume += mtrx.ask_received;
            else if( mtrx.bid_received.asset_id == 0 )
              trading_volume += mtrx.bid_received;
            if( opening_price == price() )
              opening_price = mtrx.bid_price;
            closing_price = mtrx.bid_price;

            if( mtrx.fees_collected.asset_id == base_asset->id )
                base_asset->collected_fees += mtrx.fees_collected.amount;
            else if( mtrx.fees_collected.asset_id == quote_asset->id )
                quote_asset->collected_fees += mtrx.fees_collected.amount;
          } // while( next bid && next ask )

          // update any fees collected
          _pending_state->store_asset_record( *quote_asset );
          _pending_state->store_asset_record( *base_asset );

          // Update market status and market history
          {
              omarket_status market_stat = _pending_state->get_market_status( _quote_id, _base_id );
              if( !market_stat.valid() ) market_stat = market_status( _quote_id, _base_id );
              market_stat->update_feed_price( _feed_price );
              market_stat->last_error.reset();
              _pending_state->store_market_status( *market_stat );

              update_market_history( trading_volume, opening_price, closing_price, timestamp );
          }

          wlog( "done matching orders" );
          idump( (_current_bid)(_current_ask) );

          _pending_state->apply_changes();
          return true;
    }
    catch( const fc::exception& e )
    {
        wlog( "error executing market ${quote} / ${base}\n ${e}", ("quote",quote_id)("base",base_id)("e",e.to_detail_string()) );
        omarket_status market_stat = _prior_state->get_market_status( _quote_id, _base_id );
        if( !market_stat.valid() ) market_stat = market_status( _quote_id, _base_id );
        market_stat->update_feed_price( _feed_price );
        market_stat->last_error = e;
        _prior_state->store_market_status( *market_stat );
    }
    return false;
  } // execute(...)

  void market_engine::push_market_transaction( const market_transaction& mtrx )
  { try {
      // If not an automatic market cancel
      if( mtrx.ask_paid.amount != 0
          || mtrx.ask_received.amount != 0
          || mtrx.bid_received.asset_id != 0
          || mtrx.bid_paid.amount != 0 )
      {
          FC_ASSERT( mtrx.bid_paid.amount >= 0 );
          FC_ASSERT( mtrx.ask_paid.amount >= 0 );
          FC_ASSERT( mtrx.bid_received.amount >= 0 );
          FC_ASSERT( mtrx.ask_received.amount>= 0 );
          FC_ASSERT( mtrx.bid_paid >= mtrx.ask_received );
          FC_ASSERT( mtrx.ask_paid >= mtrx.bid_received );
          FC_ASSERT( mtrx.fees_collected.amount >= 0 );
      }

      wlog( "${trx}", ("trx", fc::json::to_pretty_string( mtrx ) ) );

      _market_transactions.push_back(mtrx);
  } FC_CAPTURE_AND_RETHROW( (mtrx) ) }

  void market_engine::cancel_current_short( market_transaction& mtrx, const asset_id_type& quote_asset_id )
  {
      FC_ASSERT( _current_bid->type == short_order );
      FC_ASSERT( mtrx.bid_type == short_order );

      elog( "Canceling current short" );
      edump( (mtrx) );

      // Create automatic market cancel transaction
      mtrx.ask_paid       = asset();
      mtrx.ask_received   = asset( 0, quote_asset_id );
      mtrx.bid_received   = _current_bid->get_balance();
      mtrx.bid_paid       = asset( 0, quote_asset_id );
      mtrx.short_collateral.reset();

      // Fund refund balance record
      const balance_id_type id = withdraw_condition( withdraw_with_signature( mtrx.bid_owner ), 0 ).get_address();
      obalance_record bid_payout = _pending_state->get_balance_record( id );
      if( !bid_payout.valid() )
        bid_payout = balance_record( mtrx.bid_owner, asset( 0, 0 ), 0 );

      bid_payout->balance += mtrx.bid_received.amount;
      bid_payout->last_update = _pending_state->now();
      bid_payout->deposit_date = _pending_state->now();
      _pending_state->store_balance_record( *bid_payout );

      // Remove short order
      _current_bid->state.balance = 0;
      _pending_state->store_short_record( _current_bid->market_index, _current_bid->state );
  }

  void market_engine::pay_current_short( market_transaction& mtrx, asset_record& quote_asset, asset_record& base_asset )
  { try {
      FC_ASSERT( _current_bid->type == short_order );
      FC_ASSERT( mtrx.bid_type == short_order );

      // Because different collateral amounts create different orders, this prevents cover orders that
      // are too small to bother covering.
      if( (_current_bid->get_balance() - *mtrx.short_collateral).amount < base_asset.precision/100 )
      {
          if( _current_bid->get_balance() > *mtrx.short_collateral )
             *mtrx.short_collateral  += (_current_bid->get_balance() - *mtrx.short_collateral);
      }

      quote_asset.current_share_supply += mtrx.bid_paid.amount;

      auto collateral  = *mtrx.short_collateral + mtrx.ask_paid;
      if( mtrx.bid_paid.amount <= 0 )
      {
          FC_ASSERT( mtrx.bid_paid.amount >= 0 );
          _current_bid->state.balance -= mtrx.short_collateral->amount;
          return;
      }

      auto call_collateral = collateral;
      call_collateral.amount *= 2;
      call_collateral.amount /= 3;
      //auto cover_price = mtrx.bid_price;
      auto cover_price = mtrx.bid_paid / call_collateral;
      //cover_price.ratio *= 2;
      //cover_price.ratio /= 3;
      // auto cover_price = mtrx.bid_paid / asset( (3*collateral.amount)/4, _base_id );

      market_index_key cover_index( cover_price, _current_bid->get_owner() );
      auto ocover_record = _pending_state->get_collateral_record( cover_index );

      if( NOT ocover_record ) ocover_record = collateral_record();

      ocover_record->collateral_balance += collateral.amount;
      ocover_record->payoff_balance += mtrx.bid_paid.amount;
      ocover_record->interest_rate = _current_bid->market_index.order_price;
      ocover_record->expiration = _pending_state->now() + BTS_BLOCKCHAIN_MAX_SHORT_PERIOD_SEC;

      FC_ASSERT( ocover_record->payoff_balance >= 0, "", ("record",ocover_record) );
      FC_ASSERT( ocover_record->collateral_balance >= 0 , "", ("record",ocover_record));
      FC_ASSERT( ocover_record->interest_rate.quote_asset_id > ocover_record->interest_rate.base_asset_id,
                 "", ("record",ocover_record));

      _current_bid->state.balance -= mtrx.short_collateral->amount;

      FC_ASSERT( _current_bid->state.balance >= 0 );

      _pending_state->store_collateral_record( cover_index, *ocover_record );

      _pending_state->store_short_record( _current_bid->market_index, _current_bid->state );
  } FC_CAPTURE_AND_RETHROW( (mtrx)  ) }

  void market_engine::pay_current_bid( const market_transaction& mtrx, asset_record& quote_asset )
  { try {
      FC_ASSERT( _current_bid->type == bid_order );
      FC_ASSERT( mtrx.bid_type == bid_order );

      _current_bid->state.balance -= mtrx.bid_paid.amount;
      FC_ASSERT( _current_bid->state.balance >= 0 );

      auto bid_payout = _pending_state->get_balance_record(
                                withdraw_condition( withdraw_with_signature(mtrx.bid_owner), _base_id ).get_address() );
      if( !bid_payout )
          bid_payout = balance_record( mtrx.bid_owner, asset(0,_base_id), 0 );

      bid_payout->balance += mtrx.bid_received.amount;
      bid_payout->last_update = _pending_state->now();
      bid_payout->deposit_date = _pending_state->now();
      _pending_state->store_balance_record( *bid_payout );


      // if the balance is less than 1 XTS then it gets collected as fees.
      if( (_current_bid->get_quote_quantity() * _current_bid->get_price()).amount == 0 )
      {
          quote_asset.collected_fees += _current_bid->get_quote_quantity().amount;
          _current_bid->state.balance = 0;
      }
      _pending_state->store_bid_record( _current_bid->market_index, _current_bid->state );
  } FC_CAPTURE_AND_RETHROW( (mtrx) ) }

  void market_engine::pay_current_cover( market_transaction& mtrx, asset_record& quote_asset )
  { try {
      FC_ASSERT( _current_ask->type == cover_order );
      FC_ASSERT( mtrx.ask_type == cover_order );
      FC_ASSERT( _current_collat_record.interest_rate.quote_asset_id > _current_collat_record.interest_rate.base_asset_id );

      const asset principle = asset( _current_collat_record.payoff_balance, quote_asset.id );
      const auto cover_age = get_current_cover_age();
      const asset total_debt = get_current_cover_debt();

      asset principle_paid;
      asset interest_paid;
      if( mtrx.ask_received >= total_debt )
      {
          // Payoff the whole debt
          principle_paid = principle;
          interest_paid = mtrx.ask_received - principle_paid;
          _current_ask->state.balance = 0;
      }
      else
      {
          // Partial cover
          interest_paid = get_interest_paid( mtrx.ask_received, _current_collat_record.interest_rate, cover_age );

          if( _pending_state->get_head_block_num() < BTS_V0_4_23_FORK_BLOCK_NUM )
          {
              interest_paid = get_interest_paid_v1( mtrx.ask_received, _current_collat_record.interest_rate, cover_age );
          }

          principle_paid = mtrx.ask_received - interest_paid;
          _current_ask->state.balance -= principle_paid.amount;
      }
      FC_ASSERT( principle_paid.amount >= 0 );
      FC_ASSERT( interest_paid.amount >= 0 );
      FC_ASSERT( _current_ask->state.balance >= 0 );

      *(_current_ask->collateral) -= mtrx.ask_paid.amount;

      FC_ASSERT( *_current_ask->collateral >= 0, "",
                 ("mtrx",mtrx)("_current_ask", _current_ask)("interest_paid",interest_paid)  );

      quote_asset.current_share_supply -= principle_paid.amount;
      quote_asset.collected_fees       += interest_paid.amount;
      if( *_current_ask->collateral == 0 )
      {
          quote_asset.collected_fees -= _current_ask->state.balance;
          _current_ask->state.balance = 0;
      }

      // If debt is fully paid off and there is leftover collateral
      if( _current_ask->state.balance == 0 && *_current_ask->collateral > 0 )
      { // send collateral home to mommy & daddy
            auto ask_balance_address = withdraw_condition(
                                              withdraw_with_signature(_current_ask->get_owner()),
                                              _base_id ).get_address();

            auto ask_payout = _pending_state->get_balance_record( ask_balance_address );
            if( !ask_payout )
                ask_payout = balance_record( _current_ask->get_owner(), asset(0,_base_id), 0 );

            auto left_over_collateral = (*_current_ask->collateral);

            if( _current_collat_record.expiration > _pending_state->now() )
            {
               /** charge 5% fee for having a margin call */
               auto fee = (left_over_collateral * 5000 )/100000;
               left_over_collateral -= fee;
               // when executing a cover order, it always takes the exact price of the
               // highest bid, so there should be no fees paid *except* this.
               FC_ASSERT( mtrx.fees_collected.amount == 0 );

               // these go to the network... as dividends..
               mtrx.fees_collected += asset( fee, _base_id );
            }

            ask_payout->balance += left_over_collateral;
            ask_payout->last_update = _pending_state->now();
            ask_payout->deposit_date = _pending_state->now();

            mtrx.returned_collateral = left_over_collateral;

            _pending_state->store_balance_record( *ask_payout );
            _current_ask->collateral = 0;
      }

      _current_collat_record.collateral_balance = *_current_ask->collateral;
      _current_collat_record.payoff_balance = _current_ask->state.balance;
      _pending_state->store_collateral_record( _current_ask->market_index,
                                               _current_collat_record );
  } FC_CAPTURE_AND_RETHROW( (mtrx) ) }

  void market_engine::pay_current_ask( const market_transaction& mtrx, asset_record& base_asset )
  { try {
      FC_ASSERT( _current_ask->type == ask_order );
      FC_ASSERT( mtrx.ask_type == ask_order );

      _current_ask->state.balance -= mtrx.ask_paid.amount;
      FC_ASSERT( _current_ask->state.balance >= 0 );

      auto ask_balance_address = withdraw_condition( withdraw_with_signature(mtrx.ask_owner), _quote_id ).get_address();
      auto ask_payout = _pending_state->get_balance_record( ask_balance_address );
      if( !ask_payout )
          ask_payout = balance_record( mtrx.ask_owner, asset(0,_quote_id), 0 );
      ask_payout->balance += mtrx.ask_received.amount;
      ask_payout->last_update = _pending_state->now();
      ask_payout->deposit_date = _pending_state->now();

      _pending_state->store_balance_record( *ask_payout );

      // if the balance is less than 1 XTS * PRICE < .001 USD XTS goes to fees
      if( (_current_ask->get_quantity() * _current_ask->get_price()).amount == 0 )
      {
          base_asset.collected_fees += _current_ask->get_quantity().amount;
          _current_ask->state.balance = 0;
      }
      _pending_state->store_ask_record( _current_ask->market_index, _current_ask->state );

  } FC_CAPTURE_AND_RETHROW( (mtrx) )  } // pay_current_ask

  bool market_engine::get_next_short()
  {
      if( _short_itr.valid() )
      {
        auto bid = market_order( short_order,
                                 _short_itr.key(),
                                 _short_itr.value(),
                                 _short_itr.value().balance,
                                 _short_itr.key().order_price );
        if( bid.get_price().quote_asset_id == _quote_id &&
            bid.get_price().base_asset_id == _base_id )
        {
            --_short_itr;
            _current_bid = bid;
            return _current_bid.valid();
        }
      }
      return false;
  }

  bool market_engine::get_next_bid()
  { try {
      if( _current_bid && _current_bid->get_quantity().amount > 0 )
        return _current_bid.valid();

      ++_orders_filled;
      _current_bid.reset();

      if( _bid_itr.valid() )
      {
        auto bid = market_order( bid_order, _bid_itr.key(), _bid_itr.value() );
        if( bid.get_price().quote_asset_id == _quote_id &&
            bid.get_price().base_asset_id == _base_id )
        {
            if( _feed_price.valid() && bid.get_price() < *_feed_price && get_next_short() )
                return _current_bid.valid();

            _current_bid = bid;
            --_bid_itr;
            return _current_bid.valid();
        }
      }
      get_next_short();
      return _current_bid.valid();
  } FC_CAPTURE_AND_RETHROW() }

  bool market_engine::get_next_ask()
  { try {
      if( _current_ask && _current_ask->state.balance > 0 )
        return _current_ask.valid();

      _current_ask.reset();
      ++_orders_filled;

      /**
      *  Margin calls take priority over all other ask orders
      */
      while( _current_bid && _collateral_itr.valid() )
      {
        const auto cover_ask = market_order( cover_order,
                                             _collateral_itr.key(),
                                             order_record(_collateral_itr.value().payoff_balance),
                                             _collateral_itr.value().collateral_balance,
                                             _collateral_itr.value().interest_rate,
                                             _collateral_itr.value().expiration);

        if( cover_ask.get_price().quote_asset_id == _quote_id &&
            cover_ask.get_price().base_asset_id == _base_id )
        {
            _current_collat_record = _collateral_itr.value();
            // Don't cover unless the price is below the feed price or margin position is expired
            if( (_feed_price.valid() && cover_ask.get_price() > *_feed_price)
                || _current_collat_record.expiration <= _pending_state->now() )
            {
                _current_ask = cover_ask;
                --_collateral_itr;
                return _current_ask.valid();
            }
            if( _pending_state->get_head_block_num() >= BTS_V0_4_24_FORK_BLOCK_NUM )
            {
                --_collateral_itr;
                continue;
            }
        }
        _collateral_itr.reset();
        break;
      }

      if( _ask_itr.valid() )
      {
        const auto ask = market_order( ask_order, _ask_itr.key(), _ask_itr.value() );
        if( ask.get_price().quote_asset_id == _quote_id &&
            ask.get_price().base_asset_id == _base_id )
        {
            _current_ask = ask;
        }
        ++_ask_itr;
      }
      return _current_ask.valid();
  } FC_CAPTURE_AND_RETHROW() }


  /**
    *  This method should not affect market execution or validation and
    *  is for historical purposes only.
    */
  void market_engine::update_market_history( const asset& trading_volume,
                                             const price& opening_price,
                                             const price& closing_price,
                                             const fc::time_point_sec& timestamp )
  {
          if( trading_volume.amount > 0 && get_next_bid() && get_next_ask() )
          {
            market_history_key key(_quote_id, _base_id, market_history_key::each_block, _db_impl._head_block_header.timestamp);
            market_history_record new_record(_current_bid->get_price(),
                                            _current_ask->get_price(),
                                            opening_price,
                                            closing_price,
                                            trading_volume.amount);

            //LevelDB iterators are dumb and don't support proper past-the-end semantics.
            auto last_key_itr = _db_impl._market_history_db.lower_bound(key);
            if( !last_key_itr.valid() )
              last_key_itr = _db_impl._market_history_db.last();
            else
              --last_key_itr;

            key.timestamp = timestamp;

            //Unless the previous record for this market is the same as ours...
            if( (!(last_key_itr.valid()
                && last_key_itr.key().quote_id == _quote_id
                && last_key_itr.key().base_id == _base_id
                && last_key_itr.key().granularity == market_history_key::each_block
                && last_key_itr.value() == new_record)) )
            {
              //...add a new entry to the history table.
              _pending_state->market_history[key] = new_record;
            }

            fc::time_point_sec start_of_this_hour = timestamp - (timestamp.sec_since_epoch() % (60*60));
            market_history_key old_key(_quote_id, _base_id, market_history_key::each_hour, start_of_this_hour);
            if( auto opt = _db_impl._market_history_db.fetch_optional(old_key) )
            {
              auto old_record = *opt;
              old_record.volume += new_record.volume;
              old_record.closing_price = new_record.closing_price;
              if( new_record.highest_bid > old_record.highest_bid || new_record.lowest_ask < old_record.lowest_ask )
              {
                old_record.highest_bid = std::max(new_record.highest_bid, old_record.highest_bid);
                old_record.lowest_ask = std::min(new_record.lowest_ask, old_record.lowest_ask);
                _pending_state->market_history[old_key] = old_record;
              }
            }
            else
              _pending_state->market_history[old_key] = new_record;

            fc::time_point_sec start_of_this_day = timestamp - (timestamp.sec_since_epoch() % (60*60*24));
            old_key = market_history_key(_quote_id, _base_id, market_history_key::each_day, start_of_this_day);
            if( auto opt = _db_impl._market_history_db.fetch_optional(old_key) )
            {
              auto old_record = *opt;
              old_record.volume += new_record.volume;
              old_record.closing_price = new_record.closing_price;
              if( new_record.highest_bid > old_record.highest_bid || new_record.lowest_ask < old_record.lowest_ask )
              {
                old_record.highest_bid = std::max(new_record.highest_bid, old_record.highest_bid);
                old_record.lowest_ask = std::min(new_record.lowest_ask, old_record.lowest_ask);
                _pending_state->market_history[old_key] = old_record;
              }
            }
            else
              _pending_state->market_history[old_key] = new_record;
          }
  }

  asset market_engine::get_interest_paid(const asset& total_amount_paid, const price& apr, uint32_t age_seconds)
  {
      // TOTAL_PAID = DELTA_PRINCIPLE + DELTA_PRINCIPLE * APR * PERCENT_OF_YEAR
      // DELTA_PRINCIPLE = TOTAL_PAID / (1 + APR*PERCENT_OF_YEAR)
      // INTEREST_PAID  = TOTAL_PAID - DELTA_PRINCIPLE
      fc::real128 total_paid( total_amount_paid.amount );
      fc::real128 apr_n( (asset( BTS_BLOCKCHAIN_MAX_SHARES, apr.base_asset_id ) * apr).amount );
      fc::real128 apr_d( (asset( BTS_BLOCKCHAIN_MAX_SHARES, apr.base_asset_id ) ).amount );
      fc::real128 iapr = apr_n / apr_d;
      fc::real128 age_sec(age_seconds);
      fc::real128 sec_per_year(365 * 24 * 60 * 60);
      fc::real128 percent_of_year = age_sec / sec_per_year;

      fc::real128 delta_principle = total_paid / ( fc::real128(1) + iapr * percent_of_year );
      fc::real128 interest_paid   = total_paid - delta_principle;

      return asset( interest_paid.to_uint64(), total_amount_paid.asset_id );
  }

  asset market_engine::get_interest_owed(const asset& principle, const price& apr, uint32_t age_seconds)
  {
      // INTEREST_OWED = TOTAL_PRINCIPLE * APR * PERCENT_OF_YEAR
      fc::real128 total_principle( principle.amount );
      fc::real128 apr_n( (asset( BTS_BLOCKCHAIN_MAX_SHARES, apr.base_asset_id ) * apr).amount );
      fc::real128 apr_d( (asset( BTS_BLOCKCHAIN_MAX_SHARES, apr.base_asset_id ) ).amount );
      fc::real128 iapr = apr_n / apr_d;
      fc::real128 age_sec(age_seconds);
      fc::real128 sec_per_year(365 * 24 * 60 * 60);
      fc::real128 percent_of_year = age_sec / sec_per_year;

      fc::real128 interest_owed   = total_principle * iapr * percent_of_year;

      return asset( interest_owed.to_uint64(), principle.asset_id );
  }

  asset market_engine::get_interest_paid_v1(const asset& total_amount_paid, const price& apr, uint32_t age_seconds)
  {
      // TOTAL_PAID = DELTA_PRINCIPLE + DELTA_PRINCIPLE * APR * PERCENT_OF_YEAR
      // DELTA_PRINCIPLE = TOTAL_PAID / (1 + APR*PERCENT_OF_YEAR)
      // INTEREST_PAID  = TOTAL_PAID - DELTA_PRINCIPLE
      fc::real128 total_paid( total_amount_paid.amount );
      fc::real128 apr_n( (asset( BTS_BLOCKCHAIN_MAX_SHARES, apr.quote_asset_id ) * apr).amount );
      fc::real128 apr_d( (asset( BTS_BLOCKCHAIN_MAX_SHARES, apr.quote_asset_id ) ).amount );
      fc::real128 iapr = apr_n / apr_d;
      fc::real128 age_sec(age_seconds);
      fc::real128 sec_per_year(365 * 24 * 60 * 60);
      fc::real128 percent_of_year = age_sec / sec_per_year;

      fc::real128 delta_principle = total_paid / ( fc::real128(1) + iapr * percent_of_year );
      fc::real128 interest_paid   = total_paid - delta_principle;

      return asset( interest_paid.to_uint64(), total_amount_paid.asset_id );
  }

  asset market_engine::get_interest_owed_v1(const asset& principle, const price& apr, uint32_t age_seconds)
  {
      // INTEREST_OWED = TOTAL_PRINCIPLE * APR * PERCENT_OF_YEAR
      fc::real128 total_principle( principle.amount );
      fc::real128 apr_n( (asset( BTS_BLOCKCHAIN_MAX_SHARES, apr.quote_asset_id ) * apr).amount );
      fc::real128 apr_d( (asset( BTS_BLOCKCHAIN_MAX_SHARES, apr.quote_asset_id ) ).amount );
      fc::real128 iapr = apr_n / apr_d;
      fc::real128 age_sec(age_seconds);
      fc::real128 sec_per_year(365 * 24 * 60 * 60);
      fc::real128 percent_of_year = age_sec / sec_per_year;

      fc::real128 interest_owed   = total_principle * iapr * percent_of_year;

      return asset( interest_owed.to_uint64(), principle.asset_id );
  }

  asset market_engine::get_current_cover_debt() const
  {
      if( _pending_state->get_head_block_num() < BTS_V0_4_23_FORK_BLOCK_NUM )
      {
          return get_interest_owed_v1( _current_ask->get_balance(),
                                       _current_collat_record.interest_rate,
                                       get_current_cover_age() ) + _current_ask->get_balance();
      }

      return get_interest_owed( _current_ask->get_balance(),
                                _current_collat_record.interest_rate,
                                get_current_cover_age() ) + _current_ask->get_balance();
  }

} } } // end namespace bts::blockchain::detail
