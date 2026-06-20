// FootprintRenderer.cs
// SharpDX / Direct2D renderer for the footprint cell grid.
//
// Renders for each visible bar:
//   • Bid × Ask volume columns per price level
//   • Cell background colour (buy/sell/imbalance/stacked/zero-print)
//   • POC yellow highlight rectangle
//   • COT magenta border rectangle
//
// Z-order: layers 5–7 (drawn after NT8 candles, before VWAP overlay)
//
// Colour priority (highest wins):
//   ZeroPrint > StackedBuy > StackedSell > ImbalanceBuy > ImbalanceSell > Buy > Sell

using System;
using System.Collections.Generic;
using SharpDX;
using SharpDX.Direct2D1;
using SharpDX.DirectWrite;
using NinjaTrader.Gui.Chart;
using NinjaTrader.NinjaScript.Indicators.OFE;

namespace NinjaTrader.NinjaScript.Indicators
{
    public sealed class FootprintRenderer : IDisposable
    {
        private readonly OFEFootprintIndicator _ind;

        // SharpDX brushes — recreated on RenderTargetChanged
        private SolidColorBrush _buyBrush, _sellBrush;
        private SolidColorBrush _ibuyBrush, _isellBrush;
        private SolidColorBrush _sbuyBrush, _ssellBrush;
        private SolidColorBrush _zeroBrush, _pocBrush, _cotBrush;
        private SolidColorBrush _textBrush, _gridBrush;

        private SharpDX.DirectWrite.Factory  _dwFactory;
        private SharpDX.DirectWrite.TextFormat _textFormat;

        public FootprintRenderer(OFEFootprintIndicator ind) { _ind = ind; }

        public void OnRenderTargetChanged(RenderTarget rt)
        {
            DisposeResources();
            if (rt == null) return;

            _buyBrush   = MakeBrush(rt, _ind.GetBuyColor());
            _sellBrush  = MakeBrush(rt, _ind.GetSellColor());
            _ibuyBrush  = MakeBrush(rt, _ind.GetImbalanceBuyColor());
            _isellBrush = MakeBrush(rt, _ind.GetImbalanceSellColor());
            _sbuyBrush  = MakeBrush(rt, _ind.GetStackedBuyColor());
            _ssellBrush = MakeBrush(rt, _ind.GetStackedSellColor());
            _zeroBrush  = new SolidColorBrush(rt, Color4.Black);
            _pocBrush   = MakeBrush(rt, _ind.GetPocColor());
            _cotBrush   = new SolidColorBrush(rt, new Color4(0.878f, 0f, 0.878f, 1f)); // #E000E0
            _textBrush  = new SolidColorBrush(rt, Color4.White);
            _gridBrush  = new SolidColorBrush(rt, new Color4(1f, 1f, 1f, 0.08f));

            _dwFactory  = new SharpDX.DirectWrite.Factory();
            _textFormat = new SharpDX.DirectWrite.TextFormat(
                _dwFactory, "Consolas",
                SharpDX.DirectWrite.FontWeight.Normal,
                SharpDX.DirectWrite.FontStyle.Normal,
                _ind.GetCellFontSize());
            _textFormat.TextAlignment      = SharpDX.DirectWrite.TextAlignment.Center;
            _textFormat.ParagraphAlignment = SharpDX.DirectWrite.ParagraphAlignment.Center;
        }

        public void Render(RenderTarget rt, ChartControl cc, ChartScale cs,
                           Dictionary<long, OfeBarCloseMsg> barCache)
        {
            if (rt == null || _buyBrush == null) return;

            int firstBar = cc.FirstVisibleBarIndex;
            int lastBar  = cc.LastVisibleBarIndex;

            for (int barIdx = firstBar; barIdx <= lastBar; barIdx++)
            {
                if (barIdx < 0 || barIdx >= _ind.BarsArray[0].Count) continue;

                // Match bar by timestamp (rounded to second)
                var barTime = _ind.BarsArray[0].GetTime(barIdx);
                long tsKey  = ((DateTimeOffset)barTime.ToUniversalTime()).ToUnixTimeSeconds();

                if (!barCache.TryGetValue(tsKey, out OfeBarCloseMsg bar)) continue;
                if (bar.Levels == null || bar.Levels.Count == 0) continue;

                float xCenter = cc.GetXByBarIndex(_ind.BarsArray[0], barIdx);
                float barW    = (float)cc.GetBarPaintWidth(_ind.BarsArray[0]) * 0.45f;

                RenderBar(rt, cs, bar, xCenter, barW);
            }
        }

        private void RenderBar(RenderTarget rt, ChartScale cs,
                               OfeBarCloseMsg bar, float xCenter, float halfW)
        {
            float tickPx = cs.GetYByValue(0) - cs.GetYByValue(1); // px per price unit

            foreach (var lv in bar.Levels)
            {
                float yTop    = cs.GetYByValue(lv.Price + 0.005f); // top edge of cell
                float yBot    = cs.GetYByValue(lv.Price - 0.005f); // bottom edge
                float cellH   = Math.Max(1f, yBot - yTop);

                // Background colour (priority order)
                SolidColorBrush bg;
                if (lv.IsZeroPrint)         bg = _zeroBrush;
                else if (lv.IsStackedBuy)   bg = _sbuyBrush;
                else if (lv.IsStackedSell)  bg = _ssellBrush;
                else if (lv.IsBuyImbalance) bg = _ibuyBrush;
                else if (lv.IsSellImbalance)bg = _isellBrush;
                else if (lv.AskVol > lv.BidVol) bg = _buyBrush;
                else                        bg = _sellBrush;

                // Left cell (bid volume)
                var leftRect  = new RectangleF(xCenter - halfW, yTop, halfW, cellH);
                rt.FillRectangle(leftRect, bg);

                // Right cell (ask volume)
                var rightRect = new RectangleF(xCenter, yTop, halfW, cellH);
                rt.FillRectangle(rightRect, bg);

                // Grid separator between bid and ask
                rt.DrawLine(new Vector2(xCenter, yTop),
                            new Vector2(xCenter, yTop + cellH), _gridBrush, 0.5f);

                // Volume text — skip zero-print cells
                if (!lv.IsZeroPrint && cellH >= 8f)
                {
                    string bidTxt = lv.BidVol.ToString();
                    string askTxt = lv.AskVol.ToString();

                    rt.DrawText(bidTxt, _textFormat, leftRect,  _textBrush);
                    rt.DrawText(askTxt, _textFormat, rightRect, _textBrush);
                }

                // POC highlight (yellow fill overlay, semi-transparent)
                if (lv.IsPoc)
                {
                    var pocRect = new RectangleF(xCenter - halfW, yTop, halfW * 2, cellH);
                    _pocBrush.Opacity = 0.35f;
                    rt.FillRectangle(pocRect, _pocBrush);
                    _pocBrush.Opacity = 1.0f;
                    rt.DrawRectangle(pocRect, _pocBrush, 1.0f);
                }

                // COT border (magenta outline, no fill)
                if (lv.IsCot)
                {
                    var cotRect = new RectangleF(xCenter - halfW + 0.5f, yTop + 0.5f,
                                                 halfW * 2 - 1f, cellH - 1f);
                    rt.DrawRectangle(cotRect, _cotBrush, 1.5f);
                }
            }
        }

        public void Dispose() => DisposeResources();

        private void DisposeResources()
        {
            _buyBrush?.Dispose();   _sellBrush?.Dispose();
            _ibuyBrush?.Dispose();  _isellBrush?.Dispose();
            _sbuyBrush?.Dispose();  _ssellBrush?.Dispose();
            _zeroBrush?.Dispose();  _pocBrush?.Dispose();
            _cotBrush?.Dispose();   _textBrush?.Dispose();
            _gridBrush?.Dispose();
            _textFormat?.Dispose(); _dwFactory?.Dispose();
            _buyBrush = _sellBrush = _ibuyBrush = _isellBrush =
            _sbuyBrush = _ssellBrush = _zeroBrush = _pocBrush =
            _cotBrush  = _textBrush  = _gridBrush = null;
            _textFormat = null; _dwFactory = null;
        }

        private static SolidColorBrush MakeBrush(RenderTarget rt,
            System.Windows.Media.Color c)
            => new SolidColorBrush(rt,
               new Color4(c.R / 255f, c.G / 255f, c.B / 255f, c.A / 255f));
    }
}
