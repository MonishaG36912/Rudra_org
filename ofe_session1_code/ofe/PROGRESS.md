# OFE Coding Progress Tracker
# Last updated: Session 7 (OFEFootprintNT.cs — native NT8 tick-feed footprint indicator)
# RULE: Any new session MUST read this file FIRST before writing any code.

## ═══════════════════════════════════════
## RESUME POINT FOR NEXT SESSION (Session 8)
## ═══════════════════════════════════════
# Session 7 DONE: OFEFootprintNT.cs — self-contained NT8 footprint indicator (no C++ server)
#
# ── IMMEDIATE NEXT STEP ─────────────────────────────────────────────────────
# TEST OFEFootprintNT.cs in NT8:
#   1. Copy ONLY nt8/nt_adapter/OFEFootprintNT.cs → Documents\NinjaTrader 8\bin\Custom\Indicators\
#   2. Ctrl+F5 compile → paste any errors here to fix
#   3. Open Schwab or Coinbase chart in NT8 → drag "OFE Footprint NT" onto chart
#   4. Watch Output tab (Ctrl+5) → should see "[OFE] Bar N closed: Δ=..." per bar
#   5. After first bar close: footprint cells should appear over the candles
#
# ── PENDING TASKS (in order) ────────────────────────────────────────────────
# 1. Fix any NT8 compile errors in OFEFootprintNT.cs
# 2. Add live (current open) bar footprint rendering
# 3. Add Volume Profile histogram option
# 4. Add CVD sub-panel option
# 5. License: hw_fingerprint.cpp → license_engine.cpp (AGT-09)
# 6. Go REST+WebSocket API server (AGT-07)
# 7. Rust OFE-Script evaluator
# 8. Merge ofe_session1_code/ to production Rudra_org repo layout (include/ofe/ paths)
# 9. IQFeed + Kinetick feed adapters (AGT-05)

## ═══════════════════════════════════════
## PHASE 1 — CORE ENGINE (src/core/)
## ═══════════════════════════════════════

### tick_record.cpp         [DONE] - UniversalTickRecord factory + should_exclude_tick + logging
### ring_buffer.cpp         [DONE] - SPSCRingBuffer template impl + explicit instantiations
### tick_router.cpp         [DONE] - TickRouter MPMC routing + symbol register/unregister + logging
### lee_ready.cpp           [DONE] - LeeReadyClassifier: Quote Rule + Tick Rule + IQFeed override
### bar_types.cpp           [DONE] - BarSize, PriceLevelRecord, BarRecord helpers
### bar_engine.cpp          [DONE] - BarSeries (TIME/RANGE/VOLUME) + BarEngine + logging
### symbol_worker.cpp       [DONE] - Full 11-stage tick pipeline + lifecycle (start/stop/pause/resume)
#                                    + on_bar_close (enrichment, signal dispatch, delta snapshot)
#                                    + on_session_open/close (resets all engines)
### event_bus_redis.cpp     [DONE] - Implements InProcessEventBus (in-memory deque queues)
#                                    NOTE: file named redis.cpp but implements InProcessEventBus
#                                    Redis Streams backend is future work (AGT-03 follow-up)

## ═══════════════════════════════════════
## PHASE 2 — ANALYTICS (src/analytics/)
## ═══════════════════════════════════════

### delta_engine.cpp        [DONE] - All 5 delta formulas + surge/extreme/divergence signals + logging
### imbalance_detector.cpp  [DONE] - Single+stacked+absorption+trapped_traders + logging
### volume_profile.cpp      [DONE] - on_tick, POC (argmax total_vol),
#                                    Value Area (TWO-LEVEL expansion, NinjaTrader-compatible §12 4.2),
#                                    classify_shape (D/b/P/Thin by POC position), HVN/LVN,
#                                    get_session_snapshot all done.
#                                    NOTE: anchored profile accumulation is still a stub (future work)
### vwap_engine.cpp         [DONE] - on_tick incremental (CumPV/CumPV2/CumVol), StdDev bands ±1/2/3σ,
#                                    daily/weekly/yearly/monthly series, anchored add/remove,
#                                    detect_signals: VWAP_REACTION_LONG/SHORT + VWAP_ROTATION_LONG/SHORT,
#                                    get_snapshot, accessors all done.
### signal_detector.cpp     [DONE] - detect_all dispatcher (14 stages), push_bar_history, compute_cot,
#                                    detect_pulse (7-var weighted composite, §15 7.1–7.2),
#                                    detect_turns (6-var binary, §16 8.1),
#                                    detect_ratio (top/bottom heavy, §17 9.1),
#                                    detect_single_prints + active_magnets_ registry (§18 10.1),
#                                    check_magnet_resolutions,
#                                    detect_market_sweep (vol+levels+speed, §20 12.1),
#                                    detect_poc_slingshot (distance+delta+vol, §22 14.1),
#                                    detect_zero_prints, detect_exhaustion_prints,
#                                    detect_sequencing, detect_volume_decline,
#                                    detect_accumulation_distribution,
#                                    private scoring helpers (of/delta/poc/imbalance/volume/pa/swing)
#                                    NOTE: compute_poc_score hardcoded to 50.0f — spec clarification needed

## ═══════════════════════════════════════
## PHASE 3 — UTILITY (src/util/)
## ═══════════════════════════════════════

### logger.cpp              [DONE] - Async rotating logger, 5 levels, file+console, ANSI colour

## ═══════════════════════════════════════
## PHASE 4 — DATA FEEDS (src/feed/)
## ═══════════════════════════════════════

### coinbase_adapter.cpp    [DONE — Session 5] - Coinbase Advanced Trade WebSocket (wss://)
#                                    PIMPL pattern; IXWebSocket + nlohmann/json
#                                    Public channels (market_trades, ticker, heartbeats)
#                                    JWT ES256 auth for private channels (OpenSSL EVP_DigestSign)
#                                    Side mapping: Coinbase BUY → ASK aggressor (direct, no Lee-Ready)
#                                    Volume in satoshi-precision int64_t (volume_scale=1e8 for BTC)
### iqfeed_adapter.cpp      [TODO] - IQFeed TCP protocol 6.2, port 5009, aggressor field
### kinetick_receiver.cpp   [TODO] - Kinetick NT bridge TCP receiver
### feed_manager.cpp        [TODO] - Multi-provider management, failover logic

## ═══════════════════════════════════════
## PHASE 4b — NT8 TOOLS (tools/ + nt8/)
## ═══════════════════════════════════════

### tools/btc_live_analytics.cpp    [DONE — Session 5] - Coinbase → BarEngine+DeltaEngine+VwapEngine
#                                    TcpBroadcaster on TCP:9000, newline-delimited JSON
#                                    Messages: tick | bar | shutdown
### tools/nt_bridge_server.cpp      [DONE — Session 5] - Full-pipeline bridge §AGT-06
#                                    Engines: Bar+Delta+Vwap+VolumeProfile+Imbalance+Signal
#                                    Protocol: TCP:7777, 4-byte LE length-prefix + JSON
#                                    NT→engine: subscribe; engine→NT: bar_close, vwap, signal
### nt8/OFEAnalytics.cs             [DONE — Session 5] - NT8 simple VWAP/bands overlay (port 9000)
### nt8/nt_adapter/OFEMessageTypes.cs     [DONE — Session 5] - OfeBarCloseMsg/OfeVwapMsg/OfeSignalMsg + manual JSON parser
### nt8/nt_adapter/OFEEngineClient.cs     [DONE — Session 5] - TCP client, length-prefix framing, auto-reconnect
### nt8/nt_adapter/OFEFootprintIndicator.cs [DONE — Session 5] - NT8 NinjaIndicator, bar cache (max 500 bars)
#                                    NOTE: requires nt_bridge_server running on port 7777. Will NOT display
#                                    if no server is running. Superseded by OFEFootprintNT.cs for native feeds.
### nt8/nt_adapter/FootprintRenderer.cs   [DONE — Session 5] - SharpDX cell grid, colour priority, POC/COT
### nt8/nt_adapter/OverlayRenderer.cs     [DONE — Session 5] - VWAP + ±1σ/±2σ bands + VP histogram
### nt8/nt_adapter/DeltaPanelRenderer.cs  [DONE — Session 5] - Delta histogram + CVD polyline sub-panel

### nt8/nt_adapter/OFECoinbaseChart.cs    [DONE — Session 6] - Standalone chart panel (IsOverlay=false)
#                                    Draws its own candles from bridge server data. No NT8 bar dependency.
#                                    Status: Phase 1 only (candles+VWAP+status bar). Not recommended.
### nt8/nt_adapter/CoinbaseWebSocketClient.cs [DONE — Session 6] - Pure C# Coinbase WS client
#                                    NOTE: NOT needed if using NT8 native Coinbase connection.
#                                    Only relevant if zero NT8 data providers (edge case).
### nt8/nt_adapter/CoinbaseNTFeed.cs      [DONE — Session 6] - NT8 Connection subclass + AddOn
#                                    NOTE: NOT needed if using NT8 native Coinbase connection.
#                                    Only relevant if zero NT8 data providers (edge case).

### nt8/nt_adapter/OFEFootprintNT.cs      [DONE — Session 7] ★ PRIMARY INDICATOR ★
#                                    Self-contained footprint indicator — NO C++ server required.
#                                    Works with ANY NT8 data provider: Schwab, Coinbase, IQFeed, Kinetick, eSignal.
#                                    OnMarketData() receives raw ticks from NT8's native feed.
#                                    Lee-Ready classification (buy vs sell) computed in C#.
#                                    Accumulates bid/ask vol per price level per bar (Dictionary<long, BarLevel>).
#                                    Bar-close: computes POC, barDelta, CVD, diagonal imbalances, stacked imbalances.
#                                    _barCache keyed by NT8 bar index (int) — NO timestamp alignment issues.
#                                    OnRender(): SharpDX cells (bid=left, ask=right), POC highlight, delta label.
#                                    Status bar: "OFE Footprint │ {instrument} │ Bars: N │ CVD: ±N"
#                                    Properties: CellFontSize, ShowDeltaLabel, ShowPoc, ImbalanceRatio,
#                                                StackedThreshold, BuyColor, SellColor, all imbalance colours.
#                                    INSTALL: Copy only OFEFootprintNT.cs → NT8 Custom\Indicators\ → Ctrl+F5

## ═══════════════════════════════════════
## PHASE 5 — LICENSE (src/license/)
## ═══════════════════════════════════════

### hw_fingerprint.cpp      [TODO] - CPU ID + MAC + disk serial + OS install ID + SHA-256
### license_engine.cpp      [TODO] - RSA-2048 validation + symbol count enforcement + anti-tamper

## ═══════════════════════════════════════
## PHASE 6 — UNIT TESTS (test/)
## ═══════════════════════════════════════

### test_tick_record.cpp    [DONE] - 22 tests: size, factory, accumulatable, quote, exclude
### test_ring_buffer.cpp    [DONE] - 12 tests: push/pop, overflow, FIFO, concurrent, wrap-around
### test_lee_ready.cpp      [DONE] - 22 tests: all 5 rules, IQFeed override, state updates, edge cases
#                                    Total: 56 tests passing
### test_bar_engine.cpp     [DONE] - 17 tests: RANGE/VOLUME/TIME bars, OHLCV, POC, sorting, callbacks
### test_delta_engine.cpp   [DONE] - 14 tests: all 5 formulas, CVD, EMA seed/update, session reset
### test_imbalance_detector.cpp [DONE] - 13 tests: single/stacked/absorption/trapped/zone strength
### test_volume_profile.cpp [DONE] - 14 tests: POC, VA two-level, shape D/P/b/Thin, session reset
### test_vwap_engine.cpp    [DONE] - 15 tests: VWAP incremental, StdDev bands, signals, anchored
### test_signal_detector.cpp[DONE] - 12 tests: COT, Ratio, SinglePrint, ZeroPrint, Pulse, symbol_id
#                                    Total: 154 tests passing (was 56) — zero regressions

## ═══════════════════════════════════════
## KNOWN BUGS (unfixed)
## ═══════════════════════════════════════
# None — all known bugs resolved as of Session 3.
# compute_poc_score in signal_detector.cpp returns hardcoded 50.0f pending spec clarification.

## ═══════════════════════════════════════
## SESSION LOG
## ═══════════════════════════════════════
# Session 1: COMPLETED
#   - logger.h/cpp (util) — 5-level async rotating logger with file+console
#   - tick_record.cpp — factory + filter + trace logging
#   - ring_buffer.cpp — lock-free SPSC template impl
#   - tick_router.cpp — MPMC routing + INFO/WARN/TRACE logging
#   - lee_ready.cpp — Quote Rule + Tick Rule + IQFeed aggressor
#   - bar_types.cpp — BarSize, PriceLevelRecord, BarRecord helpers
#   - bar_engine.cpp — BarSeries + BarEngine + DEBUG/TRACE logging
#   - delta_engine.cpp — all 5 delta formulas + signals + DEBUG/INFO logging
#   - imbalance_detector.cpp — single+stacked+absorption+trapped + INFO logging
#   - test_tick_record.cpp — 22 unit tests
#   - test_ring_buffer.cpp — 12 unit tests (incl. 100K concurrent test)
#   - test_lee_ready.cpp — 22 unit tests (all classification cases)
#   - Applied Part B fixes PB-01..PB-09
#
# Session 2: AUDIT of pre-existing stubs added to repo
#   - symbol_worker.cpp: found COMPLETE — full 11-stage pipeline, lifecycle, bar-close, session mgmt
#   - event_bus_redis.cpp: found COMPLETE as InProcessEventBus (in-memory); Redis backend future work
#   - volume_profile.cpp: found PARTIAL — on_tick/POC/VA/HVN/LVN done; VA is 1-level (bug); shape stub
#   - vwap_engine.cpp: found PARTIAL — incremental VWAP fully done; detect_signals is empty stub
#   - signal_detector.cpp: found SCAFFOLD — dispatcher + compute_cot + helpers done; all detectors stubs
#   - Discovered PB-10: {:#x} in LOG macros (signal_detector:68, event_bus_redis:86, symbol_worker:101)
#
# Session 3: COMPLETED
#   - Fixed PB-10: {:#x} → {:x} in signal_detector.cpp:68, event_bus_redis.cpp:86, symbol_worker.cpp:101
#   - volume_profile.cpp: two-level VA expansion (NinjaTrader §12 4.2) + classify_shape (D/b/P/Thin)
#   - vwap_engine.cpp: detect_signals (REACTION_LONG/SHORT + ROTATION_LONG/SHORT, §13 5.4)
#   - signal_detector.cpp: ALL detectors implemented with spec-referenced inline comments:
#     detect_pulse (§15), detect_turns (§16), detect_ratio (§17), detect_single_prints (§18),
#     check_magnet_resolutions, detect_market_sweep (§20), detect_poc_slingshot (§22),
#     detect_zero_prints, detect_exhaustion_prints, detect_sequencing, detect_volume_decline,
#     detect_accumulation_distribution
#   - Added make_signal() helper to SignalDetector (header + impl)
#   - 56/56 tests passing — zero regressions
#
# Session 4: COMPLETED (AGT-10)
#   - test_bar_engine.cpp      (17 tests: RANGE/VOLUME/TIME, OHLCV, POC, callbacks)
#   - test_delta_engine.cpp    (14 tests: 5 formulas, CVD, EMA, session reset)
#   - test_imbalance_detector.cpp (13 tests: single/stacked/absorption/trapped/zone)
#   - test_volume_profile.cpp  (14 tests: POC, VA two-level, D/P/b/Thin, session)
#   - test_vwap_engine.cpp     (15 tests: VWAP, bands, REACTION/ROTATION signals, anchored)
#   - test_signal_detector.cpp (12 tests: COT, Ratio, SinglePrint, ZeroPrint, Pulse)
#   - All 154 tests passing — zero regressions
#   NOTE: VolumeProfileEngine.previous_poc_ only saved via cached_poc_ (never populated
#         by compute_poc()) — known gap; on_session_open() saves 0 unless get_session_snapshot()
#         is called first. Workaround: SymbolWorker calls get_session_snapshot() each bar close.
#
#
# Session 5: COMPLETED
#   - coinbase_adapter.cpp — Coinbase Advanced Trade WebSocket, PIMPL, JWT ES256 auth
#   - include/feed/coinbase_adapter.h — CoinbaseAdapter + CoinbaseAdapterConfig struct
#   - tools/btc_live_analytics.cpp — Coinbase → 3 engines → TCP:9000 newline-JSON broadcast
#   - nt8/OFEAnalytics.cs — simple NT8 VWAP/delta overlay on port 9000
#   - tools/nt_bridge_server.cpp — full-pipeline: 6 engines → TCP:7777 length-prefix JSON
#     Wires: BarEngine + DeltaEngine + VwapEngine + VolumeProfileEngine + ImbalanceDetector + SignalDetector
#     Sends: bar_close (full price_levels[] + imbalance/stacked/zero-print flags)
#            vwap (10Hz rate-limited), signal (immediate on detection)
#     Reads: subscribe frames from NT8 clients
#   - nt8/nt_adapter/OFEMessageTypes.cs — message structs + manual JSON parser
#   - nt8/nt_adapter/OFEEngineClient.cs — TCP client, length-prefix framing, auto-reconnect
#   - nt8/nt_adapter/OFEFootprintIndicator.cs — NT8 NinjaIndicator, bar cache keyed by ts_seconds
#   - nt8/nt_adapter/FootprintRenderer.cs — SharpDX cell grid, colour priority Z-order
#   - nt8/nt_adapter/OverlayRenderer.cs — VWAP daily + ±1σ/±2σ bands + VP histogram
#   - nt8/nt_adapter/DeltaPanelRenderer.cs — delta bar histogram + CVD polyline
#   - CMakeLists.txt: added nt_bridge_server target
#   - 154/154 tests still passing — zero regressions
#   KEY BUG (avoid): SignalDetector::push_bar_history() is private.
#                    detect_all() calls it internally. Do NOT call it externally.
#
# Session 6: PARTIAL (debug + standalone chart attempt)
#   CONTEXT: User reported OFEFootprintIndicator not displaying on NT8 charts.
#   ROOT CAUSE FOUND: OFEFootprintIndicator depends on nt_bridge_server (port 7777).
#                     If server not running, indicator shows nothing. No self-diagnostic.
#   FILES ADDED (optional/superseded):
#   - nt8/nt_adapter/OFECoinbaseChart.cs — standalone chart panel (Phase 1: candles+VWAP)
#     IsOverlay=false, own sub-panel, reads from bridge server not NT8 bar data.
#     NOT the recommended approach — superseded by OFEFootprintNT.cs.
#   - nt8/nt_adapter/CoinbaseWebSocketClient.cs — pure C# Coinbase WS client (no NT8 deps)
#     NOT needed if NT8 already has Coinbase connection.
#   - nt8/nt_adapter/CoinbaseNTFeed.cs — NT8 Connection subclass + AddOn lifecycle
#     NOT needed if NT8 already has Coinbase connection.
#   - nt8/nt_adapter/OFEFootprintIndicator.cs — added diagnostics (Print on renders 1/10/50)
#   - nt8/nt_adapter/FootprintRenderer.cs — added null guards for BarsArray.Length
#
# Session 7: COMPLETED — ★ CORRECT APPROACH ★
#   CONTEXT: User confirmed: (a) Schwab data → NT8 charts working; (b) Coinbase → NT8 working.
#            No C++ bridge server needed for data. OFE indicator must read NT8's own tick data.
#   ARCHITECTURE DECISION:
#     ANY data provider (Schwab/Coinbase/IQFeed/Kinetick/eSignal) → NT8 tick engine
#     → OFEFootprintNT reads OnMarketData() ticks → builds footprint in-memory → OnRender()
#     No C++ server, no TCP, no external dependencies.
#   FILE ADDED:
#   - nt8/nt_adapter/OFEFootprintNT.cs ★ PRIMARY NT8 FOOTPRINT INDICATOR ★
#     Single self-contained file. Install: copy to NT8 Custom\Indicators\ → Ctrl+F5 → drag onto chart.
#     OnMarketData(): track Bid/Ask, classify Last ticks Lee-Ready, accumulate per price level per bar.
#     Bar-close: POC, barDelta, CVD, diagonal imbalances (ask[P] >= ratio × bid[P-1tick]), stacked.
#     _barCache[barIndex] = BarSnapshot — keyed by NT8 bar index, no timestamp alignment needed.
#     OnRender(): SharpDX left(bid)/right(ask) cells, POC highlight, delta label, status bar.
#   154/154 C++ tests unaffected.
