# AGT-03 — Core Engine Agent
# Role: Implement C++ core infrastructure (tick pipeline, bar engine, symbol worker)
# When to use: After GATE-02 header approval
# Input: Approved headers + EPS §4 (data flow) + EPS §5 (performance requirements)
# Output: All src/core/*.cpp files

## HOW TO USE THIS PROMPT
## CRITICAL: Read PROGRESS.md FIRST to see what is already done

---

You are AGT-03, the Core Engine Agent for the Order Flow Engine project.

## FIRST ACTION — READ PROGRESS
Before writing a single line of code:
1. Read PROGRESS.md in the repo root
2. Identify which files are [DONE], [WIP], [NEXT]
3. Start ONLY from the [NEXT] file — never re-implement [DONE] files

## SESSION 1 STATUS (already done — DO NOT RE-IMPLEMENT)
- src/core/tick_record.cpp   [DONE] — factory + should_exclude_tick + TRACE logging
- src/core/ring_buffer.cpp   [DONE] — lock-free SPSC template impl + instantiations (64/256/16384)
- src/core/tick_router.cpp   [DONE] — MPMC routing + INFO/WARN/TRACE logging
- src/core/lee_ready.cpp     [DONE] — Quote Rule + Tick Rule + IQFeed override + classify_inplace
- src/core/bar_types.cpp     [DONE] — BarSize, PriceLevelRecord, BarRecord helpers
- src/core/bar_engine.cpp    [DONE] — BarSeries(TIME/RANGE/VOLUME) + BarEngine + DEBUG/TRACE logging
- src/util/logger.cpp        [DONE] — Async rotating logger, 5 levels, file+console, ANSI colour

## ALL CORE FILES STATUS (Session 2 audit + Session 3 confirmation)

### src/core/symbol_worker.cpp   [DONE]
Full 11-stage tick pipeline per run_loop() spec. Lifecycle: start/stop/pause/resume.
on_bar_close(): OHLCV enrichment, signal dispatch, delta snapshot publication.
on_session_open/close(): resets DeltaEngine, VwapEngine, VolumeProfileEngine, SignalDetector.
Logging: INFO on lifecycle events, TRACE on each tick stage (compiled away in Release).

### src/core/event_bus_redis.cpp [DONE]
Implements InProcessEventBus using in-memory std::deque queues.
NOTE: File is named redis.cpp but the live implementation is InProcessEventBus, not Redis.
Redis Streams backend is future work tracked as AGT-03 follow-up.

## FUTURE WORK (NOT yet implemented)
- Redis Streams backend for EventBus (replace InProcessEventBus)
- eSignal feed adapter (Phase 2)

## IMPLEMENTATION RULES

### Performance Requirements (EPS §5)
- Stages 1-9 (tick processing): < 30 µs P99
- No mutex, condition variable, sleep, or system call on stages 1-9
- No heap allocation on the hot tick path (except on bar open — amortised)

### Logging Requirements (from discussion)
Every method must log at the appropriate level using util/logger.h:
- TRACE: per-tick events (hot path — compiled away in Release via NDEBUG)
- DEBUG: per-bar events (bar open, bar close with OHLCV + POC + levels count)
- INFO:  lifecycle events (symbol activated, session started, worker started)
- WARN:  recoverable anomalies (tick dropped, late tick corrected, ring buffer full)
- ERROR: failures (exception caught, state corruption detected)

Use macros: LOG_TRACE, LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR
NOTE: Macro parameter 'module' is the first arg, format string second.
The OFE_LOG macro uses std::string(ofe_mod) internally to prevent
preprocessor substitution issues with string literals.

### Symbol Worker run_loop() — 11-Stage Pipeline
Implement exactly in this order per SymbolWorker header:
1.  Dequeue tick from ring buffer (try_pop)
2.  Update quote state (if BID_QUOTE or ASK_QUOTE)
3.  Lee-Ready classify (if TRADE)
4.  should_exclude_tick check
5.  Bar accumulation (BarEngine::on_tick)
6.  Delta update (DeltaEngine::on_tick)
7.  VWAP update (VwapEngine::on_tick)
8.  Volume Profile update (VolumeProfileEngine::on_tick)
9.  Tick-level signal check (Turns, VWAP Reaction)
10. Bar close check → if closing: run_bar_close_detectors()
11. Publish signals to EventBus

### Error Handling (EPS §6)
- Catch ALL exceptions in run_loop() — never let worker thread crash
- On exception: LOG_ERROR with context, set status=ERROR, attempt restart after 5s
- Never throw from noexcept methods

### Session Management
- Call on_session_open() on all sub-engines when session opens
- Call on_session_close() on all sub-engines at session close
- CVD resets at session_open per formula spec §10.2.4

## OUTPUT
After each file: update PROGRESS.md with [DONE] status
