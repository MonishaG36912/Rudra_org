// OFEFootprintIndicator.cs
// NinjaTrader 8 indicator — OFE full footprint chart renderer.
//
// Connects to nt_bridge_server (port 7777) and renders:
//   • Footprint cell grid   — bid×ask volumes per price level, imbalance colours
//   • POC highlight         — yellow bar across the bar's highest-volume level
//   • COT border            — magenta border around the Commitment of Traders level
//   • VWAP + ±1σ/±2σ bands — rendered by OverlayRenderer
//   • Volume Profile histogram — rendered by OverlayRenderer
//   • Signal arrows         — rendered by OverlayRenderer
//   • Delta + CVD sub-panel — rendered by DeltaPanelRenderer
//
// ─── Installation ────────────────────────────────────────────────────────────
//   1. Copy all nt_adapter/*.cs to:
//      Documents\NinjaTrader 8\bin\Custom\Indicators\
//   2. Tools → NinjaScript Editor → Compile (Ctrl+F5)
//   3. Drag OFEFootprintIndicator onto a BTC-USD chart
//   4. Set Engine Host / Port to match where nt_bridge_server is running
//
// ─── Dependencies ────────────────────────────────────────────────────────────
//   OFEMessageTypes.cs — protocol structs and JSON parser
//   OFEEngineClient.cs — TCP client with reconnect
//   FootprintRenderer.cs, OverlayRenderer.cs, DeltaPanelRenderer.cs — renderers

#region Using declarations
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.ComponentModel.DataAnnotations;
using System.Windows.Media;
using NinjaTrader.Gui;
using NinjaTrader.Gui.Chart;
using NinjaTrader.NinjaScript;
using SharpDX.Direct2D1;
using NinjaTrader.NinjaScript.Indicators.OFE;
#endregion

namespace NinjaTrader.NinjaScript.Indicators
{
    [Gui.CategoryOrder("OFE Connection", 1)]
    [Gui.CategoryOrder("Display",        2)]
    [Gui.CategoryOrder("Imbalances",     3)]
    [Gui.CategoryOrder("Cell Display",   4)]
    [Gui.CategoryOrder("Colours-Cells",  5)]
    public class OFEFootprintIndicator : Indicator
    {
        // ── Properties ────────────────────────────────────────────────────────

        [NinjaScriptProperty]
        [Display(Name = "Engine Host", Order = 1, GroupName = "OFE Connection")]
        public string EngineHost { get; set; }

        [NinjaScriptProperty]
        [Range(1, 65535)]
        [Display(Name = "Engine Port", Order = 2, GroupName = "OFE Connection")]
        public int EnginePort { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Symbol", Order = 3, GroupName = "OFE Connection")]
        public string SymbolName { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Delta Panel",    Order = 1, GroupName = "Display")]
        public bool ShowDeltaPanel { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show VWAP",           Order = 2, GroupName = "Display")]
        public bool ShowVwap { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show VP Histogram",   Order = 3, GroupName = "Display")]
        public bool ShowVpHistogram { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Signal Arrows",  Order = 4, GroupName = "Display")]
        public bool ShowSignalArrows { get; set; }

        [NinjaScriptProperty]
        [Range(1.0, 10.0)]
        [Display(Name = "Imbalance Ratio",     Order = 1, GroupName = "Imbalances")]
        public double ImbalanceRatio { get; set; }

        [NinjaScriptProperty]
        [Range(6, 20)]
        [Display(Name = "Cell Font Size",      Order = 1, GroupName = "Cell Display")]
        public int CellFontSize { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Buy Colour",          Order = 1, GroupName = "Colours-Cells")]
        public System.Windows.Media.Color BuyColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Sell Colour",         Order = 2, GroupName = "Colours-Cells")]
        public System.Windows.Media.Color SellColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Imbalance Buy",       Order = 3, GroupName = "Colours-Cells")]
        public System.Windows.Media.Color ImbalanceBuyColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Imbalance Sell",      Order = 4, GroupName = "Colours-Cells")]
        public System.Windows.Media.Color ImbalanceSellColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Stacked Buy",         Order = 5, GroupName = "Colours-Cells")]
        public System.Windows.Media.Color StackedBuyColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Stacked Sell",        Order = 6, GroupName = "Colours-Cells")]
        public System.Windows.Media.Color StackedSellColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "POC Colour",          Order = 7, GroupName = "Colours-Cells")]
        public System.Windows.Media.Color PocColor { get; set; }

        // ── Internal state ─────────────────────────────────────────────────────

        private OFEEngineClient _client;

        // Bar cache: maps bar close timestamp (seconds) → OfeBarCloseMsg
        // Written by NT UI thread (Drain), read by OnRender (same thread).
        private readonly Dictionary<long, OfeBarCloseMsg> _barCache
            = new Dictionary<long, OfeBarCloseMsg>(512);

        private OfeVwapMsg     _latestVwap;
        private bool           _hasVwap;
        private readonly List<OfeSignalMsg> _signals = new List<OfeSignalMsg>(64);
        private int            _arrowSeq;

        // Renderers (created in DataLoaded, disposed in Terminated)
        private FootprintRenderer    _fpRenderer;
        private OverlayRenderer      _overlayRenderer;
        private DeltaPanelRenderer   _deltaRenderer;

        private System.Windows.Threading.DispatcherTimer _refreshTimer;

        // ── NT8 lifecycle ─────────────────────────────────────────────────────

        protected override void OnStateChange()
        {
            if (State == State.SetDefaults)
            {
                Name        = "OFE Footprint";
                Description = "OFE real-time footprint chart from Coinbase → nt_bridge_server.";
                Calculate   = Calculate.OnEachTick;
                IsOverlay   = true;
                IsAutoScale = false;
                DrawOnPricePanel             = true;
                IsSuspendedWhileInactive     = false;
                ScaleJustification           = NinjaTrader.Gui.Chart.ScaleJustification.Right;

                EngineHost       = "127.0.0.1";
                EnginePort       = 7777;
                SymbolName       = "BTC-USD";
                ShowDeltaPanel   = true;
                ShowVwap         = true;
                ShowVpHistogram  = true;
                ShowSignalArrows = true;
                ImbalanceRatio   = 3.0;
                CellFontSize     = 10;

                BuyColor          = Color.FromRgb(0x21, 0x96, 0xF3); // #2196F3 blue
                SellColor         = Color.FromRgb(0xEF, 0x53, 0x50); // #EF5350 red
                ImbalanceBuyColor = Color.FromRgb(0x15, 0x65, 0xC0); // #1565C0
                ImbalanceSellColor= Color.FromRgb(0xB7, 0x1C, 0x1C); // #B71C1C
                StackedBuyColor   = Color.FromRgb(0x0D, 0x47, 0xA1); // #0D47A1
                StackedSellColor  = Color.FromRgb(0x7B, 0x00, 0x00); // #7B0000
                PocColor          = Color.FromRgb(0xFF, 0xEB, 0x3B); // #FFEB3B yellow
            }
            else if (State == State.DataLoaded)
            {
                _fpRenderer      = new FootprintRenderer(this);
                _overlayRenderer = new OverlayRenderer(this);
                _deltaRenderer   = new DeltaPanelRenderer(this);

                _client = new OFEEngineClient
                {
                    Host       = EngineHost,
                    Port       = EnginePort,
                    SymbolName = SymbolName,
                };
                _client.Start();

                // 200 ms timer to keep rendering fresh even when NT feed is quiet
                _refreshTimer = new System.Windows.Threading.DispatcherTimer
                    { Interval = TimeSpan.FromMilliseconds(200) };
                _refreshTimer.Tick += (_, __) => { Drain(); if (_hasVwap) ForceRefresh(); };
                _refreshTimer.Start();
            }
            else if (State == State.Terminated)
            {
                _refreshTimer?.Stop();
                _client?.Stop();
                _fpRenderer?.Dispose();
                _overlayRenderer?.Dispose();
                _deltaRenderer?.Dispose();
            }
        }

        // ── OnRenderTargetChanged — recreate SharpDX resources ───────────────

        protected override void OnRenderTargetChanged()
        {
            _fpRenderer?.OnRenderTargetChanged(RenderTarget);
            _overlayRenderer?.OnRenderTargetChanged(RenderTarget);
            _deltaRenderer?.OnRenderTargetChanged(RenderTarget);
        }

        // ── OnBarUpdate — drain message queue ────────────────────────────────

        protected override void OnBarUpdate()
        {
            Drain();
        }

        // ── OnRender — call all three renderers ──────────────────────────────

        protected override void OnRender(ChartControl cc, ChartScale cs)
        {
            if (RenderTarget == null) return;

            _fpRenderer?.Render(RenderTarget, cc, cs, _barCache);

            if (ShowVwap || ShowVpHistogram || ShowSignalArrows)
                _overlayRenderer?.Render(RenderTarget, cc, cs,
                    _latestVwap, _hasVwap, _signals, _barCache,
                    ShowVwap, ShowVpHistogram, ShowSignalArrows);

            if (ShowDeltaPanel)
                _deltaRenderer?.Render(RenderTarget, cc, cs, _barCache);
        }

        // ── Drain message queue (NT/WPF thread only) ──────────────────────────

        private void Drain()
        {
            OfeMessage msg;
            while (_client != null && _client.Messages.TryDequeue(out msg))
            {
                switch (msg.Type)
                {
                    case OfeMessageType.BarClose:
                        // Key by bar close time (seconds) for lookup in OnRender
                        long tsKey = msg.BarClose.Ts / 1_000_000_000L;
                        _barCache[tsKey] = msg.BarClose;
                        // Evict old entries (keep 500 bars max)
                        if (_barCache.Count > 500)
                            EvictOldestBars();
                        break;

                    case OfeMessageType.Vwap:
                        _latestVwap = msg.Vwap;
                        _hasVwap    = true;
                        break;

                    case OfeMessageType.Signal:
                        _signals.Add(msg.Signal);
                        if (_signals.Count > 200) _signals.RemoveAt(0);
                        DrawSignalArrow(msg.Signal);
                        break;
                }
            }
        }

        // ── Signal arrow rendering (NT Draw API — must run on UI thread) ──────

        private void DrawSignalArrow(OfeSignalMsg sig)
        {
            if (!ShowSignalArrows || CurrentBar < 0) return;

            string tag = "OFE_SIG_" + (++_arrowSeq);
            string lbl = ShortSignal(sig.Type);

            if (sig.IsLong)
            {
                Draw.ArrowUp(this, tag, false, 0,
                    Low[0] - TickSize * 6, Brushes.LimeGreen);
                Draw.Text(this, tag + "T", lbl, 0,
                    Low[0] - TickSize * 16, Brushes.LimeGreen);
            }
            else if (sig.IsShort)
            {
                Draw.ArrowDown(this, tag, false, 0,
                    High[0] + TickSize * 6, Brushes.OrangeRed);
                Draw.Text(this, tag + "T", lbl, 0,
                    High[0] + TickSize * 16, Brushes.OrangeRed);
            }
        }

        private void EvictOldestBars()
        {
            // Find and remove the 50 oldest entries
            var keys = new List<long>(_barCache.Keys);
            keys.Sort();
            for (int i = 0; i < Math.Min(50, keys.Count); i++)
                _barCache.Remove(keys[i]);
        }

        private static string ShortSignal(string s)
        {
            switch (s)
            {
                case "VWAP_REACTION_LONG":   return "▲ RCT";
                case "VWAP_REACTION_SHORT":  return "▼ RCT";
                case "VWAP_ROTATION_LONG":   return "▲ ROT";
                case "VWAP_ROTATION_SHORT":  return "▼ ROT";
                case "PULSE_LONG":           return "▲ PLS";
                case "PULSE_SHORT":          return "▼ PLS";
                case "TURNS_BULLISH":        return "▲ TRN";
                case "TURNS_BEARISH":        return "▼ TRN";
                default:                     return s.Length > 6 ? s.Substring(0, 6) : s;
            }
        }

        // ── Public accessors used by renderers ────────────────────────────────

        public System.Windows.Media.Color GetBuyColor()           => BuyColor;
        public System.Windows.Media.Color GetSellColor()          => SellColor;
        public System.Windows.Media.Color GetImbalanceBuyColor()  => ImbalanceBuyColor;
        public System.Windows.Media.Color GetImbalanceSellColor() => ImbalanceSellColor;
        public System.Windows.Media.Color GetStackedBuyColor()    => StackedBuyColor;
        public System.Windows.Media.Color GetStackedSellColor()   => StackedSellColor;
        public System.Windows.Media.Color GetPocColor()           => PocColor;
        public int                        GetCellFontSize()       => CellFontSize;
    }
}

#region NinjaScript generated code — do not edit manually

namespace NinjaTrader.NinjaScript.Indicators
{
    public partial class Indicator : NinjaTrader.Gui.NinjaScript.IndicatorRenderBase
    {
        private OFEFootprintIndicator[] cacheOFEFootprintIndicator;

        public OFEFootprintIndicator OFEFootprintIndicator(
            string engineHost, int enginePort, string symbolName,
            bool showDeltaPanel, bool showVwap, bool showVpHistogram, bool showSignalArrows,
            double imbalanceRatio, int cellFontSize)
            => OFEFootprintIndicator(Input, engineHost, enginePort, symbolName,
               showDeltaPanel, showVwap, showVpHistogram, showSignalArrows,
               imbalanceRatio, cellFontSize);

        public OFEFootprintIndicator OFEFootprintIndicator(
            ISeries<double> input,
            string engineHost, int enginePort, string symbolName,
            bool showDeltaPanel, bool showVwap, bool showVpHistogram, bool showSignalArrows,
            double imbalanceRatio, int cellFontSize)
        {
            if (cacheOFEFootprintIndicator != null)
                foreach (var c in cacheOFEFootprintIndicator)
                    if (c != null
                        && c.EngineHost       == engineHost
                        && c.EnginePort       == enginePort
                        && c.SymbolName       == symbolName
                        && c.ShowDeltaPanel   == showDeltaPanel
                        && c.ShowVwap         == showVwap
                        && c.ShowVpHistogram  == showVpHistogram
                        && c.ShowSignalArrows == showSignalArrows
                        && c.EqualsInput(input))
                        return c;

            return CacheIndicator<OFEFootprintIndicator>(
                new OFEFootprintIndicator
                {
                    EngineHost       = engineHost,
                    EnginePort       = enginePort,
                    SymbolName       = symbolName,
                    ShowDeltaPanel   = showDeltaPanel,
                    ShowVwap         = showVwap,
                    ShowVpHistogram  = showVpHistogram,
                    ShowSignalArrows = showSignalArrows,
                    ImbalanceRatio   = imbalanceRatio,
                    CellFontSize     = cellFontSize,
                },
                input, ref cacheOFEFootprintIndicator);
        }
    }
}

#endregion
