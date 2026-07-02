# AGT-06 — NinjaTrader Agent
# Role: NT8 C# indicator — native footprint chart reading from any NT8 data provider
# Status: SESSION 7 COMPLETE — OFEFootprintNT.cs done; pending: NT8 compile test
# When to use: Extending NT8 footprint rendering, debugging compile errors, adding VP/CVD panels
# Output: nt_adapter/OFEFootprintNT.cs (primary); legacy bridge files in nt_adapter/ for reference

## FIRST ACTION — READ PROGRESS.md

---

You are AGT-06, the NinjaTrader Agent for the Order Flow Engine project.

## ★ CRITICAL ARCHITECTURE DECISION (Session 7) ★

The user has MULTIPLE NT8 data providers already working:
- Schwab API → NT8 → stock candlestick charts (working)
- Coinbase → NT8 → crypto candlestick charts (working)
- IQFeed, Kinetick, eSignal → will work similarly (no custom code needed)

**The C++ nt_bridge_server is NOT required for NT8 footprint.**
**NT8 already receives tick data from brokers/providers via its native mechanism.**
**OFEFootprintNT.cs reads from NT8's own OnMarketData() — works with any provider.**

## CURRENT STATUS

| File | Status | Notes |
|------|--------|-------|
| `nt_adapter/OFEFootprintNT.cs`       | ★ [DONE — Session 7] ★ PRIMARY INDICATOR | Self-contained, no C++ server |
| `nt_adapter/OFEMessageTypes.cs`      | [DONE — Session 5] — legacy, bridge server only | Not needed for OFEFootprintNT |
| `nt_adapter/OFEEngineClient.cs`      | [DONE — Session 5] — legacy, bridge server only | Not needed for OFEFootprintNT |
| `nt_adapter/OFEFootprintIndicator.cs`| [DONE — Session 5] — LEGACY, requires bridge server port 7777 | Superseded |
| `nt_adapter/FootprintRenderer.cs`    | [DONE — Session 5] — legacy renderer for bridge server indicator | Not needed for OFEFootprintNT |
| `nt_adapter/OverlayRenderer.cs`      | [DONE — Session 5] — legacy VWAP/VP overlay | Not needed for OFEFootprintNT |
| `nt_adapter/DeltaPanelRenderer.cs`   | [DONE — Session 5] — legacy delta panel | Not needed for OFEFootprintNT |
| `nt_adapter/OFECoinbaseChart.cs`     | [DONE — Session 6] — standalone chart panel | Superseded by OFEFootprintNT |
| `nt_adapter/CoinbaseWebSocketClient.cs` | [DONE — Session 6] — optional | Not needed; user has native feeds |
| `nt_adapter/CoinbaseNTFeed.cs`       | [DONE — Session 6] — optional | Not needed; user has native feeds |
| `tools/nt_bridge_server.cpp`         | [DONE — Session 5] — C++ TCP server | For future advanced analytics only |
| `nt8/OFEAnalytics.cs`               | [DONE — Session 5] — simple VWAP overlay | Legacy |

## ARCHITECTURE (Session 7 — CORRECT)

```
Any data provider (Schwab / Coinbase / IQFeed / Kinetick / eSignal)
         │
         ▼  (NT8 native data connection — no custom code needed)
NinjaTrader 8 bar engine
         │
         ▼
OFEFootprintNT.cs (NinjaIndicator, IsOverlay=true)
    OnMarketData() ──► accumulate bid/ask vol per price level per bar
    OnBarUpdate()  ──► detect bar change → CloseBar() → store BarSnapshot
    OnRender()     ──► SharpDX footprint cells over NT8's own candles
```

## OFEFootprintNT.cs — KEY IMPLEMENTATION DETAILS

### Tick classification (Lee-Ready in C#)
```csharp
if (e.MarketDataType == MarketDataType.Bid)  { _lastBid = e.Price; return; }
if (e.MarketDataType == MarketDataType.Ask)  { _lastAsk = e.Price; return; }
// MarketDataType.Last:
bool isBuy = (e.Price >= _lastAsk) ? true
           : (e.Price <= _lastBid) ? false
           : (e.Price >= _prevTradePrice);   // tick-test fallback
```

### Bar-close detection (in OnMarketData)
```csharp
if (CurrentBar != _accumBarIndex) {
    if (_accumBarIndex >= 0) CloseBar(_accumBarIndex);
    _accumLevels.Clear();
    _accumBarIndex = CurrentBar;
}
```

### Price level key (float-safe)
```csharp
long priceKey = (long)Math.Round(e.Price / tickSize);
```

### Bar cache key = NT8 bar index (int)
No timestamp alignment needed. `_barCache[barIdx]` maps directly to NT8's bar.

### Imbalance formula (same as C++ engine)
Buy imbalance: `ask_vol[P] >= ratio × bid_vol[P - 1_tick]`
Stacked: 3+ consecutive same-direction imbalances (configurable via StackedThreshold)

### Installation
1. Copy ONLY `OFEFootprintNT.cs` → `Documents\NinjaTrader 8\bin\Custom\Indicators\`
2. Ctrl+F5 to compile
3. Open any chart with a working data provider
4. Drag "OFE Footprint NT" onto chart
5. Check Output tab (Ctrl+5): `[OFE] Bar N closed: Δ=+123 CVD=+456 Levels=18`

## LEGACY ARCHITECTURE (Sessions 5-6 — for reference only)

```
Coinbase WebSocket
  └── tools/nt_bridge_server.cpp  (C++ — runs on trading server)
        ├── 6 analytics engines → TCP:7777, 4-byte LE length-prefix JSON
        └── NtFramedBroadcaster

NinjaTrader 8 workspace
  └── OFEFootprintIndicator (NinjaIndicator) [LEGACY]
        ├── OFEEngineClient.cs    — TCP client to C++ engine (localhost:7777)
        ├── OFEMessageTypes.cs    — shared structs and manual JSON parser
        ├── FootprintRenderer.cs  — cell grid, colours, numbers
        ├── OverlayRenderer.cs    — VWAP, VP histogram, zones
        └── DeltaPanelRenderer.cs — delta bar + CVD sub-panel
```
**WHY NOT USED:** If nt_bridge_server not running → indicator shows nothing (no error shown to user).
Also only worked for Coinbase BTC-USD; not generic across data providers.

## FEED MODES (Session 7 — simplified)

## TCP PROTOCOL (OFE Engine ↔ NT AddOn, port 7777)

### Frame format
```
[4 bytes LE uint32: payload_length][payload_length bytes: UTF-8 JSON]
```

### OFE Engine → NT AddOn (inbound to adapter)

**bar_close** (after each bar closes):
```json
{
  "msg": "bar_close",
  "ts": 1704067200000000000,
  "sym": 1234567890,
  "o": 45000.00, "h": 45001.00, "l": 44999.00, "c": 45000.50,
  "vol": 1500000000, "delta": 20000000, "cvd": 150000000,
  "poc": 45000.00, "vah": 45002.00, "val": 44998.00,
  "shape": 0,
  "levels": [
    {"px": 44999.00, "bv": 50000000, "av": 30000000, "tv": 80000000,
     "ib": false, "is": false, "sb": false, "ss": false, "zp": false, "poc": false, "cot": false},
    ...
  ]
}
```

**vwap** (after each tick, rate-limited to 10Hz):
```json
{"msg":"vwap","ts":1704067200000000000,"sym":1234567890,"vwap":45000.25,
 "b1p":45002.0,"b1m":44998.5,"b2p":45004.0,"b2m":44997.0,"b3p":45006.0,"b3m":44995.0}
```

**signal** (when a signal fires):
```json
{"msg":"signal","ts":1704067200000000000,"sym":1234567890,
 "type":"PULSE_LONG","dir":0,"str":1,"px":45000.00,"score":82.5}
```

### NT AddOn → OFE Engine (outbound from adapter)

**subscribe** (on connect and reconnect):
```json
{"msg":"subscribe","name":"BTC-USD","id":1234567890}
```

**tick** (Kinetick mode only):
```json
{"msg":"tick","ts":1704067200000000000,"sym":1234567890,
 "px":45000.00,"vol":100000000,"side":2,"type":0}
```
- `side`: 0=ASK, 1=BID, 2=UNKNOWN (Lee-Ready will classify UNKNOWN)
- `type`: 0=TRADE, 1=BID_QUOTE, 2=ASK_QUOTE

## CELL COLOUR RULES (from Footprint Chart Spec §3)

| Condition | Colour | Hex |
|-----------|--------|-----|
| ask_vol > bid_vol | BuyColor | #2196F3 (configurable) |
| bid_vol > ask_vol | SellColor | #EF5350 (configurable) |
| IsBuyImbalance | ImbalanceBuyColor | #1565C0 (overrides primary) |
| IsSellImbalance | ImbalanceSellColor | #B71C1C (overrides primary) |
| IsStackedBuy | StackedBuyColor | #0D47A1 (overrides imbalance) |
| IsStackedSell | StackedSellColor | #7B0000 (overrides imbalance) |
| IsPoc | POC yellow highlight | #FFEB3B (additive, on top) |
| IsCot | Magenta border | #E000E0 (border, no fill) |
| IsZeroPrint | Black fill | #000000 (highest priority) |

## Z-ORDER (13 layers, back to front)

| Layer | What | Who paints |
|-------|------|-----------|
| 1 | Background | NT8 |
| 2 | VP histogram | OverlayRenderer |
| 3 | Zone rectangles (future) | OverlayRenderer |
| 4 | Candle body + wicks | NT8 |
| 5 | Cell grid (footprint) | FootprintRenderer |
| 6 | POC highlight | FootprintRenderer |
| 7 | COT box border | FootprintRenderer |
| 8 | VWAP daily line | OverlayRenderer |
| 9 | VWAP ±1σ / ±2σ bands | OverlayRenderer |
| 10 | Session H/L | NT8 |
| 11 | Delta histogram sub-panel | DeltaPanelRenderer |
| 12 | CVD polyline | DeltaPanelRenderer |
| 13 | Signal arrows | OverlayRenderer |

## NT8 EVENTS MAPPED

| NT8 Event | OFE action |
|-----------|-----------|
| `OnStateChange(DataLoaded)` | Connect to OFE engine TCP, subscribe symbol |
| `OnStateChange(Terminated)` | Disconnect, dispose SharpDX resources |
| `OnBarUpdate()` | Track currentBarIdx_ for bar-to-cache mapping |
| `OnMarketData(Last)` | Forward TRADE tick if FeedMode=Kinetick |
| `OnMarketData(Bid/Ask)` | Forward BID_QUOTE/ASK_QUOTE if FeedMode=Kinetick |
| `OnRender(...)` | Call all three renderers |
| `OnRenderTargetChanged()` | Recreate SharpDX brushes and text formats |

## PROPERTY PANEL (12 groups in NT8 UI)

| Group | Properties |
|-------|-----------|
| Connection | EngineHost, EnginePort, FeedMode, SymbolId |
| Display | ShowDelta, ShowVwap, ShowVpHistogram, ShowSignalArrows |
| Imbalances | ImbalanceRatio |
| Cell Display | CellFontSize |
| Colours-Cells | BuyColor, SellColor, ImbalanceBuyColor, ImbalanceSellColor, StackedBuyColor, StackedSellColor, PocColor |

## INSTALLATION IN NT8

1. Copy all `nt_adapter/*.cs` files to:
   `Documents/NinjaTrader 8/bin/Custom/Indicators/`
2. Also copy `OFEMessageTypes.cs` to the same folder (it's a shared class, not an indicator itself)
3. In NT8: Tools → NinjaScript Editor → Compile (Ctrl+F5)
4. Drag `OFEFootprintIndicator` onto a chart
5. Set `Engine Host = localhost`, `Engine Port = 7777`, `Feed Mode = Coinbase` (or Kinetick)

## C++ ENGINE SIDE (tools/nt_bridge_server.cpp — DONE Session 5)

The C++ bridge server (`tools/nt_bridge_server.cpp`) implements `NtFramedBroadcaster`:
- Listens on TCP port 7777 (overridable via `OFE_NT_PORT` env var)
- Accept loop spawns one reader thread per connected NT client
- Reads `subscribe` frames using `recv_all()` (blocks until full frame received)
- Broadcasts `bar_close`, `vwap`, `signal` frames to all connected clients
- Frame encoding: 4-byte LE uint32 payload_length + UTF-8 JSON bytes
- Bar number (`n`) emitted in each `bar_close` frame for NT8 bar cache keying

**Build:**
```
cmake --build build --target nt_bridge_server
./build/nt_bridge_server
OFE_NT_PORT=7778 ./build/nt_bridge_server
```

## ACCEPTANCE CRITERIA (GATE-06)

| Test | Criterion |
|------|-----------|
| FP-01 | Chart renders without errors on live Coinbase or Kinetick data |
| FP-03 | Imbalance cells verified against manual diagonal calculation |
| FP-07 | VWAP line matches NT8 built-in VWAP indicator value (±0.0001) |
| FP-08 | VP overlay longest bar is at POC price |
| FP-14 | OnRender < 16ms (60fps target) with 200 visible bars |

## KNOWN ISSUES / NEXT STEPS

1. **Sub-panel bounds** — `DeltaPanelRenderer` uses hardcoded `panelHeight = 80f`.
   Replace with `ChartPanel.GetBounds()` from the registered second data series for production.

2. **Signal bar mapping** — `OverlayRenderer.DrawSignalArrow()` uses `LastVisibleBarIndex`
   as proxy for signal bar X position. Improve by looking up exact bar by signal timestamp.

3. **Zone rendering (Layer 3)** — Stacked imbalance zones as filled rectangles not yet implemented.
   Add a `ZoneRegistry` in OFEMessageTypes, populate from `signal` messages with type=STACKED_*,
   and render in OverlayRenderer at Z-order layer 3.

4. **JSON parsing** — `OFEMessageTypes.cs` uses a manual string parser (no Newtonsoft dependency).
   If Newtonsoft is available in NT8 (it is), refactor to `JObject.Parse()` for robustness.

5. **Rendering acceptance tests** — GATE-06 criteria (FP-01, FP-03, FP-07, FP-08, FP-14) pending.
   Must be validated with live Coinbase data feeding into NT8.

## CRITICAL API NOTES (Session 5 bugs found and fixed)

- `ImbalanceDetector::detect_all()` has `[[nodiscard]]` — ALWAYS capture the return value:
  `auto imb_sigs = imb_detector.detect_all(bar, active_zones, kTickSize);`
- `SignalDetector::push_bar_history()` is **private** — do NOT call it externally.
  `detect_all()` calls it internally. Calling it externally causes double-push (bugs in Turns/Sequencing).
- VWAP rate limiting uses a local `int64_t last_vwap_ns` (not atomic) because the ixwebsocket
  tick callback is single-threaded per symbol.

## SESSION LOG

Session 5 (2026-06-20): Created tools/nt_bridge_server.cpp with NtFramedBroadcaster.
  All 6 C# files retained from Session 4. Full pipeline: Coinbase → 6 engines → TCP:7777 → NT8.
  bar_close frames include full price_levels[] with ib/is/sb/ss/zp/poc/cot per price level.
  Key design: stacked zone flags (sb/ss) derived by checking each price level against active ImbalanceZone ranges.
  154/154 tests passing — zero regressions.

Session 4: Created all 6 C# files (OFEMessageTypes, OFEEngineClient, OFEFootprintIndicator,
FootprintRenderer, OverlayRenderer, DeltaPanelRenderer). Feed mode Coinbase vs Kinetick
distinction implemented. C++ TCP server was the remaining gap (filled in Session 5).
