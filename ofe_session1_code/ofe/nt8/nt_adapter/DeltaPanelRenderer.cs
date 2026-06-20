// DeltaPanelRenderer.cs
// SharpDX / Direct2D renderer for the delta sub-panel (layer 11–12).
//
// Renders a secondary panel below the price panel containing:
//   • Per-bar delta histogram  — blue for positive delta, red for negative
//   • CVD polyline             — green continuous line tracking cumulative delta
//   • Zero line                — white dashed horizontal at delta=0
//
// Panel height: fixed 80px pinned to the bottom of the price panel.
// The panel is drawn over the NT8 price panel (IsOverlay=true); callers should
// register a second data series to get a proper sub-panel if desired, but the
// overlay approach works without extra data series setup.

using System;
using System.Collections.Generic;
using System.Linq;
using SharpDX;
using SharpDX.Direct2D1;
using NinjaTrader.Gui.Chart;
using NinjaTrader.NinjaScript.Indicators.OFE;

namespace NinjaTrader.NinjaScript.Indicators
{
    public sealed class DeltaPanelRenderer : IDisposable
    {
        private readonly OFEFootprintIndicator _ind;

        private SolidColorBrush _positiveBrush;   // positive delta bars
        private SolidColorBrush _negativeBrush;   // negative delta bars
        private SolidColorBrush _cvdBrush;        // CVD polyline
        private SolidColorBrush _zeroBrush;       // zero line
        private SolidColorBrush _bgBrush;         // panel background
        private SolidColorBrush _labelBrush;      // text labels
        private StrokeStyle     _dashStyle;

        private SharpDX.DirectWrite.Factory     _dwFactory;
        private SharpDX.DirectWrite.TextFormat  _labelFormat;

        private const float kPanelHeight = 80f;

        public DeltaPanelRenderer(OFEFootprintIndicator ind) { _ind = ind; }

        public void OnRenderTargetChanged(RenderTarget rt)
        {
            DisposeResources();
            if (rt == null) return;

            _positiveBrush = new SolidColorBrush(rt, new Color4(0.13f, 0.59f, 0.95f, 0.9f)); // blue
            _negativeBrush = new SolidColorBrush(rt, new Color4(0.94f, 0.33f, 0.31f, 0.9f)); // red
            _cvdBrush      = new SolidColorBrush(rt, new Color4(0.0f,  0.85f, 0.45f, 1.0f)); // green
            _zeroBrush     = new SolidColorBrush(rt, Color4.White);
            _bgBrush       = new SolidColorBrush(rt, new Color4(0.07f, 0.07f, 0.07f, 0.85f));
            _labelBrush    = new SolidColorBrush(rt, new Color4(0.75f, 0.75f, 0.75f, 1.0f));

            var dashProps = new StrokeStyleProperties
                { DashStyle = SharpDX.Direct2D1.DashStyle.Dash };
            _dashStyle = new StrokeStyle(rt.Factory, dashProps);

            _dwFactory   = new SharpDX.DirectWrite.Factory();
            _labelFormat = new SharpDX.DirectWrite.TextFormat(
                _dwFactory, "Consolas",
                SharpDX.DirectWrite.FontWeight.Normal,
                SharpDX.DirectWrite.FontStyle.Normal, 9f);
            _labelFormat.TextAlignment      = SharpDX.DirectWrite.TextAlignment.Center;
            _labelFormat.ParagraphAlignment = SharpDX.DirectWrite.ParagraphAlignment.Center;
        }

        public void Render(RenderTarget rt, ChartControl cc, ChartScale cs,
                           Dictionary<long, OfeBarCloseMsg> barCache)
        {
            if (rt == null || _positiveBrush == null || barCache.Count == 0) return;

            float chartW  = (float)cc.ActualWidth;
            float chartH  = (float)cc.ActualHeight;
            float panelY  = chartH - kPanelHeight;

            // Panel background
            rt.FillRectangle(new RectangleF(0, panelY, chartW, kPanelHeight), _bgBrush);

            // Gather visible bars from cache (sorted by bar number)
            var visibleBars = new List<OfeBarCloseMsg>(barCache.Values);
            visibleBars.Sort((a, b) => a.BarNum.CompareTo(b.BarNum));
            if (visibleBars.Count == 0) return;

            // Determine delta range for scaling
            int maxAbsDelta = 1;
            foreach (var b in visibleBars)
                maxAbsDelta = Math.Max(maxAbsDelta, Math.Abs(b.Delta));

            float halfH   = kPanelHeight * 0.45f;
            float zeroY   = panelY + kPanelHeight * 0.5f;
            float barUnit = chartW / Math.Max(1, visibleBars.Count);

            // Zero line
            rt.DrawLine(new Vector2(0, zeroY), new Vector2(chartW, zeroY),
                        _zeroBrush, 0.5f, _dashStyle);

            // Per-bar delta histogram
            for (int i = 0; i < visibleBars.Count; i++)
            {
                var bar   = visibleBars[i];
                float x   = i * barUnit;
                float barW= Math.Max(1f, barUnit - 1f);
                float scaledH = (float)bar.Delta / maxAbsDelta * halfH;

                SolidColorBrush brush = bar.Delta >= 0 ? _positiveBrush : _negativeBrush;

                if (bar.Delta >= 0)
                    rt.FillRectangle(new RectangleF(x, zeroY - scaledH, barW, scaledH), brush);
                else
                    rt.FillRectangle(new RectangleF(x, zeroY, barW, -scaledH), brush);

                // Delta label inside each bar (skip if too narrow)
                if (barW >= 20f)
                {
                    string lbl = bar.Delta >= 0
                        ? "+" + (bar.Delta / 1000) + "k"
                        : (bar.Delta / 1000) + "k";
                    float lblY = bar.Delta >= 0 ? zeroY - scaledH - 12f : zeroY + 2f;
                    rt.DrawText(lbl, _labelFormat,
                        new RectangleF(x, lblY, barW, 12f), _labelBrush);
                }
            }

            // CVD polyline
            if (visibleBars.Count >= 2)
            {
                long  minCvd = visibleBars.Min(b => b.Cvd);
                long  maxCvd = visibleBars.Max(b => b.Cvd);
                long  cvdRange = Math.Max(1, maxCvd - minCvd);

                for (int i = 1; i < visibleBars.Count; i++)
                {
                    float x0 = (i - 1) * barUnit + barUnit * 0.5f;
                    float x1 =  i      * barUnit + barUnit * 0.5f;
                    float y0 = zeroY - (float)(visibleBars[i - 1].Cvd - minCvd) / cvdRange * halfH;
                    float y1 = zeroY - (float)(visibleBars[i    ].Cvd - minCvd) / cvdRange * halfH;

                    rt.DrawLine(new Vector2(x0, y0), new Vector2(x1, y1), _cvdBrush, 1.5f);
                }
            }

            // Panel label
            if (visibleBars.Count > 0)
            {
                var last = visibleBars[visibleBars.Count - 1];
                string cvdTxt = last.Cvd >= 0 ? $"CVD +{last.Cvd:#,##0}" : $"CVD {last.Cvd:#,##0}";
                rt.DrawText(cvdTxt, _labelFormat,
                    new RectangleF(4f, panelY + 2f, 180f, 14f), _labelBrush);
            }
        }

        public void Dispose() => DisposeResources();

        private void DisposeResources()
        {
            _positiveBrush?.Dispose(); _negativeBrush?.Dispose();
            _cvdBrush?.Dispose();      _zeroBrush?.Dispose();
            _bgBrush?.Dispose();       _labelBrush?.Dispose();
            _dashStyle?.Dispose();
            _labelFormat?.Dispose();   _dwFactory?.Dispose();
            _positiveBrush = _negativeBrush = _cvdBrush = _zeroBrush =
            _bgBrush = _labelBrush = null;
            _dashStyle = null; _labelFormat = null; _dwFactory = null;
        }
    }
}
