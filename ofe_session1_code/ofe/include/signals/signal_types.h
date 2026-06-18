#pragma once
/**
 * signal_types.h
 * All signal type definitions, direction enums, and the SignalEvent struct.
 * Every signal produced by the engine is represented as a SignalEvent.
 *
 * Spec reference: Functional Spec §4.10 (Special Order Flow Signals),
 *                 SDK Spec §3.6 (all signal schemas)
 */

#include <cstdint>
#include <string>
#include <vector>

namespace ofe {
namespace signals {

// ── Signal type enumeration ───────────────────────────────────────────────────

/**
 * SignalType — every detectable signal type in the OFE engine.
 * Corresponds to the filter keys in the SDK WebSocket API.
 */
enum class SignalType : uint16_t {

    // ── Delta signals ──────────────────────────────────────────────────────
    DELTA_DIVERGENCE_STANDARD   = 100, ///< Price new high/low + CVD opposite direction
    DELTA_DIVERGENCE_BAR        = 101, ///< Bearish bar + positive delta (or vice versa)
    DELTA_SURGE_BULLISH         = 102, ///< Delta surge > N × average (upward)
    DELTA_SURGE_BEARISH         = 103, ///< Delta surge > N × average (downward)
    EXTREME_DELTA               = 104, ///< ABS(bar_delta) exceeds session maximum threshold
    SMALL_MIN_MAX_DELTA         = 105, ///< Max-min delta range unusually small (consolidation)

    // ── Imbalance signals ──────────────────────────────────────────────────
    SINGLE_IMBALANCE_BUY        = 200, ///< ask_vol[P] >= ratio × bid_vol[P-1tick]
    SINGLE_IMBALANCE_SELL       = 201, ///< bid_vol[P] >= ratio × ask_vol[P+1tick]
    STACKED_IMBALANCE_BUY       = 202, ///< 3+ consecutive buying imbalances
    STACKED_IMBALANCE_SELL      = 203, ///< 3+ consecutive selling imbalances
    IMBALANCE_2ND_SLOT_BUY      = 204, ///< Imbalance 1-2 levels from prior zone
    IMBALANCE_2ND_SLOT_SELL     = 205,
    IMBALANCE_RELOAD_BUY        = 206, ///< Second imbalance at same price next bar
    IMBALANCE_RELOAD_SELL       = 207,
    IMBALANCE_INVERSE_BUY       = 208, ///< Imbalance opposite to prevailing trend
    IMBALANCE_INVERSE_SELL      = 209,
    IMBALANCE_REVERSAL_BUY      = 210, ///< Imbalance in opposite direction after exhaustion
    IMBALANCE_REVERSAL_SELL     = 211,
    MULTIPLE_IMBALANCES_BUY     = 212, ///< Cluster across multiple bars at same price
    MULTIPLE_IMBALANCES_SELL    = 213,

    // ── Volume Profile signals ─────────────────────────────────────────────
    OPEN_POC                    = 300, ///< Current session POC as it builds
    ALIGNED_POCS                = 301, ///< Two or more session POCs at same price
    PROMINENT_POC               = 302, ///< POC significantly above surrounding volume
    POC_WAVE                    = 303, ///< POC migration direction during session build
    POC_SLINGSHOT_BULLISH       = 304, ///< Break above POC with strong delta
    POC_SLINGSHOT_BEARISH       = 305,
    RETAIL_SUCK                 = 306, ///< Price spikes through key level then reverses
    ENGULFING_VALUE_AREA        = 307, ///< Bar range engulfs previous value area

    // ── VWAP signals ───────────────────────────────────────────────────────
    VWAP_REACTION_LONG          = 400, ///< Price touches VWAP from above + delta confirms
    VWAP_REACTION_SHORT         = 401,
    VWAP_ROTATION_LONG          = 402, ///< Price at -1σ band, delta positive
    VWAP_ROTATION_SHORT         = 403,

    // ── Orderflows.com composite signals ──────────────────────────────────
    PULSE_LONG                  = 500, ///< Composite score >= threshold, direction LONG
    PULSE_SHORT                 = 501,
    TURNS_BULLISH               = 502, ///< TurnScore >= 3, bullish direction
    TURNS_BEARISH               = 503,
    RATIO_BOTTOM_HEAVY          = 504, ///< Big buyer at low (Bottom Heavy)
    RATIO_TOP_HEAVY             = 505, ///< Big seller at high (Top Heavy)
    SINGLE_PRINT_LAST_BUYER     = 506, ///< Zero/near-zero volume at bar high
    SINGLE_PRINT_LAST_SELLER    = 507,

    // ── Execution quality signals ──────────────────────────────────────────
    ABSORPTION                  = 600, ///< High volume + near-zero net delta at price
    TRAPPED_BUYERS              = 601, ///< Buy imbalance at bar high, bar closed low
    TRAPPED_SELLERS             = 602, ///< Sell imbalance at bar low, bar closed high
    MARKET_SWEEP_BULLISH        = 603, ///< Large aggressive orders sweep multiple levels up
    MARKET_SWEEP_BEARISH        = 604,
    MARKET_WEAKNESS_HIGH        = 605, ///< New high on declining delta/volume
    MARKET_WEAKNESS_LOW         = 606,
    EXHAUSTION_PRINT_HIGH       = 607, ///< Volume spike at high with delta reversal
    EXHAUSTION_PRINT_LOW        = 608,
    ZERO_PRINT                  = 609, ///< Price level with zero contracts inside bar
    UNFINISHED_BUSINESS_HIGH    = 610, ///< Non-zero ask at bar high (unfinished auction)
    UNFINISHED_BUSINESS_LOW     = 611,

    // ── Accumulation / Distribution ────────────────────────────────────────
    ACCUMULATION                = 700, ///< Multi-bar buying near lows
    DISTRIBUTION                = 701, ///< Multi-bar selling near highs
    RESTING_LIQUIDITY           = 702, ///< Large volume at price without moving it
    VERTICAL_LIQUIDITY          = 703, ///< Volume stacking at consecutive levels
    DELTA_TAIL_BULLISH          = 704, ///< Delta breaks above recent range
    DELTA_TAIL_BEARISH          = 705,
    ORDERFLOWS_SEQUENCING_UP    = 706, ///< Consecutive bars with increasing positive delta
    ORDERFLOWS_SEQUENCING_DOWN  = 707,
    PRICE_ACTION_DIVERGENCE     = 708, ///< Valtos U-Turn: buyers/sellers gave up
    PRICE_EXHAUSTION            = 709, ///< Price at extreme but delta does not confirm
    PRICE_DEFENSE               = 710, ///< Institutions defending a key level
    VOLUME_DECLINE              = 711, ///< Sequential volume decrease in trend direction
    ORDERFLOWS_GAPS             = 712, ///< Volume gaps inside bars (fast-move zones)
    THIN_PRINTS                 = 713, ///< Very low volume price levels
};

// ── Signal direction ──────────────────────────────────────────────────────────

enum class SignalDirection : uint8_t {
    LONG       = 0,  ///< Bullish / upward signal
    SHORT      = 1,  ///< Bearish / downward signal
    NEUTRAL    = 2,  ///< No directional bias (e.g. INFO signals)
    REVERSAL   = 3,  ///< Reversal signal (could be long or short depending on context)
    WARNING    = 4   ///< Warning / weakening signal
};

// ── Signal strength ───────────────────────────────────────────────────────────

enum class SignalStrength : uint8_t {
    WEAK        = 1,
    MODERATE    = 2,
    STRONG      = 3,
    VERY_STRONG = 4
};

// ── Signal event ─────────────────────────────────────────────────────────────

/**
 * SignalEvent
 * Complete signal record published to the EventBus and delivered to subscribers.
 * All signal types are represented as SignalEvent with type-specific
 * metadata in the `metadata` JSON string.
 *
 * Spec reference: SDK Spec §3.3-3.6 (all signal payload schemas)
 */
struct SignalEvent {
    // ── Identity ──────────────────────────────────────────────────────────
    std::string     signal_id;         ///< Unique signal ID (UUID v4)
    SignalType      type;              ///< Which signal fired
    SignalDirection direction;         ///< Long / short / neutral / reversal / warning
    SignalStrength  strength;          ///< 1-4 strength score

    // ── Symbol context ────────────────────────────────────────────────────
    uint32_t        symbol_id;         ///< Routing key
    std::string     symbol;            ///< Human-readable symbol
    std::string     watchlist_id;      ///< Watchlist this symbol belongs to (may be empty)

    // ── Timestamps (all four — spec §8.3) ─────────────────────────────────
    int64_t         detection_ts_ns;   ///< When engine determined condition met
    int64_t         bar_open_ts_ns;    ///< When triggering bar opened
    int64_t         bar_close_ts_ns;   ///< When triggering bar closed
    int64_t         exchange_ts_ns;    ///< Exchange time of last tick completing the condition

    // ── Price context ─────────────────────────────────────────────────────
    double          price_at_signal;   ///< Bar close price when signal fired
    double          suggested_entry;   ///< Suggested entry price (from signal logic)
    double          suggested_stop;    ///< Suggested stop loss price

    // ── Bar reference ─────────────────────────────────────────────────────
    std::string     bar_id;            ///< ID of the triggering bar
    double          bar_open;
    double          bar_high;
    double          bar_low;
    double          bar_close;
    int32_t         bar_delta;
    int64_t         bar_volume;
    int64_t         cumulative_delta;  ///< Session CVD at time of signal

    // ── Processing metadata ───────────────────────────────────────────────
    uint32_t        processing_latency_us; ///< Microseconds from bar_close to detection
    std::string     worker_id;             ///< Which Symbol Worker produced this signal

    // ── Type-specific payload (JSON) ──────────────────────────────────────
    /// JSON string containing signal-specific fields:
    /// - Pulse: composite_score, variable_scores{}
    /// - Turns: turn_score, variables_aligned
    /// - Imbalances: zone_id, imbalance_count, ratio, price_start, price_end
    /// - Ratio: ratio_value, acts_as (PRICE_FLOOR/CEILING)
    /// - Single Print: exhaustion_score, unfinished_auction, magnet_active
    /// - etc.
    std::string     metadata_json;
};

// ── Zone record (persistent imbalance zones) ─────────────────────────────────

/**
 * ImbalanceZone
 * Persistent stacked imbalance zone that survives across bars.
 * Created when stacked imbalance is first detected; updated as it grows;
 * deactivated when price trades through it.
 *
 * Spec reference: Functional Spec §4.3.2, SDK Spec §3.3 (zone_id persistence)
 */
struct ImbalanceZone {
    std::string     zone_id;           ///< Stable UUID for this zone
    uint32_t        symbol_id;
    SignalDirection direction;         ///< LONG (buy zone) or SHORT (sell zone)
    double          price_low;         ///< Lowest price in the zone
    double          price_high;        ///< Highest price in the zone
    int32_t         imbalance_count;   ///< Number of consecutive imbalances
    float           avg_ratio;         ///< Average imbalance ratio across levels
    float           strength_score;    ///< 1-10 composite strength
    int64_t         created_ts_ns;     ///< When zone was first created
    int64_t         last_updated_ts_ns;///< Last time zone was extended
    int64_t         resolved_ts_ns;    ///< When price traded through (0 if still active)
    bool            is_active;         ///< True until price resolves the zone
};

} // namespace signals
} // namespace ofe
