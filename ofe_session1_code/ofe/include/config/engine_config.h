#pragma once
/**
 * engine_config.h
 * All engine configuration structures — loaded from YAML, hot-reloadable.
 * Spec reference: Data Feed Spec §8 (Complete Engine Configuration Reference)
 */

#include <cstdint>
#include <string>
#include <vector>

namespace ofe {
namespace config {

/**
 * SessionFilter — which trading hours to process.
 */
enum class SessionFilter : uint8_t {
    ALL       = 0,  ///< All hours (including pre/post market)
    RTH_ONLY  = 1,  ///< Regular Trading Hours only (default)
    ETH_ONLY  = 2,  ///< Extended Trading Hours only
    GLOBEX    = 3   ///< Full 23-hour CME Globex session
};

/**
 * EngineConfig — global engine-level configuration.
 * Applies to all symbols unless overridden per-symbol.
 *
 * YAML path: engine.*
 */
struct EngineConfig {
    // Symbol capacity
    uint32_t    max_symbols           = 100;    ///< Maximum simultaneously active symbols
    uint32_t    worker_threads        = 0;      ///< 0 = auto (one per CPU core)
    uint32_t    ring_buffer_size      = 16384;  ///< Ticks per symbol ring buffer (power of 2)
    uint32_t    bar_history_size      = 20;     ///< Rolling bar history per symbol

    // Session
    SessionFilter session_filter      = SessionFilter::RTH_ONLY;
    std::string timezone              = "America/New_York";

    // Logging
    std::string log_level             = "INFO";  ///< DEBUG / INFO / WARN / ERROR
    std::string log_dir               = "./logs";
    bool        log_tick_stream       = false;   ///< Very verbose — dev only

    // Storage
    std::string timescaledb_url;        ///< PostgreSQL connection string
    std::string redis_url;              ///< Redis connection string

    // Performance
    bool        enable_cpu_affinity   = false;  ///< Pin worker threads to cores
    uint32_t    snapshot_interval_sec = 60;     ///< State checkpoint interval
};

/**
 * IndicatorConfig — per-symbol indicator parameters.
 * Can be set globally (applies to all symbols) or per-symbol (overrides global).
 *
 * All parameters are hot-reloadable via PATCH /v1/config or YAML reload.
 */
struct IndicatorConfig {
    // ── Bar construction ──────────────────────────────────────────────────
    float    default_bar_type    = 0;     ///< 0=TIME, 1=RANGE, 2=VOLUME
    uint32_t default_bar_size    = 300;   ///< 5 minutes in seconds for TIME
    double   tick_size           = 0.25;  ///< Instrument minimum price increment

    // ── Imbalance ─────────────────────────────────────────────────────────
    float    imbalance_ratio     = 3.0f;  ///< Diagonal ratio threshold (default 300%)
    int32_t  stacked_min_count   = 3;     ///< Min consecutive imbalances for stacked
    bool     imbalance_enabled   = true;

    // ── Absorption ────────────────────────────────────────────────────────
    float    absorption_vol_threshold = 500.0f;
    float    absorption_delta_ratio   = 0.10f;
    bool     absorption_enabled       = true;

    // ── Volume Profile ────────────────────────────────────────────────────
    float    value_area_pct          = 0.70f;  ///< 70% value area
    float    hvn_multiplier          = 1.5f;
    float    lvn_multiplier          = 0.5f;
    bool     show_session_profile    = true;
    bool     show_weekly_profile     = false;
    bool     show_composite_profile  = false;
    int32_t  composite_lookback_days = 20;

    // ── VWAP ──────────────────────────────────────────────────────────────
    bool     vwap_daily_enabled    = true;
    bool     vwap_weekly_enabled   = true;
    bool     vwap_yearly_enabled   = false;
    int      vwap_touch_ticks      = 3;    ///< Tolerance for VWAP touch detection
    bool     show_sd1_bands        = true;
    bool     show_sd2_bands        = true;

    // ── Pulse ─────────────────────────────────────────────────────────────
    float    pulse_score_threshold = 70.0f;
    bool     pulse_enabled         = true;
    bool     pulse_multitf_enabled = false;
    uint32_t pulse_higher_tf_secs  = 1800;  ///< 30-minute higher TF for multi-TF

    // ── Turns ─────────────────────────────────────────────────────────────
    int      turns_min_score       = 3;
    bool     turns_enabled         = true;

    // ── Ratio (Top Heavy / Bottom Heavy) ─────────────────────────────────
    float    ratio_threshold       = 3.0f;
    int32_t  ratio_min_volume      = 100;
    bool     ratio_enabled         = true;

    // ── Single Prints ─────────────────────────────────────────────────────
    int32_t  single_print_threshold = 2;
    bool     single_print_enabled   = true;

    // ── Delta Surge ───────────────────────────────────────────────────────
    float    delta_surge_threshold  = 3.0f;
    bool     delta_surge_enabled    = true;

    // ── Market Sweep ─────────────────────────────────────────────────────
    int32_t  sweep_volume_threshold = 500;
    int      sweep_level_threshold  = 5;
    bool     sweep_enabled          = true;

    // ── POC Slingshot ─────────────────────────────────────────────────────
    int      slingshot_distance_ticks = 4;
    bool     slingshot_enabled         = true;

    // ── Alerts ────────────────────────────────────────────────────────────
    bool     signal_alert_enabled   = true;
    uint32_t alert_cooldown_seconds = 300;  ///< Suppress repeat alerts for N seconds

    // ── Feature flags (enforced by license engine) ────────────────────────
    bool     feature_vwap_all_types   = true;
    bool     feature_volume_bar_type  = true;
    bool     feature_api_access       = false;
    bool     feature_automation       = false;
    bool     feature_plugin_sdk       = false;
    bool     feature_sector_agg       = false;
};

/**
 * WatchlistConfig — configuration for one user watchlist.
 */
struct WatchlistConfig {
    std::string                   watchlist_id;
    std::string                   watchlist_name;
    std::vector<std::string>      symbols;
    std::string                   data_provider;     ///< Override global provider
    IndicatorConfig               indicator_override; ///< Override global indicators
    SessionFilter                 session_filter     = SessionFilter::RTH_ONLY;
    bool                          active             = true;
};

} // namespace config
} // namespace ofe
