/**
 * imbalance_detector.cpp
 * Detects all imbalance signal types per Indicator Formula spec §11.
 *
 * Formulas implemented:
 *   3.1  Single buying/selling imbalance (diagonal comparison)
 *   3.2  Stacked imbalance (3+ consecutive same-direction)
 *   3.3  Imbalance ratio + zone strength score
 *   3.4  Absorption (high vol + near-zero net delta)
 *   Plus: Trapped Traders, 2nd Slot
 */

#include "analytics/imbalance_detector.h"
#include "util/logger.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <sstream>
#include <random>

namespace ofe {
namespace analytics {

// ── UUID helper (simple — production should use a proper UUID lib) ────────────
static std::string make_zone_id()
{
    // Simple 16-hex-char ID — unique enough for zone tracking
    static std::mt19937_64 rng(std::chrono::steady_clock::now().time_since_epoch().count());
    static std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream ss;
    ss << std::hex << dist(rng) << dist(rng);
    return ss.str().substr(0, 16);
}

static signals::SignalEvent make_base_signal(
    const core::BarRecord& bar,
    signals::SignalType type,
    signals::SignalDirection direction,
    signals::SignalStrength strength
)
{
    signals::SignalEvent sig{};
    sig.type      = type;
    sig.direction = direction;
    sig.strength  = strength;
    sig.symbol_id = bar.symbol_id;
    sig.symbol    = bar.symbol;
    sig.bar_id    = bar.bar_id;
    sig.bar_open_ts_ns  = bar.bar_open_ts_ns;
    sig.bar_close_ts_ns = bar.bar_close_ts_ns;
    sig.bar_delta       = bar.bar_delta;
    sig.bar_volume      = bar.total_volume;
    sig.price_at_signal = bar.close;

    using namespace std::chrono;
    sig.detection_ts_ns = duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()
    ).count();
    return sig;
}

// ═══════════════════════════════════════════════════════════════════════════
// ImbalanceDetector
// ═══════════════════════════════════════════════════════════════════════════

ImbalanceDetector::ImbalanceDetector(const ImbalanceConfig& config)
    : config_(config)
{}

void ImbalanceDetector::set_config(const ImbalanceConfig& config)
{
    config_ = config;
}

// ── Primary entry point ──────────────────────────────────────────────────────

std::vector<signals::SignalEvent> ImbalanceDetector::detect_all(
    core::BarRecord&                     bar,
    std::vector<signals::ImbalanceZone>& active_zones,
    double                               tick_size
)
{
    std::vector<signals::SignalEvent> result;

    // Step 1: Detect and flag all single imbalances on price levels
    auto [buy_count, sell_count] =
        detect_single_imbalances(bar.price_levels, tick_size);

    LOG_DEBUG("imbalance",
        "Imbalance scan: symbol={} bar_id={} levels={} buy_imb={} sell_imb={}",
        bar.symbol, bar.bar_id,
        bar.price_levels.size(),
        buy_count, sell_count);

    // Emit single imbalance signals if any detected
    if (buy_count > 0) {
        auto sig = make_base_signal(bar,
            signals::SignalType::SINGLE_IMBALANCE_BUY,
            signals::SignalDirection::LONG,
            signals::SignalStrength::MODERATE);
        bar.has_buy_imbalance = true;
        std::ostringstream meta;
        meta << R"({"count":)" << buy_count << "}";
        sig.metadata_json = meta.str();
        result.push_back(sig);
    }
    if (sell_count > 0) {
        auto sig = make_base_signal(bar,
            signals::SignalType::SINGLE_IMBALANCE_SELL,
            signals::SignalDirection::SHORT,
            signals::SignalStrength::MODERATE);
        bar.has_sell_imbalance = true;
        std::ostringstream meta;
        meta << R"({"count":)" << sell_count << "}";
        sig.metadata_json = meta.str();
        result.push_back(sig);
    }

    // Step 2: Detect stacked imbalances and update zone registry
    auto stacked = detect_stacked(bar.price_levels, active_zones,
                                  bar.bar_id, bar.bar_close_ts_ns);
    for (auto& s : stacked) result.push_back(std::move(s));

    // Step 3: Resolve zones that price has traded through
    update_zone_registry(active_zones, bar, tick_size);

    // Step 4: Absorption
    auto absorptions = detect_absorption(bar);
    for (auto& a : absorptions) {
        bar.has_absorption = true;
        result.push_back(std::move(a));
    }

    // Step 5: Trapped traders
    auto trapped = detect_trapped_traders(bar, tick_size);
    for (auto& t : trapped) result.push_back(std::move(t));

    return result;
}

// ── Single imbalance detection ────────────────────────────────────────────────

std::pair<int,int> ImbalanceDetector::detect_single_imbalances(
    std::vector<core::PriceLevelRecord>& levels,
    double tick_size
) const noexcept
{
    int buy_count  = 0;
    int sell_count = 0;

    const float ratio = config_.imbalance_ratio;

    for (std::size_t i = 0; i < levels.size(); ++i) {
        auto& lvl = levels[i];
        lvl.is_buy_imbalance  = false;
        lvl.is_sell_imbalance = false;

        // ── Buying imbalance: ask_vol[P] >= ratio × bid_vol[P - 1tick] ────
        // Find diagonal: the level one tick BELOW current level
        const double below_price = lvl.price - tick_size;
        const auto*  below_lvl   = find_level(levels, below_price, tick_size);

        if (below_lvl && below_lvl->bid_vol > 0 && lvl.ask_vol > 0) {
            if (static_cast<float>(lvl.ask_vol) >=
                ratio * static_cast<float>(below_lvl->bid_vol)) {
                lvl.is_buy_imbalance = true;
                buy_count++;
            }
        }

        // ── Selling imbalance: bid_vol[P] >= ratio × ask_vol[P + 1tick] ──
        const double above_price = lvl.price + tick_size;
        const auto*  above_lvl   = find_level(levels, above_price, tick_size);

        if (above_lvl && above_lvl->ask_vol > 0 && lvl.bid_vol > 0) {
            if (static_cast<float>(lvl.bid_vol) >=
                ratio * static_cast<float>(above_lvl->ask_vol)) {
                lvl.is_sell_imbalance = true;
                sell_count++;
            }
        }
    }

    return { buy_count, sell_count };
}

// ── Stacked imbalance detection ───────────────────────────────────────────────

std::vector<signals::SignalEvent> ImbalanceDetector::detect_stacked(
    const std::vector<core::PriceLevelRecord>& levels,
    std::vector<signals::ImbalanceZone>&        active_zones,
    const std::string&                          bar_id,
    int64_t                                     bar_close_ts
) const
{
    std::vector<signals::SignalEvent> result;
    const int min_count = config_.stacked_min_count;

    // Scan for consecutive buying imbalances
    {
        int streak = 0;
        int streak_start_idx = 0;
        for (int i = 0; i <= static_cast<int>(levels.size()); ++i) {
            bool is_imb = (i < static_cast<int>(levels.size())) &&
                           levels[i].is_buy_imbalance;
            if (is_imb) {
                if (streak == 0) streak_start_idx = i;
                streak++;
            } else {
                if (streak >= min_count) {
                    // Build or extend a zone
                    const double zone_low  = levels[streak_start_idx].price;
                    const double zone_high = levels[i - 1].price;

                    // Check if this extends an existing zone
                    bool extended = false;
                    for (auto& z : active_zones) {
                        if (z.direction == signals::SignalDirection::LONG &&
                            z.is_active &&
                            std::fabs(z.price_low - zone_low) < 1e-6) {
                            z.price_high           = zone_high;
                            z.imbalance_count      = streak;
                            z.last_updated_ts_ns   = bar_close_ts;
                            extended = true;
                            break;
                        }
                    }

                    if (!extended) {
                        signals::ImbalanceZone zone{};
                        zone.zone_id           = make_zone_id();
                        zone.symbol_id         = 0;  // set by caller
                        zone.direction         = signals::SignalDirection::LONG;
                        zone.price_low         = zone_low;
                        zone.price_high        = zone_high;
                        zone.imbalance_count   = streak;
                        zone.avg_ratio         = config_.imbalance_ratio;
                        zone.strength_score    = compute_zone_strength(zone);
                        zone.created_ts_ns     = bar_close_ts;
                        zone.last_updated_ts_ns = bar_close_ts;
                        zone.resolved_ts_ns    = 0;
                        zone.is_active         = true;
                        active_zones.push_back(zone);

                        LOG_INFO("imbalance",
                            "STACKED BUY ZONE: bar_id={} price={}-{} count={} strength={:.1f}",
                            bar_id, zone_low, zone_high, streak, zone.strength_score);

                        // Emit signal
                        signals::SignalEvent sig{};
                        sig.type       = signals::SignalType::STACKED_IMBALANCE_BUY;
                        sig.direction  = signals::SignalDirection::LONG;
                        sig.strength   = streak >= 5
                                       ? signals::SignalStrength::VERY_STRONG
                                       : signals::SignalStrength::STRONG;
                        sig.bar_id     = bar_id;
                        sig.bar_close_ts_ns = bar_close_ts;
                        sig.price_at_signal  = zone_high;
                        sig.suggested_entry  = zone_high;
                        sig.suggested_stop   = zone_low - 0.01;

                        using namespace std::chrono;
                        sig.detection_ts_ns = duration_cast<nanoseconds>(
                            system_clock::now().time_since_epoch()
                        ).count();

                        std::ostringstream meta;
                        meta << R"({"zone_id":")"    << zone.zone_id << "\""
                             << R"(,"count":)"        << streak
                             << R"(,"price_low":)"    << zone_low
                             << R"(,"price_high":)"   << zone_high
                             << R"(,"strength":)"     << zone.strength_score << "}";
                        sig.metadata_json = meta.str();
                        result.push_back(std::move(sig));
                    }
                }
                streak = 0;
            }
        }
    }

    // Scan for consecutive selling imbalances
    {
        int streak = 0;
        int streak_start_idx = 0;
        for (int i = 0; i <= static_cast<int>(levels.size()); ++i) {
            bool is_imb = (i < static_cast<int>(levels.size())) &&
                           levels[i].is_sell_imbalance;
            if (is_imb) {
                if (streak == 0) streak_start_idx = i;
                streak++;
            } else {
                if (streak >= min_count) {
                    const double zone_low  = levels[streak_start_idx].price;
                    const double zone_high = levels[i - 1].price;

                    bool extended = false;
                    for (auto& z : active_zones) {
                        if (z.direction == signals::SignalDirection::SHORT &&
                            z.is_active &&
                            std::fabs(z.price_high - zone_high) < 1e-6) {
                            z.price_low            = zone_low;
                            z.imbalance_count      = streak;
                            z.last_updated_ts_ns   = bar_close_ts;
                            extended = true;
                            break;
                        }
                    }

                    if (!extended) {
                        signals::ImbalanceZone zone{};
                        zone.zone_id           = make_zone_id();
                        zone.direction         = signals::SignalDirection::SHORT;
                        zone.price_low         = zone_low;
                        zone.price_high        = zone_high;
                        zone.imbalance_count   = streak;
                        zone.avg_ratio         = config_.imbalance_ratio;
                        zone.strength_score    = compute_zone_strength(zone);
                        zone.created_ts_ns     = bar_close_ts;
                        zone.last_updated_ts_ns = bar_close_ts;
                        zone.resolved_ts_ns    = 0;
                        zone.is_active         = true;
                        active_zones.push_back(zone);

                        LOG_INFO("imbalance",
                            "STACKED SELL ZONE: bar_id={} price={}-{} count={} strength={:.1f}",
                            bar_id, zone_low, zone_high, streak, zone.strength_score);

                        signals::SignalEvent sig{};
                        sig.type       = signals::SignalType::STACKED_IMBALANCE_SELL;
                        sig.direction  = signals::SignalDirection::SHORT;
                        sig.strength   = streak >= 5
                                       ? signals::SignalStrength::VERY_STRONG
                                       : signals::SignalStrength::STRONG;
                        sig.bar_id     = bar_id;
                        sig.bar_close_ts_ns = bar_close_ts;
                        sig.price_at_signal  = zone_low;
                        sig.suggested_entry  = zone_low;
                        sig.suggested_stop   = zone_high + 0.01;

                        using namespace std::chrono;
                        sig.detection_ts_ns = duration_cast<nanoseconds>(
                            system_clock::now().time_since_epoch()
                        ).count();

                        std::ostringstream meta;
                        meta << R"({"zone_id":")"    << zone.zone_id << "\""
                             << R"(,"count":)"        << streak
                             << R"(,"price_low":)"    << zone_low
                             << R"(,"price_high":)"   << zone_high
                             << R"(,"strength":)"     << zone.strength_score << "}";
                        sig.metadata_json = meta.str();
                        result.push_back(std::move(sig));
                    }
                }
                streak = 0;
            }
        }
    }

    return result;
}

// ── Absorption detection ──────────────────────────────────────────────────────

std::vector<signals::SignalEvent> ImbalanceDetector::detect_absorption(
    const core::BarRecord& bar
) const
{
    std::vector<signals::SignalEvent> result;

    for (const auto& lvl : bar.price_levels) {
        if (lvl.total_vol < static_cast<int32_t>(config_.absorption_vol_threshold))
            continue;

        const float delta_ratio =
            static_cast<float>(std::abs(lvl.delta)) /
            static_cast<float>(lvl.total_vol);

        if (delta_ratio > config_.absorption_delta_ratio)
            continue;

        auto sig = make_base_signal(bar,
            signals::SignalType::ABSORPTION,
            signals::SignalDirection::NEUTRAL,
            signals::SignalStrength::STRONG);

        sig.price_at_signal = lvl.price;
        std::ostringstream meta;
        meta << R"({"price":)"      << lvl.price
             << R"(,"bid_vol":)"    << lvl.bid_vol
             << R"(,"ask_vol":)"    << lvl.ask_vol
             << R"(,"total_vol":)"  << lvl.total_vol
             << R"(,"delta":)"      << lvl.delta
             << R"(,"delta_ratio":)" << (delta_ratio * 100.0f) << "}";
        sig.metadata_json = meta.str();
        result.push_back(std::move(sig));
    }

    return result;
}

// ── 2nd Slot imbalance ────────────────────────────────────────────────────────

std::vector<signals::SignalEvent> ImbalanceDetector::detect_2nd_slot(
    const core::BarRecord&                     bar,
    const std::vector<signals::ImbalanceZone>& active_zones
) const
{
    std::vector<signals::SignalEvent> result;
    // TODO: Implement 2nd slot detection (Phase 2 enhancement)
    (void)bar;
    (void)active_zones;
    return result;
}

// ── Trapped traders ───────────────────────────────────────────────────────────

std::vector<signals::SignalEvent> ImbalanceDetector::detect_trapped_traders(
    const core::BarRecord& bar,
    double tick_size
) const
{
    std::vector<signals::SignalEvent> result;

    const double bar_range    = bar.high - bar.low;
    const double half_range   = bar_range * 0.5;
    const double tolerance    = tick_size * 2.0;

    // Trapped sellers: sell imbalance near bar low, bar closed in upper half
    if (bar.close > bar.open + half_range * 0.5) {  // bar closed strongly up
        for (const auto& lvl : bar.price_levels) {
            if (!lvl.is_sell_imbalance) continue;
            if (std::fabs(lvl.price - bar.low) <= tolerance) {
                auto sig = make_base_signal(bar,
                    signals::SignalType::TRAPPED_SELLERS,
                    signals::SignalDirection::LONG,
                    signals::SignalStrength::STRONG);
                sig.price_at_signal = lvl.price;
                sig.suggested_entry = bar.close;
                sig.suggested_stop  = bar.low - tick_size;
                std::ostringstream meta;
                meta << R"({"trap_price":)" << lvl.price
                     << R"(,"bar_low":)"     << bar.low
                     << R"(,"bid_vol":)"     << lvl.bid_vol
                     << R"(,"ask_vol":)"     << lvl.ask_vol << "}";
                sig.metadata_json = meta.str();
                result.push_back(std::move(sig));
                break;  // one trap signal per bar
            }
        }
    }

    // Trapped buyers: buy imbalance near bar high, bar closed in lower half
    if (bar.close < bar.open - half_range * 0.5) {  // bar closed strongly down
        for (const auto& lvl : bar.price_levels) {
            if (!lvl.is_buy_imbalance) continue;
            if (std::fabs(lvl.price - bar.high) <= tolerance) {
                auto sig = make_base_signal(bar,
                    signals::SignalType::TRAPPED_BUYERS,
                    signals::SignalDirection::SHORT,
                    signals::SignalStrength::STRONG);
                sig.price_at_signal = lvl.price;
                sig.suggested_entry = bar.close;
                sig.suggested_stop  = bar.high + tick_size;
                std::ostringstream meta;
                meta << R"({"trap_price":)" << lvl.price
                     << R"(,"bar_high":)"    << bar.high
                     << R"(,"bid_vol":)"     << lvl.bid_vol
                     << R"(,"ask_vol":)"     << lvl.ask_vol << "}";
                sig.metadata_json = meta.str();
                result.push_back(std::move(sig));
                break;
            }
        }
    }

    return result;
}

// ── Zone registry maintenance ─────────────────────────────────────────────────

void ImbalanceDetector::update_zone_registry(
    std::vector<signals::ImbalanceZone>& active_zones,
    const core::BarRecord&               bar,
    double                               tick_size
) const noexcept
{
    const double tolerance = tick_size * 0.5;

    for (auto& zone : active_zones) {
        if (!zone.is_active) continue;

        // Zone is "resolved" if price traded THROUGH the zone
        // (bar's high went above zone top for sell zones, or
        //  bar's low went below zone bottom for buy zones)
        bool resolved = false;
        if (zone.direction == signals::SignalDirection::LONG) {
            // Buy zone: resolved if price fell below the zone low
            resolved = bar.low < zone.price_low - tolerance;
        } else {
            // Sell zone: resolved if price rose above zone high
            resolved = bar.high > zone.price_high + tolerance;
        }

        if (resolved) {
            zone.is_active        = false;
            zone.resolved_ts_ns   = bar.bar_close_ts_ns;
        }
    }
}

// ── Zone strength ─────────────────────────────────────────────────────────────

float ImbalanceDetector::compute_zone_strength(
    const signals::ImbalanceZone& zone
) const noexcept
{
    // Strength = count × avg_ratio × weight, clamped to [1, 10]
    const float raw = static_cast<float>(zone.imbalance_count) *
                      zone.avg_ratio *
                      config_.zone_strength_weight;
    return std::min(std::max(raw / 3.0f, 1.0f), 10.0f);
}

// ── Private: level finder ─────────────────────────────────────────────────────

const core::PriceLevelRecord* ImbalanceDetector::find_level(
    const std::vector<core::PriceLevelRecord>& levels,
    double price,
    double tick_size
) noexcept
{
    const double tolerance = tick_size * 0.01;
    for (const auto& lvl : levels) {
        if (std::fabs(lvl.price - price) < tolerance) {
            return &lvl;
        }
    }
    return nullptr;
}

} // namespace analytics
} // namespace ofe
