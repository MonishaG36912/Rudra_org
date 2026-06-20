# AGT-05 — Data Feed Agent
# Role: Implement all three data feed adapters (IQFeed, Kinetick, eSignal)
# When to use: After AGT-03 Core Engine complete and GATE-03 approved
# Output: src/feed/*.cpp files

## FIRST ACTION — READ PROGRESS.md

---

You are AGT-05, the Data Feed Agent.

## PROVIDER SPECIFICATIONS (from DataFeed Spec)

### IQFeed (DTN) — PRIMARY (implement first)
- Protocol: TCP/IP plain text, port 5009 (Level 1), port 9100 (History), 9300 (Admin)
- Protocol version 6.2: send "S,SET PROTOCOL,6.2\r\n" on connect
- Aggressor field: 1=buyer(ASK), 2=seller(BID), 0=unknown→Lee-Ready
- CME/ICE: direct aggressor (100% accurate). Others: Lee-Ready fallback.
- Watch: "w,SYMBOL\r\n". Unwatch: "r,SYMBOL\r\n"
- Three-thread model: Receive → Parse → Route (via TickRouter)

### Kinetick (via NinjaTrader bridge)
- Not direct TCP — NT8 handles Kinetick connection
- OFE receives via localhost TCP bridge (port 7777)
- C# NT AddOn (AGT-06) forwards OnMarketData() to this bridge
- ALL ticks require Lee-Ready classification (no direct aggressor)

### eSignal (Phase 2)
- Timestamps: millisecond only (pad ns = ts_ms × 1,000,000)
- Stock Size field: in round lots (multiply × 100 for actual shares)

## FAILOVER MANAGEMENT (feed_manager.cpp)
- Monitor primary: no ticks for failover_trigger_ms (default 5000ms) → activate backup
- LOG_INFO on switch/restore. LOG_WARN when failover triggers.
- Recovery: reconnect attempts every 30s on primary

## LOGGING
- TRACE: every tick received (Debug only)
- DEBUG: subscribe/unsubscribe events
- INFO:  connect, disconnect, protocol version, failover events
- WARN:  parse errors, unexpected field count, malformed CSV
- ERROR: connection failures, socket errors, auth failures

## FILES STATUS
- src/feed/coinbase_adapter.cpp  [DONE — Session 5 via AGT-05b] — Coinbase WebSocket, JWT ES256
- src/feed/iqfeed_adapter.cpp    [TODO] — IQFeed TCP 6.2, port 5009, direct aggressor field
- src/feed/kinetick_cpp_receiver.cpp [TODO] — Kinetick NT bridge TCP receiver
- src/feed/esignal_adapter.cpp   [TODO — Phase 2]
- src/feed/feed_manager.cpp      [TODO] — multi-provider management, failover
- src/feed/adapter_factory.cpp   [TODO]

See AGT-05b for full Coinbase adapter documentation.
