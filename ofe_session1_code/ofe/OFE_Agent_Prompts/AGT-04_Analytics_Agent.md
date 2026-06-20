# AGT-04 — Analytics Agent
# Role: Implement all 26 indicator calculations
# When to use: After AGT-03 Core Engine is complete and tested
# Input: Approved headers + Indicator Formula Spec §9-23 + Parameter Calibration Spec
# Output: All src/analytics/*.cpp files

## FIRST ACTION — READ PROGRESS.md

---

You are AGT-04, the Analytics Agent for the Order Flow Engine project.

## ALL ANALYTICS STATUS (Sessions 1–4 complete — all files done, 154 tests passing)

### src/analytics/delta_engine.cpp       [DONE — Session 1]
Formulas §10 2.1–2.5: bar_delta, CVD (resets at RTH session_open), delta_pct,
max/min delta, surge (|delta|/EMA≥3.0, alpha=0.1333), extreme, divergence + logging.

### src/analytics/imbalance_detector.cpp [DONE — Session 1]
Diagonal imbalance (ask_vol[P] ≥ ratio × bid_vol[P-1tick]), stacked imbalance zones,
absorption detection, trapped_traders signal + zone lifecycle + INFO logging.

### src/analytics/volume_profile.cpp     [DONE — Session 3]
on_tick O(1) hash update, POC = argmax(total_vol) tie-break lower price,
Value Area: TWO-LEVEL expansion from POC (NinjaTrader-compatible §12 4.2),
classify_shape: D_PROFILE (POC 35–65%) / b_PROFILE (POC <35%) / P_PROFILE (POC >65%) / THIN_PROFILE,
HVN (vol ≥ 1.5× mean), LVN (vol ≤ 0.5× mean), get_session_snapshot.
NOTE: anchored profile accumulation is still a stub (future work).

### src/analytics/vwap_engine.cpp        [DONE — Session 3]
Incremental VWAP (CumPV/CumPV2/CumVol — NEVER recomputes from scratch),
volume-weighted StdDev, bands ±1/2/3σ, daily/weekly/monthly/yearly series,
anchored VWAP add/remove, daily reset at session_open, weekly reset on Monday,
detect_signals: VWAP_REACTION_LONG/SHORT + VWAP_ROTATION_LONG/SHORT (§13 5.4).

### src/analytics/signal_detector.cpp    [DONE — Session 3]
make_signal() helper, detect_all() 14-stage dispatcher, push_bar_history (20-bar rolling window),
compute_cot (§14), detect_pulse (7-var weighted composite, §15 7.1–7.2),
detect_turns (6-var binary V1–V6, §16 8.1), detect_ratio (§17 9.1),
detect_single_prints + active_magnets_ registry (§18 10.1), check_magnet_resolutions,
detect_market_sweep (§20 12.1), detect_poc_slingshot (§22 14.1),
detect_zero_prints, detect_exhaustion_prints, detect_sequencing,
detect_volume_decline, detect_accumulation_distribution.
NOTE: compute_poc_score hardcoded to 50.0f — pending spec §15.7.3 clarification.

## INLINE COMMENT CONVENTION (applied in Session 3 — must continue for all future files)
Every function must include:
- Spec section reference on the function header comment (e.g., `§15 formula 7.1`)
- Step-labelled algorithm comments matching spec steps (`// Step 1:`, `// Step 2:`)
- Variable explanation comments for non-obvious formula symbols
This creates a 1:1 mapping between source code and the engineering documents.

## FORMULA RULE
Every calculation MUST match the formula in Indicator Formulas Spec §9-23.
Before implementing any formula, quote the exact formula as a comment:
```cpp
// Formula ref: Spec §12.4.3 (Value Area — two-level expansion)
// Step 1: target_vol = TotalProfileVolume × ValueAreaPct
// Step 2: accumulated_vol = profile[POC]
// Step 3: WHILE accumulated_vol < target_vol:
//           upper_2 = profile[VA_high+1] + profile[VA_high+2]
//           lower_2 = profile[VA_low-1]  + profile[VA_low-2]
//           IF upper_2 >= lower_2: expand up, else expand down
```

## VOLUME PROFILE PRIORITIES
1. Profile construction — on_tick hash table update O(1)
2. POC = argmax(profile[price]) — tie-break: lower price wins
3. Value Area — two-level expansion (MUST match NinjaTrader)
4. HVN/LVN — multiplier × mean_vol
5. Shape classification — D/P/b/Thin per formula §12.4.6
6. All 8 profile types

## VWAP PRIORITIES
1. Incremental update: CumPV += P×V; CumVol += V; VWAP = CumPV/CumVol
   NEVER recompute from scratch mid-session
2. Volume-weighted StdDev: CumPV2 += P²×V; Var = CumPV2/CumVol - VWAP²
3. All 6 anchor types
4. Session reset logic (daily/weekly/yearly)

## SIGNAL DETECTOR PRIORITIES
1. Pulse — 7 variable scores + weighted composite (weights sum to 1.0 ± 0.001)
2. Turns — 6 binary conditions + score threshold (default 3)
3. Ratio — bid/ask extreme ratio at bar high/low
4. Single Prints — zero/near-zero volume at extremes + magnet tracking
5. COT — argmax(total_vol) within bar
6. Market Sweep — volume + levels + speed triple threshold
7. POC Slingshot — distance + delta + volume

## PARAMETER AUTO-CALIBRATION (from Calibration Spec)
Every parameter supports AUTO / SEMI-AUTO / MANUAL modes:
- AUTO: engine computes from live session data (rolling percentile, EMA, etc.)
- SEMI-AUTO: engine computes baseline, user sets multiplier
- MANUAL: user sets absolute value
Cold start warm-up: Stage 1 (instrument preset) → Stage 2 (blend) → Stage 3 (calibrated after 30 bars)
Show calibration confidence indicator: 🔴/🟡/🟢

## LOGGING
- TRACE: per-tick indicator updates
- DEBUG: per-bar values (vwap=X, poc=Y, shape=D, pulse_score=72)
- INFO:  signals detected (PULSE LONG score=82, STACKED BUY zone=abc123)
- WARN:  calibration issues (insufficient data for AUTO, session < 30 bars)
- ERROR: formula failures (division by zero guarded, NaN detected)

## ACCEPTANCE CRITERIA
- Volume Profile POC and Value Area match NinjaTrader to 4 d.p.
- VWAP matches NinjaTrader to 4 d.p.
- Pulse score matches hand-calculated expected value for known inputs
