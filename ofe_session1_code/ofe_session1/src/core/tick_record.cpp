/**
 * tick_record.cpp
 * Implementation of UniversalTickRecord factory methods and helpers.
 * All hot-path methods are inline in the header. This file contains
 * only non-inline helpers and the trade condition filter.
 */

#include "core/tick_record.h"
#include "util/logger.h"
#include <chrono>
#include <cstring>

namespace ofe {
namespace core {

// ── UniversalTickRecord factory ──────────────────────────────────────────────

UniversalTickRecord UniversalTickRecord::make_trade(
    uint32_t     symbol_id,
    double       price,
    int64_t      volume,
    TickSide     side,
    int64_t      exchange_ts_ns,
    uint64_t     seq_no,
    DataProvider provider
) noexcept
{
    UniversalTickRecord t{};  // zero-init (memset to 0)
    t.symbol_id        = symbol_id;
    t.price            = price;
    t.volume           = volume;
    t.side             = side;
    t.exchange_ts_ns   = exchange_ts_ns;
    t.exchange_seq_no  = seq_no;
    t.tick_type        = TickType::TRADE;
    t.provider         = provider;
    t.day_code         = 'D';  // default regular session

    // Capture engine receipt time
    using namespace std::chrono;
    t.receive_ts_ns = duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()
    ).count();

    return t;
}

// ── Trade condition filter ────────────────────────────────────────────────────

bool should_exclude_tick(uint32_t conditions, bool rth_only) noexcept
{
    if (conditions & TC_CANCELLED) {
        LOG_TRACE("tick_record", "Excluding CANCELLED tick (conditions={:#x})", conditions);
        return true;
    }
    if (conditions & TC_SETTLEMENT) {
        LOG_TRACE("tick_record", "Excluding SETTLEMENT tick");
        return true;
    }
    if (conditions & TC_IMPLIED) {
        LOG_TRACE("tick_record", "Excluding IMPLIED tick");
        return true;
    }
    if (conditions & TC_ODD_LOT) {
        LOG_TRACE("tick_record", "Excluding ODD_LOT tick");
        return true;
    }
    if (rth_only) {
        if (conditions & TC_EXTENDED_HOURS) {
            LOG_TRACE("tick_record", "Excluding EXTENDED_HOURS tick (RTH_ONLY mode)");
            return true;
        }
        if (conditions & TC_FORM_T) {
            LOG_TRACE("tick_record", "Excluding FORM_T tick (RTH_ONLY mode)");
            return true;
        }
    }
    return false;
}

} // namespace core
} // namespace ofe
