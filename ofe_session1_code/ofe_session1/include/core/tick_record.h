#pragma once
/**
 * tick_record.h
 * Universal Tick Record — the single normalised internal tick format
 * that all three data feed adapters (IQFeed, Kinetick, eSignal) write to.
 * Symbol Workers ONLY operate on UniversalTickRecord — never on provider-
 * specific formats.  Changing provider = changing one adapter, zero engine changes.
 *
 * Spec reference: Data Feed Spec §5 (Universal Tick Record)
 */

#include <cstdint>
#include <string>
#include <array>

namespace ofe {
namespace core {

// ── Enumerations ─────────────────────────────────────────────────────────────

/**
 * Aggressor side classification result from Lee-Ready or exchange-direct.
 * Spec ref: Indicator Formulas §9 (Lee-Ready Algorithm)
 */
enum class TickSide : uint8_t {
    ASK     = 0,  ///< Aggressive buyer — ask_vol incremented
    BID     = 1,  ///< Aggressive seller — bid_vol incremented
    UNKNOWN = 2   ///< Could not classify — IQFeed aggressor=0 + midpoint trade
};

/**
 * Type of market event carried by this tick.
 * Determines how Symbol Worker processes the record.
 */
enum class TickType : uint8_t {
    TRADE     = 0,  ///< Executed trade — carries price, volume, side
    BID_QUOTE = 1,  ///< Best bid quote update — updates prevailing bid for Lee-Ready
    ASK_QUOTE = 2,  ///< Best ask quote update — updates prevailing ask for Lee-Ready
    SUMMARY   = 3   ///< Session summary (DailyHigh, DailyVolume, etc.) — not accumulated
};

/**
 * Which data provider delivered this tick.
 * Used for logging, audit, and failover tracking.
 */
enum class DataProvider : uint8_t {
    KINETICK = 0,
    IQFEED   = 1,
    ESIGNAL  = 2,
    REPLAY   = 3,  ///< Historical replay from file — used in backtesting
    SYNTHETIC = 4  ///< Synthetic tick generator — used in stress tests
};

// ── Universal Tick Record ────────────────────────────────────────────────────

/**
 * UniversalTickRecord
 * Single compact struct representing one market event from any provider.
 * Size: 64 bytes — fits in one cache line.
 * All adapters produce this; all Symbol Workers consume this.
 *
 * Field ordering is by alignment (largest first) to minimise padding.
 */
struct alignas(64) UniversalTickRecord {
    // ── Timing (24 bytes) ──────────────────────────────────────────────────
    int64_t  exchange_ts_ns;    ///< Exchange timestamp in Unix nanoseconds
    int64_t  receive_ts_ns;     ///< Engine receipt time in Unix nanoseconds (server clock)
    uint64_t exchange_seq_no;   ///< Exchange sequence number (IQFeed: TickID; eSignal: SeqID)

    // ── Price & Volume (16 bytes) ──────────────────────────────────────────
    double   price;             ///< Executed price (trade) or quote price (bid/ask update)
    int64_t  volume;            ///< Contracts/shares. Stocks: actual shares (100-lot adjusted)

    // ── Routing (4 bytes) ─────────────────────────────────────────────────
    uint32_t symbol_id;         ///< Internal symbol hash — used by Tick Router for routing

    // ── Classification (3 bytes) ──────────────────────────────────────────
    TickSide     side;          ///< BID / ASK / UNKNOWN — set by Lee-Ready or exchange direct
    TickType     tick_type;     ///< TRADE / BID_QUOTE / ASK_QUOTE / SUMMARY
    DataProvider provider;      ///< Which adapter produced this tick

    // ── Session context (1 byte) ──────────────────────────────────────────
    uint8_t  day_code;          ///< 'D'=regular session, 'X'=non-standard, 'E'=extended hours

    // ── Trade conditions (4 bytes, bitfield) ──────────────────────────────
    uint32_t trade_conditions;  ///< Exchange condition codes as bitmask (see TradeCondition enum)

    // ── Padding to 64 bytes ───────────────────────────────────────────────
    uint8_t  _pad[8];

    // ── Convenience constructors ──────────────────────────────────────────

    /// Default constructor — all zeros
    UniversalTickRecord() = default;

    /// Construct a trade tick (most common path)
    static UniversalTickRecord make_trade(
        uint32_t symbol_id,
        double   price,
        int64_t  volume,
        TickSide side,
        int64_t  exchange_ts_ns,
        uint64_t seq_no,
        DataProvider provider = DataProvider::IQFEED
    );

    /// Returns true if this tick should be accumulated into bid/ask volume buckets
    [[nodiscard]] bool is_accumulatable() const noexcept {
        return tick_type == TickType::TRADE && side != TickSide::UNKNOWN;
    }

    /// Returns true if this tick updates the prevailing quote for Lee-Ready
    [[nodiscard]] bool is_quote_update() const noexcept {
        return tick_type == TickType::BID_QUOTE || tick_type == TickType::ASK_QUOTE;
    }
};

static_assert(sizeof(UniversalTickRecord) == 64,
    "UniversalTickRecord must be exactly 64 bytes (one cache line)");

// ── Trade Condition Bitmask ───────────────────────────────────────────────────

/**
 * Exchange-reported trade condition flags.
 * Multiple conditions can be set simultaneously (bitmask).
 * Used to filter non-standard trades from OFE calculation.
 */
enum TradeCondition : uint32_t {
    TC_NORMAL          = 0x00000000,  ///< Normal trade — include in all calculations
    TC_EXTENDED_HOURS  = 0x00000001,  ///< Pre/post market — exclude if RTH_ONLY
    TC_FORM_T          = 0x00000002,  ///< Form T (extended hours regulatory)
    TC_ODD_LOT         = 0x00000004,  ///< Odd lot trade — exclude from volume analysis
    TC_CROSS           = 0x00000008,  ///< Cross trade (simultaneous buy/sell) — may exclude
    TC_CORRECTED       = 0x00000010,  ///< Corrected / amended trade
    TC_CANCELLED       = 0x00000020,  ///< Cancelled trade — must be removed retroactively
    TC_INSERTED        = 0x00000040,  ///< Inserted (late report) — insert at correct time
    TC_SETTLEMENT      = 0x00000080,  ///< Settlement price — not a live trade
    TC_IMPLIED         = 0x00000100   ///< Implied trade (spread leg) — exclude from delta
};

/**
 * Returns true if a tick with the given conditions should be excluded
 * from order flow accumulation given the current session filter.
 *
 * @param conditions  Bitmask of TradeCondition flags
 * @param rth_only    True if only Regular Trading Hours ticks should be processed
 * @return            True if tick should be EXCLUDED from accumulation
 */
[[nodiscard]] bool should_exclude_tick(uint32_t conditions, bool rth_only) noexcept;

} // namespace core
} // namespace ofe
