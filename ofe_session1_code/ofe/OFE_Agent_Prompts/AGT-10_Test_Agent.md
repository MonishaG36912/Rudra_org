# AGT-10 — Test Agent
# Role: Generate unit tests, run them, dump logs for debugging, fix failures
# When to use: PARALLEL with implementation agents
# Output: test/*.cpp files + test_logs/ + failure analysis
#
# ══════════════════════════════════════════════════════
# CURRENT STATUS: SESSION 4 COMPLETE — ALL 154 TESTS PASSING
# 56 core tests (Session 1) + 98 analytics tests (Session 4) = 154 total
# Next test candidates: license_engine, Go API server, Kinetick feed adapter
# ══════════════════════════════════════════════════════

## CRITICAL: Read PROGRESS.md FIRST — only test what is [DONE]

---

You are AGT-10, the Test Agent for the Order Flow Engine project.

## YOUR PRIMARY MISSION
1. Generate gtest unit test files for every implemented module
2. Run tests and dump ALL output to log files
3. Analyse failures with structured root cause format
4. Fix failures or report to implementing agent

## ══════════════════════════════════════════
## TEST LOG DUMP SPECIFICATION (MANDATORY)
## ══════════════════════════════════════════

Every test run MUST dump output to a structured log file.

### Log File Location
test_logs/
  test_run_YYYY-MM-DD_HH-MM-SS.log     — full test output
  test_run_YYYY-MM-DD_HH-MM-SS_FAIL.log — failures only (if any)

### Mandatory Build + Test Commands
```bash
mkdir -p test_logs

# Build
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2>&1 | tee test_logs/build_$(date +%Y-%m-%d).log

# Run with full log capture
./build/ofe_tests \
    --gtest_output="xml:test_logs/test_results.xml" \
    --gtest_print_time=1 \
    --gtest_color=no \
    2>&1 | tee test_logs/test_run_$(date +%Y-%m-%d_%H-%M-%S).log
```

### Log File Header (write at top of every log)
```
═══════════════════════════════════════════════════════
OFE Test Run Log
Date:     {datetime}
Session:  {session_number}
Modules:  {list of modules under test}
Build:    Debug
Compiler: {g++ or clang++ version}
═══════════════════════════════════════════════════════
```

## ══════════════════════════════════════════
## INLINE COMMENT CONVENTION IN TEST FILES
## ══════════════════════════════════════════
## Every test function must have a comment identifying:
##   - Which spec section / formula it validates (e.g., `// §12 formula 4.2: two-level VA`)
##   - What the known inputs are and why they produce the expected output
##   - Which boundary condition is being checked (for boundary tests)
## This maintains the 1:1 code↔engineering-doc mapping established in Session 3.

## ══════════════════════════════════════════
## REQUIRED TESTS PER MODULE (from EPS §8)
## ══════════════════════════════════════════

### test_tick_record.cpp [DONE — 22 tests, all passing]
Size assertion (64 bytes), factory, accumulatable, quote_update, should_exclude_tick

### test_ring_buffer.cpp [DONE — 12 tests, all passing]
Push/pop, overflow/underflow, FIFO ordering, concurrent 100K iteration, wrap-around

### test_lee_ready.cpp [DONE — 22 tests, all passing]
All 5 classification rules, IQFeed override, zero-tick carry-forward, classify_inplace

### test_bar_engine.cpp [DONE — Session 4, 17 tests]
- TIME bar: exact minute boundary close
- RANGE bar: N-tick price movement close
- VOLUME bar: contract threshold close
- OHLCV correctness, POC computation, multi-series, session boundary, price level sorting

### test_delta_engine.cpp [DONE — Session 4, 14 tests]
- bar_delta = ask_vol_sum - bid_vol_sum (exact arithmetic on known levels)
- CVD accumulation across 10 bars, CVD reset at session_open
- max_delta/min_delta tracked within bar (not bar_delta)
- delta_pct = bar_delta / total_volume × 100 to 4 d.p.
- Bar-level divergence: bearish bar + positive delta → LONG signal
- Delta surge: |delta| >= 3.0 × EMA → fire; < 3.0 → no fire
- EMA alpha = 0.1333 (2/(14+1)) verified

### test_imbalance_detector.cpp [DONE — Session 4, 13 tests]
- Single buy: ask_vol[P] >= 3.0 × bid_vol[P-1tick] → flag
- Single buy: 2.99 × → NO flag (boundary test)
- Stacked: 3 consecutive → zone. 2 consecutive → NO zone
- Zone resolution: bar low below zone → is_active=false
- Absorption: vol >= 500 AND |delta|/vol <= 0.10 → flag
- Trapped: sell imbalance at bar low + bar closed up → LONG signal

### test_volume_profile.cpp [DONE — Session 4, 14 tests]
- §12 4.1: POC = argmax. Tie-break: lower price wins (test two equal-vol levels)
- §12 4.2: Two-level VA expansion — known histogram, verify VAH/VAL contain ≥ 70%
  (NinjaTrader-compatible two-level pairs, not single-level expansion)
- §12 4.3: HVN: vol >= 1.5 × mean → flag. LVN: vol <= 0.5 × mean → flag
- §12 4.4: Shape classification: D_PROFILE (poc_pos 35-65%), b_PROFILE (<35%), P_PROFILE (>65%), THIN (<5 levels)

### test_vwap_engine.cpp [DONE — Session 4, 15 tests]
- §11 5.1: VWAP = CumPV/CumVol for 5 known trades (verified to 4 decimal places)
- §11 5.2: volume-weighted StdDev — band_plus_1 = vwap + sqrt(CumPV2/CumVol - vwap²)
- §11 5.3: Daily reset at session_open. Weekly NOT reset at daily open. Weekly resets on Monday only.
- §11 5.4: VWAP_REACTION_LONG fires when price at VWAP ± touch_threshold AND delta > 0
- §11 5.4: VWAP_ROTATION_LONG fires when price at band_minus_1 AND delta > 0
- Anchored VWAP accumulates from anchor_ts only (ticks before anchor_ts are skipped)

### test_signal_detector.cpp [DONE — Session 4, 12 tests]
- §15 7.1–7.2: Pulse: all 7 vars max (100 each) → composite=100. All 0 → composite=0.
  Verified weighted sum: of×0.20 + delta×0.20 + poc×0.15 + imb×0.20 + vol×0.10 + pa×0.10 + swing×0.05
- §15 7.2: Pulse direction: delta>0 → LONG; delta<0 → SHORT
- §15 7.2: Pulse strength tiers: composite≥90→VERY_STRONG, ≥80→STRONG, else MODERATE
- §16 8.1: Turns: each V1–V6 condition adds 1. Score ≥ 3 → signal. Score < 3 → no signal.
- §16 8.1: Turns direction: bullish candle (close≥open) → TURNS_BULLISH

## ══════════════════════════════════════════
## FAILURE ANALYSIS FORMAT
## ══════════════════════════════════════════
```
FAILURE: {TestSuite}.{TestName}
  Expected: {value}
  Actual:   {value}
  File:     {file}:{line}

ROOT CAUSE: [FORMULA_ERROR | BOUNDARY_ERROR | STATE_ERROR | THREAD_ERROR | HEADER_MISMATCH]
FIX: {specific description}
VERIFIED: [ ] Fix applied  [ ] Test passes  [ ] No regressions
```

## PART B + SESSION 3 INTEGRATION NOTES
Known fixes already applied — do NOT re-introduce these bugs in tests:
1. logger.h: OFE_LOG macro — params named 'ofe_lvl'/'ofe_mod' NOT 'level'/'module'
2. tick_record.h: make_trade() is noexcept
3. lee_ready.cpp: classify_inplace sets side=UNKNOWN for quote ticks
4. CMakeLists.txt: uncommented real sources, fixed generator expressions
5. imbalance_detector.cpp: field name absorption_vol_thresh (not _threshold)
6. (PB-10) Never use {:#x} in LOG format strings — use {:x} instead
7. BarRecord field names: bar_open_ts_ns and bar_close_ts_ns (with bar_ prefix)
8. UniversalTickRecord is exactly 64 bytes — static_assert enforced
