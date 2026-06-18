#pragma once
/**
 * ofe.h
 * Master include — includes all Order Flow Engine public headers.
 * Plugins and NinjaTrader adapter include only this file.
 *
 * Order Flow Engine v1.0
 * Spec: Functional Specification v4.0
 */

// ── Core infrastructure ───────────────────────────────────────────────────────
#include "core/tick_record.h"
#include "core/ring_buffer.h"
#include "core/tick_router.h"
#include "core/lee_ready.h"
#include "core/bar_types.h"
#include "core/bar_engine.h"
#include "core/symbol_worker.h"
#include "core/event_bus.h"

// ── Analytics / indicators ────────────────────────────────────────────────────
#include "analytics/delta_engine.h"
#include "analytics/imbalance_detector.h"
#include "analytics/volume_profile.h"
#include "analytics/vwap_engine.h"
#include "analytics/signal_detector.h"

// ── Signal types ──────────────────────────────────────────────────────────────
#include "signals/signal_types.h"

// ── Data feed adapters ────────────────────────────────────────────────────────
#include "feed/adapter_interface.h"

// ── Configuration ─────────────────────────────────────────────────────────────
#include "config/engine_config.h"

// ── License engine ────────────────────────────────────────────────────────────
#include "license/license_engine.h"

// ── API server ────────────────────────────────────────────────────────────────
#include "api/api_server.h"

/// OFE engine version
#define OFE_VERSION_MAJOR 1
#define OFE_VERSION_MINOR 0
#define OFE_VERSION_PATCH 0
#define OFE_VERSION_STR   "1.0.0"
