/**
 * bar_engine.cpp
 * BarSeries: builds one stream of bars (TIME, RANGE, or VOLUME) from ticks.
 * BarEngine: manages up to 4 simultaneous BarSeries per symbol.
 *
 * PERFORMANCE RULES:
 *   - on_tick() must complete in < 5µs on the hot path
 *   - No heap allocation on on_tick() except on bar open (amortised)
 *   - price_levels vector is pre-reserved to avoid rehashing
 */

#include "core/bar_engine.h"
#include "util/logger.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <stdexcept>
#include <sstream>

namespace ofe {
namespace core {

// ═══════════════════════════════════════════════════════════════════════════
// BarSeries
// ═══════════════════════════════════════════════════════════════════════════

BarSeries::BarSeries(
    BarSize             bar_size,
    const std::string&  symbol,
    uint32_t            symbol_id,
    double              tick_size,
    BarCloseCallback    on_bar_close
)
    : bar_size_(bar_size)
    , symbol_(symbol)
    , symbol_id_(symbol_id)
    , tick_size_(tick_size)
    , on_bar_close_(std::move(on_bar_close))
{
    if (!bar_size.is_valid()) {
        LOG_ERROR("bar_engine",
            "Invalid bar size for symbol={}: type={} size={}",
            symbol,
            static_cast<int>(bar_size.type),
            bar_size.size_value);
        throw std::invalid_argument("BarSeries: invalid bar size");
    }
    if (tick_size <= 0.0) {
        LOG_ERROR("bar_engine",
            "Invalid tick_size={} for symbol={}", tick_size, symbol);
        throw std::invalid_argument("BarSeries: tick_size must be positive");
    }
    LOG_DEBUG("bar_engine",
        "BarSeries created: symbol={} bar_size={} tick_size={}",
        symbol, bar_size.label(), tick_size);
}

// ── Tick processing ──────────────────────────────────────────────────────────

void BarSeries::on_tick(
    const UniversalTickRecord& tick,
    int64_t                    session_open_ts_ns
)
{
    // Only process trade ticks that can be accumulated
    if (!tick.is_accumulatable()) return;

    // Open first bar if none exists
    if (!current_bar_) {
        open_new_bar(tick.exchange_ts_ns, tick.price);
    }

    // Check if this tick triggers bar close BEFORE accumulating
    bool close_bar = should_close(tick);

    // Accumulate volume into the current bar's price level
    PriceLevelRecord& level = get_or_create_level(tick.price);

    if (tick.side == TickSide::ASK) {
        level.ask_vol   += static_cast<int32_t>(tick.volume);
    } else {
        level.bid_vol   += static_cast<int32_t>(tick.volume);
    }
    level.total_vol  = level.ask_vol + level.bid_vol;
    level.delta      = level.ask_vol - level.bid_vol;

    // Update bar OHLCV
    current_bar_->total_volume += tick.volume;
    if (tick.price > current_bar_->high) current_bar_->high = tick.price;
    if (tick.price < current_bar_->low)  current_bar_->low  = tick.price;
    current_bar_->close = tick.price;

    // Close bar if triggered
    if (close_bar) {
        close_current_bar(tick.exchange_ts_ns);
    }
    (void)session_open_ts_ns;  // used only for TIME bar alignment (see should_close)
}

void BarSeries::force_close_bar(int64_t close_ts_ns)
{
    if (current_bar_) {
        close_current_bar(close_ts_ns);
    }
}

void BarSeries::on_session_open(int64_t session_open_ts_ns)
{
    // Force-close any open bar at session end
    if (current_bar_) {
        close_current_bar(session_open_ts_ns);
    }
    // Reset last bar reference (session is fresh)
    last_bar_.reset();
    (void)session_open_ts_ns;
}

// ── Bar open / close ─────────────────────────────────────────────────────────

void BarSeries::open_new_bar(int64_t open_ts_ns, double open_price)
{
    current_bar_ = std::make_unique<BarRecord>();
    current_bar_->symbol_id   = symbol_id_;
    current_bar_->symbol      = symbol_;
    current_bar_->bar_size    = bar_size_;
    current_bar_->open        = open_price;
    current_bar_->high        = open_price;
    current_bar_->low         = open_price;
    current_bar_->close       = open_price;
    current_bar_->bar_open_ts_ns = open_ts_ns;
    current_bar_->is_complete = false;
    current_bar_->total_volume = 0;

    std::ostringstream ss;
    ss << symbol_ << "_" << open_ts_ns << "_" << bar_size_.label();
    current_bar_->bar_id = ss.str();

    current_bar_->price_levels.reserve(64);

    LOG_TRACE("bar_engine",
        "Bar opened: symbol={} bar_id={} open={} bar_size={}",
        symbol_, current_bar_->bar_id, open_price, bar_size_.label());
}

void BarSeries::close_current_bar(int64_t close_ts_ns)
{
    if (!current_bar_) return;

    current_bar_->bar_close_ts_ns = close_ts_ns;
    current_bar_->is_complete     = true;
    current_bar_->is_bullish      = current_bar_->close > current_bar_->open;

    std::sort(
        current_bar_->price_levels.begin(),
        current_bar_->price_levels.end(),
        [](const PriceLevelRecord& a, const PriceLevelRecord& b) {
            return a.price < b.price;
        }
    );

    if (!current_bar_->price_levels.empty()) {
        auto poc_it = std::max_element(
            current_bar_->price_levels.begin(),
            current_bar_->price_levels.end(),
            [](const PriceLevelRecord& a, const PriceLevelRecord& b) {
                return a.total_vol < b.total_vol;
            }
        );
        current_bar_->poc_price  = poc_it->price;
        current_bar_->poc_volume = poc_it->total_vol;
        poc_it->is_poc = true;
        current_bar_->cot_price = current_bar_->poc_price;
    }

    LOG_DEBUG("bar_engine",
        "Bar closed: symbol={} bar_id={} O={} H={} L={} C={} vol={} "
        "levels={} poc={} bullish={}",
        symbol_,
        current_bar_->bar_id,
        current_bar_->open,
        current_bar_->high,
        current_bar_->low,
        current_bar_->close,
        current_bar_->total_volume,
        current_bar_->price_levels.size(),
        current_bar_->poc_price,
        current_bar_->is_bullish ? "yes" : "no");

    if (on_bar_close_) {
        on_bar_close_(*current_bar_);
    }

    last_bar_    = std::move(current_bar_);
    current_bar_.reset();
}

// ── Bar close trigger ─────────────────────────────────────────────────────────

bool BarSeries::should_close(const UniversalTickRecord& tick) const noexcept
{
    if (!current_bar_) return false;

    switch (bar_size_.type) {

        case BarType::TIME: {
            // Close when tick's exchange_ts crosses the next time boundary.
            // Boundary = floor(bar_open / bar_size_ns) * bar_size_ns + bar_size_ns
            const int64_t bar_size_ns =
                static_cast<int64_t>(bar_size_.size_value) * 1'000'000'000LL;
            const int64_t bar_start_boundary =
                (current_bar_->bar_open_ts_ns / bar_size_ns) * bar_size_ns;
            const int64_t bar_end_boundary =
                bar_start_boundary + bar_size_ns;
            return tick.exchange_ts_ns >= bar_end_boundary;
        }

        case BarType::RANGE: {
            // Close when price moves more than size_value ticks from bar open.
            const double range =
                std::fabs(tick.price - current_bar_->open);
            const double threshold =
                static_cast<double>(bar_size_.size_value) * tick_size_;
            return range >= threshold;
        }

        case BarType::VOLUME: {
            // Close when accumulated volume reaches the threshold.
            return current_bar_->total_volume >=
                   static_cast<int64_t>(bar_size_.size_value);
        }
    }
    return false;
}

// ── Price level accessor ──────────────────────────────────────────────────────

PriceLevelRecord& BarSeries::get_or_create_level(double price)
{
    // Round price to nearest tick to handle floating-point imprecision
    const double rounded = std::round(price / tick_size_) * tick_size_;

    // Linear scan — bars typically have < 50 levels, linear is faster than hash
    for (auto& lvl : current_bar_->price_levels) {
        if (std::fabs(lvl.price - rounded) < tick_size_ * 0.01) {
            return lvl;
        }
    }

    // Not found — create new level
    PriceLevelRecord lvl{};
    lvl.price = rounded;
    current_bar_->price_levels.push_back(lvl);
    return current_bar_->price_levels.back();
}

// ── Accessors ─────────────────────────────────────────────────────────────────

const BarRecord* BarSeries::current_bar() const noexcept
{
    return current_bar_.get();
}

const BarRecord* BarSeries::last_bar() const noexcept
{
    return last_bar_.get();
}

const BarSize& BarSeries::bar_size() const noexcept
{
    return bar_size_;
}

// ═══════════════════════════════════════════════════════════════════════════
// BarEngine
// ═══════════════════════════════════════════════════════════════════════════

BarEngine::BarEngine(
    const std::string& symbol,
    uint32_t           symbol_id,
    double             tick_size,
    BarCloseCallback   bar_close_cb
)
    : symbol_(symbol)
    , symbol_id_(symbol_id)
    , tick_size_(tick_size)
    , bar_close_cb_(std::move(bar_close_cb))
{
    series_.reserve(4);  // max 4 simultaneous series per spec
}

bool BarEngine::add_series(BarSize bar_size)
{
    if (series_.size() >= 4) return false;

    // Prevent duplicate bar sizes
    for (const auto& s : series_) {
        if (s->bar_size().type        == bar_size.type &&
            s->bar_size().size_value  == bar_size.size_value) {
            return false;  // already exists
        }
    }

    series_.push_back(std::make_unique<BarSeries>(
        bar_size, symbol_, symbol_id_, tick_size_, bar_close_cb_
    ));
    return true;
}

void BarEngine::remove_series(BarSize bar_size)
{
    series_.erase(
        std::remove_if(series_.begin(), series_.end(),
            [&](const std::unique_ptr<BarSeries>& s) {
                return s->bar_size().type       == bar_size.type &&
                       s->bar_size().size_value == bar_size.size_value;
            }),
        series_.end()
    );
}

void BarEngine::on_tick(
    const UniversalTickRecord& tick,
    int64_t                    session_open_ts_ns
)
{
    for (auto& s : series_) {
        s->on_tick(tick, session_open_ts_ns);
    }
}

void BarEngine::on_session_open(int64_t session_open_ts_ns)
{
    for (auto& s : series_) {
        s->on_session_open(session_open_ts_ns);
    }
}

void BarEngine::on_session_close(int64_t session_close_ts_ns)
{
    for (auto& s : series_) {
        s->force_close_bar(session_close_ts_ns);
    }
}

size_t BarEngine::series_count() const noexcept
{
    return series_.size();
}

const BarRecord* BarEngine::current_bar(BarSize bar_size) const noexcept
{
    for (const auto& s : series_) {
        if (s->bar_size().type       == bar_size.type &&
            s->bar_size().size_value == bar_size.size_value) {
            return s->current_bar();
        }
    }
    return nullptr;
}

const BarRecord* BarEngine::last_bar(BarSize bar_size) const noexcept
{
    for (const auto& s : series_) {
        if (s->bar_size().type       == bar_size.type &&
            s->bar_size().size_value == bar_size.size_value) {
            return s->last_bar();
        }
    }
    return nullptr;
}

} // namespace core
} // namespace ofe
