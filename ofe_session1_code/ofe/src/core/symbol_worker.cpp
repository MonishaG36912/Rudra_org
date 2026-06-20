/**
 * symbol_worker.cpp
 * SymbolWorker — the atomic processing unit for one symbol.
 * Owns all per-symbol state; runs the full tick processing pipeline
 * on a dedicated thread.
 *
 * Pipeline stages (per tick):
 *   1. Dequeue tick from ring buffer
 *   2. Update prevailing quote
 *   3. Lee-Ready classification
 *   4. Bar accumulation via BarEngine
 *   5. Delta update via DeltaEngine
 *   6. VWAP update via VwapEngine
 *   7. Volume Profile update via VolumeProfileEngine
 *   8. Tick-level signal check
 *   9-10. Bar close handling → on_bar_close() (triggered by BarEngine callback)
 *  11. Notify script evaluators (stub — future work)
 */

#include "core/symbol_worker.h"
#include "core/bar_engine.h"
#include "core/event_bus.h"
#include "analytics/volume_profile.h"
#include "analytics/vwap_engine.h"
#include "analytics/signal_detector.h"
#include "util/logger.h"

#include <chrono>
#include <stdexcept>

namespace ofe {
namespace core {

// ── Helper ────────────────────────────────────────────────────────────────────

static inline int64_t now_ns() noexcept
{
    using namespace std::chrono;
    return duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()).count();
}

// ── Constructor / Destructor ──────────────────────────────────────────────────

SymbolWorker::SymbolWorker(
    std::string                      symbol,
    uint32_t                         symbol_id,
    const config::EngineConfig&      engine_config,
    const config::IndicatorConfig&   indicator_config,
    std::shared_ptr<EventBus>        event_bus
)
    : symbol_(std::move(symbol))
    , symbol_id_(symbol_id)
    , event_bus_(std::move(event_bus))
    , indicator_config_(indicator_config)
{
    (void)engine_config;  // reserved for CPU affinity, logging, etc.

    ring_buffer_ = std::make_unique<TickRingBuffer>();

    // Bar engine — callback fires on_bar_close() for every closed bar
    bar_engine_ = std::make_unique<BarEngine>(
        symbol_, symbol_id_,
        indicator_config_.tick_size,
        [this](const BarRecord& bar) { on_bar_close(bar); }
    );

    // Add the default bar series from config
    const auto btype = static_cast<BarType>(
        static_cast<uint32_t>(indicator_config_.default_bar_type));
    default_bar_size_ = BarSize{btype, indicator_config_.default_bar_size};
    bar_engine_->add_series(default_bar_size_);

    delta_engine_ = std::make_unique<analytics::DeltaEngine>();

    analytics::VolumeProfileConfig vpc{};
    vpc.value_area_pct            = indicator_config_.value_area_pct;
    vpc.hvn_threshold_multiplier  = indicator_config_.hvn_multiplier;
    vpc.lvn_threshold_multiplier  = indicator_config_.lvn_multiplier;
    vpc.composite_lookback_days   = indicator_config_.composite_lookback_days;
    profile_engine_ = std::make_unique<analytics::VolumeProfileEngine>(
        indicator_config_.tick_size, vpc);

    vwap_engine_ = std::make_unique<analytics::VwapEngine>(
        indicator_config_.tick_size,
        indicator_config_.vwap_touch_ticks);

    analytics::SignalDetectorConfig sdc{};
    sdc.pulse_score_threshold    = indicator_config_.pulse_score_threshold;
    sdc.turns_min_score          = indicator_config_.turns_min_score;
    sdc.ratio_threshold          = indicator_config_.ratio_threshold;
    sdc.ratio_min_volume         = indicator_config_.ratio_min_volume;
    sdc.single_print_threshold   = indicator_config_.single_print_threshold;
    sdc.sweep_volume_threshold   = indicator_config_.sweep_volume_threshold;
    sdc.sweep_level_threshold    = indicator_config_.sweep_level_threshold;
    sdc.slingshot_distance_ticks = indicator_config_.slingshot_distance_ticks;
    signal_detector_ = std::make_unique<analytics::SignalDetector>(sdc, symbol_id_);

    LOG_INFO("symbol_worker",
        "SymbolWorker created: symbol={} symbol_id={:x} tick_size={}",
        symbol_, symbol_id_, indicator_config_.tick_size);
}

SymbolWorker::~SymbolWorker()
{
    if (worker_thread_.joinable()) {
        stop_requested_.store(true, std::memory_order_release);
        worker_thread_.join();
    }
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void SymbolWorker::start()
{
    const auto expected = WorkerStatus::IDLE;
    if (status_.load(std::memory_order_acquire) != expected) {
        LOG_WARN("symbol_worker",
            "start() called on non-idle worker: symbol={}", symbol_);
        return;
    }

    status_.store(WorkerStatus::STARTING, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);

    worker_thread_ = std::thread(&SymbolWorker::run_loop, this);

    LOG_INFO("symbol_worker", "Worker starting: symbol={}", symbol_);
}

void SymbolWorker::stop(uint32_t timeout_ms)
{
    if (!worker_thread_.joinable()) return;

    status_.store(WorkerStatus::STOPPING, std::memory_order_release);
    stop_requested_.store(true, std::memory_order_release);

    // Give the thread time to drain remaining ticks, then join unconditionally
    (void)timeout_ms;
    worker_thread_.join();

    LOG_INFO("symbol_worker",
        "Worker stopped: symbol={} ticks_processed={} bars_closed={} signals={}",
        symbol_, stats_.ticks_processed, stats_.bars_closed, stats_.signals_emitted);
}

void SymbolWorker::pause()
{
    paused_.store(true, std::memory_order_release);
    status_.store(WorkerStatus::PAUSED, std::memory_order_release);
    LOG_INFO("symbol_worker", "Worker paused: symbol={}", symbol_);
}

void SymbolWorker::resume()
{
    paused_.store(false, std::memory_order_release);
    status_.store(WorkerStatus::ACTIVE, std::memory_order_release);
    LOG_INFO("symbol_worker", "Worker resumed: symbol={}", symbol_);
}

// ── Ring buffer access ────────────────────────────────────────────────────────

TickRingBuffer* SymbolWorker::ring_buffer() noexcept
{
    return ring_buffer_.get();
}

// ── Configuration hot-reload ──────────────────────────────────────────────────

void SymbolWorker::update_config(const config::IndicatorConfig& config)
{
    indicator_config_ = config;

    analytics::SignalDetectorConfig sdc{};
    sdc.pulse_score_threshold    = config.pulse_score_threshold;
    sdc.turns_min_score          = config.turns_min_score;
    sdc.ratio_threshold          = config.ratio_threshold;
    sdc.ratio_min_volume         = config.ratio_min_volume;
    sdc.single_print_threshold   = config.single_print_threshold;
    sdc.sweep_volume_threshold   = config.sweep_volume_threshold;
    sdc.sweep_level_threshold    = config.sweep_level_threshold;
    sdc.slingshot_distance_ticks = config.slingshot_distance_ticks;
    signal_detector_->set_config(sdc);

    LOG_INFO("symbol_worker", "Config updated: symbol={}", symbol_);
}

// ── State queries ─────────────────────────────────────────────────────────────

const std::string& SymbolWorker::symbol()    const noexcept { return symbol_; }
uint32_t           SymbolWorker::symbol_id() const noexcept { return symbol_id_; }

WorkerStatus SymbolWorker::status() const noexcept
{
    return status_.load(std::memory_order_acquire);
}

WorkerStats SymbolWorker::stats() const noexcept
{
    return stats_;
}

const BarRecord* SymbolWorker::last_bar() const noexcept
{
    return bar_engine_->last_bar(default_bar_size_);
}

const BarRecord* SymbolWorker::current_bar() const noexcept
{
    return bar_engine_->current_bar(default_bar_size_);
}

int64_t SymbolWorker::cumulative_delta() const noexcept
{
    return delta_state_.cumulative_delta;
}

double SymbolWorker::session_vwap() const noexcept
{
    return vwap_engine_->daily_vwap();
}

// ── Main processing loop ──────────────────────────────────────────────────────

void SymbolWorker::run_loop()
{
    status_.store(WorkerStatus::ACTIVE, std::memory_order_release);

    // Initialise session with current time as a placeholder
    session_open_ts_ns_ = now_ns();
    on_session_open(session_open_ts_ns_);

    LOG_INFO("symbol_worker", "Worker active: symbol={}", symbol_);

    while (!stop_requested_.load(std::memory_order_acquire)) {
        auto tick_opt = ring_buffer_->try_pop();
        if (!tick_opt.has_value()) {
            std::this_thread::yield();
            continue;
        }
        stats_.ticks_processed++;
        stats_.ring_buffer_drops = ring_buffer_->dropped_count();
        process_tick(*tick_opt);
    }

    // Drain remaining ticks before exiting
    while (true) {
        auto tick_opt = ring_buffer_->try_pop();
        if (!tick_opt.has_value()) break;
        stats_.ticks_processed++;
        process_tick(*tick_opt);
    }

    status_.store(WorkerStatus::STOPPED, std::memory_order_release);
}

// ── Per-tick pipeline ─────────────────────────────────────────────────────────

void SymbolWorker::process_tick(UniversalTickRecord& tick)
{
    // Stage 2: Update prevailing quote for Lee-Ready classifier
    if (tick.is_quote_update()) {
        classifier_.update_quote(tick.tick_type, tick.price, lee_ready_state_);
        return;
    }

    if (tick.tick_type != TickType::TRADE) return;

    // Stage 3: Lee-Ready classification
    classifier_.classify_inplace(tick, lee_ready_state_);

    if (tick.side == TickSide::ASK) {
        stats_.ticks_classified_ask++;
    } else if (tick.side == TickSide::BID) {
        stats_.ticks_classified_bid++;
    } else {
        stats_.ticks_unclassified++;
        return;  // cannot accumulate unclassified ticks
    }

    // Stage 4: Bar accumulation (may trigger on_bar_close via callback)
    bar_engine_->on_tick(tick, session_open_ts_ns_);

    // Stage 5: Delta update
    delta_engine_->on_tick(tick, delta_state_);

    // Stage 6: VWAP update
    vwap_engine_->on_tick(tick);

    // Stage 7: Volume Profile update
    profile_engine_->on_tick(tick);

    // Stage 8: Tick-level VWAP signal check (non-blocking, paused-aware)
    if (!paused_.load(std::memory_order_relaxed)) {
        auto vwap_sigs = vwap_engine_->detect_signals(
            tick.price,
            delta_state_.bar_delta,
            tick.exchange_ts_ns);

        for (auto& sig : vwap_sigs) {
            sig.symbol_id = symbol_id_;
            sig.symbol    = symbol_;
            if (event_bus_->publish_signal(sig)) {
                stats_.signals_emitted++;
            }
        }
    }

    // Stage 11: OFE-Script evaluator notification (stub — future work)
}

// ── Bar close handler ─────────────────────────────────────────────────────────

void SymbolWorker::on_bar_close(const BarRecord& bar)
{
    stats_.bars_closed++;

    // Copy bar so DeltaEngine can fill in delta fields (bar_delta, cvd, etc.)
    BarRecord enriched = bar;
    enriched.detection_ts_ns = now_ns();

    // Stage 5b: Finalize delta metrics for this bar
    delta_engine_->on_bar_close(enriched, delta_state_);

    // Publish the fully enriched bar to event bus always (even when paused)
    event_bus_->publish_bar(enriched);

    if (paused_.load(std::memory_order_relaxed)) return;

    // Get current snapshots for signal detectors
    const int64_t snap_ts = enriched.detection_ts_ns;
    auto profile_snap = profile_engine_->get_session_snapshot();
    auto vwap_snap    = vwap_engine_->get_snapshot(enriched.close, snap_ts);

    // Stages 9-10: Run all bar-close signal detectors
    auto sigs = signal_detector_->detect_all(
        enriched, delta_state_, profile_snap, vwap_snap, active_zones_);

    for (auto& sig : sigs) {
        // Ensure symbol context is set (detector may not know symbol string)
        sig.symbol_id         = symbol_id_;
        sig.symbol            = symbol_;
        sig.detection_ts_ns   = snap_ts;
        sig.bar_open_ts_ns    = enriched.bar_open_ts_ns;
        sig.bar_close_ts_ns   = enriched.bar_close_ts_ns;
        sig.price_at_signal   = enriched.close;
        sig.bar_id            = enriched.bar_id;
        sig.bar_open          = enriched.open;
        sig.bar_high          = enriched.high;
        sig.bar_low           = enriched.low;
        sig.bar_close         = enriched.close;
        sig.bar_delta         = enriched.bar_delta;
        sig.bar_volume        = enriched.total_volume;
        sig.cumulative_delta  = delta_state_.cumulative_delta;

        if (event_bus_->publish_signal(sig)) {
            stats_.signals_emitted++;
        }
    }

    // Publish delta snapshot for live streaming clients
    event_bus_->publish_delta_snapshot(
        symbol_id_,
        delta_state_.bar_delta,
        delta_state_.cumulative_delta,
        enriched.total_volume,
        snap_ts);

    LOG_DEBUG("symbol_worker",
        "Bar close: symbol={} O={} H={} L={} C={} vol={} delta={} signals={}",
        symbol_, enriched.open, enriched.high, enriched.low, enriched.close,
        enriched.total_volume, enriched.bar_delta, sigs.size());
}

// ── Session management ────────────────────────────────────────────────────────

void SymbolWorker::on_session_open(int64_t session_open_ts_ns)
{
    session_open_ts_ns_ = session_open_ts_ns;

    bar_engine_->on_session_open(session_open_ts_ns);
    delta_engine_->on_session_open(delta_state_);
    profile_engine_->on_session_open(session_open_ts_ns);
    vwap_engine_->on_session_open(session_open_ts_ns);
    vwap_engine_->check_weekly_reset(session_open_ts_ns);
    vwap_engine_->check_yearly_reset(session_open_ts_ns);

    LOG_INFO("symbol_worker",
        "Session open: symbol={} ts_ns={}", symbol_, session_open_ts_ns);
}

void SymbolWorker::on_session_close(int64_t session_close_ts_ns)
{
    bar_engine_->on_session_close(session_close_ts_ns);
    profile_engine_->on_session_close(session_close_ts_ns);

    LOG_INFO("symbol_worker",
        "Session close: symbol={} ts_ns={} bars={} signals={}",
        symbol_, session_close_ts_ns, stats_.bars_closed, stats_.signals_emitted);
}

} // namespace core
} // namespace ofe
