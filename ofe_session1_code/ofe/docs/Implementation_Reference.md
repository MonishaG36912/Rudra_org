# OFE Implementation Reference
# Spec-to-Code Traceability Document
# Last updated: Session 3 (2026-06-19)
#
# PURPOSE
# -------
# This document is the living bridge between the engineering PDFs and the C++ source code.
# Every inline comment written in the source files (§-references, step labels, variable
# explanations) traces back to a row in this document.
#
# HOW TO USE
# ----------
# 1. New engineer onboarding: read a spec PDF section, then find its row here to jump
#    directly to the implementing file and function.
# 2. Code review: cross-check that implementation matches spec; deviations are listed
#    explicitly in the DEVIATION column.
# 3. Spec updates: when a PDF is revised, this document flags which functions must change.
#
# SOURCE DOCUMENTS
# ----------------
#   [IF] OrderFlow_Indicator_Formulas.pdf  (29 pages, v1.0)
#   [EPS] OFE_Engineering_Product_Spec.pdf  (9 pages, v1.1)
#   [SWS] OrderFlow_Software_Specification_v1.pdf
#   [V4]  OrderFlow_Spec_v4_Master.pdf
#
# COLUMN LEGEND
# -------------
#   Spec §     : Section in the source PDF
#   Status     : MATCHES | PARTIAL | DEVIATION | PENDING | STUB
#   File       : C++ source file (relative to ofe_session1_code/ofe/)
#   Function   : Implementing function name
#   Notes      : Implementation decisions, constraints, or gaps not visible in spec

---

## ══════════════════════════════════════════════════════════════════
## PART 1 — INDICATOR FORMULAS  [IF]
## Source: OrderFlow_Indicator_Formulas.pdf
## ══════════════════════════════════════════════════════════════════

### §1 — Tick Classification (Lee-Ready Algorithm)  [IF pp.2–3]

| Spec § | Status | File | Function | Notes |
|--------|--------|------|----------|-------|
| §1.1 Quote Rule | MATCHES | src/core/lee_ready.cpp | `classify()` | P_t > M_t → ASK; P_t < M_t → BID; P_t = M_t → tick rule |
| §1.1 Tick Rule | MATCHES | src/core/lee_ready.cpp | `classify()` | P_t > P_{t-1} → ASK; P_t < P_{t-1} → BID; equal → carry-forward LAST |
| §1.1 IQFeed Override | MATCHES | src/core/lee_ready.cpp | `classify_inplace()` | aggressor=1→ASK, 2→BID, 0→apply Lee-Ready. CME/ICE only. |
| §1.1 Quote tick side | MATCHES | src/core/lee_ready.cpp | `classify_inplace()` | Quote ticks (BID_QUOTE/ASK_QUOTE) set side=UNKNOWN — do not participate in classification |
| §1 Volume buckets | MATCHES | src/core/bar_engine.cpp | `on_tick()` | ask_vol[price]+=vol if ASK; bid_vol[price]+=vol if BID; total_vol always += vol |

---

### §2 — Delta Calculations  [IF pp.4–6]

| Spec § | Status | File | Function | Notes |
|--------|--------|------|----------|-------|
| §2.1 Bar Delta | MATCHES | src/analytics/delta_engine.cpp | `on_tick()` | bar_delta = SUM(ask_vol) - SUM(bid_vol) across all price levels in bar |
| §2.2 Max/Min Delta | MATCHES | src/analytics/delta_engine.cpp | `on_tick()` | max_delta = MAX(bar_delta per bar); min_delta = MIN(bar_delta per bar) — tracked within session |
| §2.3 Delta Percentage | MATCHES | src/analytics/delta_engine.cpp | `on_bar_close()` | delta_pct = bar_delta / total_volume × 100 |
| §2.4 CVD | MATCHES | src/analytics/delta_engine.cpp | `on_bar_close()` + `on_session_open()` | cumulative_delta += bar_delta each bar; resets to 0 at RTH session_open (NOT at midnight) |
| §2.5 Delta Divergence | MATCHES | src/analytics/delta_engine.cpp | `detect_divergence()` | Bearish bar (close < open) + positive delta → LONG signal. Bullish bar + negative delta → SHORT. |
| §2.5 Delta Surge | MATCHES | src/analytics/delta_engine.cpp | `detect_surge()` | \|bar_delta\| / EMA(\|bar_delta\|, 14) >= 3.0 → surge. EMA alpha = 2/(14+1) = 0.1333. |

**Implementation notes:**
- EMA seeded with first bar's \|bar_delta\|; no warm-up period required by spec — immediate from bar 1.
- CVD reset tied to `on_session_open()` call from SymbolWorker — not automatic on wall-clock.

---

### §3 — Imbalance Detection  [IF pp.7–9]

| Spec § | Status | File | Function | Notes |
|--------|--------|------|----------|-------|
| §3.1 Single Buy Imbalance | MATCHES | src/analytics/imbalance_detector.cpp | `scan_bar()` | ask_vol[P] >= ratio × bid_vol[P - 1_tick] (DIAGONAL comparison — ask at P vs bid one tick below) |
| §3.1 Single Sell Imbalance | MATCHES | src/analytics/imbalance_detector.cpp | `scan_bar()` | bid_vol[P] >= ratio × ask_vol[P + 1_tick] |
| §3.1 Ratio default | MATCHES | include/analytics/imbalance_detector.h | `ImbalanceConfig` | ratio_threshold default = 3.0 (300%) |
| §3.2 Stacked Imbalance | MATCHES | src/analytics/imbalance_detector.cpp | `scan_bar()` | 3 consecutive imbalance cells (same direction) → zone created; 2 → no zone |
| §3.3 Imbalance Ratio | MATCHES | src/analytics/imbalance_detector.cpp | `scan_bar()` | Ratio stored per cell; used for display and scoring |
| §3.4 Absorption | MATCHES | src/analytics/imbalance_detector.cpp | `detect_absorption()` | BarVolume >= 500 AND \|bar_delta\|/BarVolume <= 0.10 → absorption flag |

**Implementation notes:**
- Field name: `absorption_vol_thresh` (not `absorption_vol_threshold`) — confirmed in Part B fix PB-05.
- Trapped traders signal (sell imbalance at bar low + bar closed up → LONG) is in `detect_trapped()`.

---

### §4 — Volume Profile Calculations  [IF pp.10–12]

| Spec § | Status | File | Function | Notes |
|--------|--------|------|----------|-------|
| §4.1 Profile Construction | MATCHES | src/analytics/volume_profile.cpp | `on_tick()` | price_bucket = ROUND(price / tick_size) × tick_size; profile[bucket] += volume. O(1) hash table. |
| §4.2 POC | MATCHES | src/analytics/volume_profile.cpp | `compute_poc()` | argmax(profile[price]); tie-break: lower price wins (conservative, closer to value) |
| §4.3 Value Area | MATCHES | src/analytics/volume_profile.cpp | `compute_value_area()` | Two-level expansion: compare upper_2 = vol[hi+1]+vol[hi+2] vs lower_2 = vol[lo-1]+vol[lo-2]; expand toward larger pair |
| §4.3 VA edge case | PARTIAL | src/analytics/volume_profile.cpp | `compute_value_area()` | If winning side has only 1 level available (not 2), add just that 1 level — spec's worked example assumes 2 always available |
| §4.4 HVN | MATCHES | src/analytics/volume_profile.cpp | `compute_hvn_lvn()` | HVN[price] = TRUE if profile[price] >= HVNMultiplier × mean_vol (default 1.5) |
| §4.5 LVN | MATCHES | src/analytics/volume_profile.cpp | `compute_hvn_lvn()` | LVN[price] = TRUE if profile[price] <= LVNMultiplier × mean_vol (default 0.5) |
| §4.6 Shape Classification | **DEVIATION** | src/analytics/volume_profile.cpp | `classify_shape()` | See deviation note below |

**⚠ DEVIATION — §4.6 Shape Classification:**

The spec (§4.6) defines shape using a **statistical formula**:
```
profile_stdev = SQRT( SUM((price[i] - mean)^2 × profile[i]) / TotalVol )
poc_percentile = (POC - ProfileLow) / (ProfileHigh - ProfileLow)
tail_pct_lower = volume below (POC - 1.5×stdev) / TotalVol
tail_pct_upper = volume above (POC + 1.5×stdev) / TotalVol
D-Profile: poc_percentile in [0.35, 0.65] AND stdev < threshold
P-Profile: poc_percentile > 0.60 AND tail_pct_lower > 0.15
b-Profile: poc_percentile < 0.40 AND tail_pct_upper > 0.15
```

The implementation uses a **simplified positional formula** (no stdev, no tail percentages):
```cpp
poc_pos = (poc_price - session_low) / (session_high - session_low)
poc_pos in [0.35, 0.65] → D_PROFILE
poc_pos < 0.35           → b_PROFILE  (bullish: body at bottom)
poc_pos > 0.65           → P_PROFILE  (bearish: body at top)
< 5 price levels         → THIN_PROFILE
```

**Why simplified:** The positional method matches NinjaTrader's visual classification and is faster (O(1) vs O(n)). The tail_pct thresholds in the spec require per-session stdev computation which adds latency on bar close.

**Action required:** Confirm with product owner whether the statistical formula (spec) or positional formula (code) is the required behavior. If statistical is required, this function must be rewritten.

**Implementation note:** Anchored profile accumulation is a stub — `add_anchored_profile()` and `remove_anchored_profile()` are no-ops. Future work.

---

### §5 — VWAP  [IF pp.13–15]

| Spec § | Status | File | Function | Notes |
|--------|--------|------|----------|-------|
| §5.1 Core VWAP | MATCHES | src/analytics/vwap_engine.cpp | `on_tick()` | CumPV += P×V; CumPV2 += P²×V; CumVol += V; VWAP = CumPV/CumVol. NEVER recomputes from scratch. |
| §5.2 Anchor Types | MATCHES | src/analytics/vwap_engine.cpp | `add_anchored_vwap()` | daily/weekly/monthly/yearly series + arbitrary anchor_ts_ns. Stored in anchored_series_ map. |
| §5.3 StdDev Bands | MATCHES | src/analytics/vwap_engine.cpp | `recompute()` | Var = CumPV2/CumVol - VWAP²; StdDev = sqrt(Var); bands at ±1/2/3σ |
| §5.4 VWAP Reaction Long | MATCHES | src/analytics/vwap_engine.cpp | `detect_signals()` | price within touch_threshold_ticks of daily_vwap AND bar_delta > 0 |
| §5.4 VWAP Reaction Short | MATCHES | src/analytics/vwap_engine.cpp | `detect_signals()` | price within touch_threshold_ticks of daily_vwap AND bar_delta < 0 |
| §5.4 VWAP Rotation Long | MATCHES | src/analytics/vwap_engine.cpp | `detect_signals()` | price at band_minus_1 (−1σ) AND bar_delta > 0 |
| §5.4 VWAP Rotation Short | MATCHES | src/analytics/vwap_engine.cpp | `detect_signals()` | price at band_plus_1 (+1σ) AND bar_delta < 0 |

**Implementation notes:**
- Session reset: `on_session_open()` resets daily_series_ only. Weekly checked in `check_weekly_reset()` (Monday only). Yearly in `check_yearly_reset()` (Jan 1 only). Monthly series is accumulated but no monthly reset implemented yet.
- Guard: `detect_signals()` returns empty if `!daily_series_.is_valid()` (needs at least one accumulated tick).
- Bar OHLC not available inside `detect_signals()`; price_at_signal and detection_ts_ns are populated; SymbolWorker enriches the OHLC fields after the call returns.

---

### §6 — Commitment of Traders Intrabar (COT)  [IF p.16]

| Spec § | Status | File | Function | Notes |
|--------|--------|------|----------|-------|
| §6.1 COT Price | MATCHES | src/analytics/signal_detector.cpp | `compute_cot()` | COT = argmax(total_vol[P]) within bar's price_levels. Tie-break: lower price. Sets bar.cot_price in-place. |

**Implementation notes:**
- `compute_cot()` is `static` — no instance state needed, pure computation on BarRecord.
- Called at the very start of `detect_all()` (step 3) so all subsequent detectors see bar.cot_price populated.

---

### §7 — Orderflows Pulse  [IF pp.17–19]

| Spec § | Status | File | Function | Notes |
|--------|--------|------|----------|-------|
| §7.1 Variable 1: OF Score | MATCHES | src/analytics/signal_detector.cpp | `compute_of_score()` | CLAMP(\|BarDelta\| / BarVolume × 200, 0, 100) |
| §7.1 Variable 2: Delta Score | MATCHES | src/analytics/signal_detector.cpp | `compute_delta_score()` | CLAMP(\|BarDelta\| / MaxDelta_session × 100, 0, 100) |
| §7.1 Variable 3: POC Score | **PENDING** | src/analytics/signal_detector.cpp | `compute_poc_score()` | **Hardcoded to 50.0f** — formula requires COT_price (bar.cot_price) vs BarClose. Waiting for spec §7.1 clarification on which COT price to use (bar COT vs session POC). |
| §7.1 Variable 4: Imbalance Score | MATCHES | src/analytics/signal_detector.cpp | `compute_imbalance_score()` | CLAMP(ImbalanceCount / 5 × 100, 0, 100); stacked bonus ×1.5 if stacked_count >= min |
| §7.1 Variable 5: Volume Score | MATCHES | src/analytics/signal_detector.cpp | `compute_volume_score()` | CLAMP(BarVolume / AvgBarVolume × 100, 0, 100). AvgBarVolume = cum_volume / bar_count |
| §7.1 Variable 6: Price Action Score | MATCHES | src/analytics/signal_detector.cpp | `compute_pa_score()` | BodyRatio = CandleBody/BarRange; PA_score = CLAMP(BodyRatio × (BarVol/AvgVol) × 100, 0, 100) |
| §7.1 Variable 7: Swing Score | MATCHES | src/analytics/signal_detector.cpp | `compute_swing_score()` | SwingPct = (Close - SwingLow_N)/(SwingHigh_N - SwingLow_N); LONG: score=SwingPct×100; SHORT: score=(1-SwingPct)×100 |
| §7.2 Composite Score | MATCHES | src/analytics/signal_detector.cpp | `detect_pulse()` | Weighted sum of 7 scores; weights: OF=0.20, D=0.20, POC=0.15, IMB=0.20, VOL=0.10, PA=0.10, SW=0.05 (sum=1.00) |
| §7.2 Signal threshold | MATCHES | src/analytics/signal_detector.cpp | `detect_pulse()` | PulseScore >= config_.pulse_score_threshold (default 70) → emit signal |
| §7.2 Direction | **PARTIAL** | src/analytics/signal_detector.cpp | `detect_pulse()` | Implementation: direction = sign(bar_delta). Spec requires NEUTRAL if delta and candle signs conflict. Neutral/no-emit path not yet implemented. |
| §7.2 Strength tiers | ADDED | src/analytics/signal_detector.cpp | `detect_pulse()` | composite >= 90 → VERY_STRONG; >= 80 → STRONG; else MODERATE. Not in spec — engineering addition for UI display. |

**⚠ PENDING — §7.1 Variable 3 (POC Score):**
- Spec formula: `POC_position = ABS(BarClose - COT_price) / (BarHigh - BarLow); POC_score = CLAMP((1 - POC_position) × 100, 0, 100)`
- Implementation: `return 50.0f` (hardcoded neutral)
- **Action required:** Clarify with product owner whether COT_price = bar.cot_price (intrabar POC) or session_poc (from VolumeProfileSnapshot). The `detect_pulse()` signature has access to both. Once confirmed, replace the hardcoded 50.0f with the formula.

**⚠ PARTIAL — §7.2 Direction (NEUTRAL case):**
- Spec: if delta > 0 but close < open (or vice versa), direction = NEUTRAL and no signal fires.
- Implementation: direction is determined solely from sign(bar_delta), ignoring candle direction conflict.
- **Action required:** Add the delta/candle conflict check before emitting the pulse signal.

---

### §8 — Orderflows Turns  [IF pp.20–21]

| Spec § | Status | File | Function | Notes |
|--------|--------|------|----------|-------|
| §8.1 V1 — POC Proximity | **DEVIATION** | src/analytics/signal_detector.cpp | `detect_turns()` | Spec: +1 if ABS(BarHigh - COT_price) <= 2_ticks. Code: +1 if level at extreme has high volume (>= 1.5× avg). See deviation note. |
| §8.1 V2 — Vol Exhaustion | **DEVIATION** | src/analytics/signal_detector.cpp | `detect_turns()` | Spec: +1 if ask_vol[BarHigh] < VolumeExhaustThreshold. Code: +1 if bar.total_volume < bar_history_.back().total_volume (declining bar volume). |
| §8.1 V3 — Sufficient Volume | MATCHES | src/analytics/signal_detector.cpp | `detect_turns()` | +1 if BarVolume >= turns_min_volume_ratio × AvgBarVolume (default 0.5×) |
| §8.1 V4 — Reversal Candle | MATCHES | src/analytics/signal_detector.cpp | `detect_turns()` | Bullish: close > open. Bearish: close < open. (Spec adds >50% of range condition — partially matched.) |
| §8.1 V5 — Delta Confirm | MATCHES | src/analytics/signal_detector.cpp | `detect_turns()` | Bullish: bar_delta > 0. Bearish: bar_delta < 0. |
| §8.1 V6 — Swing Context | MATCHES | src/analytics/signal_detector.cpp | `detect_turns()` | Bullish: bar.low == min of bar_history_ lows. Bearish: bar.high == max of bar_history_ highs. |
| §8.1 Score threshold | MATCHES | src/analytics/signal_detector.cpp | `detect_turns()` | turn_score >= config_.turns_min_score (default 3) → emit. >= 5 → STRONG. |

**⚠ DEVIATION — §8.1 V1 and V2:**
- **V1 in spec**: checks proximity of bar extreme to COT price (institutional level at turn).
- **V1 in code**: checks if total_vol at bar extreme >= 1.5× session_avg_vol_per_level (high volume at extreme).
- This captures the same *economic intent* (institutional activity at the extreme) but via a different measurement.

- **V2 in spec**: checks if volume at bar extreme (ask_vol at high OR bid_vol at low) is below an exhaustion threshold — last buyer/seller signal.
- **V2 in code**: checks if bar's total_volume is less than the previous bar's total_volume — volume declining.
- This is a broader measure (full bar, not just extreme level).

**Action required:** Decide whether to align V1/V2 with spec exactly (requires COT price and level-specific volume lookup at extremes) or document the alternative definitions as intentional design choices.

**Implementation notes:**
- `bar_history_` includes the current bar (pushed at top of `detect_all()` before any detectors run). So `bar_history_.back()` is the current bar; `bar_history_[size-2]` is the previous bar.
- Requires at least 2 bars in history for V2 (volume decline) and V6 (swing context checks).

---

### §9 — Orderflows Ratio  [IF p.22]

| Spec § | Status | File | Function | Notes |
|--------|--------|------|----------|-------|
| §9.1 Bottom Heavy | MATCHES | src/analytics/signal_detector.cpp | `detect_ratio()` | bid_vol[BarLow] >= ratio_threshold × ask_vol[BarLow] AND total_vol >= ratio_min_volume |
| §9.1 Top Heavy | MATCHES | src/analytics/signal_detector.cpp | `detect_ratio()` | ask_vol[BarHigh] >= ratio_threshold × bid_vol[BarHigh] AND total_vol >= ratio_min_volume |
| §9.1 SessionHigh/Low proximity | PARTIAL | src/analytics/signal_detector.cpp | `detect_ratio()` | Spec requires BarHigh within N_ticks of SessionHigh. Implementation checks BarHigh/BarLow price levels exist in bar.price_levels — does not yet enforce session high/low proximity. |

**Implementation notes:**
- Price level lookup: lambda `find_level` iterates `bar.price_levels` searching for `|level.price - target| < tick_size × 0.5`.
- If the bar high/low level is not found in price_levels (e.g., zero-volume tick), no signal fires — conservative behavior.

---

### §10 — Single Prints  [IF p.23]

| Spec § | Status | File | Function | Notes |
|--------|--------|------|----------|-------|
| §10.1 Last Buyer at High | MATCHES | src/analytics/signal_detector.cpp | `detect_single_prints()` | total_vol at bar.high <= single_print_threshold (default 2) → SINGLE_PRINT_LAST_BUYER |
| §10.1 Last Seller at Low | MATCHES | src/analytics/signal_detector.cpp | `detect_single_prints()` | total_vol at bar.low <= single_print_threshold → SINGLE_PRINT_LAST_SELLER |
| §10.1 Magnet tracking | ADDED | src/analytics/signal_detector.cpp | `detect_single_prints()` | active_magnets_ registry: each unresolved single print stored with price, is_high, created_ts_ns, exhaustion_score, unfinished_auction flag |
| §10.1 Magnet resolution | ADDED | src/analytics/signal_detector.cpp | `check_magnet_resolutions()` | If bar range [low, high] contains a magnet price → resolved; emits THIN_PRINTS signal |

**Implementation notes:**
- `unfinished_auction` = true if only one side has volume at the extreme (e.g., only asks, no bids at bar high) — indicates price left without opposing trade.
- These functions are non-const (modify active_magnets_).

---

### §11 — Delta Surge  [IF p.24]

*(Implemented in DeltaEngine — see §2.5 Delta Surge row above.)*

---

### §12 — Market Sweep  [IF p.25]

| Spec § | Status | File | Function | Notes |
|--------|--------|------|----------|-------|
| §12.1 Volume threshold | MATCHES | src/analytics/signal_detector.cpp | `detect_market_sweep()` | bar.total_volume >= sweep_volume_threshold (default 500) |
| §12.1 Level count threshold | MATCHES | src/analytics/signal_detector.cpp | `detect_market_sweep()` | bar.price_levels.size() >= sweep_level_threshold (default 5) |
| §12.1 Speed threshold | MATCHES | src/analytics/signal_detector.cpp | `detect_market_sweep()` | speed = levels/duration_ms >= sweep_speed_threshold (default 2.0 levels/ms) |

**Implementation notes:**
- Duration uses `bar.bar_close_ts_ns - bar.bar_open_ts_ns` (note: `bar_` prefix on both fields — this is the BarRecord naming convention).
- If duration_ms == 0 (instantaneous), speed check is skipped (treated as infinitely fast → passes).
- Strength = STRONG if volume >= 3× sweep_volume_threshold.

---

### §13 — Trapped Traders  [IF p.26]

| Spec § | Status | File | Function | Notes |
|--------|--------|------|----------|-------|
| §13.1 Trapped traders | MATCHES | src/analytics/imbalance_detector.cpp | `detect_trapped()` | Sell imbalance at bar low + bar closed up → LONG signal. Buy imbalance at bar high + bar closed down → SHORT. |

*(Note: spec §13 = Trapped Traders. §13 in vwap_engine inline comments refers to the VWAP section 5 of the spec — there is a numbering mismatch between how the spec labels its sections and how the inline comments reference them. The inline comments use the physical section numbers from the Indicator Formulas PDF.)*

---

### §14 — POC Slingshot  [IF p.27]

| Spec § | Status | File | Function | Notes |
|--------|--------|------|----------|-------|
| §14.1 Distance condition | MATCHES | src/analytics/signal_detector.cpp | `detect_poc_slingshot()` | ABS(bar.close - profile.poc) >= slingshot_distance_ticks × tick_size (default 4 ticks) |
| §14.1 Delta strength | MATCHES | src/analytics/signal_detector.cpp | `detect_poc_slingshot()` | ABS(bar_delta) >= session_avg_abs_delta × slingshot_delta_mult (default 1.5×) |
| §14.1 Volume confirm | MATCHES | src/analytics/signal_detector.cpp | `detect_poc_slingshot()` | bar.total_volume >= session_avg_vol × slingshot_volume_mult (default 1.2×) |

**Implementation notes:**
- `effective_tick` fallback: if tick_size is not passed or is 0, defaults to 1.0 to prevent division-by-zero.
- `session_avg_vol` = delta.bar_total_vol / delta.bar_count — uses DeltaState for session-level stats rather than VolumeProfileSnapshot.

---

## ══════════════════════════════════════════════════════════════════
## PART 2 — DETECTORS NOT IN INDICATOR FORMULAS PDF
## These are implemented but not specified in the formulas PDF.
## Source: OrderFlow_Spec_v4_Master.pdf or internal engineering decisions.
## ══════════════════════════════════════════════════════════════════

| Detector | Status | File | Function | Notes |
|----------|--------|------|----------|-------|
| Zero Prints | INTERNAL | src/analytics/signal_detector.cpp | `detect_zero_prints()` | Scans price_levels for is_zero_print == true. Skips bar extremes (high/low). Zero volume inside bar = price level not traded. |
| Exhaustion Prints | INTERNAL | src/analytics/signal_detector.cpp | `detect_exhaustion_prints()` | Level at extreme has total_vol >= 2× avg_level_vol AND bar_delta opposes direction. Spec not in formulas PDF — derived from trading logic. |
| Orderflows Sequencing | INTERNAL | src/analytics/signal_detector.cpp | `detect_sequencing()` | 3 consecutive bars with strictly rising delta → SEQUENCING_UP; falling → SEQUENCING_DOWN. Requires bar_history_.size() >= 3. |
| Volume Decline | INTERNAL | src/analytics/signal_detector.cpp | `detect_volume_decline()` | 3 consecutive bars with declining total_volume in same delta direction → warning signal. |
| Accumulation/Distribution | INTERNAL | src/analytics/signal_detector.cpp | `detect_accumulation_distribution()` | 3 consecutive positive delta bars with held lows → ACCUMULATION. 3 consecutive negative delta bars with falling highs → DISTRIBUTION. |

---

## ══════════════════════════════════════════════════════════════════
## PART 3 — ENGINEERING PRODUCT SPEC  [EPS]
## Source: OFE_Engineering_Product_Spec.pdf
## ══════════════════════════════════════════════════════════════════

### Logging System  [EPS pp.2–4]

| EPS Item | Status | File | Notes |
|----------|--------|------|-------|
| 5-level logger | MATCHES | src/util/logger.cpp | TRACE/DEBUG/INFO/WARN/ERROR + OFF |
| Async background IO | MATCHES | src/util/logger.cpp | Background thread drains std::queue<LogRecord> |
| Daily rotation | MATCHES | src/util/logger.cpp | ofe_YYYY-MM-DD.log |
| ANSI colour | MATCHES | src/util/logger.cpp | Grey=TRACE, Cyan=DEBUG, White=INFO, Yellow=WARN, BoldRed=ERROR |
| Macro param names | MATCHES | include/util/logger.h | `ofe_lvl` and `ofe_mod` (NOT `level`/`module`) — prevents field name collision. PB-01 fix. |
| No {:#x} in macros | MATCHES | all LOG_* call sites | `#` is preprocessor stringify inside variadic macros. Use `{:x}`. PB-10 fix (3 files). |
| TRACE compiled away | MATCHES | include/util/logger.h | `#ifdef NDEBUG` — zero cost in Release builds |
| Source file:line appended | MATCHES | include/util/logger.h | Only on DEBUG and TRACE levels |

### SymbolWorker Pipeline  [EPS §4]

| Stage | Status | File | Notes |
|-------|--------|------|-------|
| Stage 1: Dequeue tick | MATCHES | src/core/symbol_worker.cpp | try_pop from SPSC ring buffer |
| Stage 2: Quote state update | MATCHES | src/core/symbol_worker.cpp | Updates best_bid/best_ask for Lee-Ready |
| Stage 3: Lee-Ready classify | MATCHES | src/core/symbol_worker.cpp | Only for TRADE ticks |
| Stage 4: should_exclude_tick | MATCHES | src/core/symbol_worker.cpp | Drops ticks that fail exclusion check |
| Stage 5: Bar accumulation | MATCHES | src/core/symbol_worker.cpp | BarEngine::on_tick() |
| Stage 6: Delta update | MATCHES | src/core/symbol_worker.cpp | DeltaEngine::on_tick() |
| Stage 7: VWAP update | MATCHES | src/core/symbol_worker.cpp | VwapEngine::on_tick() |
| Stage 8: Volume Profile update | MATCHES | src/core/symbol_worker.cpp | VolumeProfileEngine::on_tick() |
| Stage 9: Tick-level signal check | MATCHES | src/core/symbol_worker.cpp | VWAP Reaction/Rotation signals |
| Stage 10: Bar close detectors | MATCHES | src/core/symbol_worker.cpp | `on_bar_close()` — all 14 signal detectors |
| Stage 11: Publish to EventBus | MATCHES | src/core/symbol_worker.cpp | InProcessEventBus publish_signal/publish_bar/publish_delta_snapshot |

**Implementation notes:**
- No heap allocation on stages 1-9 (hot path). Bar open pre-allocates price_levels vector.
- No mutex on TickRouter::route() — shared_lock only (read-side of shared_mutex).
- EventBus is InProcessEventBus (in-memory). Redis Streams backend is future work.

### UniversalTickRecord  [EPS / tick_record.h]

| Item | Status | File | Notes |
|------|--------|------|-------|
| 64-byte size | MATCHES | include/core/tick_record.h | `static_assert(sizeof(UniversalTickRecord) == 64)` — never change |
| Cache alignment | MATCHES | include/core/tick_record.h | `alignas(64)` — one record per cache line |
| BarRecord field naming | MATCHES | include/core/bar_types.h | Timestamps named `bar_open_ts_ns` and `bar_close_ts_ns` (with `bar_` prefix) — not `open_ts_ns` |

---

## ══════════════════════════════════════════════════════════════════
## PART 4 — OPEN ITEMS AND ACTION REQUIRED
## ══════════════════════════════════════════════════════════════════

| # | Priority | Item | Owner | Status |
|---|----------|------|-------|--------|
| 1 | HIGH | §4.6 Shape Classification: statistical (spec) vs positional (code) — decide and align | Product Owner | OPEN |
| 2 | HIGH | §7.1 Variable 3 POC Score: confirm whether COT_price = bar.cot_price or session POC | Product Owner | OPEN |
| 3 | HIGH | §7.2 Pulse direction: implement NEUTRAL case (delta/candle conflict → no signal) | Engineering | OPEN |
| 4 | HIGH | §8.1 Turns V1/V2: align with spec (COT proximity + level exhaustion) or document intentional deviation | Product Owner | OPEN |
| 5 | MEDIUM | §9.1 Ratio: add session high/low proximity check (N_ticks from SessionHigh/SessionLow) | Engineering | OPEN |
| 6 | MEDIUM | Anchored Volume Profile: `add_anchored_profile()` is a stub | Engineering | OPEN |
| 7 | MEDIUM | Monthly VWAP reset: series accumulates but never resets at month boundary | Engineering | OPEN |
| 8 | LOW | Redis Streams EventBus: replace InProcessEventBus with real Redis backend | Engineering | FUTURE |
| 9 | LOW | eSignal feed adapter (Phase 2) | Engineering | FUTURE |

---

## ══════════════════════════════════════════════════════════════════
## PART 5 — SESSION LOG
## ══════════════════════════════════════════════════════════════════

| Session | Date | What was built | Test count |
|---------|------|----------------|-----------|
| Session 1 | 2026-06 | All 19 headers, core src files, delta, imbalance, logger, 3 test files | 56/56 |
| Session 2 | 2026-06 | Audit of pre-existing stubs; discovered PB-10; no new files | 56/56 |
| Session 3 | 2026-06-19 | volume_profile, vwap_engine, signal_detector (all detectors); PB-10 fixed | 56/56 |
| Session 4 | TBD | Unit tests: test_bar_engine, test_delta_engine, test_imbalance, test_volume_profile, test_vwap_engine, test_signal_detector | target: 120+ |
