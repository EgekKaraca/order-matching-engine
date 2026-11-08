// portfolio.hpp — position, cash, and fees. All integer.
//
// Cash is carried in (price unit x share) and position in shares, so nothing
// here rounds. A backtest that accumulates floating-point cash over a hundred
// thousand fills is one whose last digits are noise, and the headline number of
// this project is a ratio of two PnLs — precisely where accumulated noise would
// show up.
//
// Fees are per share in price units rather than basis points of notional, for
// the same reason: a bps fee needs a division and a rounding rule, and the
// rounding rule then quietly becomes part of the result.
#ifndef SIM_PORTFOLIO_HPP
#define SIM_PORTFOLIO_HPP

#include "sim/types.hpp"

#include <cstdint>

#include "match/types.hpp"

namespace sim {

// Negative means a rebate paid TO the strategy. US equity maker-taker venues
// typically rebate the maker and charge the taker, which is exactly the
// economics a passive market maker is trying to harvest — so getting the sign
// right here is not a detail.
struct FeeSchedule {
    std::int64_t maker_per_share = 0;  // negative = rebate received
    std::int64_t taker_per_share = 0;  // positive = fee paid

    static FeeSchedule none() { return FeeSchedule{0, 0}; }
};

class Portfolio {
  public:
    explicit Portfolio(FeeSchedule fees = FeeSchedule::none()) : fees_(fees) {}

    void apply(const Fill& fill) {
        const auto qty = static_cast<std::int64_t>(fill.quantity.units);
        const std::int64_t signed_qty = fill.side == match::Side::Buy ? qty : -qty;

        position_ += signed_qty;
        // Buying spends cash, selling raises it.
        cash_ -= signed_qty * fill.price.ticks;

        const std::int64_t fee = (fill.maker ? fees_.maker_per_share : fees_.taker_per_share) * qty;
        cash_ -= fee;
        fees_paid_ += fee;

        ++fill_count_;
        filled_shares_ += qty;
        if (fill.side == match::Side::Buy) {
            bought_ += qty;
        } else {
            sold_ += qty;
        }
    }

    std::int64_t position() const { return position_; }
    std::int64_t cash() const { return cash_; }
    std::int64_t fees_paid() const { return fees_paid_; }
    std::size_t fill_count() const { return fill_count_; }
    std::int64_t filled_shares() const { return filled_shares_; }
    std::int64_t bought() const { return bought_; }
    std::int64_t sold() const { return sold_; }

    // Total PnL marked at a price, in price units x shares. Inventory is marked
    // to the mid rather than assumed liquidatable at the touch — assuming the
    // touch would hand the strategy a free half-spread on every unclosed
    // position, which is the same class of error the whole project is about.
    //
    // Takes twice the mid so the caller need not round a half-tick mid; the
    // result is likewise doubled and halved once at the end.
    std::int64_t pnl_x2(std::int64_t mark_mid_x2) const {
        return 2 * cash_ + position_ * mark_mid_x2;
    }

  private:
    FeeSchedule fees_;
    std::int64_t position_ = 0;
    std::int64_t cash_ = 0;
    std::int64_t fees_paid_ = 0;
    std::int64_t filled_shares_ = 0;
    std::int64_t bought_ = 0;
    std::int64_t sold_ = 0;
    std::size_t fill_count_ = 0;
};

}  // namespace sim

#endif  // SIM_PORTFOLIO_HPP
