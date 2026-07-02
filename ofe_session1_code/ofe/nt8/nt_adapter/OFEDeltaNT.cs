// OFEDeltaNT.cs — Delta Candle Sub-Panel, NinjaTrader 8
//
// Renders per-bar delta as OHLC candles + CVD line in a dedicated sub-panel.
//
// Delta OHLC per bar:
//   Open  = 0          (resets every bar — body drawn from zero line)
//   High  = max running delta during bar  (upper wick tip)
//   Low   = min running delta during bar  (lower wick tip)
//   Close = total ask vol − total bid vol (body top or bottom)
//
// Any NT8 data provider (Schwab, Coinbase, IQFeed, Kinetick, etc.)
// Requires: Chart Properties → Data → Tick Replay = Yes (for historical bars)
// Install:  copy to Documents\NinjaTrader 8\bin\Custom\Indicators\ → Ctrl+F5
//┌─────────────────────────────────────────────────── Price chart ─────┐
//│  candles from Schwab / Coinbase / any provider                       │
//├─────────────────────────────────────────────────── OFE Delta NT ─────┤
//│                              ┐                                        │
//│         │                   │ wick (MaxRunΔ)                         │
//│   ┌─────┤ body              │                                        │
//│ ──┤(+Δ) │─────── zero ──────│──── zero ─────────── (gold CVD line)  │
//│   └─────┘                   │ body                                   │
//│                          ────┤(−Δ)                                   │
//│                              │ wick (MinRunΔ)                        │
//│                              ┘                                        │
//│                                                    CVD +1234 •        │
//└───────────────────────────────────────────────────────────────────────┘


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
using SharpDX.DirectWrite;
#endregion

namespace NinjaTrader.NinjaScript.Indicators
{
    public class OFEDeltaNT : Indicator
    {
        // ══════════════════════════════════════════════════════════════════════
        //  PROPERTIES
        // ══════════════════════════════════════════════════════════════════════

        [NinjaScriptProperty]
        [Display(Name = "Bull Delta Color", Order = 1, GroupName = "Colours",
            Description = "Candle color when bar delta ≥ 0 (net buying pressure).")]
        public System.Windows.Media.Color BullColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Bear Delta Color", Order = 2, GroupName = "Colours",
            Description = "Candle color when bar delta < 0 (net selling pressure).")]
        public System.Windows.Media.Color BearColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "CVD Line Color", Order = 3, GroupName = "Colours",
            Description = "Color of the Cumulative Volume Delta line.")]
        public System.Windows.Media.Color CvdColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Zero Line Color", Order = 4, GroupName = "Colours")]
        public System.Windows.Media.Color ZeroLineColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show CVD Line", Order = 1, GroupName = "Display",
            Description = "Overlay cumulative delta as a gold line on the delta candles.")]
        public bool ShowCvd { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Zero Line", Order = 2, GroupName = "Display",
            Description = "Draw a horizontal reference line at delta = 0.")]
        public bool ShowZeroLine { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Delta Value", Order = 3, GroupName = "Display",
            Description = "Print the bar delta value above/below each candle body.")]
        public bool ShowDeltaValue { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show CVD Value", Order = 4, GroupName = "Display",
            Description = "Print the running CVD value next to the last CVD dot.")]
        public bool ShowCvdValue { get; set; }

        [NinjaScriptProperty][Range(20, 95)]
        [Display(Name = "Candle Width %", Order = 5, GroupName = "Display",
            Description = "Width of each candle body as a % of bar spacing (default 60).")]
        public int CandleWidthPct { get; set; }

        // ══════════════════════════════════════════════════════════════════════
        //  INNER TYPE
        // ══════════════════════════════════════════════════════════════════════

        private struct DeltaBar
        {
            public long High;   // max running delta during bar
            public long Low;    // min running delta during bar
            public long Close;  // final bar delta  (= total ask vol - total bid vol)
            public long Cvd;    // cumulative delta at bar close
        }

        // ══════════════════════════════════════════════════════════════════════
        //  STATE
        // ══════════════════════════════════════════════════════════════════════

        private readonly Dictionary<int, DeltaBar> _cache = new Dictionary<int, DeltaBar>(1024);

        private long   _runDelta;
        private long   _barHigh;
        private long   _barLow;
        private long   _cvd;
        private int    _accumIdx     = -1;
        private long   _prevCumVol   = 0;
        private double _lastTickPrice = 0;
        private bool   _prevIsBuy    = false;

        // ══════════════════════════════════════════════════════════════════════
        //  SharpDX RESOURCES
        // ══════════════════════════════════════════════════════════════════════

        private SharpDX.Direct2D1.SolidColorBrush _bullFill, _bearFill;
        private SharpDX.Direct2D1.SolidColorBrush _bullEdge, _bearEdge;
        private SharpDX.Direct2D1.SolidColorBrush _cvdBrush, _zeroBrush, _textBrush, _wickBrush;
        private SharpDX.DirectWrite.Factory    _dw;
        private SharpDX.DirectWrite.TextFormat _valFmt, _cvdFmt;

        // ══════════════════════════════════════════════════════════════════════
        //  LIFECYCLE
        // ══════════════════════════════════════════════════════════════════════

        protected override void OnStateChange()
        {
            if (State == State.SetDefaults)
            {
                Name                     = "OFE Delta NT";
                Description              = "Delta OHLC candles (Open=0, High=MaxRunΔ, Low=MinRunΔ, Close=BarΔ) + CVD line.";
                Calculate                = Calculate.OnEachTick;
                IsOverlay                = false;
                DrawOnPricePanel         = false;
                IsAutoScale              = true;
                IsSuspendedWhileInactive = false;

                BullColor      = System.Windows.Media.Color.FromRgb(0x21, 0x96, 0xF3); // blue
                BearColor      = System.Windows.Media.Color.FromRgb(0xEF, 0x53, 0x50); // red
                CvdColor       = System.Windows.Media.Color.FromRgb(0xFF, 0xD7, 0x00); // gold
                ZeroLineColor  = System.Windows.Media.Color.FromArgb(0x80, 0xAA, 0xAA, 0xAA); // gray 50%
                ShowCvd        = true;
                ShowZeroLine   = true;
                ShowDeltaValue = true;
                ShowCvdValue   = true;
                CandleWidthPct = 60;

                // Anchor plots — invisible, but drive NT8 auto-scale for this sub-panel
                AddPlot(new Stroke(System.Windows.Media.Brushes.Transparent, 1), PlotStyle.Line, "DeltaHigh");
                AddPlot(new Stroke(System.Windows.Media.Brushes.Transparent, 1), PlotStyle.Line, "DeltaLow");
            }
            else if (State == State.DataLoaded)
            {
                _cvd = 0;
                Print($"[OFE Delta] {Instrument.MasterInstrument.Name}  {BarsPeriod}"
                    + $"  loaded={BarsArray[0].Count}");
            }
            else if (State == State.Terminated)
            {
                DisposeResources();
            }
        }

        // ══════════════════════════════════════════════════════════════════════
        //  TICK PROCESSING  (OnBarUpdate only — OnEachTick fires for every tick)
        // ══════════════════════════════════════════════════════════════════════

        protected override void OnBarUpdate()
        {
            if (BarsInProgress != 0) return;

            // Bar transition: finalise the bar that just closed
            if (IsFirstTickOfBar && _accumIdx >= 0)
            {
                StoreBar(_accumIdx);
                _runDelta      = 0;
                _barHigh       = 0;
                _barLow        = 0;
                _prevCumVol    = 0;
                _lastTickPrice = 0;
            }
            _accumIdx = CurrentBar;

            // Per-tick volume = delta of the bar's running cumulative volume
            long cumVol  = (long)Volume[0];
            long tickVol = Math.Max(1L, cumVol - _prevCumVol);
            _prevCumVol  = cumVol;

            // Lee-Ready: bid/ask first (updated during tick replay by NT8),
            // fall back to price-movement test, then sticky direction for flat prices
            double price = Close[0];
            double ask   = GetCurrentAsk();
            double bid   = GetCurrentBid();
            bool   isBuy;
            if      (ask > 0 && price >= ask)                      isBuy = true;
            else if (bid > 0 && price <= bid)                      isBuy = false;
            else if (price > _lastTickPrice && _lastTickPrice > 0) isBuy = true;
            else if (price < _lastTickPrice)                        isBuy = false;
            else                                                    isBuy = _prevIsBuy;
            _prevIsBuy     = isBuy;
            _lastTickPrice = price;

            _runDelta += isBuy ? tickVol : -tickVol;
            if (_runDelta > _barHigh) _barHigh = _runDelta;
            if (_runDelta < _barLow)  _barLow  = _runDelta;

            // Drive sub-panel auto-scale (keep 0 in range so zero line is always visible)
            Values[0][0] = _barHigh > 0 ? (double)_barHigh : 0.0;
            Values[1][0] = _barLow  < 0 ? (double)_barLow  : 0.0;
        }

        private void StoreBar(int idx)
        {
            _cvd += _runDelta;
            _cache[idx] = new DeltaBar
            {
                High  = _barHigh,
                Low   = _barLow,
                Close = _runDelta,
                Cvd   = _cvd,
            };
            // Debug: print first 5 bars to NT8 Output window (Ctrl+Alt+O)
            if (idx <= 5)
                Print($"[OFEDelta] bar={idx} high={_barHigh} low={_barLow} close={_runDelta} cvd={_cvd} prevCumVol={_prevCumVol}");
        }

        // ══════════════════════════════════════════════════════════════════════
        //  RENDERING
        // ══════════════════════════════════════════════════════════════════════

        public override void OnRenderTargetChanged()
        {
            DisposeResources();
            if (RenderTarget == null) return;

            _bullFill = Mk(BullColor, 0.75f);
            _bearFill = Mk(BearColor, 0.75f);
            _bullEdge = Mk(BullColor, 1.00f);
            _bearEdge = Mk(BearColor, 1.00f);
            _cvdBrush = Mk(CvdColor,  1.00f);
            _zeroBrush = new SharpDX.Direct2D1.SolidColorBrush(RenderTarget,
                new SharpDX.Color4(ZeroLineColor.R / 255f, ZeroLineColor.G / 255f,
                                   ZeroLineColor.B / 255f, ZeroLineColor.A / 255f));
            _wickBrush = new SharpDX.Direct2D1.SolidColorBrush(RenderTarget, new SharpDX.Color4(0.85f, 0.85f, 0.85f, 0.8f));
            _textBrush = new SharpDX.Direct2D1.SolidColorBrush(RenderTarget, new SharpDX.Color4(1f, 1f, 1f, 0.9f));

            _dw     = new SharpDX.DirectWrite.Factory();
            _valFmt = MkFmt(9f,  SharpDX.DirectWrite.TextAlignment.Center);
            _cvdFmt = MkFmt(10f, SharpDX.DirectWrite.TextAlignment.Leading);
        }

        protected override void OnRender(ChartControl cc, ChartScale cs)
        {
            if (RenderTarget == null || _bullFill == null) return;
            if (ChartBars == null || BarsArray == null || BarsArray.Length == 0) return;

            int fromIdx = ChartBars.FromIndex;
            int toIdx   = ChartBars.ToIndex;
            if (fromIdx >= toIdx) return;

            // Candle half-width from bar spacing
            double x0  = cc.GetXByBarIndex(ChartBars, fromIdx);
            double x1  = cc.GetXByBarIndex(ChartBars, Math.Min(fromIdx + 1, toIdx));
            float  spc = (float)Math.Abs(x1 - x0);
            float  hw  = spc * CandleWidthPct / 200f;

            float chartW = (float)cc.ActualWidth;

            // Zero line
            if (ShowZeroLine)
            {
                float yz = (float)cs.GetYByValue(0);
                RenderTarget.DrawLine(new Vector2(0f, yz), new Vector2(chartW, yz), _zeroBrush, 1f);
            }

            var cvdPts = ShowCvd ? new List<Vector2>(toIdx - fromIdx + 2) : null;

            for (int i = fromIdx; i <= toIdx; i++)
            {
                DeltaBar db;
                bool     isLive = false;

                if (i == _accumIdx)
                {
                    // Currently forming bar — show live state at 50% opacity
                    db = new DeltaBar
                    {
                        High  = _barHigh,
                        Low   = _barLow,
                        Close = _runDelta,
                        Cvd   = _cvd + _runDelta,
                    };
                    isLive = true;
                }
                else if (!_cache.TryGetValue(i, out db))
                    continue;

                float cx     = (float)cc.GetXByBarIndex(ChartBars, i);
                float yHigh  = (float)cs.GetYByValue(db.High);
                float yLow   = (float)cs.GetYByValue(db.Low);
                float yClose = (float)cs.GetYByValue(db.Close);
                float yZero  = (float)cs.GetYByValue(0);

                bool   isBull = db.Close >= 0;
                var    fill   = isBull ? _bullFill : _bearFill;
                var    edge   = isBull ? _bullEdge : _bearEdge;

                // ── Wick (High → Low through centre) ─────────────────────────
                RenderTarget.DrawLine(new Vector2(cx, yHigh), new Vector2(cx, yLow), _wickBrush, 1f);

                // ── Body (Zero → Close) ───────────────────────────────────────
                float bodyTop    = Math.Min(yZero, yClose);
                float bodyBottom = Math.Max(yZero, yClose);
                float bodyH      = Math.Max(1f, bodyBottom - bodyTop);
                var   rect       = new RectangleF(cx - hw, bodyTop, hw * 2f, bodyH);

                if (isLive) { fill.Opacity = 0.45f; edge.Opacity = 0.45f; }
                RenderTarget.FillRectangle(rect, fill);
                RenderTarget.DrawRectangle(rect, edge, 1f);
                if (isLive) { fill.Opacity = 0.75f; edge.Opacity = 1.00f; }

                // ── Delta value label ─────────────────────────────────────────
                if (ShowDeltaValue && spc >= 30f)
                {
                    string txt = db.Close == 0 ? "0"
                               : db.Close  > 0 ? $"+{db.Close}"
                                               : $"{db.Close}";
                    // Place above body for bull, below for bear
                    float ty = isBull ? bodyTop - 13f : bodyBottom + 2f;
                    RenderTarget.DrawText(txt, _valFmt,
                        new RectangleF(cx - hw * 2f, ty, hw * 4f, 12f), _textBrush);
                }

                // ── CVD point ─────────────────────────────────────────────────
                cvdPts?.Add(new Vector2(cx, (float)cs.GetYByValue(db.Cvd)));
            }

            // ── CVD polyline + terminal dot ───────────────────────────────────
            if (cvdPts != null && cvdPts.Count >= 2)
            {
                for (int i = 1; i < cvdPts.Count; i++)
                    RenderTarget.DrawLine(cvdPts[i - 1], cvdPts[i], _cvdBrush, 2f);

                var last = cvdPts[cvdPts.Count - 1];
                RenderTarget.FillEllipse(new SharpDX.Direct2D1.Ellipse(last, 3.5f, 3.5f), _cvdBrush);

                // Optional: CVD value text at the last point
                if (ShowCvdValue && _cvdFmt != null)
                {
                    long lastCvd = _accumIdx >= 0 ? _cvd + _runDelta : _cvd;
                    string cvdTxt = lastCvd >= 0 ? $"CVD +{lastCvd}" : $"CVD {lastCvd}";
                    RenderTarget.DrawText(cvdTxt, _cvdFmt,
                        new RectangleF(last.X + 6f, last.Y - 8f, 120f, 16f), _cvdBrush);
                }
            }
        }

        // ══════════════════════════════════════════════════════════════════════
        //  HELPERS
        // ══════════════════════════════════════════════════════════════════════

        private SharpDX.Direct2D1.SolidColorBrush Mk(System.Windows.Media.Color c, float alpha)
            => new SharpDX.Direct2D1.SolidColorBrush(RenderTarget,
               new SharpDX.Color4(c.R / 255f, c.G / 255f, c.B / 255f, alpha));

        private SharpDX.DirectWrite.TextFormat MkFmt(float size, SharpDX.DirectWrite.TextAlignment align)
        {
            var f = new SharpDX.DirectWrite.TextFormat(
                _dw, "Consolas",
                SharpDX.DirectWrite.FontWeight.Normal,
                SharpDX.DirectWrite.FontStyle.Normal, size);
            f.TextAlignment      = align;
            f.ParagraphAlignment = SharpDX.DirectWrite.ParagraphAlignment.Center;
            return f;
        }

        private void DisposeResources()
        {
            void D(IDisposable x) { x?.Dispose(); }
            D(_bullFill); D(_bearFill); D(_bullEdge); D(_bearEdge);
            D(_cvdBrush); D(_zeroBrush); D(_wickBrush); D(_textBrush);
            D(_valFmt); D(_cvdFmt); D(_dw);
            _bullFill = _bearFill = _bullEdge = _bearEdge =
            _cvdBrush = _zeroBrush = _wickBrush = _textBrush = null;
            _valFmt = _cvdFmt = null;
            _dw = null;
        }
    }
}
