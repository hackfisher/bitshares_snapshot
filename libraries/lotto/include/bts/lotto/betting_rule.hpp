#pragma once
#include <bts/lotto/rule.hpp>

namespace bts { namespace lotto {
    namespace detail  { class betting_rule_impl; }

    class betting_rule : public rule
    {
        public:

            betting_rule(lotto_db* db, ticket_type t, asset_id_type u);
            virtual ~betting_rule() override;

            virtual void                  open(const fc::path& dir, bool create = true) override;
            virtual void                  close() override;

            virtual asset jackpot_payout(const meta_ticket_output& meta_ticket_out) override;

            //virtual void store(const full_block& blk, const signed_transactions& deterministic_trxs, const block_evaluation_state_ptr& state);

        protected:
            std::unique_ptr<detail::betting_rule_impl> my;
    };

    typedef std::shared_ptr<betting_rule> betting_rule_ptr;

} } // bts::lotto

