/**
 * lee_ready.cpp
 * Lee-Ready Tick Classification Algorithm implementation.
 * Lee, C. & Ready, M. (1991). Inferring Trade Direction from Intraday Data.
 * Journal of Finance, 46(2), 733-746.
 *
 * PERFORMANCE: All methods are O(1) with no heap allocation.
 * classify_trade() is the hot path — called on every trade tick.
 */

#include "core/lee_ready.h"
#include <cmath>

namespace ofe {
namespace core {

// ── LeeReadyClassifier public interface ──────────────────────────────────────

TickSide LeeReadyClassifier::classify_trade(
    double          trade_price,
    uint8_t         exchange_aggressor,
    LeeReadyState&  state
) const noexcept
{
    TickSide result = TickSide::UNKNOWN;

    // ── Rule 0: IQFeed direct aggressor (CME/ICE only) ────────────────────
    // Exchange-reported aggressor is 100% accurate — always prefer it.
    if (exchange_aggressor == 1) {
        result = TickSide::ASK;   // buyer aggressor
    } else if (exchange_aggressor == 2) {
        result = TickSide::BID;   // seller aggressor
    }
    // ── Rule 1: Quote Rule ────────────────────────────────────────────────
    else if (state.is_ready()) {
        result = quote_rule(trade_price, state.midpoint());

        // ── Rule 2: Tick Rule (fallback when price == midpoint) ───────────
        if (result == TickSide::UNKNOWN && state.prev_trade_price != 0.0) {
            result = tick_rule(trade_price, state.prev_trade_price,
                               state.last_classified);
        }
    }
    // ── No quote yet: tick rule only ─────────────────────────────────────
    else if (state.prev_trade_price != 0.0) {
        result = tick_rule(trade_price, state.prev_trade_price,
                           state.last_classified);
    }

    // Update state
    state.prev_trade_price = trade_price;
    state.last_classified  = result;

    return result;
}

void LeeReadyClassifier::update_quote(
    TickType        tick_type,
    double          price,
    LeeReadyState&  state
) const noexcept
{
    if (tick_type == TickType::BID_QUOTE) {
        state.prevailing_bid = price;
    } else if (tick_type == TickType::ASK_QUOTE) {
        state.prevailing_ask = price;
    }
}

void LeeReadyClassifier::classify_inplace(
    UniversalTickRecord& tick,
    LeeReadyState&       state
) const noexcept
{
    if (tick.tick_type == TickType::TRADE) {
        tick.side = classify_trade(
            tick.price,
            0,          // exchange_aggressor: adapters set this separately
            state
        );
    } else if (tick.is_quote_update()) {
        update_quote(tick.tick_type, tick.price, state);
        tick.side = TickSide::UNKNOWN;  // quote ticks have no aggressor side
    } else {
        tick.side = TickSide::UNKNOWN;
    }
}

// ── Private helpers ──────────────────────────────────────────────────────────

TickSide LeeReadyClassifier::quote_rule(
    double price,
    double midpoint
) noexcept
{
    // Small epsilon to handle floating-point imprecision at the midpoint
    constexpr double EPSILON = 1e-9;

    if (price > midpoint + EPSILON) return TickSide::ASK;
    if (price < midpoint - EPSILON) return TickSide::BID;
    return TickSide::UNKNOWN;  // exactly at midpoint — apply tick rule
}

TickSide LeeReadyClassifier::tick_rule(
    double   price,
    double   prev_price,
    TickSide last_classified
) noexcept
{
    constexpr double EPSILON = 1e-9;

    if (price > prev_price + EPSILON) return TickSide::ASK;  // uptick
    if (price < prev_price - EPSILON) return TickSide::BID;  // downtick
    return last_classified;                                   // zero-tick: carry forward
}

} // namespace core
} // namespace ofe
