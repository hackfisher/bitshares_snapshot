#pragma once
#include <bts/lotto/dice_rule.hpp>
#include <bts/lotto/lotto_config.hpp>

#include <bts/lotto/lotto_db.hpp>

#include <bts/db/level_map.hpp>

namespace bts { namespace lotto {
    namespace detail
    { 
        class dice_rule_impl        {        public:            dice_rule_impl(){}        };
    }

    dice_rule::dice_rule(lotto_db* db, ticket_type t, asset_id_type u)
        :rule(db, t, u), my(new detail::dice_rule_impl())
    {
    }

    dice_rule::~dice_rule()
    {
    }

    uint64_t dice_rule::calculate_payout(uint64_t block_random, uint64_t ticket_random, uint16_t odds, uint64_t amt)
    {
        uint64_t range = 100000000;
        double house_edge = 0.01;
        double payout = 0;

        if ( ( ( (block_random % range  + ticket_random % range ) % range ) * odds) < range )
        {
            payout = amt * odds * (1 - house_edge);
        }

        return (uint64_t)payout;
    }

    // https://bitsharestalk.org/index.php?topic=4505.0
    asset dice_rule::jackpot_payout(const meta_ticket_output& meta_ticket_out)
    {
        try {
            auto headnum = _lotto_db->get_head_block_num();
            auto trx_loc = _lotto_db->get_transaction_location(meta_ticket_out.out_idx.trx_id);
            FC_ASSERT(meta_ticket_out.ticket_op.ticket.ticket_func == get_ticket_type());
            
            FC_ASSERT(headnum >= BTS_LOTTO_BLOCKS_BEFORE_JACKPOTS_DRAW + trx_loc->block_num);

            uint64_t random_number = _lotto_db->fetch_blk_random_number(trx_loc->block_num + BTS_LOTTO_BLOCKS_BEFORE_JACKPOTS_DRAW);

            auto trx_hash = meta_ticket_out.out_idx.trx_id;
            uint64_t trx_random = fc::sha256::hash((char*)&trx_hash, sizeof(trx_hash))._hash[0];

            auto ticket = meta_ticket_out.ticket_op.ticket.as<dice_ticket>();

            uint64_t payout = calculate_payout(random_number, trx_random, ticket.odds, meta_ticket_out.amount.amount);

            return asset(payout, meta_ticket_out.amount.asset_id);

        } FC_RETHROW_EXCEPTIONS(warn, "Error calculating jackpots for dice ticket ${m}", ("m", meta_ticket_out));
    }

} } // bts::lotto