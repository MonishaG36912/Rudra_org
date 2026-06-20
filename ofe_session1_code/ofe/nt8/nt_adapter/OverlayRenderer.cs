// OverlayRenderer.cs
// SharpDX / Direct2D renderer for price-panel overlays:
//   Layer 8:  VWAP daily line (gold, solid, 2px)
//   Layer 9:  ±1σ bands (cyan dashed), ±2σ bands (steel-blue dashed)
//   Layer 3:  Volume Profile histogram (right-side vertical bars, semi-transparent)
//   Layer 13: Signal arrows (NT Draw API handles these; see OFEFootprintIndicator)
//
// All prices come from the latest OfeVwapMsg and the bar cache (OfeBarCloseMsg).

using System;
using System.Collections.Generic;
using SharpDX;
using SharpDX.Direct2D1;
using NinjaTrader.Gui.Chart;
using NinjaTrader.NinjaScript.Indicators.OFE;

namespace NinjaTrader.NinjaScript.Indicators
{
    public sealed class OverlayRenderer : IDisposable
    {
        private readonly OFEFootprintIndicator _ind;

        private SolidColorBrush _vwapBrush;
        private SolidColorBrush _sd1Brush;
        private SolidColorBrush _sd2Brush;
        private SolidColorBrush _vpBrush;
        private SolidColorBrush _vpPocBrush;
        private StrokeStyle     _dashStyle;

        public OverlayRenderer(OFEFootprintIndicator ind) { _ind = ind; }

        public void OnRenderTargetChanged(RenderTarget rt)
        {
            DisposeResources();
            if (rt == null) return;

            _vwapBrush = new SolidColorBrush(rt, new Color4(1.0f, 0.84f, 0.0f, 1.0f));   // gold
            _sd1Brush  = new SolidColorBrush(rt, new Color4(0.37f, 0.74f, 0.91f, 0.9f)); // cyan
            _sd2Brush  = new SolidColorBrush(rt, new Color4(0.27f, 0.51f, 0.71f, 0.9f)); // steel-blue
            _vpBrush   = new SolidColorBrush(rt, new Color4(0.5f, 0.5f, 0.5f, 0.35f));   // semi-transparent grey
            _vpPocBrush= new SolidColorBrush(rt, new Color4(1.0f, 0.92f, 0.23f, 0.6f));  // yellow-gold POC bar

            var dashProps = new StrokeStyleProperties
                { DashStyle = SharpDX.Direct2D1.DashStyle.Dash };
            _dashStyle = new StrokeStyle(rt.Factory, dashProps);
        }

        public void Render(RenderTarget rt, ChartControl cc, ChartScale cs,
                           OfeVwapMsg vwap, bool hasVwap,
                           List<OfeSignalMsg> signals,
                           Dictionary<long, OfeBarCloseMsg> barCache,
                           bool showVwap, bool showVp, bool showArrows)
        {
            if (rt == null || _vwapBrush == null) return;

            float chartW = (float)cc.ActualWidth;
            float chartH = (float)cc.ActualHeight;

            // ── VWAP lines and bands ──────────────────────────────────────────
            if (showVwap && hasVwap && vwap.Vwap > 0)
            {
                DrawHLine(rt, cs, vwap.Vwap,  chartW, _vwapBrush, 2.0f, null);

                if (vwap.B1p > 0)
                {
                    DrawHLine(rt, cs, vwap.B1p, chartW, _sd1Brush, 1.0f, _dashStyle);
                    DrawHLine(rt, cs, vwap.B1m, chartW, _sd1Brush, 1.0f, _dashStyle);
                }
                if (vwap.B2p > 0)
                {
                    DrawHLine(rt, cs, vwap.B2p, chartW, _sd2Brush, 1.0f, _dashStyle);
                    DrawHLine(rt, cs, vwap.B2m, chartW, _sd2Brush, 1.0f, _dashStyle);
                }
            }

            // ── Volume Profile histogram ──────────────────────────────────────
            if (showVp)
                DrawVpHistogram(rt, cc, cs, barCache, chartW, chartH);
        }

        // ── Horizontal price line ─────────────────────────────────────────────

        private static void DrawHLine(RenderTarget rt, ChartScale cs,
                                      double price, float chartW,
                                      SolidColorBrush brush, float strokeW,
                                      StrokeStyle style)
        {
            float y = cs.GetYByValue(price);
            if (style != null)
                rt.DrawLine(new Vector2(0, y), new Vector2(chartW, y), brush, strokeW, style);
            else
                rt.DrawLine(new Vector2(0, y), new Vector2(chartW, y), brush, strokeW);
        }

        // ── Volume Profile histogram (right-side) ────────────────────────────
        // Merges all visible bar price levels into a session profile histogram.
        // Draws horizontal bars from the right margin inward, proportional to volume.

        private void DrawVpHistogram(RenderTarget rt, ChartControl cc, ChartScale cs,
                                     Dictionary<long, OfeBarCloseMsg> barCache,
                                     float chartW, float chartH)
        {
            if (barCache.Count == 0) return;

            // Accumulate volume by price across all cached bars
            var profile = new Dictionary<double, long>(256);
            double? pocPrice    = null;
            long    pocVol      = 0;
            OfeBarCloseMsg lastBar = null;

            foreach (var kvp in barCache.Values)
            {
                lastBar = kvp;
                foreach (var lv in kvp.Levels)
                {
                    long tv = lv.TotalVol;
                    if (!profile.TryGetValue(lv.Price, out long existing))
                        existing = 0;
                    profile[lv.Price] = existing + tv;
                    if (existing + tv > pocVol)
                    {
                        pocVol  = existing + tv;
                        pocPrice= lv.Price;
                    }
                }
            }

            if (profile.Count == 0) return;

            // Histogram panel: right 15% of chart width, max 120px
            float histMaxW = Math.Min(chartW * 0.15f, 120f);
            float histX    = chartW - histMaxW - 2f;

            foreach (var kvp in profile)
            {
                double price = kvp.Key;
                long   vol   = kvp.Value;
                float  barW  = (float)((double)vol / pocVol) * histMaxW;
                float  yTop  = cs.GetYByValue(price + 0.005);
                float  yBot  = cs.GetYByValue(price - 0.005);
                float  barH  = Math.Max(1f, yBot - yTop);

                bool isPoc = pocPrice.HasValue && Math.Abs(price - pocPrice.Value) < 0.005;
                var brush  = isPoc ? _vpPocBrush : _vpBrush;

                rt.FillRectangle(new RectangleF(histX + histMaxW - barW, yTop, barW, barH), brush);
            }

            // VAH / VAL dashed lines from the last bar
            if (lastBar != null && lastBar.Vah > 0)
            {
                DrawHLine(rt, cs, lastBar.Vah, histX + histMaxW, _vpPocBrush, 1f, _dashStyle);
                DrawHLine(rt, cs, lastBar.Val, histX + histMaxW, _vpPocBrush, 1f, _dashStyle);
            }
        }

        public void Dispose() => DisposeResources();

        private void DisposeResources()
        {
            _vwapBrush?.Dispose();
            _sd1Brush?.Dispose();
            _sd2Brush?.Dispose();
            _vpBrush?.Dispose();
            _vpPocBrush?.Dispose();
            _dashStyle?.Dispose();
            _vwapBrush = _sd1Brush = _sd2Brush = _vpBrush = _vpPocBrush = null;
            _dashStyle = null;
        }
    }
}
