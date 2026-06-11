# Rudra_org — Order Flow Engine (OFE)

## What Is This?

This project is a **production-grade order flow engine** written in C++. Order flow is the study of *who is buying and selling, at what price, and in what quantity* — the raw information beneath every price movement in a financial market.

The engine is designed to ingest a live stream of market ticks (individual trades and quote updates), classify them, build bars, compute analytics, and emit trading signals — all at very high speed with minimal latency.

Everything lives in header files (`include/`) so there is no separate library to compile. You just include what you need.

---

## The Big Picture

```
Live Market Feed
      |
      v
  TickRouter          <-- routes each trade to the right worker by symbol
      |
      v
  SymbolWorker        <-- 11-stage pipeline per symbol
   |   |   |
   v   v   v
 LeeReady  BarEngine  DeltaEngine  VwapEngine  VolumeProfile  ImbalanceDetector
                              |
                              v
                       SignalDetector  <-- combines all analytics into a signal
                              |
                              v
                         EventBus      <-- publishes SignalEvent to subscribers
```

---

## Module-by-Module Explanation

### `ofe::core` — The Engine Room

#### `tick_record.h` — A Single Market Event
A `UniversalTickRecord` is one row of market data: a price, a size, a bid, an ask, a timestamp, and which symbol it belongs to. It is exactly **64 bytes** and aligned to a 64-byte boundary so it fits perfectly in one CPU cache line. This matters at high frequency — a misaligned struct causes extra memory fetches and slows everything down.

#### `ring_buffer.h` — Lock-Free Queue Between Threads
A ring buffer is a fixed-size circular queue. This one is **SPSC** (Single Producer, Single Consumer) and **lock-free** — one thread pushes ticks in, another pops them out, with no mutex. It uses atomic operations and powers-of-two sizing so the modulo wrap is a cheap bitmask. This is the highway between the feed thread and the worker threads.

#### `tick_router.h` — Sending Ticks to the Right Worker
When ticks arrive from a feed, they could be for any of thousands of symbols. The router splits them into **64 shards** using `symbol_id % shard_count`. Each shard has its own queue and mutex, so workers for different symbols never block each other.

#### `lee_ready.h` — Was This Trade a Buy or a Sell?
Raw market data often does not label whether a trade was buyer-initiated or seller-initiated. The **Lee-Ready algorithm** infers direction from the quote at the time of the trade:
- If the trade price is at or above the ask → **Buy** (aggressor lifted the offer)
- If the trade price is at or below the bid → **Sell** (aggressor hit the bid)
- If it is between bid and ask → use the midpoint tick test
- If still ambiguous → use a caller-supplied override

#### `bar_engine.h` / `bar_types.h` — Building Bars from Ticks
A **bar** (also called a candle) summarizes a group of ticks into Open/High/Low/Close plus volume, VWAP, delta, and more. Three bar types are supported:

| Type | Closes when... |
|------|----------------|
| Time | A fixed time interval elapses (e.g. 1 minute) |
| Volume | A fixed number of contracts trade (e.g. 1000 lots) |
| Range | Price moves a fixed number of ticks (e.g. 5 ticks) |

Each bar also tracks **buy volume**, **sell volume**, **delta** (buy − sell), **CVD** (cumulative volume delta), **VWAP**, and candle geometry (body size, upper/lower wicks).

#### `symbol_worker.h` — The Per-Symbol Pipeline
Each symbol gets its own worker thread running an **11-stage pipeline**. Ticks arrive in an ingress ring buffer; the worker pops them and runs each through all 11 stages in sequence. Stages are plain `std::function` callbacks, so you can wire in any logic (bar building, analytics, risk checks, etc.). After all stages, the tick is published to the `EventBus`.

#### `event_bus.h` — Broadcasting Signals
A simple abstract interface. After a tick is fully processed, a `SignalEvent` is published here. Anything that wants to react to signals (a strategy, a logger, a risk manager) implements `EventBus` and gets called.

---

### `ofe::analytics` — The Intelligence Layer

#### `delta_engine.h` — Tracking Buying vs Selling Pressure
Delta = buy volume − sell volume for a bar. A strongly positive delta means buyers are aggressive; a negative delta means sellers are. The engine also tracks:
- **Bar delta** — delta for the current bar only
- **CVD** (Cumulative Volume Delta) — running total across all bars; divergences between CVD and price are a key signal
- **Max/min delta** — the range of delta within a bar

#### `imbalance_detector.h` — Finding Order Book Imbalances
An imbalance occurs when one side of the order book at a price level is overwhelmed by the opposite side at the *diagonal* price level. Concretely:

- **Buy imbalance**: the bid volume at price P−1 is ≥ 3× the ask volume at price P (buyers stacking below the offer)
- **Sell imbalance**: the ask volume at price P+1 is ≥ 3× the bid volume at price P (sellers stacking above the bid)

Multiple consecutive imbalances form a **stack**. When both sides are simultaneously heavy it is called **absorption** — large passive players absorbing aggressive flow.

#### `volume_profile.h` — Where Did Volume Cluster?
A volume profile is a histogram of volume by price level for a session or bar. Key concepts:
- **POC** (Point of Control) — the price level with the most volume traded; acts as a magnet
- **Value Area** — the range of prices that contains 70% of the session's volume; the market's accepted fair value zone
- **Value Area High / Low** — the boundaries of the value area; breakouts above/below are significant

#### `vwap_engine.h` — Volume-Weighted Average Price
**VWAP** is the average price weighted by volume. Institutional traders use it as a benchmark. The engine also computes standard deviation bands (±1σ, ±2σ) around VWAP — trades above the upper band are stretched long; trades below the lower band are stretched short.

#### `signal_detector.h` — Combining Everything into a Score
The `SignalDetector` takes outputs from all the analytics above and produces four composite scores:

| Score | Meaning |
|-------|---------|
| **Pulse** | Short-term directional momentum (delta + imbalance + VWAP displacement) |
| **Turns** | Potential reversal signal (delta divergence + surge) |
| **COT** | Commitment-of-traders directional bias from Lee-Ready classification |
| **Sweep** | Aggressive absorption or stacking activity |

These scores are combined into a `SignalEvent` with a `strength` (0–1) and `confidence` field, and a `SignalType` enum that names the pattern (e.g. `PulseLong`, `SweepBuy`, `TurnShort`).

---

### Supporting Modules

#### `ofe::signals` — Signal Taxonomy
Defines 60+ named signal types (`BuyImbalance`, `CvdBullish`, `LiquidityGrabLong`, etc.) and `ImbalanceZone` classifications. All signal events carry timestamps, symbol IDs, scores, and zone labels.

#### `ofe::config` — Engine Configuration
Parses a YAML file into an `EngineConfig` struct. Covers everything: thread counts, bar thresholds, signal thresholds, tier quotas, API port, feature flags (enable/disable VWAP, delta, Lee-Ready, etc.).

#### `ofe::license` — Hardware-Bound Licensing
Collects a hardware fingerprint (CPU ID, MAC address, disk serial, machine ID) and exposes an interface for RSA-2048 token verification against that fingerprint. Concrete verification is injected via `ILicenseVerifier`.

#### `ofe::api` — Symbol Quota Enforcement
Controls how many symbols each client tier can subscribe to:

| Tier | Default Limit |
|------|--------------|
| Bronze | 32 symbols |
| Silver | 128 symbols |
| Gold | 512 symbols |
| Platinum | 2048 symbols |

#### `ofe::feed` — Data Feed Adapters
Defines the `IDataFeedAdapter` interface that any market data feed (Kinetick, eSignal, IQFeed, etc.) must implement to plug into the engine.

---

## Test Suite — All 56 Tests Explained

The tests live in `test/engine_tests.cpp` and use Google Test. Run them with:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Or run a specific group:

```bash
./build/ofe_tests --gtest_filter="VwapEngineTest.*"
```

### RingBuffer (9 tests)
Verifies the lock-free queue is correct under both single-threaded and concurrent use.
- Correct capacity, empty state, and size reporting
- `try_push` returns false when the buffer is full
- `try_pop` returns false when the buffer is empty
- A single push-then-pop roundtrip works correctly
- The circular index wraps around without corrupting data
- `clear()` drains all items
- **Concurrent test**: 100,000 items produced and consumed across two threads arrive in the correct order with nothing lost

### TickRecord (3 tests)
Verifies the struct layout guarantees that the compiler cannot silently break.
- Exactly 64 bytes in size
- Exactly 64-byte aligned (one cache line)
- All fields zero-initialize by default

### TickRouter (4 tests)
Verifies ticks land in the correct shard.
- `symbol_id % shard_count` maps symbols to shards correctly
- A dispatched tick can be popped from the correct shard (and not from any other)
- Popping from an empty shard returns false
- Default construction creates 64 shards

### LeeReadyClassifier (4 tests)
Verifies the trade direction inference rules.
- A trade at the ask price is classified as Buy
- A trade at the bid price is classified as Sell
- A trade between bid and ask but closer to the ask is classified as Buy (tick test)
- When both standard rules fail, a caller-supplied override is used

### BarSeries (7 tests)
Verifies bar construction across all three bar types.
- A volume bar does not close early and closes exactly at the volume threshold
- A range bar closes when the high−low range reaches the threshold
- A time bar closes when the next tick arrives after the time window has elapsed
- Open, High, Low, Close are tracked correctly across ticks
- VWAP equals the volume-weighted average of all trade prices in the bar
- Buy volume and sell volume are accumulated from tick direction (price vs bid/ask)
- Delta (buy − sell) is correct at bar close

### DeltaEngine (5 tests)
Verifies delta and CVD bookkeeping.
- A single update sets bar delta, CVD, and update count correctly
- Two opposing updates net CVD to zero
- Max delta tracks the highest delta seen across updates
- Min delta tracks the lowest (most negative) delta seen
- Reset clears all state back to zero

### ImbalanceDetector (5 tests)
Verifies the diagonal comparison logic.
- `is_buy_imbalance` and `is_sell_imbalance` return true when the ratio meets the threshold and false when it does not
- When ask volume is zero, the alternate code path fires correctly
- `compare()` returns accurate diagonal ratios (bid÷ask, ask÷bid)
- `compare()` correctly sets the stacked flag when either imbalance is present
- Absorption is detected when both sides of the book exceed the opposite side

### VolumeProfile (5 tests)
Verifies POC and value area calculation.
- The 70% value area expands symmetrically around the POC (the original 3-test fixture)
- An empty profile returns a zero snapshot without crashing
- The POC is always the price level with the highest volume
- Adding the same price twice accumulates volume on that level
- Reset clears the profile completely

### VwapEngine (5 tests)
Verifies VWAP and band computation.
- A single trade's VWAP equals its own price
- Two trades at different prices and equal volume → VWAP is their arithmetic mean
- Adding a trade with zero volume is silently ignored
- Upper/lower bands are exactly ±1σ and ±2σ around VWAP
- Reset returns VWAP and volume to zero

### SignalDetector (5 tests)
Verifies the composite scoring and signal selection.
- Pulse score is always clamped to [−1, 1] even with extreme inputs
- Sweep score is exactly 1.0 when the absorption flag is set
- COT score is exactly +1.0 for a Buy-direction trade and −1.0 for Sell
- `evaluate()` correctly populates all fields of the returned `SignalEvent`
- `strength` is in [0, 1] and equals `confidence`

### SymbolQuotaEnforcer (2 tests)
Verifies the per-tier subscription limit.
- Allocation is allowed when the current count is below the tier's limit
- Allocation is rejected at or above the tier's limit, and custom limits set via `set_limits()` are respected immediately

### LicenseEngine (2 tests)
Verifies the token verification delegation pattern.
- Without an injected verifier, `verify_token()` returns false (fail-safe)
- With an injected `ILicenseVerifier` implementation, the call is correctly delegated and the verifier's result is returned

---

## Project Structure

```
include/
  ofe/
    core/
      tick_record.h       # UniversalTickRecord struct (64 bytes, cache-aligned)
      ring_buffer.h       # Lock-free SPSC ring buffer
      tick_router.h       # Symbol-to-shard dispatch
      lee_ready.h         # Trade direction classifier
      bar_engine.h        # Time / Range / Volume bar construction
      bar_types.h         # BarRecord and BarType definitions
      symbol_worker.h     # 11-stage per-symbol pipeline thread
      event_bus.h         # Abstract signal publisher interface
    analytics/
      delta_engine.h      # Buy/sell delta and CVD tracking
      imbalance_detector.h # Diagonal order book imbalance detection
      volume_profile.h    # POC and value area computation
      vwap_engine.h       # VWAP with standard deviation bands
      signal_detector.h   # Composite signal scoring
    signals/
      signal_types.h      # SignalType enum, ImbalanceZone, SignalEvent struct
    config/
      engine_config.h     # YAML-based engine configuration
    feed/
      adapter_interface.h # IDataFeedAdapter for plugging in data sources
    license/
      license_engine.h    # Hardware fingerprint + RSA token verification
    api/
      api_server.h        # Symbol quota enforcement by tier
    ofe.h                 # Single include for the whole library
test/
  engine_tests.cpp        # 56 Google Test cases
CMakeLists.txt            # CMake build — fetches GoogleTest and yaml-cpp automatically
```
