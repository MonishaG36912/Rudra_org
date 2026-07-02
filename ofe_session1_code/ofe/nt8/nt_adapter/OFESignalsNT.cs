// OFESignalsNT.cs — OFE Signal Indicators, NinjaTrader 8
// Testing version: signal markers only, NO footprint rendering, NO SharpDX.
//
// Works with ANY NT8 data provider. No external bridge required.
// Requires: Chart Properties → Data → Tick Replay = Yes (for historical bars)
//
// Install: copy to Documents\NinjaTrader 8\bin\Custom\Indicators\
//          Tools → NinjaScript Editor → Compile (Ctrl+F5)
//          Drag "OFE Signals NT" onto any tick/minute/volume chart.
//
// Output: arrows/diamonds on chart + BarDelta sub-panel + Print() log in Output window.

#region Using declarations
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.ComponentModel.DataAnnotations;
using System.Linq;
using System.Windows.Media;
using NinjaTrader.Cbi;
using NinjaTrader.Data;
using NinjaTrader.Gui;
using NinjaTrader.Gui.Chart;
using NinjaTrader.NinjaScript;
using NinjaTrader.NinjaScript.DrawingTools;
#endregion

namespace NinjaTrader.NinjaScript.Indicators
{
    [Gui.CategoryOrder("Global",         1)]
    [Gui.CategoryOrder("Imbalance",      2)]
    [Gui.CategoryOrder("Delta Signals",  3)]
    [Gui.CategoryOrder("POC Signals",    4)]
    [Gui.CategoryOrder("Volume Signals", 5)]
    [Gui.CategoryOrder("Extreme Levels", 6)]
    [Gui.CategoryOrder("Colours",        7)]
    public class OFESignalsNT : Indicator
    {
        // ══════════════════════════════════════════════════════════════════════
        //  PROPERTIES
        // ══════════════════════════════════════════════════════════════════════

        // ── Global ────────────────────────────────────────────────────────────
        [NinjaScriptProperty][Range(1,100)]
        [Display(Name="Value Area %", Order=1, GroupName="Global")]
        public int ValueAreaPercent { get; set; }

        [NinjaScriptProperty][Range(0,9999)]
        [Display(Name="Min Imbalance Volume", Order=2, GroupName="Global")]
        public int MinImbalanceVolume { get; set; }

        [NinjaScriptProperty][Range(100,2000)]
        [Display(Name="Imbalance Trigger %", Order=3, GroupName="Global")]
        public int ImbalanceTriggerPct { get; set; }

        [NinjaScriptProperty][Range(1,20)]
        [Display(Name="Swing Period", Order=4, GroupName="Global")]
        public int SwingPeriod { get; set; }

        [NinjaScriptProperty][Range(1,6)]
        [Display(Name="Momentum Strength", Order=5, GroupName="Global")]
        public int MomentumStrength { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Print Signals to Output", Order=6, GroupName="Global",
            Description="Write each bar's active signals to the Output window.")]
        public bool PrintSignals { get; set; }

        // ── Imbalance ─────────────────────────────────────────────────────────
        [NinjaScriptProperty][Range(2,10)]
        [Display(Name="Stacked Threshold", Order=1, GroupName="Imbalance")]
        public int StackedThreshold { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Stacked Imbalance", Order=2, GroupName="Imbalance")]
        public bool ShowStackedImbalance { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Volume Imbalance", Order=3, GroupName="Imbalance")]
        public bool ShowVolumeImbalance { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Multiple Imbalance", Order=4, GroupName="Imbalance")]
        public bool ShowMultipleImbalance { get; set; }

        [NinjaScriptProperty][Range(2,20)]
        [Display(Name="Multiple Imbalance Count", Order=5, GroupName="Imbalance")]
        public int MultipleImbalanceCount { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Imbalance Reversal", Order=6, GroupName="Imbalance")]
        public bool ShowImbalanceReversal { get; set; }

        [NinjaScriptProperty][Range(1,9999)]
        [Display(Name="Min Imbalance Reversal Vol", Order=7, GroupName="Imbalance")]
        public int MinImbalanceReversalVolume { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Imbalance Auction", Order=8, GroupName="Imbalance")]
        public bool ShowImbalanceAuction { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Inverse Imbalance", Order=9, GroupName="Imbalance")]
        public bool ShowInverseImbalance { get; set; }

        // ── Delta Signals ─────────────────────────────────────────────────────
        [NinjaScriptProperty]
        [Display(Name="Show Delta Divergence", Order=1, GroupName="Delta Signals")]
        public bool ShowDeltaDivergence { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Day Extreme Only (Divergence)", Order=2, GroupName="Delta Signals")]
        public bool DeltaDivergenceDayExtremeOnly { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Delta Surge", Order=3, GroupName="Delta Signals")]
        public bool ShowDeltaSurge { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Delta Breakout", Order=4, GroupName="Delta Signals")]
        public bool ShowDeltaBreakout { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Delta Stack", Order=5, GroupName="Delta Signals")]
        public bool ShowDeltaStack { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Delta Tail", Order=6, GroupName="Delta Signals")]
        public bool ShowDeltaTail { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Price Action Divergence", Order=7, GroupName="Delta Signals")]
        public bool ShowPriceActionDivergence { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Accum/Dist", Order=8, GroupName="Delta Signals")]
        public bool ShowAccumDist { get; set; }

        // ── POC Signals ───────────────────────────────────────────────────────
        [NinjaScriptProperty]
        [Display(Name="Show Value Area", Order=1, GroupName="POC Signals")]
        public bool ShowValueArea { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Engulfing VA", Order=2, GroupName="POC Signals")]
        public bool ShowEngulfingVA { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Prominent POC", Order=3, GroupName="POC Signals")]
        public bool ShowProminentPoc { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show POC Shadow", Order=4, GroupName="POC Signals")]
        public bool ShowPocShadows { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show OF Ratio", Order=5, GroupName="POC Signals")]
        public bool ShowOFRatio { get; set; }

        [NinjaScriptProperty][Range(1,1000)]
        [Display(Name="OF Ratio Exhaustion Level", Order=6, GroupName="POC Signals")]
        public int OFRatioExhaustionLevel { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show OF Gaps", Order=7, GroupName="POC Signals")]
        public bool ShowOFGaps { get; set; }

        // ── Volume Signals ────────────────────────────────────────────────────
        [NinjaScriptProperty]
        [Display(Name="Show Buying/Selling Tail", Order=1, GroupName="Volume Signals")]
        public bool ShowBuySellTail { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Zero Print", Order=2, GroupName="Volume Signals")]
        public bool ShowZeroPrint { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Exhaustion Prints", Order=3, GroupName="Volume Signals")]
        public bool ShowExhaustionPrints { get; set; }

        [NinjaScriptProperty][Range(1,9999)]
        [Display(Name="Exhaustion Max Volume", Order=4, GroupName="Volume Signals")]
        public int ExhaustionMaxVolume { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Stopping Volume", Order=5, GroupName="Volume Signals")]
        public bool ShowStoppingVolume { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Retail Suck", Order=6, GroupName="Volume Signals")]
        public bool ShowRetailSuck { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Volume Decline", Order=7, GroupName="Volume Signals")]
        public bool ShowVolumeDecline { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Vertical Liquidity", Order=8, GroupName="Volume Signals")]
        public bool ShowVerticalLiquidity { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Resting Liquidity", Order=9, GroupName="Volume Signals")]
        public bool ShowRestingLiquidity { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Market Sweep", Order=10, GroupName="Volume Signals")]
        public bool ShowMarketSweep { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Market Weakness", Order=11, GroupName="Volume Signals")]
        public bool ShowMarketWeakness { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show OF Sequencing", Order=12, GroupName="Volume Signals")]
        public bool ShowOFSequencing { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Unfinished Auction", Order=13, GroupName="Volume Signals")]
        public bool ShowUnfinishedAuctions { get; set; }

        // ── Extreme Levels ────────────────────────────────────────────────────
        [NinjaScriptProperty]
        [Display(Name="Show Extreme Bar Delta", Order=1, GroupName="Extreme Levels")]
        public bool ShowExtremeBarDelta { get; set; }

        [NinjaScriptProperty][Range(1,999999)]
        [Display(Name="Extreme Bar Delta Threshold", Order=2, GroupName="Extreme Levels")]
        public int ExtremeBarDeltaThreshold { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Extreme Bar Volume", Order=3, GroupName="Extreme Levels")]
        public bool ShowExtremeBarVolume { get; set; }

        [NinjaScriptProperty][Range(1,999999)]
        [Display(Name="Extreme Bar Volume Threshold", Order=4, GroupName="Extreme Levels")]
        public int ExtremeBarVolumeThreshold { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Extreme Delta Threshold", Order=5, GroupName="Extreme Levels")]
        public bool ShowExtremeDeltaThreshold { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Extreme Delta Volume", Order=6, GroupName="Extreme Levels")]
        public bool ShowExtremeDeltaVolume { get; set; }

        [NinjaScriptProperty][Range(1,100)]
        [Display(Name="Extreme Delta/Volume % Threshold", Order=7, GroupName="Extreme Levels")]
        public int ExtremeDeltaVolumeThresholdPct { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Extreme Delta Change", Order=8, GroupName="Extreme Levels")]
        public bool ShowExtremeDeltaChange { get; set; }

        [NinjaScriptProperty][Range(1,999999)]
        [Display(Name="Extreme Delta Change Threshold", Order=9, GroupName="Extreme Levels")]
        public int ExtremeDeltaChangeThreshold { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Extreme Delta Change %", Order=10, GroupName="Extreme Levels")]
        public bool ShowExtremeDeltaChangePct { get; set; }

        [NinjaScriptProperty][Range(1,9999)]
        [Display(Name="Extreme Delta Change % Threshold", Order=11, GroupName="Extreme Levels")]
        public int ExtremeDeltaChangePctThreshold { get; set; }

        [NinjaScriptProperty]
        [Display(Name="Show Small Min/Max Delta", Order=12, GroupName="Extreme Levels")]
        public bool ShowSmallMinMaxDelta { get; set; }

        // ── Colours ───────────────────────────────────────────────────────────
        [NinjaScriptProperty][XmlIgnore]
        [Display(Name="Delta Divergence", Order=1, GroupName="Colours")]
        public System.Windows.Media.Color DeltaDivergenceColor { get; set; }

        [NinjaScriptProperty][XmlIgnore]
        [Display(Name="OF Ratio Bull", Order=2, GroupName="Colours")]
        public System.Windows.Media.Color OFRatioBullColor { get; set; }

        [NinjaScriptProperty][XmlIgnore]
        [Display(Name="OF Ratio Bear", Order=3, GroupName="Colours")]
        public System.Windows.Media.Color OFRatioBearColor { get; set; }

        [NinjaScriptProperty][XmlIgnore]
        [Display(Name="Engulfing VA Bull", Order=4, GroupName="Colours")]
        public System.Windows.Media.Color EngulfingVABullColor { get; set; }

        [NinjaScriptProperty][XmlIgnore]
        [Display(Name="Engulfing VA Bear", Order=5, GroupName="Colours")]
        public System.Windows.Media.Color EngulfingVABearColor { get; set; }

        [NinjaScriptProperty][XmlIgnore]
        [Display(Name="Stacked Buy", Order=6, GroupName="Colours")]
        public System.Windows.Media.Color StackedBuyColor { get; set; }

        [NinjaScriptProperty][XmlIgnore]
        [Display(Name="Stacked Sell", Order=7, GroupName="Colours")]
        public System.Windows.Media.Color StackedSellColor { get; set; }

        // ══════════════════════════════════════════════════════════════════════
        //  INNER TYPES
        // ══════════════════════════════════════════════════════════════════════

        private struct BarLevel
        {
            public double Price;
            public long   BidVol;
            public long   AskVol;
            public long   TotalVol => BidVol + AskVol;
            public long   Delta    => AskVol - BidVol;
        }

        private class BarSnapshot
        {
            public int    BarIndex;
            public double Open, High, Low, Close;
            public long   BarDelta, TotalVolume, Cvd;
            public long   MaxDelta, MinDelta;
            public double PocPrice, VaLow, VaHigh;
            public List<BarLevel> Levels;
            public bool[] IsBuyImbalance, IsSellImbalance;
            public bool[] IsStackedBuy,   IsStackedSell;

            // Signals
            public bool SigDeltaDivergence, SigDeltaSurge, SigDeltaBreakout;
            public bool SigDeltaStack, SigDeltaTail;
            public bool SigBuyingTail, SigSellingTail;
            public bool SigZeroBuy, SigZeroSell;
            public bool SigExhaustionBull, SigExhaustionBear;
            public bool SigStoppingVolume, SigRetailSuck, SigVolumeDecline;
            public bool SigMarketSweep, SigMarketWeakness, SigOFSequencing;
            public bool SigImbalanceReversal, SigImbalanceAuction, SigMultipleImbalance;
            public bool SigVerticalLiquidity, SigRestingLiquidity;
            public bool SigUnfinishedAuction, SigPocShadow;
            public bool SigOFRatioBull, SigOFRatioBear;
            public bool SigEngulfingVABull, SigEngulfingVABear;
            public bool SigOFGaps, SigAccumDist, SigPriceActionDiv;
            public bool SigExtremeBarDelta, SigExtremeBarVolume;
            public bool SigExtremeDeltaThreshold, SigExtremeDeltaVolume;
            public bool SigExtremeDeltaChange, SigExtremeDeltaChangePct;
            public bool SigSmallMinMaxDelta;

            public bool IsUpBar   => Close >= Open;
            public bool IsDownBar => Close <  Open;
        }

        // ══════════════════════════════════════════════════════════════════════
        //  STATE
        // ══════════════════════════════════════════════════════════════════════

        private readonly Dictionary<long, BarLevel>    _accumLevels = new Dictionary<long, BarLevel>(256);
        private readonly Dictionary<int,  BarSnapshot> _barCache    = new Dictionary<int, BarSnapshot>(512);
        private int    _accumBarIndex  = -1;
        private int    _maxCacheBars   = 2000;
        private double _lastBid, _lastAsk, _prevTradePrice;
        private long   _cvd;
        private long   _prevBarDelta, _prevPrevBarDelta;
        private double _prevVaLow, _prevVaHigh;
        private double _dayHigh, _dayLow;

        // ══════════════════════════════════════════════════════════════════════
        //  LIFECYCLE
        // ══════════════════════════════════════════════════════════════════════

        protected override void OnStateChange()
        {
            if (State == State.SetDefaults)
            {
                Name        = "OFE Signals NT";
                Description = "OFE signal markers — 48 Orderflows indicators, no footprint rendering.";
                Calculate   = Calculate.OnEachTick;
                IsOverlay   = true;
                IsAutoScale = false;
                DrawOnPricePanel         = true;
                IsSuspendedWhileInactive = false;

                ValueAreaPercent           = 70;
                MinImbalanceVolume         = 10;
                ImbalanceTriggerPct        = 400;
                SwingPeriod                = 5;
                MomentumStrength           = 3;
                PrintSignals               = true;

                StackedThreshold           = 3;
                ShowStackedImbalance       = true;
                ShowVolumeImbalance        = true;
                ShowMultipleImbalance      = false;
                MultipleImbalanceCount     = 5;
                ShowImbalanceReversal      = false;
                MinImbalanceReversalVolume = 10;
                ShowImbalanceAuction       = false;
                ShowInverseImbalance       = false;

                ShowDeltaDivergence           = true;
                DeltaDivergenceDayExtremeOnly = true;
                ShowDeltaSurge                = false;
                ShowDeltaBreakout             = false;
                ShowDeltaStack                = false;
                ShowDeltaTail                 = false;
                ShowPriceActionDivergence     = false;
                ShowAccumDist                 = false;

                ShowValueArea                 = true;
                ShowEngulfingVA               = true;
                ShowProminentPoc              = true;
                ShowPocShadows                = true;
                ShowOFRatio                   = true;
                OFRatioExhaustionLevel        = 30;
                ShowOFGaps                    = false;

                ShowBuySellTail               = false;
                ShowZeroPrint                 = false;
                ShowExhaustionPrints          = false;
                ExhaustionMaxVolume           = 10;
                ShowStoppingVolume            = false;
                ShowRetailSuck                = false;
                ShowVolumeDecline             = false;
                ShowVerticalLiquidity         = false;
                ShowRestingLiquidity          = false;
                ShowMarketSweep               = false;
                ShowMarketWeakness            = false;
                ShowOFSequencing              = false;
                ShowUnfinishedAuctions        = false;

                ShowExtremeBarDelta            = false;
                ExtremeBarDeltaThreshold       = 1000;
                ShowExtremeBarVolume           = false;
                ExtremeBarVolumeThreshold      = 5000;
                ShowExtremeDeltaThreshold      = false;
                ShowExtremeDeltaVolume         = false;
                ExtremeDeltaVolumeThresholdPct = 60;
                ShowExtremeDeltaChange         = false;
                ExtremeDeltaChangeThreshold    = 500;
                ShowExtremeDeltaChangePct      = false;
                ExtremeDeltaChangePctThreshold = 200;
                ShowSmallMinMaxDelta           = false;

                DeltaDivergenceColor  = Color.FromRgb(0xFF, 0xD7, 0x00); // gold
                OFRatioBullColor      = Color.FromRgb(0x00, 0x7A, 0xFF); // blue
                OFRatioBearColor      = Color.FromRgb(0xEF, 0x53, 0x50); // red
                EngulfingVABullColor  = Color.FromRgb(0x01, 0x57, 0x9B); // dark blue
                EngulfingVABearColor  = Color.FromRgb(0x8B, 0x00, 0x00); // dark red
                StackedBuyColor       = Color.FromRgb(0x0D, 0x47, 0xA1); // deep blue
                StackedSellColor      = Color.FromRgb(0x7B, 0x00, 0x00); // deep red

                // BarDelta sub-panel plot
                AddPlot(new Stroke(Brushes.DodgerBlue, 2), PlotStyle.Bar, "BarDelta");
            }
            else if (State == State.DataLoaded)
            {
                _dayHigh      = double.MinValue;
                _dayLow       = double.MaxValue;
                int loaded    = BarsArray[0].Count;
                int live      = ComputeLiveBuffer();
                _maxCacheBars = loaded + live;
                Print($"[OFE Signals] {Instrument.MasterInstrument.Name}  {BarsPeriod}"
                    + $"  loaded={loaded}  liveBuffer={live}");
            }
        }

        protected override void OnBarUpdate()
        {
            // Push BarDelta into the plot series so it shows in the sub-panel
            if (_barCache.TryGetValue(CurrentBar, out var snap))
                Values[0][0] = snap.BarDelta;
        }

        // ══════════════════════════════════════════════════════════════════════
        //  TICK PROCESSING
        // ══════════════════════════════════════════════════════════════════════

        protected override void OnMarketData(MarketDataEventArgs e)
        {
            if (e.MarketDataType == MarketDataType.Bid) { _lastBid = e.Price; return; }
            if (e.MarketDataType == MarketDataType.Ask) { _lastAsk = e.Price; return; }
            if (e.MarketDataType != MarketDataType.Last) return;

            if (CurrentBar != _accumBarIndex)
            {
                if (_accumBarIndex >= 0 && _accumLevels.Count > 0)
                    CloseBar(_accumBarIndex);
                _accumLevels.Clear();
                _accumBarIndex = CurrentBar;
            }

            bool isBuy;
            if      (_lastAsk > 0 && e.Price >= _lastAsk) isBuy = true;
            else if (_lastBid > 0 && e.Price <= _lastBid) isBuy = false;
            else                                           isBuy = e.Price >= _prevTradePrice;
            _prevTradePrice = e.Price;

            double tickSize = Instrument.MasterInstrument.TickSize;
            long   pk       = (long)Math.Round(e.Price / tickSize);
            BarLevel lv;
            if (!_accumLevels.TryGetValue(pk, out lv))
                lv = new BarLevel { Price = pk * tickSize };

            long vol = Math.Max(1L, (long)e.Volume);
            if (isBuy) lv.AskVol += vol; else lv.BidVol += vol;
            _accumLevels[pk] = lv;
        }

        // ══════════════════════════════════════════════════════════════════════
        //  BAR CLOSE
        // ══════════════════════════════════════════════════════════════════════

        private void CloseBar(int barIdx)
        {
            var snap = BuildSnapshot(barIdx, _accumLevels);
            if (snap == null) return;

            _cvd       += snap.BarDelta;
            snap.Cvd    = _cvd;
            if (snap.High > _dayHigh) _dayHigh = snap.High;
            if (snap.Low  < _dayLow)  _dayLow  = snap.Low;

            ComputeInterBarSignals(snap);
            _barCache[barIdx] = snap;

            ApplySignals(snap);

            _prevPrevBarDelta = _prevBarDelta;
            _prevBarDelta     = snap.BarDelta;
            _prevVaLow        = snap.VaLow;
            _prevVaHigh       = snap.VaHigh;

            if (_barCache.Count > _maxCacheBars) EvictOldest();
        }

        private BarSnapshot BuildSnapshot(int barIdx, Dictionary<long, BarLevel> src)
        {
            if (src.Count == 0) return null;

            var levels = new List<BarLevel>(src.Values);
            levels.Sort((a, b) => a.Price.CompareTo(b.Price));
            int n = levels.Count;

            double tickSize  = Instrument.MasterInstrument.TickSize;
            double imbalRatio = ImbalanceTriggerPct / 100.0;

            long totalAsk = 0, totalBid = 0, pocVol = 0;
            double pocPrice = levels[0].Price;
            long maxDelta = long.MinValue, minDelta = long.MaxValue;
            foreach (var lv in levels)
            {
                totalAsk += lv.AskVol;
                totalBid += lv.BidVol;
                if (lv.TotalVol > pocVol) { pocVol = lv.TotalVol; pocPrice = lv.Price; }
                if (lv.Delta > maxDelta) maxDelta = lv.Delta;
                if (lv.Delta < minDelta) minDelta = lv.Delta;
            }

            double barHigh  = barIdx < BarsArray[0].Count ? BarsArray[0].GetHigh(barIdx)  : levels[n-1].Price;
            double barLow   = barIdx < BarsArray[0].Count ? BarsArray[0].GetLow(barIdx)   : levels[0].Price;
            double barOpen  = barIdx < BarsArray[0].Count ? BarsArray[0].GetOpen(barIdx)  : levels[0].Price;
            double barClose = barIdx < BarsArray[0].Count ? BarsArray[0].GetClose(barIdx) : levels[n-1].Price;

            // Value Area — two-level expansion from POC
            long vaTarget = (long)Math.Ceiling(totalAsk + totalBid) * ValueAreaPercent / 100;
            long vaAccum  = pocVol;
            int  pocIdx   = levels.FindIndex(lv => Math.Abs(lv.Price - pocPrice) < tickSize * 0.5);
            if (pocIdx < 0) pocIdx = n / 2;
            int vaLo = pocIdx, vaHi = pocIdx;
            while (vaAccum < vaTarget && (vaLo > 0 || vaHi < n-1))
            {
                long addLo = vaLo > 0     ? levels[vaLo-1].TotalVol : 0;
                long addHi = vaHi < n-1   ? levels[vaHi+1].TotalVol : 0;
                if      (addLo >= addHi && vaLo > 0) { vaLo--; vaAccum += levels[vaLo].TotalVol; }
                else if (vaHi < n-1)                 { vaHi++; vaAccum += levels[vaHi].TotalVol; }
                else if (vaLo > 0)                   { vaLo--; vaAccum += levels[vaLo].TotalVol; }
                else break;
            }

            // Diagonal imbalances
            var isBuy  = new bool[n];
            var isSell = new bool[n];
            for (int i = 0; i < n; i++)
            {
                if (i > 0   && levels[i-1].BidVol >= MinImbalanceVolume && levels[i].AskVol >= imbalRatio * levels[i-1].BidVol) isBuy[i]  = true;
                if (i < n-1 && levels[i+1].AskVol >= MinImbalanceVolume && levels[i].BidVol >= imbalRatio * levels[i+1].AskVol) isSell[i] = true;
            }

            // Stacked imbalances
            var isSB = new bool[n]; var isSS = new bool[n]; int consec = 0;
            for (int i = 0; i < n; i++)     { consec = isBuy[i]  ? consec+1 : 0; if (consec >= StackedThreshold) for (int k = i-consec+1; k<=i; k++) isSB[k] = true; }
            consec = 0;
            for (int i = n-1; i >= 0; i--)  { consec = isSell[i] ? consec+1 : 0; if (consec >= StackedThreshold) for (int k = i+consec-1; k>=i; k--) isSS[k] = true; }

            long barDelta = totalAsk - totalBid;
            long totalVol = totalAsk + totalBid;
            bool isUpCandle = barClose >= barOpen;

            bool zeroBuy     = levels[0].BidVol == 0;
            bool zeroSell    = levels[n-1].AskVol == 0;
            bool exhBull     = levels[0].BidVol > 0    && levels[0].BidVol    <= ExhaustionMaxVolume;
            bool exhBear     = levels[n-1].AskVol > 0  && levels[n-1].AskVol  <= ExhaustionMaxVolume;
            bool buyingTail  = zeroBuy  || exhBull;
            bool sellingTail = zeroSell || exhBear;

            bool volDecBull = false, volDecBear = false;
            if (n >= 3)
            {
                volDecBull = levels[2].BidVol > levels[1].BidVol && levels[1].BidVol > levels[0].BidVol;
                volDecBear = levels[n-3].AskVol > levels[n-2].AskVol && levels[n-2].AskVol > levels[n-1].AskVol;
            }

            bool retailBull = false, retailBear = false;
            if (n >= 3)
            {
                retailBull = levels[0].BidVol > levels[1].BidVol && levels[1].BidVol > levels[2].BidVol && levels[0].BidVol >= MinImbalanceVolume*3;
                retailBear = levels[n-1].AskVol > levels[n-2].AskVol && levels[n-2].AskVol > levels[n-3].AskVol && levels[n-1].AskVol >= MinImbalanceVolume*3;
            }

            bool imbalRevBull = false, imbalRevBear = false;
            if (n >= 2 &&  isUpCandle) imbalRevBull = levels[0].AskVol   >= imbalRatio * levels[1].BidVol   && levels[0].AskVol   >= MinImbalanceReversalVolume;
            if (n >= 2 && !isUpCandle) imbalRevBear = levels[n-1].BidVol >= imbalRatio * levels[n-2].AskVol && levels[n-1].BidVol >= MinImbalanceReversalVolume;

            int buyImbalCount  = isBuy.Count(b => b);
            int sellImbalCount = isSell.Count(b => b);
            bool multiImbal = (isUpCandle  && buyImbalCount  >= MultipleImbalanceCount)
                           || (!isUpCandle && sellImbalCount >= MultipleImbalanceCount);

            bool stopVol = false;
            if (n >= 2)
            {
                long edgeLo  = levels[0].TotalVol + (n > 1 ? levels[1].TotalVol : 0);
                long edgeHi  = levels[n-1].TotalVol + (n > 1 ? levels[n-2].TotalVol : 0);
                long avgVol  = totalVol / n;
                stopVol = edgeLo >= avgVol*3 || edgeHi >= avgVol*3;
            }

            bool smallMinMax     = Math.Abs(maxDelta) <= MinImbalanceVolume || Math.Abs(minDelta) <= MinImbalanceVolume;
            bool extrBarDelta    = Math.Abs(barDelta)  >= ExtremeBarDeltaThreshold;
            bool extrBarVolume   = totalVol             >= ExtremeBarVolumeThreshold;
            bool extrDeltaThresh = (barDelta > 0 && barDelta >= maxDelta * 0.95) || (barDelta < 0 && barDelta <= minDelta * 0.95);
            bool extrDeltaVol    = totalVol > 0 && Math.Abs(barDelta) * 100 / totalVol >= ExtremeDeltaVolumeThresholdPct;
            bool accumDist       = isUpCandle ? totalAsk > totalBid : totalBid > totalAsk;
            bool priceActDiv     = (isUpCandle && barDelta < 0) || (!isUpCandle && barDelta > 0);

            double bodyHigh = Math.Max(barOpen, barClose);
            double bodyLow  = Math.Min(barOpen, barClose);
            bool pocShadow  = pocPrice > bodyHigh || pocPrice < bodyLow;

            bool deltaTailBull = false, deltaTailBear = false;
            if (isUpCandle && n >= 3)  { deltaTailBull = levels[0].Delta < 0;       for (int i=1;i<n;i++) if (levels[i].Delta<0) { deltaTailBull=false; break; } }
            if (!isUpCandle && n >= 3) { deltaTailBear = levels[n-1].Delta > 0;     for (int i=0;i<n-1;i++) if (levels[i].Delta>0) { deltaTailBear=false; break; } }

            bool unfinished = false;
            if (n >= 1)
            {
                bool atHigh = Math.Abs(levels[n-1].Price - barHigh) < tickSize;
                bool atLow  = Math.Abs(levels[0].Price   - barLow)  < tickSize;
                unfinished  = (atHigh && levels[n-1].BidVol>0 && levels[n-1].AskVol>0)
                           || (atLow  && levels[0].BidVol>0   && levels[0].AskVol>0);
            }

            bool ofRatioBull = false, ofRatioBear = false;
            if (levels[0].AskVol > 0 || levels[n-1].BidVol > 0)
            {
                if (isUpCandle)  ofRatioBull = (double)levels[0].AskVol     / Math.Max(1, levels[n-1].BidVol) >= OFRatioExhaustionLevel;
                else             ofRatioBear = (double)levels[n-1].BidVol   / Math.Max(1, levels[0].AskVol)   >= OFRatioExhaustionLevel;
            }

            bool vertLiq = false;
            if (n >= 3)
            {
                long avgLvlVol = totalVol / n;
                int cb = 0, ca = 0;
                for (int i=0;i<n;i++) { cb = levels[i].BidVol>=avgLvlVol*2?cb+1:0; ca = levels[i].AskVol>=avgLvlVol*2?ca+1:0; if (cb>=3||ca>=3){vertLiq=true;break;} }
            }

            bool restLiq = false;
            long restThresh = Math.Max(MinImbalanceVolume*10, totalVol / Math.Max(1,n) * 5);
            foreach (var lv in levels) if (lv.BidVol>=restThresh || lv.AskVol>=restThresh) { restLiq=true; break; }

            bool marketSweep = false;
            int sweepConsec = 0;
            for (int i=0;i<n;i++) { sweepConsec = (levels[i].BidVol==0||levels[i].AskVol==0) ? sweepConsec+1 : 0; if (sweepConsec>=3){marketSweep=true;break;} }

            bool ofSeq = false;
            if (n >= 4) { bool aa=true, ab=true; for(int i=1;i<Math.Min(n,5);i++){if(levels[i].AskVol<=levels[i-1].AskVol)aa=false;if(levels[i].BidVol<=levels[i-1].BidVol)ab=false;} ofSeq=aa||ab; }

            bool mkWeak = false;
            if (n>=3 && !isUpCandle) { bool bg = levels[0].BidVol < levels[n/2].BidVol; mkWeak = bg && totalBid < totalBid/2+1; }

            bool imbalAuction = (!isUpCandle && isSell[n-1]) || (isUpCandle && isBuy[0]);

            return new BarSnapshot
            {
                BarIndex             = barIdx,
                Open=barOpen, High=barHigh, Low=barLow, Close=barClose,
                BarDelta             = barDelta,
                TotalVolume          = totalVol,
                MaxDelta             = maxDelta,
                MinDelta             = minDelta,
                PocPrice             = pocPrice,
                VaLow                = levels[vaLo].Price,
                VaHigh               = levels[vaHi].Price,
                Levels               = levels,
                IsBuyImbalance       = isBuy,
                IsSellImbalance      = isSell,
                IsStackedBuy         = isSB,
                IsStackedSell        = isSS,
                SigZeroBuy           = zeroBuy,
                SigZeroSell          = zeroSell,
                SigExhaustionBull    = exhBull,
                SigExhaustionBear    = exhBear,
                SigBuyingTail        = buyingTail,
                SigSellingTail       = sellingTail,
                SigVolumeDecline     = volDecBull || volDecBear,
                SigRetailSuck        = retailBull || retailBear,
                SigImbalanceReversal = imbalRevBull || imbalRevBear,
                SigMultipleImbalance = multiImbal,
                SigStoppingVolume    = stopVol,
                SigSmallMinMaxDelta  = smallMinMax,
                SigExtremeBarDelta   = extrBarDelta,
                SigExtremeBarVolume  = extrBarVolume,
                SigExtremeDeltaThreshold = extrDeltaThresh,
                SigExtremeDeltaVolume    = extrDeltaVol,
                SigAccumDist         = accumDist,
                SigPriceActionDiv    = priceActDiv,
                SigPocShadow         = pocShadow,
                SigDeltaTail         = deltaTailBull || deltaTailBear,
                SigUnfinishedAuction = unfinished,
                SigOFRatioBull       = ofRatioBull,
                SigOFRatioBear       = ofRatioBear,
                SigVerticalLiquidity = vertLiq,
                SigRestingLiquidity  = restLiq,
                SigMarketSweep       = marketSweep,
                SigOFSequencing      = ofSeq,
                SigMarketWeakness    = mkWeak,
                SigImbalanceAuction  = imbalAuction,
            };
        }

        private void ComputeInterBarSignals(BarSnapshot snap)
        {
            if (ShowDeltaDivergence)
            {
                bool atDayHigh = Math.Abs(snap.High - _dayHigh) < Instrument.MasterInstrument.TickSize;
                bool atDayLow  = Math.Abs(snap.Low  - _dayLow)  < Instrument.MasterInstrument.TickSize;
                bool div = (snap.IsUpBar && snap.BarDelta < 0) || (snap.IsDownBar && snap.BarDelta > 0);
                snap.SigDeltaDivergence = div && (!DeltaDivergenceDayExtremeOnly || atDayHigh || atDayLow);
            }
            if (ShowDeltaSurge)
            {
                bool bullSurge = snap.BarDelta>0 && _prevBarDelta>0 && snap.BarDelta>_prevBarDelta && _prevBarDelta>_prevPrevBarDelta;
                bool bearSurge = snap.BarDelta<0 && _prevBarDelta<0 && snap.BarDelta<_prevBarDelta && _prevBarDelta<_prevPrevBarDelta;
                snap.SigDeltaSurge = bullSurge || bearSurge;
            }
            if (ShowDeltaBreakout)
                snap.SigDeltaBreakout = (snap.BarDelta>0 && snap.BarDelta>_prevBarDelta) || (snap.BarDelta<0 && snap.BarDelta<_prevBarDelta);
            if (ShowExtremeDeltaChange)
                snap.SigExtremeDeltaChange = Math.Abs(snap.BarDelta - _prevBarDelta) >= ExtremeDeltaChangeThreshold;
            if (ShowExtremeDeltaChangePct && _prevBarDelta != 0)
                snap.SigExtremeDeltaChangePct = Math.Abs((snap.BarDelta - _prevBarDelta) * 100L / _prevBarDelta) >= ExtremeDeltaChangePctThreshold;
            if (ShowEngulfingVA && _prevVaLow != 0)
            {
                snap.SigEngulfingVABull = snap.IsUpBar   && snap.VaLow <= _prevVaLow && snap.VaHigh >= _prevVaHigh;
                snap.SigEngulfingVABear = snap.IsDownBar && snap.VaLow <= _prevVaLow && snap.VaHigh >= _prevVaHigh;
            }
            if (ShowOFGaps && _prevVaLow != 0)
                snap.SigOFGaps = snap.VaLow > _prevVaHigh || snap.VaHigh < _prevVaLow;
            if (ShowDeltaStack && snap.Levels != null)
            {
                int pos=0, neg=0;
                foreach (var lv in snap.Levels)
                {
                    pos = lv.Delta >  MinImbalanceVolume ? pos+1 : 0;
                    neg = lv.Delta < -MinImbalanceVolume ? neg+1 : 0;
                    if (pos>=3||neg>=3) { snap.SigDeltaStack=true; break; }
                }
            }
        }

        // ══════════════════════════════════════════════════════════════════════
        //  SIGNAL MARKERS
        // ══════════════════════════════════════════════════════════════════════

        private void ApplySignals(BarSnapshot snap)
        {
            int b = CurrentBar - snap.BarIndex;
            if (b < 0 || b > CurrentBar) return;

            double ts   = Instrument.MasterInstrument.TickSize;
            double lo   = snap.Low;
            double hi   = snap.High;
            int    idx  = snap.BarIndex;

            // Brushes (created inline; acceptable for bar-close frequency)
            Brush gold    = new SolidColorBrush(Colors.Gold);
            Brush lime    = new SolidColorBrush(Colors.Lime);
            Brush dodger  = new SolidColorBrush(Colors.DodgerBlue);
            Brush orange  = new SolidColorBrush(Colors.OrangeRed);
            Brush cyan    = new SolidColorBrush(Colors.Cyan);
            Brush magenta = new SolidColorBrush(Colors.Magenta);
            Brush teal    = new SolidColorBrush(Colors.Teal);
            Brush yellow  = new SolidColorBrush(Colors.Yellow);
            Brush white   = new SolidColorBrush(Colors.White);
            Brush gray    = new SolidColorBrush(Colors.Gray);

            // Stacking offsets to prevent overlap
            int slotUp = 1, slotDn = 1;
            double BaseUp() => lo - ts * (slotUp++ * 3);
            double BaseDn() => hi + ts * (slotDn++ * 3);

            // ── Directional signal arrows ────────────────────────────────────

            if (ShowDeltaDivergence && snap.SigDeltaDivergence)
            {
                if (snap.IsUpBar)   Draw.ArrowUp(  "div_"  + idx, true, b, BaseUp(), gold);
                else                Draw.ArrowDown("div_"  + idx, true, b, BaseDn(), gold);
            }
            if (ShowExhaustionPrints)
            {
                if (snap.SigExhaustionBull) Draw.ArrowUp(  "exhb_" + idx, true, b, BaseUp(), lime);
                if (snap.SigExhaustionBear) Draw.ArrowDown("exhs_" + idx, true, b, BaseDn(), orange);
            }
            if (ShowBuySellTail)
            {
                if (snap.SigBuyingTail)  Draw.ArrowUp(  "btail_" + idx, true, b, BaseUp(), dodger);
                if (snap.SigSellingTail) Draw.ArrowDown("stail_" + idx, true, b, BaseDn(), orange);
            }
            if (ShowOFRatio)
            {
                if (snap.SigOFRatioBull) Draw.ArrowUp(  "ofr_b_" + idx, true, b, BaseUp(), new SolidColorBrush(OFRatioBullColor));
                if (snap.SigOFRatioBear) Draw.ArrowDown("ofr_s_" + idx, true, b, BaseDn(), new SolidColorBrush(OFRatioBearColor));
            }
            if (ShowEngulfingVA)
            {
                if (snap.SigEngulfingVABull) Draw.ArrowUp(  "eva_b_" + idx, true, b, BaseUp(), new SolidColorBrush(EngulfingVABullColor));
                if (snap.SigEngulfingVABear) Draw.ArrowDown("eva_s_" + idx, true, b, BaseDn(), new SolidColorBrush(EngulfingVABearColor));
            }
            if (ShowMarketSweep && snap.SigMarketSweep)
            {
                if (snap.IsUpBar)   Draw.ArrowUp(  "swp_" + idx, true, b, BaseUp(), cyan);
                else                Draw.ArrowDown("swp_" + idx, true, b, BaseDn(), cyan);
            }
            if (ShowImbalanceReversal && snap.SigImbalanceReversal)
            {
                if (snap.IsUpBar)   Draw.TriangleUp(  "irev_" + idx, true, b, BaseUp(), dodger);
                else                Draw.TriangleDown("irev_" + idx, true, b, BaseDn(), orange);
            }
            if (ShowMultipleImbalance && snap.SigMultipleImbalance)
            {
                if (snap.IsUpBar)   Draw.ArrowUp(  "mimb_" + idx, true, b, BaseUp(), new SolidColorBrush(StackedBuyColor));
                else                Draw.ArrowDown("mimb_" + idx, true, b, BaseDn(), new SolidColorBrush(StackedSellColor));
            }
            if (ShowZeroPrint)
            {
                if (snap.SigZeroBuy)  Draw.ArrowUp(  "zrb_" + idx, true, b, BaseUp(), white);
                if (snap.SigZeroSell) Draw.ArrowDown("zrs_" + idx, true, b, BaseDn(), white);
            }

            // ── Neutral diamonds (below bar) ─────────────────────────────────

            double DmPos() => lo - ts * (slotUp++ * 2);

            if (ShowStoppingVolume    && snap.SigStoppingVolume)    Draw.Diamond("sv_"   + idx, true, b, DmPos(), teal);
            if (ShowDeltaSurge        && snap.SigDeltaSurge)        Draw.Diamond("dsu_"  + idx, true, b, DmPos(), yellow);
            if (ShowDeltaBreakout     && snap.SigDeltaBreakout)     Draw.Dot    ("dbk_"  + idx, true, b, DmPos(), yellow);
            if (ShowDeltaTail         && snap.SigDeltaTail)         Draw.Diamond("dtl_"  + idx, true, b, DmPos(), gray);
            if (ShowDeltaStack        && snap.SigDeltaStack)        Draw.Dot    ("dstk_" + idx, true, b, DmPos(), gray);
            if (ShowVolumeDecline     && snap.SigVolumeDecline)     Draw.Diamond("vdec_" + idx, true, b, DmPos(), gray);
            if (ShowRetailSuck        && snap.SigRetailSuck)        Draw.Dot    ("ret_"  + idx, true, b, DmPos(), orange);
            if (ShowVerticalLiquidity && snap.SigVerticalLiquidity) Draw.Diamond("vliq_" + idx, true, b, DmPos(), lime);
            if (ShowRestingLiquidity  && snap.SigRestingLiquidity)  Draw.Dot    ("rliq_" + idx, true, b, DmPos(), lime);
            if (ShowOFSequencing      && snap.SigOFSequencing)      Draw.Dot    ("ofs_"  + idx, true, b, DmPos(), dodger);
            if (ShowMarketWeakness    && snap.SigMarketWeakness)    Draw.Diamond("mkw_"  + idx, true, b, DmPos(), gray);
            if (ShowUnfinishedAuctions&& snap.SigUnfinishedAuction) Draw.Diamond("ufa_"  + idx, true, b, DmPos(), white);
            if (ShowPocShadows        && snap.SigPocShadow)         Draw.Dot    ("poc_"  + idx, true, b, DmPos(), yellow);
            if (ShowPriceActionDivergence && snap.SigPriceActionDiv)Draw.Diamond("pad_"  + idx, true, b, DmPos(), gold);
            if (ShowAccumDist         && snap.SigAccumDist)         Draw.Dot    ("acd_"  + idx, true, b, DmPos(), gray);
            if (ShowImbalanceAuction  && snap.SigImbalanceAuction)  Draw.Dot    ("iauc_" + idx, true, b, DmPos(), cyan);
            if (ShowOFGaps            && snap.SigOFGaps)            Draw.Diamond("ofg_"  + idx, true, b, DmPos(), cyan);
            if (ShowSmallMinMaxDelta  && snap.SigSmallMinMaxDelta)  Draw.Dot    ("smd_"  + idx, true, b, DmPos(), gray);
            if (ShowExtremeBarDelta   && snap.SigExtremeBarDelta)   Draw.Diamond("ebd_"  + idx, true, b, DmPos(), magenta);
            if (ShowExtremeBarVolume  && snap.SigExtremeBarVolume)  Draw.Dot    ("ebv_"  + idx, true, b, DmPos(), magenta);
            if (ShowExtremeDeltaThreshold && snap.SigExtremeDeltaThreshold) Draw.Dot("edt_" + idx, true, b, DmPos(), magenta);
            if (ShowExtremeDeltaVolume    && snap.SigExtremeDeltaVolume)     Draw.Diamond("edv_" + idx, true, b, DmPos(), magenta);
            if (ShowExtremeDeltaChange    && snap.SigExtremeDeltaChange)     Draw.Diamond("edc_" + idx, true, b, DmPos(), orange);
            if (ShowExtremeDeltaChangePct && snap.SigExtremeDeltaChangePct)  Draw.Dot("edcp_" + idx, true, b, DmPos(), orange);

            // ── Output window log ─────────────────────────────────────────────
            if (PrintSignals)
            {
                var active = new System.Text.StringBuilder();
                if (snap.SigDeltaDivergence)   active.Append("DeltaDiv ");
                if (snap.SigDeltaSurge)        active.Append("DeltaSurge ");
                if (snap.SigDeltaBreakout)     active.Append("DeltaBrk ");
                if (snap.SigDeltaTail)         active.Append("DeltaTail ");
                if (snap.SigDeltaStack)        active.Append("DeltaStack ");
                if (snap.SigBuyingTail)        active.Append("BuyTail ");
                if (snap.SigSellingTail)       active.Append("SellTail ");
                if (snap.SigZeroBuy)           active.Append("ZeroBuy ");
                if (snap.SigZeroSell)          active.Append("ZeroSell ");
                if (snap.SigExhaustionBull)    active.Append("ExhBull ");
                if (snap.SigExhaustionBear)    active.Append("ExhBear ");
                if (snap.SigOFRatioBull)       active.Append("RatioBull ");
                if (snap.SigOFRatioBear)       active.Append("RatioBear ");
                if (snap.SigEngulfingVABull)   active.Append("EngulfVABull ");
                if (snap.SigEngulfingVABear)   active.Append("EngulfVABear ");
                if (snap.SigStoppingVolume)    active.Append("StopVol ");
                if (snap.SigRetailSuck)        active.Append("RetailSuck ");
                if (snap.SigVolumeDecline)     active.Append("VolDecline ");
                if (snap.SigMarketSweep)       active.Append("Sweep ");
                if (snap.SigMarketWeakness)    active.Append("MktWeak ");
                if (snap.SigOFSequencing)      active.Append("OFSeq ");
                if (snap.SigVerticalLiquidity) active.Append("VertLiq ");
                if (snap.SigRestingLiquidity)  active.Append("RestLiq ");
                if (snap.SigImbalanceReversal) active.Append("ImbalRev ");
                if (snap.SigImbalanceAuction)  active.Append("ImbalAuction ");
                if (snap.SigMultipleImbalance) active.Append("MultiImbal ");
                if (snap.SigUnfinishedAuction) active.Append("Unfinished ");
                if (snap.SigPocShadow)         active.Append("PocShadow ");
                if (snap.SigOFGaps)            active.Append("OFGaps ");
                if (snap.SigPriceActionDiv)    active.Append("PADiv ");
                if (snap.SigAccumDist)         active.Append("AccumDist ");
                if (snap.SigExtremeBarDelta)   active.Append("ExtBarDelta ");
                if (snap.SigExtremeBarVolume)  active.Append("ExtBarVol ");
                if (snap.SigExtremeDeltaThreshold) active.Append("ExtDeltaThr ");
                if (snap.SigExtremeDeltaVolume)    active.Append("ExtDeltaVol ");
                if (snap.SigExtremeDeltaChange)    active.Append("ExtDeltaChg ");
                if (snap.SigExtremeDeltaChangePct) active.Append("ExtDeltaChg% ");
                if (snap.SigSmallMinMaxDelta)  active.Append("SmallMM ");

                if (active.Length > 0)
                    Print($"[OFE] Bar#{idx}  Δ={snap.BarDelta:+#;-#;0}  CVD={snap.Cvd:+#;-#;0}"
                        + $"  Vol={snap.TotalVolume}  Sigs: {active}");
            }
        }

        // ══════════════════════════════════════════════════════════════════════
        //  HELPERS
        // ══════════════════════════════════════════════════════════════════════

        private void EvictOldest()
        {
            var keys = new List<int>(_barCache.Keys);
            keys.Sort();
            int remove = Math.Max(50, keys.Count / 10);
            for (int i = 0; i < Math.Min(remove, keys.Count); i++)
                _barCache.Remove(keys[i]);
        }

        private int ComputeLiveBuffer()
        {
            if (BarsPeriod == null) return 500;
            switch (BarsPeriod.BarsPeriodType)
            {
                case BarsPeriodType.Second: return Math.Max(200, 86400 / Math.Max(1, BarsPeriod.Value));
                case BarsPeriodType.Minute: return Math.Max(100, 1440  / Math.Max(1, BarsPeriod.Value));
                case BarsPeriodType.Day:    return 5;
                case BarsPeriodType.Week:   return 4;
                case BarsPeriodType.Month:  return 3;
                default:                    return 2000;
            }
        }
    }
}
