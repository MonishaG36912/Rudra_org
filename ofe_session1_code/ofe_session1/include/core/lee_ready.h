#pragma once
/**
 * lee_ready.h
 * Lee-Ready Tick Classification Algorithm (Lee & Ready, J. Finance 1991).
 * Determines whether each trade was initiated by an aggressive buyer (ASK)
 * or aggressive seller (BID).
 *
 * Rules applied in order:
 *   1. IQFeed Direct Aggressor (CME/ICE only) — 100% accurate
 *   2. Quote Rule: compare price to bid-ask midpoint
 *   3. Tick Rule: compare price to previous trade price
 *
 * Spec reference: Indicator Formulas §9 (Tick Classification)
 * Academic reference: Lee, C. & Ready, M. (1991). Inferring Trade Direction
 *   from Intraday Data. Journal of Finance, 46(2), 733-746.
 */

#include <cstdint>
#include "tick_record.h"

namespace ofe {
namespace core {

/**
 * LeeReadyState
 * Per-symbol state maintained between tick calls.
 * Symbol Worker holds one LeeReadyState per symbol — updated on every tick.
 */
struct LeeReadyState {
    double   prev_trade_price = 0.0;  ///< Last trade price (for Tick Rule)
    double   prevailing_bid   = 0.0;  ///< Current best bid (for Quote Rule)
    double   prevailing_ask   = 0.0;  ///< Current best ask (for Quote Rule)
    TickSide last_classified  = TickSide::UNKNOWN;  ///< Result of last classification

    /// Returns true if state is initialised (at least one quote received)
    [[nodiscard]] bool is_ready() const noexcept {
        return prevailing_bid > 0.0 && prevailing_ask > 0.0;
    }

    /// Returns midpoint of the prevailing spread
    [[nodiscard]] double midpoint() const noexcept {
        return (prevailing_bid + prevailing_ask) * 0.5;
    }
};

/**
 * LeeReadyClassifier
 * Stateless classifier — all state is passed in/out via LeeReadyState.
 * This allows one instance to be shared safely across threads (one state per symbol).
 */
class LeeReadyClassifier {
public:
    LeeReadyClassifier() = default;

    /**
     * Classify a trade tick as ASK-side or BID-side.
     * Updates state.prev_trade_price and state.last_classified.
     *
     * Classification logic:
     *   - If exchange_aggressor is set (1 or 2): use direct classification
     *   - Else if price > midpoint: ASK (Quote Rule)
     *   - Else if price < midpoint: BID (Quote Rule)
     *   - Else if price > prev_trade: ASK (Tick Rule — uptick)
     *   - Else if price < prev_trade: BID (Tick Rule — downtick)
     *   - Else: carry forward last_classified (zero-tick rule)
     *
     * @param trade_price        Executed trade price
     * @param exchange_aggressor IQFeed aggressor field: 0=unknown, 1=buyer, 2=seller
     * @param state              Per-symbol state (updated in-place)
     * @return                   Classified side: ASK, BID, or UNKNOWN
     */
    [[nodiscard]] TickSide classify_trade(
        double          trade_price,
        uint8_t         exchange_aggressor,
        LeeReadyState&  state
    ) const noexcept;

    /**
     * Update the prevailing quote. Call this for every BID_QUOTE or ASK_QUOTE tick
     * BEFORE classifying the next trade tick.
     *
     * @param tick_type  Must be BID_QUOTE or ASK_QUOTE
     * @param price      New bid or ask price
     * @param state      Per-symbol state (updated in-place)
     */
    void update_quote(
        TickType        tick_type,
        double          price,
        LeeReadyState&  state
    ) const noexcept;

    /**
     * Classify a complete UniversalTickRecord in-place.
     * Sets tick.side based on the classification result.
     * Also handles quote updates internally.
     *
     * @param tick   Tick to classify (modified in-place: tick.side is set)
     * @param state  Per-symbol state (updated in-place)
     */
    void classify_inplace(
        UniversalTickRecord& tick,
        LeeReadyState&       state
    ) const noexcept;

private:
    /**
     * Apply Quote Rule: compare price to spread midpoint.
     * @return ASK if above midpoint, BID if below, UNKNOWN if at midpoint
     */
    [[nodiscard]] static TickSide quote_rule(
        double price,
        double midpoint
    ) noexcept;

    /**
     * Apply Tick Rule: compare price to previous trade price.
     * @return ASK if uptick, BID if downtick, last_classified if zero-tick
     */
    [[nodiscard]] static TickSide tick_rule(
        double   price,
        double   prev_price,
        TickSide last_classified
    ) noexcept;
};

} // namespace core
} // namespace ofe
