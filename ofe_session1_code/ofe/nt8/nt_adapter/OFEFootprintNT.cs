// OFEFootprintNT.cs
// OFE Footprint Chart — native NinjaTrader 8 indicator.
//
// Works with ANY NT8 data provider (Schwab, Coinbase, IQFeed, Kinetick, eSignal…).
// No external server, no TCP connection, no C++ bridge required.
//
// How it works
// ────────────
//  1. OnMarketData() receives every raw tick NT8 gets from the connected data provider.
//  2. Each trade tick is classified as BUY or SELL using the last known bid/ask price
//     (Lee-Ready algorithm — same logic as the C++ LeeReadyClassifier).
//  3. Bid volume and ask volume are accumulated per price level per bar.
//  4. When a bar closes, delta / POC / diagonal imbalances are computed and the
//     completed bar is stored by NT8 bar index.
//  5. OnRender() draws footprint cells directly over NT8's own candles using SharpDX.
//
// ─── Installation ────────────────────────────────────────────────────────────
//   1. Copy OFEFootprintNT.cs to:
//      Documents\NinjaTrader 8\bin\Custom\Indicators\
//      (the other nt_adapter files are NOT needed — this file is self-contained)
//   2. Tools → NinjaScript Editor → Compile (Ctrl+F5)
//   3. Open any chart with a working data provider (Schwab, Coinbase, etc.)
//   4. Drag "OFE Footprint NT" onto the chart
//
// ─── Notes ───────────────────────────────────────────────────────────────────
//   • Requires tick-level data from the data provider (MarketDataType.Last events).
//   • Trade direction uses the last bid/ask price before each trade.
//     For providers that do not stream bid/ask separately, tick-test fallback is used.
//   • Footprint cells appear on all bars — historical bars via tick replay, live bar
//     while forming (delta label shows ~ suffix).
//   • For historical footprint: Chart Properties → Data → Tick Replay = Yes
//     (IsTickReplay is a strategy-only property; indicators cannot force it.)
//   • Tick replay needs stored tick history from the data provider (typically 7–30 days).

#region Using declarations
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.ComponentModel.DataAnnotations;
using NinjaTrader.Cbi;
using NinjaTrader.Data;
using NinjaTrader.Gui;
using NinjaTrader.Gui.Chart;
using NinjaTrader.NinjaScript;
using SharpDX;
using SharpDX.Direct2D1;
using SharpDX.DirectWrite;
#endregion

namespace NinjaTrader.NinjaScript.Indicators
{
    [Gui.CategoryOrder("Display",    1)]
    [Gui.CategoryOrder("Imbalance",  2)]
    [Gui.CategoryOrder("Colours",    3)]
    public class OFEFootprintNT : Indicator
    {
        // ── Properties ────────────────────────────────────────────────────────

        [NinjaScriptProperty]
        [Range(6, 24)]
        [Display(Name = "Cell Font Size", Order = 1, GroupName = "Display")]
        public int CellFontSize { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Delta Label", Order = 2, GroupName = "Display")]
        public bool ShowDeltaLabel { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show POC", Order = 3, GroupName = "Display")]
        public bool ShowPoc { get; set; }

        [NinjaScriptProperty]
        [Range(1.0, 10.0)]
        [Display(Name = "Imbalance Ratio", Order = 1, GroupName = "Imbalance")]
        public double ImbalanceRatio { get; set; }

        [NinjaScriptProperty]
        [Range(2, 5)]
        [Display(Name = "Stacked Threshold", Order = 2, GroupName = "Imbalance")]
        public int StackedThreshold { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Buy Colour",          Order = 1, GroupName = "Colours")]
        public System.Windows.Media.Color BuyColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Sell Colour",         Order = 2, GroupName = "Colours")]
        public System.Windows.Media.Color SellColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Imbalance Buy",       Order = 3, GroupName = "Colours")]
        public System.Windows.Media.Color ImbalanceBuyColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Imbalance Sell",      Order = 4, GroupName = "Colours")]
        public System.Windows.Media.Color ImbalanceSellColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Stacked Buy",         Order = 5, GroupName = "Colours")]
        public System.Windows.Media.Color StackedBuyColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Stacked Sell",        Order = 6, GroupName = "Colours")]
        public System.Windows.Media.Color StackedSellColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "POC Colour",          Order = 7, GroupName = "Colours")]
        public System.Windows.Media.Color PocColor { get; set; }

        // ── Per-price-level data within the current bar ────────────────────────

        private struct BarLevel
        {
            public double Price;
            public long   BidVol;
            public long   AskVol;
            public long   TotalVol => BidVol + AskVol;
            public long   Delta    => AskVol - BidVol;
        }

        // ── Completed bar snapshot ─────────────────────────────────────────────

        private sealed class BarSnapshot
        {
            public int    BarIndex;
            public double High, Low;
            public int    Delta;          // bar-level: sum(ask) - sum(bid)
            public long   Cvd;            // cumulative delta at bar close
            public double PocPrice;       // price level with highest TotalVol
            public List<BarLevel> Levels; // sorted ascending by price

            // Imbalance flags per level (parallel to Levels list)
            public bool[] IsBuyImbalance;
            public bool[] IsSellImbalance;
            public bool[] IsStackedBuy;
            public bool[] IsStackedSell;
        }

        // ── Accumulation state ─────────────────────────────────────────────────

        // Keyed by price-level integer (price / tickSize, rounded)
        private readonly Dictionary<long, BarLevel> _accumLevels
            = new Dictionary<long, BarLevel>(256);

        private int    _accumBarIndex = -1;  // which NT8 bar is being accumulated
        private double _lastBid;             // latest bid price (Lee-Ready)
        private double _lastAsk;             // latest ask price (Lee-Ready)
        private double _prevTradePrice;      // previous trade price (tick-test fallback)
        private long   _cvd;                 // cumulative delta (session)

        // ── Bar cache ─────────────────────────────────────────────────────────

        // Key = NT8 bar index; value = completed bar snapshot
        private readonly Dictionary<int, BarSnapshot> _barCache
            = new Dictionary<int, BarSnapshot>(512);

        // Sized in DataLoaded based on bar period (1-min→5d, 5-min→15d, daily→90d …)
        private int _maxCacheBars = 2000;

        // Live-bar snapshot cache — rebuilt only when a new tick arrives, not on every render
        private BarSnapshot _liveSnap;
        private bool        _liveDirty = true;

        // ── SharpDX resources ──────────────────────────────────────────────────

        private SolidColorBrush _buyBrush, _sellBrush;
        private SolidColorBrush _ibuyBrush, _isellBrush;
        private SolidColorBrush _sbuyBrush, _ssellBrush;
        private SolidColorBrush _pocBrush, _textBrush, _gridBrush, _dividerBrush;
        private SolidColorBrush _statusBgBrush, _statusTextBrush;

        private SharpDX.DirectWrite.Factory    _dw;
        private SharpDX.DirectWrite.TextFormat _cellFmt, _statusFmt;

        // ── NT8 lifecycle ──────────────────────────────────────────────────────

        protected override void OnStateChange()
        {
            if (State == State.SetDefaults)
            {
                Name        = "OFE Footprint NT";
                Description = "OFE footprint chart — reads from any NT8 data provider.";
                Calculate   = Calculate.OnEachTick;
                // IsTickReplay is a Strategy-only property — not settable on indicators.
                // To get historical footprint data: Chart Properties → Data → Tick Replay = Yes
                IsOverlay   = true;
                IsAutoScale = false;
                DrawOnPricePanel             = true;
                IsSuspendedWhileInactive     = false;
                ScaleJustification           = ScaleJustification.Right;

                CellFontSize      = 10;
                ShowDeltaLabel    = true;
                ShowPoc           = true;
                ImbalanceRatio    = 3.0;
                StackedThreshold  = 3;

                BuyColor          = System.Windows.Media.Color.FromRgb(0x21, 0x96, 0xF3);
                SellColor         = System.Windows.Media.Color.FromRgb(0xEF, 0x53, 0x50);
                ImbalanceBuyColor = System.Windows.Media.Color.FromRgb(0x15, 0x65, 0xC0);
                ImbalanceSellColor= System.Windows.Media.Color.FromRgb(0xB7, 0x1C, 0x1C);
                StackedBuyColor   = System.Windows.Media.Color.FromRgb(0x0D, 0x47, 0xA1);
                StackedSellColor  = System.Windows.Media.Color.FromRgb(0x7B, 0x00, 0x00);
                PocColor          = System.Windows.Media.Color.FromRgb(0xFF, 0xEB, 0x3B);
            }
            else if (State == State.DataLoaded)
            {
                // NT8 tells us exactly how many bars it loaded — keep all of them,
                // then add a live-session buffer on top so we never evict during
                // historical tick replay and only roll off oldest bars during live trading.
                int loadedBars = BarsArray[0].Count;
                int liveBuffer = ComputeLiveBuffer();
                _maxCacheBars  = loadedBars + liveBuffer;
                Print($"[OFE] {Instrument.MasterInstrument.Name}  {BarsPeriod}"
                    + $"  tick={Instrument.MasterInstrument.TickSize}"
                    + $"  loaded={loadedBars}  liveBuffer={liveBuffer}");
            }
            else if (State == State.Terminated)
            {
                DisposeResources();
            }
        }

        // ── OnBarUpdate — detect bar close ─────────────────────────────────────

        protected override void OnBarUpdate()
        {
            // Nothing to do here — all tick work happens in OnMarketData.
            // Bar-close detection happens there too (CurrentBar change).
        }

        // ── OnMarketData — core tick processing ────────────────────────────────

        protected override void OnMarketData(MarketDataEventArgs e)
        {
            // Track latest bid / ask for Lee-Ready classification
            if (e.MarketDataType == MarketDataType.Bid)
            {
                _lastBid = e.Price;
                return;
            }
            if (e.MarketDataType == MarketDataType.Ask)
            {
                _lastAsk = e.Price;
                return;
            }

            // Only process trade prints (Last)
            if (e.MarketDataType != MarketDataType.Last) return;

            // ── Detect bar change → close previous bar ─────────────────────────
            if (CurrentBar != _accumBarIndex)
            {
                // Finalize the bar that just closed (if we were accumulating one)
                if (_accumBarIndex >= 0 && _accumLevels.Count > 0)
                    CloseBar(_accumBarIndex);

                // Clear accumulation for the new bar
                _accumLevels.Clear();
                _accumBarIndex = CurrentBar;
            }

            // ── Classify trade direction (Lee-Ready) ───────────────────────────
            bool isBuy;
            if (_lastAsk > 0 && e.Price >= _lastAsk)
                isBuy = true;
            else if (_lastBid > 0 && e.Price <= _lastBid)
                isBuy = false;
            else
                isBuy = e.Price >= _prevTradePrice;   // tick-test fallback

            _prevTradePrice = e.Price;

            // ── Accumulate into current bar ────────────────────────────────────
            double tickSize = Instrument.MasterInstrument.TickSize;
            long   priceKey = PriceKey(e.Price, tickSize);

            BarLevel lv;
            if (!_accumLevels.TryGetValue(priceKey, out lv))
                lv = new BarLevel { Price = Math.Round(e.Price / tickSize) * tickSize };

            long vol = Math.Max(1L, (long)e.Volume);
            if (isBuy) lv.AskVol += vol;
            else        lv.BidVol += vol;

            _accumLevels[priceKey] = lv;
            _liveDirty = true;
        }

        // ── Bar-close processing ───────────────────────────────────────────────

        private void CloseBar(int barIdx)
        {
            var snap = BuildSnapshot(barIdx, _accumLevels);
            if (snap == null) return;
            _cvd    += snap.Delta;
            snap.Cvd = _cvd;
            _barCache[barIdx] = snap;
            _liveSnap  = null;     // bar closed — live cache is stale
            _liveDirty = true;
            if (_barCache.Count > _maxCacheBars) EvictOldest();
        }

        private BarSnapshot BuildSnapshot(int barIdx, Dictionary<long, BarLevel> source)
        {
            if (source.Count == 0) return null;

            var levels = new List<BarLevel>(source.Values);
            levels.Sort((a, b) => a.Price.CompareTo(b.Price));
            int n = levels.Count;

            double pocPrice = levels[0].Price;
            long   pocVol = 0, totalAsk = 0, totalBid = 0;
            foreach (var lv in levels)
            {
                if (lv.TotalVol > pocVol) { pocVol = lv.TotalVol; pocPrice = lv.Price; }
                totalAsk += lv.AskVol;
                totalBid += lv.BidVol;
            }

            var isBuy  = new bool[n];
            var isSell = new bool[n];
            for (int i = 0; i < n; i++)
            {
                if (i > 0 && levels[i - 1].BidVol > 0)
                    isBuy[i]  = levels[i].AskVol >= ImbalanceRatio * levels[i - 1].BidVol;
                if (i < n - 1 && levels[i + 1].AskVol > 0)
                    isSell[i] = levels[i].BidVol >= ImbalanceRatio * levels[i + 1].AskVol;
            }

            var isSB = new bool[n];
            var isSS = new bool[n];
            int consec = 0;
            for (int i = 0; i < n; i++)
            {
                if (isBuy[i]) consec++; else consec = 0;
                if (consec >= StackedThreshold)
                    for (int k = i - consec + 1; k <= i; k++) isSB[k] = true;
            }
            consec = 0;
            for (int i = n - 1; i >= 0; i--)
            {
                if (isSell[i]) consec++; else consec = 0;
                if (consec >= StackedThreshold)
                    for (int k = i + consec - 1; k >= i; k--) isSS[k] = true;
            }

            double barHigh = barIdx < BarsArray[0].Count ? BarsArray[0].GetHigh(barIdx) : levels[n - 1].Price;
            double barLow  = barIdx < BarsArray[0].Count ? BarsArray[0].GetLow(barIdx)  : levels[0].Price;

            return new BarSnapshot
            {
                BarIndex        = barIdx,
                High            = barHigh,
                Low             = barLow,
                Delta           = (int)(totalAsk - totalBid),
                PocPrice        = pocPrice,
                Levels          = levels,
                IsBuyImbalance  = isBuy,
                IsSellImbalance = isSell,
                IsStackedBuy    = isSB,
                IsStackedSell   = isSS,
            };
        }

        // ── OnRenderTargetChanged ──────────────────────────────────────────────

        public override void OnRenderTargetChanged()
        {
            DisposeResources();
            if (RenderTarget == null) return;

            _buyBrush    = Mk(BuyColor);
            _sellBrush   = Mk(SellColor);
            _ibuyBrush   = Mk(ImbalanceBuyColor);
            _isellBrush  = Mk(ImbalanceSellColor);
            _sbuyBrush   = Mk(StackedBuyColor);
            _ssellBrush  = Mk(StackedSellColor);
            _pocBrush    = Mk(PocColor);
            _textBrush    = new SolidColorBrush(RenderTarget, SharpDX.Color4.White);
            _gridBrush    = new SolidColorBrush(RenderTarget, new SharpDX.Color4(1f, 1f, 1f, 0.12f));
            _dividerBrush = new SolidColorBrush(RenderTarget, new SharpDX.Color4(1f, 1f, 1f, 0.5f));
            _statusBgBrush   = new SolidColorBrush(RenderTarget, new SharpDX.Color4(0f, 0f, 0f, 0.65f));
            _statusTextBrush = new SolidColorBrush(RenderTarget, new SharpDX.Color4(0f, 0.85f, 0.3f, 1f));

            _dw        = new SharpDX.DirectWrite.Factory();
            _cellFmt   = MkFmt((float)CellFontSize, SharpDX.DirectWrite.TextAlignment.Center);
            _statusFmt = MkFmt(11f, SharpDX.DirectWrite.TextAlignment.Leading);
        }

        // ── OnRender ──────────────────────────────────────────────────────────

        protected override void OnRender(ChartControl cc, ChartScale cs)
        {
            if (RenderTarget == null || _buyBrush == null) return;
            if (ChartBars == null || BarsArray == null || BarsArray.Length == 0) return;

            int firstBar  = ChartBars.FromIndex;
            int lastBar   = ChartBars.ToIndex;
            if (BarsArray[0].Count < 1) return;

            RenderStatus(cc);

            double tickSize = Instrument.MasterInstrument.TickSize;

            // Closed bars
            for (int barIdx = firstBar; barIdx <= lastBar; barIdx++)
            {
                BarSnapshot snap;
                if (!_barCache.TryGetValue(barIdx, out snap)) continue;
                if (snap.Levels == null || snap.Levels.Count == 0) continue;
                DrawBarSnapshot(cc, cs, tickSize, snap, isLive: false);
            }

            // Live (forming) bar — rebuild snapshot only when a new tick arrived
            if (_accumLevels.Count > 0
                && _accumBarIndex >= firstBar && _accumBarIndex <= lastBar
                && !_barCache.ContainsKey(_accumBarIndex))
            {
                if (_liveDirty)
                {
                    _liveSnap  = BuildSnapshot(_accumBarIndex, _accumLevels);
                    _liveDirty = false;
                }
                if (_liveSnap != null)
                    DrawBarSnapshot(cc, cs, tickSize, _liveSnap, isLive: true);
            }
        }

        private void DrawBarSnapshot(ChartControl cc, ChartScale cs,
                                     double tickSize, BarSnapshot snap, bool isLive)
        {
            if (snap.Levels == null || snap.Levels.Count == 0) return;

            float xCenter = cc.GetXByBarIndex(ChartBars, snap.BarIndex);
            float halfW   = (float)cc.GetBarPaintWidth(ChartBars) * 0.45f;
            if (halfW < 2f) return;

            float minH = CellFontSize + 4f;
            float minW = CellFontSize * 3.5f;
            int   n    = snap.Levels.Count;

            // Pixel height of one tick at current zoom level
            float tickPx = Math.Abs(cs.GetYByValue(snap.Levels[0].Price + tickSize)
                                  - cs.GetYByValue(snap.Levels[0].Price));
            tickPx = Math.Max(0.1f, tickPx);

            // Merge adjacent tick levels so each display row is at least minH px tall.
            // When zoomed in (tickPx >= minH) merge = 1 = full per-tick detail.
            int  merge       = Math.Max(1, (int)Math.Ceiling(minH / tickPx));
            bool canShowText = (tickPx * merge >= minH) && (halfW >= minW);

            for (int i = 0; i < n; i += merge)
            {
                int end = Math.Min(i + merge, n);

                // Accumulate volumes and flags over merged tick range
                long bidSum = 0, askSum = 0;
                bool hasBuyImb = false, hasSellImb = false;
                bool hasStkBuy = false, hasStkSell = false;
                bool hasPoc    = false;

                for (int j = i; j < end; j++)
                {
                    bidSum    += snap.Levels[j].BidVol;
                    askSum    += snap.Levels[j].AskVol;
                    hasBuyImb  |= snap.IsBuyImbalance[j];
                    hasSellImb |= snap.IsSellImbalance[j];
                    hasStkBuy  |= snap.IsStackedBuy[j];
                    hasStkSell |= snap.IsStackedSell[j];
                    if (Math.Abs(snap.Levels[j].Price - snap.PocPrice) < tickSize * 0.5)
                        hasPoc = true;
                }

                // Pixel bounds: levels[end-1] is the highest price in this row
                float yTop  = cs.GetYByValue(snap.Levels[end - 1].Price + tickSize * 0.5);
                float yBot  = cs.GetYByValue(snap.Levels[i].Price       - tickSize * 0.5);
                float cellH = Math.Max(1f, yBot - yTop);

                var leftRect  = new SharpDX.RectangleF(xCenter - halfW, yTop, halfW, cellH);
                var rightRect = new SharpDX.RectangleF(xCenter,         yTop, halfW, cellH);

                // Left cell = bid (sell-side pressure) — always red-family
                SolidColorBrush leftBg  = hasStkSell ? _ssellBrush
                                        : hasSellImb  ? _isellBrush
                                        :               _sellBrush;
                // Right cell = ask (buy-side pressure) — always blue-family
                SolidColorBrush rightBg = hasStkBuy   ? _sbuyBrush
                                        : hasBuyImb    ? _ibuyBrush
                                        :                _buyBrush;

                RenderTarget.FillRectangle(leftRect,  leftBg);
                RenderTarget.FillRectangle(rightRect, rightBg);

                // Horizontal row separator at top edge
                if (cellH >= 2f)
                    RenderTarget.DrawLine(new SharpDX.Vector2(xCenter - halfW, yTop),
                                          new SharpDX.Vector2(xCenter + halfW, yTop),
                                          _gridBrush, 0.5f);

                // Vertical centre divider — clearly separates bid (left) from ask (right)
                RenderTarget.DrawLine(new SharpDX.Vector2(xCenter, yTop),
                                      new SharpDX.Vector2(xCenter, yBot),
                                      _dividerBrush, 1f);

                // Numbers only when each row is tall enough and bar is wide enough
                if (canShowText)
                {
                    RenderTarget.DrawText(bidSum.ToString(), _cellFmt, leftRect,  _textBrush);
                    RenderTarget.DrawText(askSum.ToString(), _cellFmt, rightRect, _textBrush);
                }

                // POC highlight
                if (ShowPoc && hasPoc)
                {
                    var pocRect = new SharpDX.RectangleF(xCenter - halfW, yTop, halfW * 2f, cellH);
                    _pocBrush.Opacity = 0.4f;
                    RenderTarget.FillRectangle(pocRect, _pocBrush);
                    _pocBrush.Opacity = 1.0f;
                    RenderTarget.DrawRectangle(pocRect, _pocBrush, 1.5f);
                }
            }

            // Delta label centred below the bar
            if (ShowDeltaLabel && halfW >= 10f)
            {
                float  yDelta = cs.GetYByValue(snap.Low) + 2f;
                string suffix = isLive ? "~" : "";
                string dLabel = snap.Delta >= 0
                    ? "+" + snap.Delta + suffix
                    :        snap.Delta + suffix;
                var dRect = new SharpDX.RectangleF(xCenter - halfW, yDelta,
                                                   halfW * 2f, CellFontSize + 4f);
                RenderTarget.DrawText(dLabel, _cellFmt, dRect, _textBrush);
            }
        }

        // ── Status overlay ─────────────────────────────────────────────────────

        private void RenderStatus(ChartControl cc)
        {
            if (_statusBgBrush == null || _statusFmt == null) return;
            float w = (float)cc.ActualWidth;
            string txt = $"  OFE Footprint  │  {Instrument.MasterInstrument.Name}  {BarsPeriod}"
                       + $"  │  Bars: {_barCache.Count}/{_maxCacheBars}"
                       + $"  │  CVD: {_cvd:+#;-#;0}";
            RenderTarget.FillRectangle(new SharpDX.RectangleF(0f, 0f, w, 22f), _statusBgBrush);
            RenderTarget.DrawText(txt, _statusFmt,
                new SharpDX.RectangleF(4f, 3f, w - 8f, 18f), _statusTextBrush);
        }

        // ── Helpers ────────────────────────────────────────────────────────────

        private static long PriceKey(double price, double tickSize)
            => (long)Math.Round(price / tickSize);

        private SharpDX.Direct2D1.SolidColorBrush Mk(System.Windows.Media.Color c)
            => new SharpDX.Direct2D1.SolidColorBrush(RenderTarget,
               new SharpDX.Color4(c.R / 255f, c.G / 255f, c.B / 255f, c.A / 255f));

        private SharpDX.DirectWrite.TextFormat MkFmt(float size,
            SharpDX.DirectWrite.TextAlignment align)
        {
            var f = new SharpDX.DirectWrite.TextFormat(
                _dw, "Consolas",
                SharpDX.DirectWrite.FontWeight.Normal,
                SharpDX.DirectWrite.FontStyle.Normal, size);
            f.TextAlignment      = align;
            f.ParagraphAlignment = SharpDX.DirectWrite.ParagraphAlignment.Center;
            return f;
        }

        private void EvictOldest()
        {
            var keys = new List<int>(_barCache.Keys);
            keys.Sort();
            // Evict 10 % of current cache in one pass to amortise the sort cost
            int toRemove = Math.Max(50, keys.Count / 10);
            for (int i = 0; i < Math.Min(toRemove, keys.Count); i++)
                _barCache.Remove(keys[i]);
        }

        // Returns the number of additional live bars to keep beyond the initially
        // loaded historical set — sized to cover one full session of live trading
        // before oldest history starts rolling off.
        private int ComputeLiveBuffer()
        {
            if (BarsPeriod == null) return 500;

            switch (BarsPeriod.BarsPeriodType)
            {
                case BarsPeriodType.Second:
                {
                    int secs = Math.Max(1, BarsPeriod.Value);
                    // 1 full 24-hour session (futures / crypto always run 24 h)
                    return Math.Max(200, 86400 / secs);
                }
                case BarsPeriodType.Minute:
                {
                    int mins = Math.Max(1, BarsPeriod.Value);
                    // 1 full 24-hour session — covers both equity (6.5 h) and
                    // futures/crypto (23-24 h) without having to detect the session type
                    return Math.Max(100, 1440 / mins);
                }
                case BarsPeriodType.Day:   return   5;  // 1 trading week
                case BarsPeriodType.Week:  return   4;  // 1 month of weeks
                case BarsPeriodType.Month: return   3;  // 1 quarter
                // Tick / Volume / Range bars: rate varies wildly; give a generous buffer
                default:                   return 2000;
            }
        }

        private void DisposeResources()
        {
            _buyBrush?.Dispose();   _sellBrush?.Dispose();
            _ibuyBrush?.Dispose();  _isellBrush?.Dispose();
            _sbuyBrush?.Dispose();  _ssellBrush?.Dispose();
            _pocBrush?.Dispose();   _textBrush?.Dispose();
            _gridBrush?.Dispose();  _dividerBrush?.Dispose();
            _statusBgBrush?.Dispose(); _statusTextBrush?.Dispose();
            _cellFmt?.Dispose();    _statusFmt?.Dispose();
            _dw?.Dispose();
            _buyBrush = _sellBrush = _ibuyBrush = _isellBrush =
            _sbuyBrush = _ssellBrush = _pocBrush = _textBrush =
            _gridBrush = _dividerBrush = _statusBgBrush = _statusTextBrush = null;
            _cellFmt = _statusFmt = null; _dw = null;
        }

    }
}
