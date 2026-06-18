/**
 * bar_types.cpp
 * BarRecord, PriceLevelRecord, and BarSize helper implementations.
 */

#include "core/bar_types.h"
#include <cmath>
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace ofe {
namespace core {

// ── BarSize helpers ───────────────────────────────────────────────────────────

std::string BarSize::label() const
{
    std::ostringstream ss;
    switch (type) {
        case BarType::TIME: {
            if (size_value % 3600 == 0)
                ss << (size_value / 3600) << "h";
            else if (size_value % 60 == 0)
                ss << (size_value / 60) << "m";
            else
                ss << size_value << "s";
            break;
        }
        case BarType::RANGE:
            ss << size_value << "-range";
            break;
        case BarType::VOLUME:
            if (size_value >= 1000)
                ss << (size_value / 1000) << "K-vol";
            else
                ss << size_value << "-vol";
            break;
    }
    return ss.str();
}

bool BarSize::is_valid() const noexcept
{
    if (size_value == 0) return false;
    switch (type) {
        case BarType::TIME:
            // Must be >= 5 seconds, <= monthly (~31 days)
            return size_value >= 5 && size_value <= 31 * 24 * 3600;
        case BarType::RANGE:
            return size_value >= 1 && size_value <= 500;
        case BarType::VOLUME:
            return size_value >= 50 && size_value <= 1'000'000;
    }
    return false;
}

BarSize BarSize::time_seconds(uint32_t seconds) noexcept
{
    return { BarType::TIME, seconds };
}

BarSize BarSize::time_minutes(uint32_t minutes) noexcept
{
    return { BarType::TIME, minutes * 60 };
}

BarSize BarSize::range_ticks(uint32_t ticks) noexcept
{
    return { BarType::RANGE, ticks };
}

BarSize BarSize::volume_contracts(uint32_t contracts) noexcept
{
    return { BarType::VOLUME, contracts };
}

// ── PriceLevelRecord helpers ─────────────────────────────────────────────────

double PriceLevelRecord::buy_imbalance_ratio(double bid_vol_diagonal) const noexcept
{
    if (bid_vol_diagonal <= 0.0) return 0.0;
    return static_cast<double>(ask_vol) / bid_vol_diagonal;
}

double PriceLevelRecord::sell_imbalance_ratio(double ask_vol_diagonal) const noexcept
{
    if (ask_vol_diagonal <= 0.0) return 0.0;
    return static_cast<double>(bid_vol) / ask_vol_diagonal;
}

// ── BarRecord helpers ─────────────────────────────────────────────────────────

const PriceLevelRecord* BarRecord::get_level(double price) const noexcept
{
    // price_levels is sorted ascending by price
    // Binary search for the target price (within floating-point tolerance)
    constexpr double EPSILON = 1e-7;

    auto it = std::lower_bound(
        price_levels.begin(), price_levels.end(), price,
        [](const PriceLevelRecord& lvl, double p) {
            return lvl.price < p - 1e-7;
        }
    );

    if (it != price_levels.end() &&
        std::fabs(it->price - price) < EPSILON) {
        return &(*it);
    }
    return nullptr;
}

double BarRecord::range_ticks(double tick_size) const noexcept
{
    if (tick_size <= 0.0) return 0.0;
    return (high - low) / tick_size;
}

bool BarRecord::is_high_volume(double avg_bar_volume) const noexcept
{
    if (avg_bar_volume <= 0.0) return false;
    return static_cast<double>(total_volume) >= avg_bar_volume;
}

} // namespace core
} // namespace ofe
