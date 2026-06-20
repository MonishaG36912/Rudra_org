#pragma once
/**
 * symbol_worker.h
 * Symbol Worker — the atomic processing unit for one symbol.
 * Each active symbol has exactly one SymbolWorker running on its own thread.
 * Workers are completely independent: no shared mutable state across workers.
 *
 * Internal processing pipeline (per tick):
 *   1. Dequeue tick from ring buffer
 *   2. Update prevailing quote (if quote tick)
 *   3. Lee-Ready classification (if trade tick)
 *   4. Bar accumulation — update price level bid_vol / ask_vol
 *   5. Delta update — running bar_delta, max/min, CVD
 *   6. VWAP update — incremental CumPV / CumVol
 *   7. Volume Profile update — price bucket accumulation
 *   8. Tick-level signal check (Turns, VWAP Reaction, Delta Surge)
 *   9. Bar close check → if closing: run all bar-close signal detectors
 *  10. Publish signals to EventBus
 *  11. Notify OFE-Script evaluators
 *
 * Spec reference: Multi-Symbol Scaling Spec §2.2-2.3
 */

#include <cstdint>
#include <string>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <vector>
#include "tick_record.h"
#include "ring_buffer.h"
#include "bar_types.h"
#include "lee_ready.h"
#include "../config/engine_config.h"
#include "../analytics/delta_engine.h"

namespace ofe {

namespace analytics {
class VolumeProfileEngine;
class VwapEngine;
class SignalDetector;
} // namespace analytics

namespace core {

// Forward declarations
class BarEngine;
class EventBus;

// ── Worker state ──────────────────────────────────────────────────────────────

/**
 * WorkerStatus — current lifecycle state of a Symbol Worker.
 */
enum class WorkerStatus : uint8_t {
    IDLE        = 0,  ///< Constructed but not started
    STARTING    = 1,  ///< Thread starting, loading historical state
    BACKFILLING = 2,  ///< Requesting + processing historical backfill from provider
    ACTIVE      = 3,  ///< Processing live ticks normally
    PAUSED      = 4,  ///< Feed subscribed but signal detection suspended
    STOPPING    = 5,  ///< Graceful shutdown in progress
    STOPPED     = 6,  ///< Thread exited; state saved to DB
    ERROR       = 7   ///< Unrecoverable error; worker must be restarted
};

/**
 * WorkerStats — per-symbol performance and health metrics.
 * Exposed via the /v1/symbols/{symbol}/stats REST endpoint.
 */
struct WorkerStats {
    uint64_t ticks_processed;       ///< Total ticks processed this session
    uint64_t ticks_classified_ask;  ///< Ticks classified as ASK (aggressive buyers)
    uint64_t ticks_classified_bid;  ///< Ticks classified as BID (aggressive sellers)
    uint64_t ticks_unclassified;    ///< Ticks where Lee-Ready returned UNKNOWN
    uint64_t bars_closed;           ///< Number of completed bars this session
    uint64_t signals_emitted;       ///< Number of signals emitted this session
    uint64_t pipeline_latency_us;   ///< P99 tick-to-signal latency in microseconds
    uint64_t ring_buffer_drops;     ///< Ticks dropped due to ring buffer overflow
    WorkerStatus status;
};

// ── Symbol Worker ─────────────────────────────────────────────────────────────

/**
 * SymbolWorker
 * Owns all per-symbol state and runs the complete tick processing pipeline
 * for one symbol on a dedicated thread.
 *
 * Lifecycle:
 *   1. Construct with symbol config
 *   2. call start() → thread spawned, historical backfill requested
 *   3. Ticks arrive in ring_buffer() — pushed by Tick Router
 *   4. Worker processes ticks and emits signals via EventBus
 *   5. call stop() → graceful shutdown, state saved
 */
class SymbolWorker {
public:
    /**
     * Construct a Symbol Worker for the given symbol.
     *
     * @param symbol          Exchange symbol string (e.g. "ES#F")
     * @param symbol_id       Pre-computed routing hash
     * @param engine_config   Global engine configuration
     * @param indicator_config Per-symbol indicator configuration
     * @param event_bus       Shared event bus for signal publication
     */
    SymbolWorker(
        std::string                      symbol,
        uint32_t                         symbol_id,
        const config::EngineConfig&      engine_config,
        const config::IndicatorConfig&   indicator_config,
        std::shared_ptr<EventBus>        event_bus
    );

    ~SymbolWorker();

    SymbolWorker(const SymbolWorker&) = delete;
    SymbolWorker& operator=(const SymbolWorker&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /**
     * Start the worker thread and begin processing.
     * Triggers historical backfill from data provider before going live.
     * Returns immediately — backfill happens on the worker thread.
     */
    void start();

    /**
     * Gracefully stop the worker.
     * Drains remaining ticks from the ring buffer, saves state to DB,
     * then exits the thread.
     *
     * @param timeout_ms  Max milliseconds to wait for graceful drain (default 2000)
     */
    void stop(uint32_t timeout_ms = 2000);

    /**
     * Pause signal detection without stopping tick collection.
     * Ticks continue to flow and state (bars, delta, profile) continues
     * to update, but no signals are emitted. Saves CPU on paused symbols.
     */
    void pause();

    /**
     * Resume signal detection after a pause.
     */
    void resume();

    // ── Ring buffer access (for Tick Router) ─────────────────────────────

    /**
     * Returns a pointer to this worker's inbound tick ring buffer.
     * The Tick Router pushes ticks here; the worker thread reads from it.
     * This pointer is valid for the lifetime of the SymbolWorker.
     */
    [[nodiscard]] TickRingBuffer* ring_buffer() noexcept;

    // ── Configuration hot-reload ──────────────────────────────────────────

    /**
     * Apply a new indicator configuration without stopping the worker.
     * Takes effect on the next bar close.
     *
     * @param config  New indicator config (e.g. changed imbalance_ratio)
     */
    void update_config(const config::IndicatorConfig& config);

    // ── State queries ─────────────────────────────────────────────────────

    [[nodiscard]] const std::string& symbol()    const noexcept;
    [[nodiscard]] uint32_t           symbol_id() const noexcept;
    [[nodiscard]] WorkerStatus       status()    const noexcept;
    [[nodiscard]] WorkerStats        stats()     const noexcept;

    /// Returns the most recently completed BarRecord, or nullptr if no bar yet
    [[nodiscard]] const BarRecord* last_bar() const noexcept;

    /// Returns the current in-progress (incomplete) bar, or nullptr
    [[nodiscard]] const BarRecord* current_bar() const noexcept;

    /// Returns current session CVD
    [[nodiscard]] int64_t cumulative_delta() const noexcept;

    /// Returns current session VWAP
    [[nodiscard]] double  session_vwap()    const noexcept;

private:
    /// Main processing loop — runs on worker thread
    void run_loop();

    /// Process one tick through the full pipeline (stages 1-11)
    void process_tick(UniversalTickRecord& tick);

    /// Called when a bar closes — runs all bar-close signal detectors
    void on_bar_close(const BarRecord& bar);

    /// Called on session open — reset CVD, VWAP, session state
    void on_session_open(int64_t session_open_ts_ns);

    /// Called on session close — snapshot state for persistence
    void on_session_close(int64_t session_close_ts_ns);

    // ── Per-symbol owned state ────────────────────────────────────────────
    std::string                               symbol_;
    uint32_t                                  symbol_id_;
    std::unique_ptr<TickRingBuffer>           ring_buffer_;
    std::unique_ptr<BarEngine>                bar_engine_;
    std::unique_ptr<analytics::DeltaEngine>   delta_engine_;
    std::unique_ptr<analytics::VolumeProfileEngine> profile_engine_;
    std::unique_ptr<analytics::VwapEngine>    vwap_engine_;
    std::unique_ptr<analytics::SignalDetector> signal_detector_;
    LeeReadyClassifier                        classifier_;
    LeeReadyState                             lee_ready_state_;

    // ── Shared resources ─────────────────────────────────────────────────
    std::shared_ptr<EventBus>                 event_bus_;

    // ── Thread and lifecycle ──────────────────────────────────────────────
    std::thread                               worker_thread_;
    std::atomic<WorkerStatus>                 status_{WorkerStatus::IDLE};
    std::atomic<bool>                         stop_requested_{false};
    std::atomic<bool>                         paused_{false};

    // ── Mutable state (accessed only from worker thread) ─────────────────
    config::IndicatorConfig                   indicator_config_;
    WorkerStats                               stats_{};
    analytics::DeltaState                     delta_state_{};
    int64_t                                   session_open_ts_ns_{0};
    std::vector<signals::ImbalanceZone>       active_zones_{};
    BarSize                                   default_bar_size_{BarType::TIME, 300};
};

} // namespace core
} // namespace ofe
