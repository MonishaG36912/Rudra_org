# OFE — Order Flow Engine Continuation Bundle
# Generated: 2026-06-19 | Updated: 2026-06-21 | Session: 7 (OFEFootprintNT — native NT8 tick indicator)
# PURPOSE: Feed this file to any AI tool to resume development exactly where we stopped.

---

## 1. PRODUCT IDENTITY

**What:** Commercial real-time order flow analytics engine for institutional-grade trading.
**Platform:** NinjaTrader 8 (v1.0), then Sierra Chart, TradingView, Bookmap, web dashboard.
**Revenue:** SaaS subscription: Free($0/1sym) → Starter($29/5) → Trader($79/25) → Professional($199/100) → Elite($499/500) → Enterprise(custom/500+).
**Core output:** Footprint chart — bid×ask volume at every price level inside every bar, with imbalance zones, delta, CVD, VWAP, Volume Profile, and composite signals overlaid.

---

## 2. ARCHITECTURE (DECIDED — DO NOT CHANGE)

```
Layer 6: Clients         NinjaTrader C# AddOn | React Portal | SDKs
Layer 5: API             Go REST + WebSocket | Rust OFE-Script evaluator
Layer 4: Event Bus       Redis Streams (signals, bars, deltas)
Layer 3: Analytics       SymbolWorker pool → Delta | Imbalance | VP | VWAP | Signals
Layer 2: Routing         TickRouter (MPMC) → per-symbol SPSC ring buffers (16384 slots)
Layer 1: Feeds           IQFeed TCP | Kinetick NT bridge | eSignal COM
```

**Key decisions (locked):**
- C++20 core (deterministic latency, cache-line control)
- Go API server (goroutines for 10K+ WebSocket connections)
- Rust OFE-Script evaluator (memory safety for user scripts)
- Symbol-sharded: each symbol = independent thread, zero cross-symbol contention
- Lock-free SPSC ring buffer (not mutex queue)
- UniversalTickRecord: exactly 64 bytes, cache-aligned, `static_assert` enforced
- Lee-Ready + IQFeed direct aggressor (CME/ICE use exchange-reported; others fall back to Lee-Ready)
- 3 parameter modes: AUTO / SEMI-AUTO / MANUAL for ALL 40 indicator parameters
- Redis Streams for inter-layer event bus
- TimescaleDB for time-series persistence

---

## 3. CODEBASE STATE

### GitHub Repo: `Rudra_org`
- Include path: `include/ofe/core/`, `include/ofe/analytics/`, etc. (nested under `ofe/`)
- Session code used flat `include/core/` — **must adapt all #include paths when merging**
- Repo has 3 existing tests (ring buffer SPSC, imbalance diagonal, VP value area)

### Headers (GATE-02 COMPLETED — 19 files, 3302 lines)

#### core/ (8)
| Header | Key Types |
|--------|-----------|
| `tick_record.h` | `UniversalTickRecord` (64B, cache-aligned), `TickSide{ASK,BID,UNKNOWN}`, `TickType{TRADE,BID_QUOTE,ASK_QUOTE,SUMMARY}`, `DataProvider{IQFEED,KINETICK,ESIGNAL,REPLAY}`, `TradeCondition` bitmask (TC_NORMAL..TC_FORM_T), `make_trade()` factory, `should_exclude_tick()` |
| `ring_buffer.h` | `SPSCRingBuffer<T,Capacity>` — power-of-2, false-sharing-free head/tail (cacheline padding), `try_push/try_pop/peek`, `dropped_count()`. Alias: `TickRingBuffer = SPSCRingBuffer<UniversalTickRecord, 16384>` |
| `tick_router.h` | `TickRouter(max_symbols)` — `register_symbol(symbol, id, buffer*)`, `unregister_symbol(id)`, `route(tick)` hot-path (shared_lock read), `compute_symbol_id(symbol)` FNV-1a hash, `RouterStats` |
| `lee_ready.h` | `LeeReadyState{prev_trade_price, prevailing_bid/ask, last_classified}`, `LeeReadyClassifier::classify_trade(price, exchange_aggressor, state)` → Quote Rule → Tick Rule → carry-forward, `update_quote()`, `classify_inplace(tick, state)` |
| `bar_types.h` | `BarType{TIME,RANGE,VOLUME}`, `BarSize{type,size_value}`, `PriceLevelRecord{price, bid_vol, ask_vol, delta, is_buy/sell_imbalance, is_poc/cot/zero_print}`, `BarRecord` (OHLCV + 28 OFE fields + `vector<PriceLevelRecord> price_levels`) |
| `bar_engine.h` | `BarSeries(bar_size, symbol, tick_size, callback)` — `on_tick/force_close/on_session_open`. `BarEngine` — up to 4 simultaneous series. `BarCloseCallback = function<void(BarRecord&)>` |
| `symbol_worker.h` | `WorkerStatus{IDLE,STARTING,BACKFILLING,RUNNING,PAUSED,DRAINING,ERROR,STOPPED}`, `SymbolWorker` — owns ring_buffer, lee_ready_state, bar_engine, all analytics engines. `start/stop/pause/resume`, `update_config()` hot-reload |
| `event_bus.h` | `IEventBus` abstract: `publish_bar/publish_signal/publish_delta_snapshot`, `queue_depth()`, `is_healthy()` |

#### analytics/ (5)
| Header | Key Types |
|--------|-----------|
| `delta_engine.h` | `DeltaState{bar_delta, running_delta, max/min_delta, cumulative_delta, session_avg_abs_delta, bar_count}`, `DeltaEngine(ema_alpha=0.1333)` — `on_tick/on_bar_close/on_session_open`, `detect_divergence/surge/extreme/small_range` |
| `imbalance_detector.h` | `ImbalanceConfig{imbalance_ratio=3.0, stacked_min_count=3, absorption_vol_thresh=500, absorption_delta_ratio=0.10, zone_strength_weight=1.0}`, `ImbalanceDetector::detect_all(bar, zones, tick_size)` → single + stacked + absorption + trapped |
| `volume_profile.h` | `ProfileType` (8 types), `ProfileShape{D,P,b,Thin}`, `VolumeProfileEngine::on_tick/compute_poc/compute_value_area(two-level expansion)/classify_shape/get_snapshot`. Hash map keyed by `price×1000` integer |
| `vwap_engine.h` | `VwapType` (6), `AnchorType` (8), `VwapSeries{CumPV, CumPV2, CumVol, vwap, bands ±1/2/3σ}`, `VwapEngine::on_tick` incremental, daily/weekly/yearly resets, anchored add/remove, `detect_signals` |
| `signal_detector.h` | `SignalDetector::detect_all(bar)` — Pulse(7-var weighted composite, threshold≥70), Turns(6 binary conditions, threshold≥3), Ratio(extreme bid/ask), SinglePrints(magnet tracking), COT(argmax total_vol), MarketSweep(vol+levels+speed), POCSlingshot. Rolling `bar_history_` deque + `active_magnets_` registry |

#### Other (6)
| Header | Key Types |
|--------|-----------|
| `signals/signal_types.h` | `SignalType` enum (62 values, grouped: Delta 100s, Imbalance 200s, Profile 300s, VWAP 400s, Composite 500s, Execution 600s, Accum/Dist 700s), `SignalDirection{LONG,SHORT,NEUTRAL}`, `SignalStrength{WEAK,MODERATE,STRONG,VERY_STRONG}`, `SignalEvent` (30 fields incl 4 timestamps + metadata_json), `ImbalanceZone` |
| `feed/adapter_interface.h` | `IDataFeedAdapter` abstract: connect/disconnect/subscribe/unsubscribe/request_history, `create_adapter()` factory |
| `config/engine_config.h` | `SessionFilter{ALL,RTH_ONLY,ETH_ONLY,GLOBEX}`, `EngineConfig`, `IndicatorConfig` (48 params), `WatchlistConfig` |
| `license/license_engine.h` | `SubscriptionTier` (6), `HardwareFingerprint` (5 fields + SHA-256), `LicenseToken` (RSA-2048 sig), `LicenseEngine::can_allocate_symbol()` Layer-1 enforcement |
| `api/api_server.h` | `IApiServer`: 7 publish methods, `SymbolQuota` |
| `ofe.h` | Master include, `OFE_VERSION_STR "1.0.0"` |

### Implementation Files

| File | Status | Key Content |
|------|--------|-------------|
| `src/util/logger.cpp` | DONE | Async rotating logger, 5 levels (TRACE/DEBUG/INFO/WARN/ERROR), background IO thread, daily file rotation, ANSI console, queue cap 65536 |
| `src/core/tick_record.cpp` | DONE | `make_trade()` factory, `should_exclude_tick()` with TRACE logging |
| `src/core/ring_buffer.cpp` | DONE | Template impl bodies + explicit instantiations (64, 256, 16384 slots) |
| `src/core/tick_router.cpp` | DONE | MPMC routing with shared_mutex, INFO register/unregister, WARN throttled drops (every 100th) and unknowns (every 1000th) |
| `src/core/lee_ready.cpp` | DONE | Quote Rule (above/below midpoint), Tick Rule (uptick/downtick/zero-tick carry-forward), IQFeed override (aggressor 1=ASK, 2=BID), `classify_inplace` sets `side=UNKNOWN` for quote ticks |
| `src/core/bar_types.cpp` | DONE | `BarSize::label/is_valid`, factories, `BarRecord::get_level` binary search, `PriceLevelRecord` helpers |
| `src/core/bar_engine.cpp` | DONE | `BarSeries::on_tick` → `should_close` → `close_current_bar` (sorts levels, computes POC, calls callback). TIME=nanosecond boundary, RANGE=tick movement, VOLUME=contract threshold. BarEngine manages up to 4 series. |
| `src/core/symbol_worker.cpp` | DONE | Full 11-stage tick pipeline in `run_loop` → `process_tick`. Stages: dequeue → quote update → Lee-Ready → bar accumulation → delta → VWAP → VP → tick-level signals. `on_bar_close` enriches bar, runs all bar-close detectors, publishes enriched bar + signals + delta snapshot to event bus. Lifecycle: start/stop/pause/resume. Session open/close resets all engines. |
| `src/core/event_bus_redis.cpp` | DONE (InProcess) | Implements `InProcessEventBus` (in-memory deque queues for bars and signals). `publish_bar`, `publish_signal`, `publish_delta_snapshot`, `drain_bars`, `drain_signals`, queue-depth health check. **Note:** file is named `event_bus_redis.cpp` but implements the in-process backend. Redis Streams backend is future work. |
| `src/analytics/delta_engine.cpp` | DONE | `on_tick` (O(1) hot path), `on_bar_close` (bar_delta, CVD+=, max/min, EMA update, delta_pct), `on_session_open` (zero all), `detect_surge` (magnitude vs 3×EMA), `detect_divergence` (bar-level: bearish bar + positive delta → LONG), `detect_extreme`, `detect_small_range` |
| `src/analytics/imbalance_detector.cpp` | DONE | `detect_all` entry point → `detect_single_imbalances` (diagonal: `ask_vol[P] >= ratio × bid_vol[P-1tick]`), `detect_stacked` (3+ consecutive → zone, extends existing zones), `detect_absorption` (vol >= thresh AND |delta|/vol <= 0.10), `detect_trapped_traders` (imbalance at extreme + opposite close), `update_zone_registry` (resolve zones when price trades through) |
| `src/analytics/volume_profile.cpp` | PARTIAL | `on_tick` accumulates bid/ask into hash map, `compute_poc` (argmax total_vol), `compute_value_area` (single-level expansion — **MUST change to two-level per spec**), `is_hvn/is_lvn` (vs session mean), `get_session_snapshot`. **Missing:** two-level VA expansion, `classify_shape` (returns UNCLASSIFIED stub), anchored profile accumulation (stubs). |
| `src/analytics/vwap_engine.cpp` | PARTIAL | `on_tick` incremental (CumPV/CumPV2/CumVol for daily/weekly/monthly/yearly and all anchored series), `recompute` StdDev bands ±1/2/3σ, `on_session_open` daily reset, `check_weekly_reset` (Monday), `check_yearly_reset` (Jan 1), `add/remove_anchored_vwap`, `get_snapshot`. **Missing:** `detect_signals` returns `{}` — no VWAP reaction/rotation/band-touch signals. |
| `src/analytics/signal_detector.cpp` | SCAFFOLD | `detect_all` dispatcher wired to all sub-detectors. `push_bar_history`. `compute_cot` (argmax price_levels by total_vol). Private scoring helpers: `compute_of_score`, `compute_delta_score`, `compute_imbalance_score`, `compute_volume_score`, `compute_pa_score`, `compute_swing_score` (all partially implemented; `compute_poc_score` hardcoded). **Missing:** `detect_pulse`, `detect_turns`, `detect_ratio`, `detect_single_prints`, `detect_market_sweep`, `detect_poc_slingshot`, accumulation/distribution, volume decline, sequencing, zero/exhaustion prints, magnet registry — all return `nullptr` or `{}`. |

### Unit Tests (56/56 PASSING — Session 1 baseline)

| File | Tests | Coverage |
|------|-------|----------|
| `test/test_tick_record.cpp` | 22 | sizeof==64, alignof==64, make_trade fields, is_accumulatable (5 cases), is_quote_update (3), should_exclude (9 flag combos), zero-init |
| `test/test_ring_buffer.cpp` | 12 | push/pop single, move semantics, overflow returns false + drop count, FIFO ordering (10 items), peek non-destructive, size_approx, concurrent 100K producer/consumer (sum verification), capacity constant, empty/full flags, wrap-around (32+32) |
| `test/test_lee_ready.cpp` | 22 | IQFeed aggressor 1→ASK / 2→BID / 0→fallback, quote rule above/below midpoint, midpoint fallback to tick rule, uptick/downtick/zero-tick carry, state updates, sequence of 4 trades, update_quote bid/ask/ignore-trade, is_ready/midpoint, classify_inplace trade+quote, no-data→UNKNOWN, float precision at midpoint |

---

## 4. BUILD & TEST COMMANDS

**Script:** `ofe_session1_code/ofe/build.sh` — run from anywhere, dispatches by name.
**Build dirs:** `build/` = Debug+ASan, `build_rel/` = Release, `build_test/` = Debug (test binary lives here).

```bash
# ── CONFIGURE (first time, or after CMakeLists changes) ──────────────────────
cd ofe_session1_code/ofe
cmake -S . -B build      -DCMAKE_BUILD_TYPE=Debug    # Debug + ASan + UBSan
cmake -S . -B build_rel  -DCMAKE_BUILD_TYPE=Release  # Release O3+LTO
cmake -S . -B build_test -DCMAKE_BUILD_TYPE=Debug    # dedicated test dir

# ── BUILD (incremental) ───────────────────────────────────────────────────────
cmake --build build      -j$(nproc)   # debug engine + lib
cmake --build build_rel  -j$(nproc)   # release engine + lib
cmake --build build_test -j$(nproc)   # debug engine + ofe_tests binary

# ── RUN ALL TESTS ────────────────────────────────────────────────────────────
ctest --test-dir build_test --output-on-failure        # summary view
./build_test/ofe_tests --gtest_color=yes               # full gtest output

# ── RUN ONE SUITE ────────────────────────────────────────────────────────────
./build_test/ofe_tests --gtest_filter="TickRecord*"
./build_test/ofe_tests --gtest_filter="RingBuffer*"
./build_test/ofe_tests --gtest_filter="LeeReady*"

# ── RUN ONE TEST CASE ────────────────────────────────────────────────────────
./build_test/ofe_tests --gtest_filter="TickRecord.SizeIs64Bytes"

# ── BUILD.SH SHORTCUTS ───────────────────────────────────────────────────────
./build.sh build_test          # incremental build
./build.sh run_tests           # ctest (logs → test_logs/last_run.log)
./build.sh run_tests_verbose   # full gtest (logs → test_logs/last_run_verbose.log)
./build.sh run_suite LeeReady  # single suite
./build.sh build_and_test      # build + full test in one shot
./build.sh nuke_and_reconfigure  # wipe all build dirs and reconfigure

# ── CLEAN REBUILD ────────────────────────────────────────────────────────────
cmake --build build_test --target clean && cmake --build build_test -j$(nproc)
```

**Test log files** are written to `test_logs/` (gitignored). Each run overwrites `last_run.log` / `last_run_verbose.log`.

---

## 5. KEY FORMULAS (implement exactly)

```
Delta:      bar_delta = SUM(ask_vol) - SUM(bid_vol) per bar
CVD:        cumulative_delta += bar_delta each bar. RESETS to 0 at session_open.
Delta%:     bar_delta / total_volume × 100
Surge:      |bar_delta| / EMA(|bar_delta|, 14) >= 3.0 → fire. EMA alpha = 2/(14+1) = 0.1333

Imbalance:  ask_vol[P] >= ratio × bid_vol[P - 1_tick]  (DIAGONAL, not horizontal)
Stacked:    3+ consecutive same-direction imbalances → zone
Absorption: total_vol >= threshold AND |delta|/total_vol <= 0.10

Value Area: Two-level expansion from POC until 70% volume accumulated.
            Compare upper_2 = profile[VA_high+1]+profile[VA_high+2] vs
                    lower_2 = profile[VA_low-1]+profile[VA_low-2]
            Expand toward larger pair. Repeat until accumulated >= 70%.

VWAP:       CumPV += P×V; CumVol += V; VWAP = CumPV/CumVol (INCREMENTAL ONLY)
StdDev:     CumPV2 += P²×V; Variance = CumPV2/CumVol - VWAP²; σ = sqrt(Var)

Pulse:      7 variables × weights (sum=1.0): order_flow(0.20), delta(0.20),
            poc(0.15), imbalances(0.20), volume(0.10), price_action(0.10),
            swing(0.05). Score 0-100. Threshold ≥ 70 → signal.

Turns:      6 binary conditions at bar extreme. Score = count. ≥ 3 → signal.
            V1:high_vol_at_extreme V2:vol_exhaustion V3:sufficient_vol
            V4:reversal_candle V5:delta_confirms V6:swing_context

COT:        cot_price = argmax(bid_vol[P] + ask_vol[P]) within bar
Ratio:      bid_vol[BarLow]/ask_vol[BarLow] >= threshold → Bottom Heavy (big buyer)
Single:     total_vol at bar extreme ≤ 2 → last buyer/seller → magnet
```

---

## 6. PARAMETER AUTO-CALIBRATION (ALL 40 PARAMS)

**3 modes for every parameter:**
- **AUTO:** Engine computes from live session data (percentile, EMA, rolling avg)
- **SEMI-AUTO:** Engine computes baseline, user sets multiplier (0.5=sensitive, 2.0=selective)
- **MANUAL:** User sets absolute value

**Cold start warm-up (3 stages):**
- Stage 1 (bars 0-10): instrument preset (ES=500 absorption, NQ=300, CL=200)
- Stage 2 (bars 11-30): blend preset 70% + live 30%
- Stage 3 (30+ bars): 100% live session data. Confidence indicator: 🔴→🟡→🟢

**Instrument presets:** ES(500/1000/3.0×), NQ(300/600/3.0×), CL(200/400/3.0×), GC(100/200/3.0×), AAPL(5000/10000/3.0×)

**API transparency:** `GET /v1/config/{symbol}/baselines` returns auto_value + effective_value + mode for every parameter.

---

## 7. LOGGING SYSTEM (IMPLEMENTED)

**Architecture:** Async queue → background IO thread → daily rotating files + ANSI console.
**Levels:** TRACE(0, Debug-only), DEBUG(1, Debug-only), INFO(2), WARN(3), ERROR(4)
**Format:** `2026-06-07 14:32:07.291847362 | DEBUG | T:ab12cd34 | bar_engine            | message  [file.cpp:187]`
**Macros:** `LOG_TRACE("module", "msg val={}", val)` — TRACE/DEBUG compiled away when NDEBUG defined.

**CRITICAL MACRO PITFALLS (discovered in Part B):**
1. Parameter names must be `ofe_lvl`/`ofe_mod` — NOT `level`/`module` (field name collision)
2. Module string assigned via `std::string(ofe_mod)` — NOT direct `= (ofe_mod)` (preprocessor substitution)
3. Never use `{:#x}` in format strings — `#` is preprocessor stringification operator in variadic macros

---

## 8. PART B INTEGRATION FIXES (APPLIED)

| ID | File | Fix |
|----|------|-----|
| PB-01 | logger.h | OFE_LOG param `level` → `ofe_lvl` (field name collision) |
| PB-02 | logger.h | `_rec.module = (module)` → `std::string(ofe_mod)` (literal substitution) |
| PB-03 | tick_record.h | Added `noexcept` to `make_trade()` declaration |
| PB-04 | lee_ready.cpp | `classify_inplace` sets `side=UNKNOWN` for quote ticks |
| PB-05 | CMakeLists.txt | Commented out empty `ofe_server`/`ofe_tick_gen` targets |
| PB-06 | CMakeLists.txt | Fixed generator expressions (semicolons between flags) |
| PB-07 | imbalance_detector.cpp | `absorption_vol_threshold` → `absorption_vol_thresh` |
| PB-08 | *.cpp | `{:#x}` → `{:x}` (preprocessor `#` conflict) |
| PB-09 | tick_router.cpp | Added `<mutex>` and `<shared_mutex>` includes |

### PENDING FIXES (Session 2 — not yet applied)

| ID | File | Fix |
|----|------|-----|
| PB-10 | signal_detector.cpp:68 | `{:#x}` → `{:x}` in LOG_DEBUG (PB-08 violation introduced in stub generation) |
| PB-10 | event_bus_redis.cpp:86 | `{:#x}` → `{:x}` in LOG_TRACE (same) |
| PB-10 | symbol_worker.cpp:101 | `{:#x}` → `{:x}` in LOG_INFO (same) |

---

## 9. PENDING TASKS (in priority order)

| Priority | Task | Agent | Status |
|----------|------|-------|--------|
| 0 | Fix PB-10: `{:#x}` → `{:x}` in signal_detector:68, event_bus_redis:86, symbol_worker:101 | Manual | PENDING |
| 1 | `volume_profile.cpp` — two-level VA expansion + `classify_shape` | AGT-04 | PARTIAL |
| 2 | `vwap_engine.cpp` — implement `detect_signals` | AGT-04 | PARTIAL |
| 3 | `signal_detector.cpp` — Pulse(7-var), Turns(6-var), Ratio, SinglePrints, MarketSweep, POCSlingshot, magnet registry | AGT-04 | SCAFFOLD |
| 4 | `symbol_worker.cpp` | AGT-03 | DONE |
| 5 | `event_bus_redis.cpp` (InProcessEventBus done; Redis Streams backend) | AGT-03 | DONE (in-process) |
| 6 | `test_bar_engine.cpp` (TIME/RANGE/VOLUME close, OHLCV, POC) | AGT-10 | TODO |
| 7 | `test_delta_engine.cpp` (all 5 formulas, CVD reset, surge, EMA) | AGT-10 | TODO |
| 8 | `test_imbalance.cpp` (single/stacked/absorption/trapped/zone) | AGT-10 | TODO |
| 9 | `test_volume_profile.cpp` (POC, VA two-level 70%, HVN/LVN, shape) | AGT-10 | TODO |
| 10 | `test_vwap_engine.cpp` (incremental, StdDev, daily/weekly reset) | AGT-10 | TODO |
| 11 | `test_signal_detector.cpp` (Pulse 7-var, Turns 6-var) | AGT-10 | TODO |
| 12 | `iqfeed_adapter.cpp` (TCP 5009, protocol 6.2, aggressor) | AGT-05 | TODO |
| 13 | `kinetick_receiver.cpp` (localhost TCP bridge) | AGT-05 | TODO |
| 14 | `feed_manager.cpp` (multi-provider, failover 5s trigger) | AGT-05 | TODO |
| 15 | `hw_fingerprint.cpp` (CPUID+MAC+disk+mobo+OSID → SHA-256) | AGT-09 | TODO |
| 16 | `license_engine.cpp` (RSA-2048, symbol count, anti-tamper) | AGT-09 | TODO |
| 17 | NinjaTrader C# AddOn — OFEFootprintNT.cs ★ DONE Session 7 ★ | AGT-06 | DONE |
|    | → Test in NT8: compile Ctrl+F5, drag onto Schwab/Coinbase chart, verify "[OFE] Bar N closed" in Output | | NEXT |
|    | → Fix any NT8 compile errors from API mismatches | AGT-06 | IF NEEDED |
|    | → Add live (open) bar rendering | AGT-06 | TODO |
| 18 | Go REST+WebSocket API server | AGT-07 | TODO |
| 19 | Rust OFE-Script evaluator | AGT-07 | TODO |
| 20 | Merge into Rudra_org repo (adapt include paths) | Manual | TODO |

## NT8 FOOTPRINT — CRITICAL CONTEXT (Sessions 6-7)

### What the user has
- Schwab API → NT8 data provider → candlestick charts for stocks WORKING
- Coinbase → NT8 connection → basic candle chart WORKING
- IQFeed, Kinetick, eSignal → will also connect to NT8 natively (no custom code needed)
- C++ nt_bridge_server is NOT required for NT8 footprint — NT8 already has tick data

### Why old OFEFootprintIndicator.cs did not display
OFEFootprintIndicator depends on nt_bridge_server running on port 7777.
If server not running → indicator shows "OFE OFFLINE" and draws nothing.
This was the root cause of non-display reported in Sessions 6-7.

### Correct approach (Session 7)
OFEFootprintNT.cs reads NT8's own tick data via OnMarketData().
No C++ server, no TCP. Works with any NT8 data provider.
Install: ONLY copy OFEFootprintNT.cs to NT8 Custom\Indicators\ — no other files needed.

---

## 10. CONSTRAINTS

- **Never re-implement [DONE] files.** Read PROGRESS.md first.
- **UniversalTickRecord must remain 64 bytes.** `static_assert` enforced.
- **No heap allocation on hot path** (stages 1-9 of tick processing).
- **No mutex on TickRouter::route()** — shared_lock only (allows concurrent reads).
- **Value Area MUST use two-level expansion** (not one-level). Must match NinjaTrader.
- **VWAP MUST be incremental** — never recompute from scratch mid-session.
- **Pulse weights MUST sum to 1.0 ± 0.001.** Engine validates and rejects otherwise.
- **CVD resets to 0 at session_open.** Not at midnight — at RTH open.
- **Imbalance formula is DIAGONAL** — `ask_vol[P]` vs `bid_vol[P - 1_tick]`, NOT horizontal.
- **All test runs MUST dump to test_logs/** with structured headers and timestamps.
- **Rudra_org repo uses `include/ofe/core/`** (nested). Session code used `include/core/` (flat). All `#include` directives must be adapted when merging.

---

## 11. DOCUMENT LIBRARY

| Document | Content |
|----------|---------|
| `OrderFlow_Spec_v4_Master` | 23-section functional spec, all indicators |
| `OFE_Engineering_Product_Spec` (v1.1) | Architecture, perf reqs, error handling, security, logging system, Part B fixes, agent pipeline |
| `OFE_User_Guide` | Trader-facing: footprint reading, 4 trade setups, FAQ, glossary |
| `OrderFlow_Execution_Plan` | 14 agents, 6 phases, 28 weeks, 8 human gates |
| `OrderFlow_Indicator_Formulas` | All 26 indicator calculation formulas |
| `OrderFlow_Parameter_Calibration_Spec` | AUTO/SEMI-AUTO/MANUAL for 40 params, cold start, instrument presets |
| `OrderFlow_FootprintChart_Spec` | Cell colours, Z-order (13 layers), overlays, NT8 impl, 14 acceptance criteria |
| `OrderFlow_External_Developer_SDK_Spec` | REST/WebSocket/OFE-Script/backtesting |
| `OrderFlow_DataFeed_MultiSymbol_Spec` | IQFeed/Kinetick/eSignal protocols + 1-to-1000 scaling |
| `OrderFlow_MultiSymbol_Scaling_Spec` | Symbol-sharded architecture |
| `OrderFlow_Timestamp_Specification` | Signal vs snapshot timestamps, 4-timestamp model |

---

## 12. AGENT PROMPTS (in agents/prompts/)

AGT-01(Architecture) → AGT-02(Headers,DONE) → AGT-03(Core) → AGT-04(Analytics) → AGT-05(Feeds) → AGT-06(NinjaTrader) → AGT-07(API) → AGT-09(License) → AGT-10(Tests,parallel) → AGT-12(Security) → AGT-13(DevOps) → AGT-14(Review)

**To resume:** paste the relevant AGT-XX prompt + this continuation bundle into a new session. The agent reads PROGRESS.md, finds [NEXT], and continues from exactly where we stopped.

---
*End of continuation bundle. Next action: fix PB-10 (3-line change), then complete volume_profile.cpp two-level VA + classify_shape (AGT-04).*