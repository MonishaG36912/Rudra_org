// OFEFootprintNT.cs  — OFE Footprint Chart, NinjaTrader 8
// Session 10: all 48 Orderflows Trader 8 indicators implemented as NT8 Properties.
//
// Works with ANY NT8 data provider. No external bridge required.
// Requires: Chart Properties → Data → Tick Replay = Yes  (for historical bars)
//
// Install: copy to Documents\NinjaTrader 8\bin\Custom\Indicators\
//          Tools → NinjaScript Editor → Compile (Ctrl+F5)

#region Using declarations
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.ComponentModel.DataAnnotations;
using System.Linq;
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
    // ── Category ordering in the NT8 Properties dialog ────────────────────────
    [Gui.CategoryOrder("Global",          1)]
    [Gui.CategoryOrder("Chart Type",      2)]
    [Gui.CategoryOrder("Display",         3)]
    [Gui.CategoryOrder("Value Area",      4)]
    [Gui.CategoryOrder("Imbalance",       5)]
    [Gui.CategoryOrder("Delta Signals",   6)]
    [Gui.CategoryOrder("POC Signals",     7)]
    [Gui.CategoryOrder("Volume Signals",  8)]
    [Gui.CategoryOrder("Extreme Levels",  9)]
    [Gui.CategoryOrder("Swing / Moment", 10)]
    [Gui.CategoryOrder("Volume Profile", 11)]
    [Gui.CategoryOrder("Summary Bar",    12)]
    [Gui.CategoryOrder("Colours",        13)]
    public class OFEFootprintNT : Indicator
    {
        // ══════════════════════════════════════════════════════════════════════
        //  NT8 PROPERTIES
        // ══════════════════════════════════════════════════════════════════════

        // ── Global Settings ───────────────────────────────────────────────────
        [NinjaScriptProperty]
        [Range(1, 100)]
        [Display(Name = "Value Area %", Order = 1, GroupName = "Global",
            Description = "% of bar volume defining the value area (default 70).")]
        public int ValueAreaPercent { get; set; }

        [NinjaScriptProperty]
        [Range(0, 9999)]
        [Display(Name = "Min Imbalance Volume", Order = 2, GroupName = "Global",
            Description = "Minimum contracts at a level to qualify as an imbalance (default 10).")]
        public int MinImbalanceVolume { get; set; }

        [NinjaScriptProperty]
        [Range(100, 2000)]
        [Display(Name = "Imbalance Trigger %", Order = 3, GroupName = "Global",
            Description = "Ratio % for imbalance detection — 400 = 4:1 (default 400).")]
        public int ImbalanceTriggerPct { get; set; }

        [NinjaScriptProperty]
        [Range(1, 20)]
        [Display(Name = "Swing Period", Order = 4, GroupName = "Global",
            Description = "Bars left/right used to identify swing highs/lows (default 5).")]
        public int SwingPeriod { get; set; }

        [NinjaScriptProperty]
        [Range(1, 6)]
        [Display(Name = "Momentum Strength", Order = 5, GroupName = "Global",
            Description = "1 = first sign of momentum, 6 = strongest. Filter for momentum-aware indicators (default 3).")]
        public int MomentumStrength { get; set; }

        [NinjaScriptProperty]
        [Range(0, 20)]
        [Display(Name = "Signal Spacing (ticks)", Order = 6, GroupName = "Global",
            Description = "Minimum tick gap between signal markers (default 1).")]
        public int SignalSpacingTicks { get; set; }

        // ── Chart Type ────────────────────────────────────────────────────────
        [NinjaScriptProperty]
        [Display(Name = "Footprint Type", Order = 1, GroupName = "Chart Type",
            Description = "BidAsk | Delta | Volume | DiagonalDelta")]
        public FootprintMode FootprintType { get; set; }

        [NinjaScriptProperty]
        [Range(1, 20)]
        [Display(Name = "Ticks Aggregation", Order = 2, GroupName = "Chart Type",
            Description = "Merge N ticks into one display row (1 = full per-tick detail).")]
        public int TicksAggregation { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Shorten Big Numbers", Order = 3, GroupName = "Chart Type",
            Description = "Show volumes as 47.5K / 1.1M instead of full digits.")]
        public bool ShortenBigNumbers { get; set; }

        // ── Display ────────────────────────────────────────────────────────────
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
        [Display(Name = "Show Delta Histogram", Order = 4, GroupName = "Display",
            Description = "Color footprint cells by per-level delta (positive = blue, negative = red).")]
        public bool ShowDeltaHistogram { get; set; }

        // ── Value Area ────────────────────────────────────────────────────────
        [NinjaScriptProperty]
        [Display(Name = "Show Value Area", Order = 1, GroupName = "Value Area",
            Description = "Highlight value area: green (up bar), red (down bar), gray (doji). Default ON.")]
        public bool ShowValueArea { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Engulfing VA", Order = 2, GroupName = "Value Area",
            Description = "Engulfing Value Area — current VA engulfs previous bar VA. Default ON.")]
        public bool ShowEngulfingVA { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show VA Absorption", Order = 3, GroupName = "Value Area",
            Description = "Value Area Absorption — unique absorption within the VA. Default OFF.")]
        public bool ShowValueAreaAbsorption { get; set; }

        // ── Imbalance ─────────────────────────────────────────────────────────
        [NinjaScriptProperty]
        [Range(2, 10)]
        [Display(Name = "Stacked Threshold", Order = 1, GroupName = "Imbalance",
            Description = "Consecutive imbalances required for a stacked imbalance (default 3).")]
        public int StackedThreshold { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Stacked Imbalance", Order = 2, GroupName = "Imbalance",
            Description = "3+ imbalances stacked in same direction. Default ON.")]
        public bool ShowStackedImbalance { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Volume Imbalance", Order = 3, GroupName = "Imbalance",
            Description = "Color imbalance text blue (ask) or red (bid) in the footprint cells. Default ON.")]
        public bool ShowVolumeImbalance { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Multiple Imbalance", Order = 4, GroupName = "Imbalance",
            Description = "Box around bar when multiple imbalances present. Default OFF.")]
        public bool ShowMultipleImbalance { get; set; }

        [NinjaScriptProperty]
        [Range(2, 20)]
        [Display(Name = "Multiple Imbalance Count", Order = 5, GroupName = "Imbalance",
            Description = "Number of imbalances to trigger Multiple Imbalance signal (default 5).")]
        public int MultipleImbalanceCount { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Imbalance Reversal", Order = 6, GroupName = "Imbalance",
            Description = "Reversal at bar edges — trapped traders. Default OFF.")]
        public bool ShowImbalanceReversal { get; set; }

        [NinjaScriptProperty]
        [Range(1, 9999)]
        [Display(Name = "Min Reversal Volume", Order = 7, GroupName = "Imbalance",
            Description = "Min contracts for an imbalance reversal (default 10).")]
        public int MinImbalanceReversalVolume { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Imbalance Auction", Order = 8, GroupName = "Imbalance",
            Description = "Imbalance at bar edge the market retreated from. Default OFF.")]
        public bool ShowImbalanceAuction { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Imbalance Reload", Order = 9, GroupName = "Imbalance",
            Description = "Same-price imbalance on 2 consecutive bars. Default OFF.")]
        public bool ShowImbalanceReload { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Inverse Imbalance", Order = 10, GroupName = "Imbalance",
            Description = "Opposite of stacked imbalance — trapped breakout traders. Default OFF.")]
        public bool ShowInverseImbalance { get; set; }

        // ── Delta Signals ─────────────────────────────────────────────────────
        [NinjaScriptProperty]
        [Display(Name = "Show Delta Divergence", Order = 1, GroupName = "Delta Signals",
            Description = "New high on neg delta or new low on pos delta. Default ON (Day extreme only).")]
        public bool ShowDeltaDivergence { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Divergence: Day Extreme Only", Order = 2, GroupName = "Delta Signals",
            Description = "Limit delta divergence to High-of-Day/Low-of-Day. Default ON.")]
        public bool DeltaDivergenceDayExtremeOnly { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Delta Stack", Order = 3, GroupName = "Delta Signals",
            Description = "3+ strong directional deltas in one bar. Default OFF.")]
        public bool ShowDeltaStack { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Delta Surge", Order = 4, GroupName = "Delta Signals",
            Description = "Directional delta increase over 3 bars. Default OFF.")]
        public bool ShowDeltaSurge { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Delta Tail", Order = 5, GroupName = "Delta Signals",
            Description = "Neg delta at bar low on up candle (bullish) or vice versa. Default OFF.")]
        public bool ShowDeltaTail { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Delta Breakout", Order = 6, GroupName = "Delta Signals",
            Description = "Increasing delta causing a breakout. Default OFF.")]
        public bool ShowDeltaBreakout { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Price Action Divergence", Order = 7, GroupName = "Delta Signals",
            Description = "Green candle + neg delta (bullish demand) or red candle + pos delta (bearish supply). Default OFF.")]
        public bool ShowPriceActionDivergence { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Accumulation/Distribution", Order = 8, GroupName = "Delta Signals",
            Description = "Analyzes bar for accumulation (bullish) or distribution (bearish). Default OFF.")]
        public bool ShowAccumDist { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "A/D Passive Traders Filter", Order = 9, GroupName = "Delta Signals",
            Description = "Extra filter for strong bidders/offers in Accumulation/Distribution. Default OFF.")]
        public bool AccumDistPassiveFilter { get; set; }

        // ── POC Signals ───────────────────────────────────────────────────────
        [NinjaScriptProperty]
        [Display(Name = "Show POC Shadows", Order = 1, GroupName = "POC Signals",
            Description = "POC in the wick of a bar = volume rejection. Default ON.")]
        public bool ShowPocShadows { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Prominent POC", Order = 2, GroupName = "POC Signals",
            Description = "Prominent POC acting as support/resistance. Cyan (bull), Magenta (bear). Default ON.")]
        public bool ShowProminentPoc { get; set; }

        [NinjaScriptProperty]
        [Range(0, 100)]
        [Display(Name = "Prominent POC Look Back Bars", Order = 3, GroupName = "POC Signals",
            Description = "How many prior bars to compare. 0 = look anywhere in structure.")]
        public int ProminentPocLookBackBars { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Aligned POC", Order = 4, GroupName = "POC Signals",
            Description = "Two consecutive POCs at the same level = near-term S/R. Default OFF.")]
        public bool ShowAlignedPoc { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Open POC", Order = 5, GroupName = "POC Signals",
            Description = "POC outside previous bar's range. Default OFF.")]
        public bool ShowOpenPoc { get; set; }

        [NinjaScriptProperty]
        [Range(0, 50)]
        [Display(Name = "Open POC Look Back Bars", Order = 6, GroupName = "POC Signals",
            Description = "Bars to compare for Open POC detection (default 1).")]
        public int OpenPocLookBackBars { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show POC Slingshot", Order = 7, GroupName = "POC Signals",
            Description = "POC trade setup: colors bar POC green/red. Default OFF.")]
        public bool ShowPocSlingshot { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show POC Wave", Order = 8, GroupName = "POC Signals",
            Description = "Three-bar POC setup. Default OFF.")]
        public bool ShowPocWave { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Orderflows Ratio", Order = 9, GroupName = "POC Signals",
            Description = "Exhaustion ratio (≥30) or defense ratio (≤0.69). Default ON.")]
        public bool ShowOFRatio { get; set; }

        [NinjaScriptProperty]
        [Range(10, 200)]
        [Display(Name = "Ratio: Exhaustion Level", Order = 10, GroupName = "POC Signals",
            Description = "Ratio at/above this value = price exhaustion (default 30).")]
        public int OFRatioExhaustionLevel { get; set; }

        // ── Volume Signals ────────────────────────────────────────────────────
        [NinjaScriptProperty]
        [Display(Name = "Show Buying/Selling Tail", Order = 1, GroupName = "Volume Signals",
            Description = "Tail = lack of passive buying/selling at bar edge. Default OFF.")]
        public bool ShowBuySellTail { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Zero Print", Order = 2, GroupName = "Volume Signals",
            Description = "Zero volume at bar edge = momentum sign. Default OFF.")]
        public bool ShowZeroPrint { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Exhaustion Prints", Order = 3, GroupName = "Volume Signals",
            Description = "Small prints at bar edge = exhaustion. Default OFF.")]
        public bool ShowExhaustionPrints { get; set; }

        [NinjaScriptProperty]
        [Range(1, 999)]
        [Display(Name = "Exhaustion Max Volume", Order = 4, GroupName = "Volume Signals",
            Description = "Max contracts at edge to qualify as exhaustion print (default 10).")]
        public int ExhaustionMaxVolume { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Stopping Volume", Order = 5, GroupName = "Volume Signals",
            Description = "High volume in narrow range halting price movement. Default OFF.")]
        public bool ShowStoppingVolume { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Retail Suck", Order = 6, GroupName = "Volume Signals",
            Description = "Heavy volume at edge decreasing toward center = absorption. Default OFF.")]
        public bool ShowRetailSuck { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Volume Decline", Order = 7, GroupName = "Volume Signals",
            Description = "Declining bid/ask volume at bar edge = weakening aggression. Default OFF.")]
        public bool ShowVolumeDecline { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Vertical Liquidity", Order = 8, GroupName = "Volume Signals",
            Description = "Heavier than normal volume over consecutive levels. Default OFF.")]
        public bool ShowVerticalLiquidity { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Resting Liquidity", Order = 9, GroupName = "Volume Signals",
            Description = "Big passive bids/offers that traded = S/R. Default OFF.")]
        public bool ShowRestingLiquidity { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Market Sweep", Order = 10, GroupName = "Volume Signals",
            Description = "Trade through several consecutive levels at once. Default OFF.")]
        public bool ShowMarketSweep { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Market Weakness", Order = 11, GroupName = "Volume Signals",
            Description = "Order flow weakening on directional move = reversal signal. Default OFF.")]
        public bool ShowMarketWeakness { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Orderflows Sequencing", Order = 12, GroupName = "Volume Signals",
            Description = "Aggressive traders getting past larger bids/offers. Default OFF.")]
        public bool ShowOFSequencing { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Orderflows Gaps", Order = 13, GroupName = "Volume Signals",
            Description = "Shift in value area with momentum pickup. Default OFF.")]
        public bool ShowOFGaps { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Unfinished Auctions", Order = 14, GroupName = "Volume Signals",
            Description = "Incomplete price discovery at session extremes. Default OFF.")]
        public bool ShowUnfinishedAuctions { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Small Min/Max Delta", Order = 15, GroupName = "Volume Signals",
            Description = "Bar with little/no aggressive buyer or seller control. Default OFF.")]
        public bool ShowSmallMinMaxDelta { get; set; }

        // ── Extreme Levels ────────────────────────────────────────────────────
        [NinjaScriptProperty]
        [Display(Name = "Show Extreme Bar Delta", Order = 1, GroupName = "Extreme Levels")]
        public bool ShowExtremeBarDelta { get; set; }

        [NinjaScriptProperty]
        [Range(1, 999999)]
        [Display(Name = "Extreme Bar Delta Threshold", Order = 2, GroupName = "Extreme Levels",
            Description = "Bar delta must exceed this value (default 1000).")]
        public int ExtremeBarDeltaThreshold { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Extreme Bar Volume", Order = 3, GroupName = "Extreme Levels")]
        public bool ShowExtremeBarVolume { get; set; }

        [NinjaScriptProperty]
        [Range(1, 9999999)]
        [Display(Name = "Extreme Bar Volume Threshold", Order = 4, GroupName = "Extreme Levels",
            Description = "Bar volume must exceed this value (default 5000).")]
        public int ExtremeBarVolumeThreshold { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Extreme Price Delta", Order = 5, GroupName = "Extreme Levels",
            Description = "Price levels with delta above threshold highlighted in footprint.")]
        public bool ShowExtremePriceDelta { get; set; }

        [NinjaScriptProperty]
        [Range(1, 999999)]
        [Display(Name = "Extreme Price Delta Threshold", Order = 6, GroupName = "Extreme Levels",
            Description = "Per-level delta must exceed this (default 200).")]
        public int ExtremePriceDeltaThreshold { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Extreme Price Level Volume", Order = 7, GroupName = "Extreme Levels",
            Description = "Highlight price levels with exceptional total volume.")]
        public bool ShowExtremePriceLevelVolume { get; set; }

        [NinjaScriptProperty]
        [Range(1, 9999999)]
        [Display(Name = "Extreme Price Level Vol Threshold", Order = 8, GroupName = "Extreme Levels",
            Description = "Per-level total volume must exceed this (default 500).")]
        public int ExtremePriceLevelVolThreshold { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Extreme Delta Threshold", Order = 9, GroupName = "Extreme Levels",
            Description = "Bars closing within 95% of Max or Min Delta.")]
        public bool ShowExtremeDeltaThreshold { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Extreme Delta/Volume", Order = 10, GroupName = "Extreme Levels",
            Description = "Delta as % of total bar volume exceeds threshold.")]
        public bool ShowExtremeDeltaVolume { get; set; }

        [NinjaScriptProperty]
        [Range(1, 100)]
        [Display(Name = "Extreme Delta/Volume %", Order = 11, GroupName = "Extreme Levels",
            Description = "Delta/Volume ratio % threshold (default 60).")]
        public int ExtremeDeltaVolumeThresholdPct { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Extreme Delta Change", Order = 12, GroupName = "Extreme Levels",
            Description = "Large absolute change in delta bar-to-bar.")]
        public bool ShowExtremeDeltaChange { get; set; }

        [NinjaScriptProperty]
        [Range(1, 999999)]
        [Display(Name = "Extreme Delta Change Threshold", Order = 13, GroupName = "Extreme Levels",
            Description = "Absolute delta change threshold (default 500).")]
        public int ExtremeDeltaChangeThreshold { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Extreme Delta Change %", Order = 14, GroupName = "Extreme Levels",
            Description = "Large % change in delta bar-to-bar.")]
        public bool ShowExtremeDeltaChangePct { get; set; }

        [NinjaScriptProperty]
        [Range(1, 10000)]
        [Display(Name = "Extreme Delta Change % Threshold", Order = 15, GroupName = "Extreme Levels",
            Description = "% change in delta threshold (default 200).")]
        public int ExtremeDeltaChangePctThreshold { get; set; }

        // ── Volume Profile ────────────────────────────────────────────────────
        [NinjaScriptProperty]
        [Display(Name = "Show Volume Profile", Order = 1, GroupName = "Volume Profile",
            Description = "Display the day's volume profile. Default OFF.")]
        public bool ShowVolumeProfile { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Volume Profile On Right", Order = 2, GroupName = "Volume Profile",
            Description = "Show profile on right (true) or left (false) of chart.")]
        public bool VolumeProfileOnRight { get; set; }

        // ── Summary Bar ───────────────────────────────────────────────────────
        [NinjaScriptProperty]
        [Display(Name = "Show Summary Bar", Order = 1, GroupName = "Summary Bar",
            Description = "Show the data bar at the bottom of each bar column.")]
        public bool ShowSummaryBar { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show CVD in Status", Order = 2, GroupName = "Summary Bar",
            Description = "Include Cumulative Delta in the status overlay.")]
        public bool ShowCvdInStatus { get; set; }

        // ── Colours ───────────────────────────────────────────────────────────
        [NinjaScriptProperty]
        [Display(Name = "Buy (Ask) Cell",         Order = 1, GroupName = "Colours")]
        public System.Windows.Media.Color BuyColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Sell (Bid) Cell",        Order = 2, GroupName = "Colours")]
        public System.Windows.Media.Color SellColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Imbalance Ask",          Order = 3, GroupName = "Colours")]
        public System.Windows.Media.Color ImbalanceBuyColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Imbalance Bid",          Order = 4, GroupName = "Colours")]
        public System.Windows.Media.Color ImbalanceSellColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Stacked Buy",            Order = 5, GroupName = "Colours")]
        public System.Windows.Media.Color StackedBuyColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Stacked Sell",           Order = 6, GroupName = "Colours")]
        public System.Windows.Media.Color StackedSellColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "POC",                    Order = 7, GroupName = "Colours")]
        public System.Windows.Media.Color PocColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Prominent POC Bull",     Order = 8, GroupName = "Colours")]
        public System.Windows.Media.Color ProminentPocBullColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Prominent POC Bear",     Order = 9, GroupName = "Colours")]
        public System.Windows.Media.Color ProminentPocBearColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Value Area Bull",        Order = 10, GroupName = "Colours")]
        public System.Windows.Media.Color ValueAreaBullColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Value Area Bear",        Order = 11, GroupName = "Colours")]
        public System.Windows.Media.Color ValueAreaBearColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Engulfing VA Bull",      Order = 12, GroupName = "Colours")]
        public System.Windows.Media.Color EngulfingVABullColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Engulfing VA Bear",      Order = 13, GroupName = "Colours")]
        public System.Windows.Media.Color EngulfingVABearColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Delta Divergence",       Order = 14, GroupName = "Colours")]
        public System.Windows.Media.Color DeltaDivergenceColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "OF Ratio Bull",          Order = 15, GroupName = "Colours")]
        public System.Windows.Media.Color OFRatioBullColor { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "OF Ratio Bear",          Order = 16, GroupName = "Colours")]
        public System.Windows.Media.Color OFRatioBearColor { get; set; }

        // ══════════════════════════════════════════════════════════════════════
        //  DATA STRUCTURES
        // ══════════════════════════════════════════════════════════════════════

        public enum FootprintMode { BidAsk, Delta, Volume, DiagonalDelta }

        private struct BarLevel
        {
            public double Price;
            public long   BidVol;
            public long   AskVol;
            public long   TotalVol => BidVol + AskVol;
            public long   Delta    => AskVol - BidVol;
        }

        private sealed class BarSnapshot
        {
            public int    BarIndex;
            public double Open, High, Low, Close;
            public long   BarDelta;
            public long   TotalVolume;
            public long   MaxDelta;
            public long   MinDelta;
            public long   Cvd;
            public double PocPrice;
            public double VaLow, VaHigh;
            public List<BarLevel> Levels;

            // Per-level flags
            public bool[] IsBuyImbalance;
            public bool[] IsSellImbalance;
            public bool[] IsStackedBuy;
            public bool[] IsStackedSell;
            public bool[] IsExtrPriceDelta;
            public bool[] IsExtrPriceLevelVol;

            // Bar-level signals
            public bool SigDeltaDivergence;
            public bool SigDeltaStack;
            public bool SigDeltaSurge;
            public bool SigDeltaTail;
            public bool SigDeltaBreakout;
            public bool SigPriceActionDiv;
            public bool SigAccumDist;
            public bool SigBuyingTail, SigSellingTail;
            public bool SigZeroBuy, SigZeroSell;
            public bool SigExhaustionBull, SigExhaustionBear;
            public bool SigStoppingVolume;
            public bool SigRetailSuck;
            public bool SigVolumeDecline;
            public bool SigMarketSweep;
            public bool SigMarketWeakness;
            public bool SigOFSequencing;
            public bool SigOFGaps;
            public bool SigUnfinishedAuction;
            public bool SigSmallMinMaxDelta;
            public bool SigExtremeBarDelta;
            public bool SigExtremeBarVolume;
            public bool SigExtremeDeltaThreshold;
            public bool SigExtremeDeltaVolume;
            public bool SigExtremeDeltaChange;
            public bool SigExtremeDeltaChangePct;
            public bool SigImbalanceAuction;
            public bool SigImbalanceReversal;
            public bool SigImbalanceReload;
            public bool SigInverseImbalance;
            public bool SigMultipleImbalance;
            public bool SigAlignedPoc;
            public bool SigOpenPoc;
            public bool SigPocShadow;
            public bool SigProminentPocBull, SigProminentPocBear;
            public bool SigPocSlingshot;
            public bool SigPocWave;
            public bool SigOFRatioBull, SigOFRatioBear;
            public bool SigEngulfingVABull, SigEngulfingVABear;
            public bool SigVAAbsorption;
            public bool SigVerticalLiquidity;
            public bool SigRestingLiquidity;
            public bool IsUpBar  => Close >= Open;
            public bool IsDownBar => Close < Open;
        }

        // ══════════════════════════════════════════════════════════════════════
        //  ACCUMULATION STATE
        // ══════════════════════════════════════════════════════════════════════

        private readonly Dictionary<long, BarLevel> _accumLevels = new Dictionary<long, BarLevel>(256);
        private int    _accumBarIndex  = -1;
        private double _lastBid, _lastAsk, _prevTradePrice;
        private long   _cvd;
        private long   _prevBarDelta;
        private long   _prevPrevBarDelta;
        private double _prevVaLow, _prevVaHigh;
        private double _dayHigh, _dayLow;
        private long   _prevBarVolume;

        private readonly Dictionary<int, BarSnapshot> _barCache = new Dictionary<int, BarSnapshot>(512);
        private int         _maxCacheBars = 2000;
        private BarSnapshot _liveSnap;
        private bool        _liveDirty = true;

        // Volume Profile accumulation for current day
        private readonly Dictionary<long, long> _dayVolProfile = new Dictionary<long, long>(512);

        // ══════════════════════════════════════════════════════════════════════
        //  SharpDX RESOURCES
        // ══════════════════════════════════════════════════════════════════════

        private SolidColorBrush _buyBrush, _sellBrush;
        private SolidColorBrush _ibuyBrush, _isellBrush;
        private SolidColorBrush _sbuyBrush, _ssellBrush;
        private SolidColorBrush _pocBrush, _ppocBullBrush, _ppocBearBrush;
        private SolidColorBrush _textBrush, _gridBrush, _dividerBrush;
        private SolidColorBrush _statusBgBrush, _statusTextBrush;
        private SolidColorBrush _vaBullBrush, _vaBearBrush, _vaGrayBrush;
        private SolidColorBrush _evaBullBrush, _evaBearBrush;
        private SolidColorBrush _divBullBrush, _divBearBrush;
        private SolidColorBrush _ratioBullBrush, _ratioBearBrush;
        private SolidColorBrush _imbalAskBrush, _imbalBidBrush;
        private SolidColorBrush _multiImbalBullBrush, _multiImbalBearBrush;

        private SharpDX.DirectWrite.Factory    _dw;
        private SharpDX.DirectWrite.TextFormat _cellFmt, _statusFmt, _signalFmt;

        // ══════════════════════════════════════════════════════════════════════
        //  NT8 LIFECYCLE
        // ══════════════════════════════════════════════════════════════════════

        protected override void OnStateChange()
        {
            if (State == State.SetDefaults)
            {
                Name        = "OFE Footprint NT";
                Description = "OFE Footprint — 48 Orderflows Trader 8 indicators, any NT8 data provider.";
                Calculate   = Calculate.OnEachTick;
                IsOverlay   = true;
                IsAutoScale = false;
                DrawOnPricePanel         = true;
                IsSuspendedWhileInactive = false;
                ScaleJustification       = ScaleJustification.Right;

                // Global
                ValueAreaPercent            = 70;
                MinImbalanceVolume          = 10;
                ImbalanceTriggerPct         = 400;
                SwingPeriod                 = 5;
                MomentumStrength            = 3;
                SignalSpacingTicks          = 1;

                // Chart type
                FootprintType               = FootprintMode.BidAsk;
                TicksAggregation            = 1;
                ShortenBigNumbers           = false;

                // Display
                CellFontSize                = 10;
                ShowDeltaLabel              = true;
                ShowPoc                     = true;
                ShowDeltaHistogram          = true;

                // Value Area
                ShowValueArea               = true;
                ShowEngulfingVA             = true;
                ShowValueAreaAbsorption     = false;

                // Imbalance
                StackedThreshold            = 3;
                ShowStackedImbalance        = true;
                ShowVolumeImbalance         = true;
                ShowMultipleImbalance       = false;
                MultipleImbalanceCount      = 5;
                ShowImbalanceReversal       = false;
                MinImbalanceReversalVolume  = 10;
                ShowImbalanceAuction        = false;
                ShowImbalanceReload         = false;
                ShowInverseImbalance        = false;

                // Delta signals
                ShowDeltaDivergence         = true;
                DeltaDivergenceDayExtremeOnly = true;
                ShowDeltaStack              = false;
                ShowDeltaSurge              = false;
                ShowDeltaTail               = false;
                ShowDeltaBreakout           = false;
                ShowPriceActionDivergence   = false;
                ShowAccumDist               = false;
                AccumDistPassiveFilter      = false;

                // POC signals
                ShowPocShadows              = true;
                ShowProminentPoc            = true;
                ProminentPocLookBackBars    = 0;
                ShowAlignedPoc              = false;
                ShowOpenPoc                 = false;
                OpenPocLookBackBars         = 1;
                ShowPocSlingshot            = false;
                ShowPocWave                 = false;
                ShowOFRatio                 = true;
                OFRatioExhaustionLevel      = 30;

                // Volume signals
                ShowBuySellTail             = false;
                ShowZeroPrint               = false;
                ShowExhaustionPrints        = false;
                ExhaustionMaxVolume         = 10;
                ShowStoppingVolume          = false;
                ShowRetailSuck              = false;
                ShowVolumeDecline           = false;
                ShowVerticalLiquidity       = false;
                ShowRestingLiquidity        = false;
                ShowMarketSweep             = false;
                ShowMarketWeakness          = false;
                ShowOFSequencing            = false;
                ShowOFGaps                  = false;
                ShowUnfinishedAuctions      = false;
                ShowSmallMinMaxDelta        = false;

                // Extreme levels
                ShowExtremeBarDelta          = false;
                ExtremeBarDeltaThreshold     = 1000;
                ShowExtremeBarVolume         = false;
                ExtremeBarVolumeThreshold    = 5000;
                ShowExtremePriceDelta        = false;
                ExtremePriceDeltaThreshold   = 200;
                ShowExtremePriceLevelVolume  = false;
                ExtremePriceLevelVolThreshold= 500;
                ShowExtremeDeltaThreshold    = false;
                ShowExtremeDeltaVolume       = false;
                ExtremeDeltaVolumeThresholdPct = 60;
                ShowExtremeDeltaChange       = false;
                ExtremeDeltaChangeThreshold  = 500;
                ShowExtremeDeltaChangePct    = false;
                ExtremeDeltaChangePctThreshold = 200;

                // Volume Profile
                ShowVolumeProfile            = false;
                VolumeProfileOnRight         = true;

                // Summary
                ShowSummaryBar               = true;
                ShowCvdInStatus              = true;

                // Colours
                BuyColor                = System.Windows.Media.Color.FromRgb(0x21, 0x96, 0xF3);  // blue
                SellColor               = System.Windows.Media.Color.FromRgb(0xEF, 0x53, 0x50);  // red
                ImbalanceBuyColor       = System.Windows.Media.Color.FromRgb(0x00, 0x96, 0xFF);
                ImbalanceSellColor      = System.Windows.Media.Color.FromRgb(0xD3, 0x20, 0x20);
                StackedBuyColor         = System.Windows.Media.Color.FromRgb(0x0D, 0x47, 0xA1);
                StackedSellColor        = System.Windows.Media.Color.FromRgb(0x7B, 0x00, 0x00);
                PocColor                = System.Windows.Media.Color.FromRgb(0xFF, 0xEB, 0x3B);  // yellow
                ProminentPocBullColor   = System.Windows.Media.Color.FromRgb(0x00, 0xFF, 0xFF);  // cyan
                ProminentPocBearColor   = System.Windows.Media.Color.FromRgb(0xFF, 0x00, 0xFF);  // magenta
                ValueAreaBullColor      = System.Windows.Media.Color.FromRgb(0x1B, 0x5E, 0x20);  // dark green
                ValueAreaBearColor      = System.Windows.Media.Color.FromRgb(0x7F, 0x00, 0x00);  // dark red
                EngulfingVABullColor    = System.Windows.Media.Color.FromRgb(0x01, 0x57, 0x9B);  // blue
                EngulfingVABearColor    = System.Windows.Media.Color.FromRgb(0x8B, 0x00, 0x00);  // darker red
                DeltaDivergenceColor    = System.Windows.Media.Color.FromRgb(0xFF, 0xD7, 0x00);  // gold
                OFRatioBullColor        = System.Windows.Media.Color.FromRgb(0x00, 0x7A, 0xFF);  // blue
                OFRatioBearColor        = System.Windows.Media.Color.FromRgb(0xEF, 0x53, 0x50);  // red
            }
            else if (State == State.DataLoaded)
            {
                int loadedBars  = BarsArray[0].Count;
                int liveBuffer  = ComputeLiveBuffer();
                _maxCacheBars   = loadedBars + liveBuffer;
                _dayHigh        = double.MinValue;
                _dayLow         = double.MaxValue;
                Print($"[OFE] {Instrument.MasterInstrument.Name}  {BarsPeriod}"
                    + $"  tick={Instrument.MasterInstrument.TickSize}"
                    + $"  loaded={loadedBars}  liveBuffer={liveBuffer}");
            }
            else if (State == State.Terminated)
            {
                DisposeResources();
            }
        }

        protected override void OnBarUpdate() { }

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
            if (_lastAsk > 0 && e.Price >= _lastAsk)       isBuy = true;
            else if (_lastBid > 0 && e.Price <= _lastBid)  isBuy = false;
            else                                            isBuy = e.Price >= _prevTradePrice;
            _prevTradePrice = e.Price;

            double tickSize = Instrument.MasterInstrument.TickSize;
            long   priceKey = PriceKey(e.Price, tickSize);
            BarLevel lv;
            if (!_accumLevels.TryGetValue(priceKey, out lv))
                lv = new BarLevel { Price = Math.Round(e.Price / tickSize) * tickSize };

            long vol = Math.Max(1L, (long)e.Volume);
            if (isBuy) lv.AskVol += vol; else lv.BidVol += vol;
            _accumLevels[priceKey] = lv;

            // Volume profile
            long vpKey = priceKey;
            long vpVol;
            _dayVolProfile.TryGetValue(vpKey, out vpVol);
            _dayVolProfile[vpKey] = vpVol + vol;

            _liveDirty = true;
        }

        // ══════════════════════════════════════════════════════════════════════
        //  BAR CLOSE PROCESSING
        // ══════════════════════════════════════════════════════════════════════

        private void CloseBar(int barIdx)
        {
            var snap = BuildSnapshot(barIdx, _accumLevels);
            if (snap == null) return;

            _cvd          += snap.BarDelta;
            snap.Cvd       = _cvd;

            // Update day extremes for delta divergence
            if (snap.High > _dayHigh) _dayHigh = snap.High;
            if (snap.Low  < _dayLow)  _dayLow  = snap.Low;

            // Detect signals requiring inter-bar context
            ComputeInterBarSignals(snap);

            _barCache[barIdx] = snap;
            _liveSnap   = null;
            _liveDirty  = true;

            // Shift history
            _prevPrevBarDelta = _prevBarDelta;
            _prevBarDelta     = snap.BarDelta;
            _prevVaLow        = snap.VaLow;
            _prevVaHigh       = snap.VaHigh;
            _prevBarVolume    = snap.TotalVolume;

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

            // Totals
            long totalAsk = 0, totalBid = 0;
            long pocVol = 0;
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

            // OHLC from NT8
            double barHigh  = barIdx < BarsArray[0].Count ? BarsArray[0].GetHigh(barIdx)  : levels[n - 1].Price;
            double barLow   = barIdx < BarsArray[0].Count ? BarsArray[0].GetLow(barIdx)   : levels[0].Price;
            double barOpen  = barIdx < BarsArray[0].Count ? BarsArray[0].GetOpen(barIdx)  : levels[0].Price;
            double barClose = barIdx < BarsArray[0].Count ? BarsArray[0].GetClose(barIdx) : levels[n - 1].Price;

            // Value Area (expand from POC until ValueAreaPercent% of volume covered)
            long vaTarget = (long)Math.Ceiling(totalAsk + totalBid) * ValueAreaPercent / 100;
            long vaAccum  = pocVol;
            int  pocIdx   = levels.FindIndex(lv => Math.Abs(lv.Price - pocPrice) < tickSize * 0.5);
            if (pocIdx < 0) pocIdx = n / 2;
            int vaLo = pocIdx, vaHi = pocIdx;
            while (vaAccum < vaTarget && (vaLo > 0 || vaHi < n - 1))
            {
                long addLo = vaLo > 0     ? levels[vaLo - 1].TotalVol : 0;
                long addHi = vaHi < n - 1 ? levels[vaHi + 1].TotalVol : 0;
                if (addLo >= addHi && vaLo > 0)      { vaLo--; vaAccum += levels[vaLo].TotalVol; }
                else if (vaHi < n - 1)               { vaHi++; vaAccum += levels[vaHi].TotalVol; }
                else if (vaLo > 0)                   { vaLo--; vaAccum += levels[vaLo].TotalVol; }
                else break;
            }

            // Diagonal imbalances (buy: ask[i] vs bid[i-1]; sell: bid[i] vs ask[i+1])
            var isBuy  = new bool[n];
            var isSell = new bool[n];
            for (int i = 0; i < n; i++)
            {
                if (i > 0)
                {
                    long askV = levels[i].AskVol;
                    long bidV = levels[i - 1].BidVol;
                    if (bidV >= MinImbalanceVolume && askV >= imbalRatio * bidV) isBuy[i] = true;
                }
                if (i < n - 1)
                {
                    long bidV = levels[i].BidVol;
                    long askV = levels[i + 1].AskVol;
                    if (askV >= MinImbalanceVolume && bidV >= imbalRatio * askV) isSell[i] = true;
                }
            }

            // Stacked imbalances
            var isSB = new bool[n];
            var isSS = new bool[n];
            int consec = 0;
            for (int i = 0; i < n; i++)
            {
                consec = isBuy[i] ? consec + 1 : 0;
                if (consec >= StackedThreshold)
                    for (int k = i - consec + 1; k <= i; k++) isSB[k] = true;
            }
            consec = 0;
            for (int i = n - 1; i >= 0; i--)
            {
                consec = isSell[i] ? consec + 1 : 0;
                if (consec >= StackedThreshold)
                    for (int k = i + consec - 1; k >= i; k--) isSS[k] = true;
            }

            // Extreme price-level flags
            var isExtrPD  = new bool[n];
            var isExtrPLV = new bool[n];
            for (int i = 0; i < n; i++)
            {
                isExtrPD[i]  = ShowExtremePriceDelta       && Math.Abs(levels[i].Delta) >= ExtremePriceDeltaThreshold;
                isExtrPLV[i] = ShowExtremePriceLevelVolume && levels[i].TotalVol >= ExtremePriceLevelVolThreshold;
            }

            long barDelta = totalAsk - totalBid;

            // Zero prints / exhaustion prints at bar edges
            bool zeroBuy  = levels[0].BidVol == 0;                 // buying zero: no bids at low
            bool zeroSell = levels[n - 1].AskVol == 0;             // selling zero: no asks at high
            bool exhBull  = levels[0].BidVol > 0 && levels[0].BidVol <= ExhaustionMaxVolume;
            bool exhBear  = levels[n - 1].AskVol > 0 && levels[n - 1].AskVol <= ExhaustionMaxVolume;

            // Buying/Selling tail: lack of counter-volume at edge
            bool buyingTail  = zeroBuy  || exhBull;
            bool sellingTail = zeroSell || exhBear;

            // Volume decline at bar edges (3 levels)
            bool volDecBull = false, volDecBear = false;
            if (n >= 3)
            {
                // Bullish: bid volume at bottom 3 levels declining (sellers weakening)
                volDecBull = levels[2].BidVol > levels[1].BidVol && levels[1].BidVol > levels[0].BidVol;
                // Bearish: ask volume at top 3 levels declining (buyers weakening)
                volDecBear = levels[n - 3].AskVol > levels[n - 2].AskVol && levels[n - 2].AskVol > levels[n - 1].AskVol;
            }

            // Retail suck: heavy volume at edge that decreases toward center
            bool retailBull = false, retailBear = false;
            if (n >= 3)
            {
                retailBull = levels[0].BidVol > levels[1].BidVol && levels[1].BidVol > levels[2].BidVol && levels[0].BidVol >= MinImbalanceVolume * 3;
                retailBear = levels[n - 1].AskVol > levels[n - 2].AskVol && levels[n - 2].AskVol > levels[n - 3].AskVol && levels[n - 1].AskVol >= MinImbalanceVolume * 3;
            }

            // Imbalance reversal: first two levels at bottom of up candle / top of down candle
            bool imbalRevBull = false, imbalRevBear = false;
            bool isUpCandle = barClose >= barOpen;
            if (n >= 2 && isUpCandle)
                imbalRevBull = levels[0].AskVol >= imbalRatio * levels[1].BidVol && levels[0].AskVol >= MinImbalanceReversalVolume;
            if (n >= 2 && !isUpCandle)
                imbalRevBear = levels[n - 1].BidVol >= imbalRatio * levels[n - 2].AskVol && levels[n - 1].BidVol >= MinImbalanceReversalVolume;

            // Multiple imbalance count
            int buyImbalCount  = isBuy.Count(b => b);
            int sellImbalCount = isSell.Count(b => b);
            bool multiImbal = (isUpCandle  && buyImbalCount  >= MultipleImbalanceCount)
                           || (!isUpCandle && sellImbalCount >= MultipleImbalanceCount);

            // Stopping volume: high volume in narrow price range at bar extreme
            bool stopVol = false;
            if (n >= 2)
            {
                long edgeLo = levels[0].TotalVol + (n > 1 ? levels[1].TotalVol : 0);
                long edgeHi = levels[n - 1].TotalVol + (n > 1 ? levels[n - 2].TotalVol : 0);
                long avgVol = (totalAsk + totalBid) / n;
                stopVol = edgeLo >= avgVol * 3 || edgeHi >= avgVol * 3;
            }

            // Small min/max delta
            bool smallMinMax = Math.Abs(maxDelta) <= MinImbalanceVolume || Math.Abs(minDelta) <= MinImbalanceVolume;

            // Extreme bar delta / volume
            bool extrBarDelta  = Math.Abs(barDelta)  >= ExtremeBarDeltaThreshold;
            bool extrBarVolume = (totalAsk + totalBid) >= ExtremeBarVolumeThreshold;

            // Extreme delta threshold: bar closes within 95% of its max or min delta
            bool extrDeltaThresh = (barDelta > 0 && barDelta >= maxDelta * 0.95)
                                || (barDelta < 0 && barDelta <= minDelta * 0.95);

            // Extreme delta/volume
            long totalVol = totalAsk + totalBid;
            bool extrDeltaVol = totalVol > 0
                && Math.Abs(barDelta) * 100 / totalVol >= ExtremeDeltaVolumeThresholdPct;

            // Accumulation/Distribution: majority of aggressive volume in direction of candle
            bool accumDist = false;
            if (isUpCandle)  accumDist = totalAsk > totalBid;
            else             accumDist = totalBid > totalAsk;

            // Price Action Divergence
            bool priceActDiv = (isUpCandle && barDelta < 0) || (!isUpCandle && barDelta > 0);

            // POC Shadow: POC in bar wick (not in body)
            double bodyHigh = Math.Max(barOpen, barClose);
            double bodyLow  = Math.Min(barOpen, barClose);
            bool pocShadow  = pocPrice > bodyHigh || pocPrice < bodyLow;

            // Delta tail
            bool deltaTailBull = false, deltaTailBear = false;
            if (isUpCandle && n >= 3)
            {
                deltaTailBull = levels[0].Delta < 0;   // bottom level negative delta on up candle
                for (int i = 1; i < n; i++) if (levels[i].Delta < 0) { deltaTailBull = false; break; }
            }
            if (!isUpCandle && n >= 3)
            {
                deltaTailBear = levels[n - 1].Delta > 0; // top level positive delta on down candle
                for (int i = 0; i < n - 1; i++) if (levels[i].Delta > 0) { deltaTailBear = false; break; }
            }

            // Unfinished auction: both bid and ask present at session high or low
            bool unfinished = false;
            if (n >= 1)
            {
                bool atHigh = Math.Abs(levels[n - 1].Price - barHigh) < tickSize;
                bool atLow  = Math.Abs(levels[0].Price - barLow)      < tickSize;
                unfinished  = (atHigh && levels[n - 1].BidVol > 0 && levels[n - 1].AskVol > 0)
                           || (atLow  && levels[0].BidVol > 0       && levels[0].AskVol > 0);
            }

            // Orderflows Ratio: bid_vol_at_low / ask_vol_at_high (or inverse)
            bool ofRatioBull = false, ofRatioBear = false;
            if (levels[0].AskVol > 0 && levels[n - 1].BidVol > 0)
            {
                double ratioBull = (double)levels[0].AskVol  / levels[n - 1].BidVol;
                double ratioBear = (double)levels[n - 1].BidVol / levels[0].AskVol;
                ofRatioBull = ratioBull >= OFRatioExhaustionLevel || ratioBull <= 0.069;
                ofRatioBear = ratioBear >= OFRatioExhaustionLevel || ratioBear <= 0.069;
                // Simplify: exhaustion on up candle if asks at low >> bids at high = defended
                if (isUpCandle)  ofRatioBull = (double)levels[0].AskVol / Math.Max(1, levels[n - 1].BidVol) >= OFRatioExhaustionLevel;
                else             ofRatioBear = (double)levels[n - 1].BidVol / Math.Max(1, levels[0].AskVol) >= OFRatioExhaustionLevel;
            }

            // Vertical liquidity: heavier than average volume over 3+ consecutive bid or ask levels
            bool vertLiq = false;
            if (n >= 3)
            {
                long avgLvlVol = (totalAsk + totalBid) / n;
                int consecBid = 0, consecAsk = 0;
                for (int i = 0; i < n; i++)
                {
                    consecBid = levels[i].BidVol >= avgLvlVol * 2 ? consecBid + 1 : 0;
                    consecAsk = levels[i].AskVol >= avgLvlVol * 2 ? consecAsk + 1 : 0;
                    if (consecBid >= 3 || consecAsk >= 3) { vertLiq = true; break; }
                }
            }

            // Resting liquidity: large single-level bid or ask
            bool restLiq = false;
            long restThreshold = Math.Max(MinImbalanceVolume * 10, (totalAsk + totalBid) / Math.Max(1, n) * 5);
            foreach (var lv in levels)
                if (lv.BidVol >= restThreshold || lv.AskVol >= restThreshold) { restLiq = true; break; }

            // Market sweep: consecutive levels with zero counter-volume (3+ in a row)
            bool marketSweep = false;
            int sweepConsec = 0;
            for (int i = 0; i < n; i++)
            {
                bool noCounter = levels[i].BidVol == 0 || levels[i].AskVol == 0;
                sweepConsec = noCounter ? sweepConsec + 1 : 0;
                if (sweepConsec >= 3) { marketSweep = true; break; }
            }

            // Orderflows Sequencing: ascending bid/offer volumes over consecutive levels
            bool ofSeq = false;
            if (n >= 4)
            {
                bool ascAsk = true, ascBid = true;
                for (int i = 1; i < Math.Min(n, 5); i++)
                {
                    if (levels[i].AskVol <= levels[i - 1].AskVol) ascAsk = false;
                    if (levels[i].BidVol <= levels[i - 1].BidVol) ascBid = false;
                }
                ofSeq = ascAsk || ascBid;
            }

            // Market weakness: order flow weakening in direction of move (market weakness)
            // Bullish MW: declining bid volume on way down; Bearish MW: declining ask volume on way up
            bool mkWeak = false;
            if (n >= 3)
            {
                if (!isUpCandle)
                {
                    // Selling but bids getting stronger = buying market weakness
                    bool bidsGrowing = levels[0].BidVol < levels[n / 2].BidVol;
                    mkWeak = bidsGrowing && totalBid < totalBid / 2 + 1;
                }
            }

            // Imbalance auction: imbalance at the top of a down candle or bottom of an up candle
            bool imbalAuction = false;
            if (!isUpCandle && isSell[n - 1]) imbalAuction = true;
            if (isUpCandle  && isBuy[0])      imbalAuction = true;

            // OF Gaps: shift in value area with momentum (VA moved completely above/below prior VA)
            // Will be computed in inter-bar signals

            var snap = new BarSnapshot
            {
                BarIndex               = barIdx,
                Open                   = barOpen,
                High                   = barHigh,
                Low                    = barLow,
                Close                  = barClose,
                BarDelta               = barDelta,
                TotalVolume            = totalVol,
                MaxDelta               = maxDelta,
                MinDelta               = minDelta,
                PocPrice               = pocPrice,
                VaLow                  = levels[vaLo].Price,
                VaHigh                 = levels[vaHi].Price,
                Levels                 = levels,
                IsBuyImbalance         = isBuy,
                IsSellImbalance        = isSell,
                IsStackedBuy           = isSB,
                IsStackedSell          = isSS,
                IsExtrPriceDelta       = isExtrPD,
                IsExtrPriceLevelVol    = isExtrPLV,
                SigZeroBuy             = zeroBuy,
                SigZeroSell            = zeroSell,
                SigExhaustionBull      = exhBull,
                SigExhaustionBear      = exhBear,
                SigBuyingTail          = buyingTail,
                SigSellingTail         = sellingTail,
                SigVolumeDecline       = volDecBull || volDecBear,
                SigRetailSuck          = retailBull || retailBear,
                SigImbalanceReversal   = imbalRevBull || imbalRevBear,
                SigMultipleImbalance   = multiImbal,
                SigStoppingVolume      = stopVol,
                SigSmallMinMaxDelta    = smallMinMax,
                SigExtremeBarDelta     = extrBarDelta,
                SigExtremeBarVolume    = extrBarVolume,
                SigExtremeDeltaThreshold = extrDeltaThresh,
                SigExtremeDeltaVolume  = extrDeltaVol,
                SigAccumDist           = accumDist,
                SigPriceActionDiv      = priceActDiv,
                SigPocShadow           = pocShadow,
                SigDeltaTail           = deltaTailBull || deltaTailBear,
                SigUnfinishedAuction   = unfinished,
                SigOFRatioBull         = ofRatioBull,
                SigOFRatioBear         = ofRatioBear,
                SigVerticalLiquidity   = vertLiq,
                SigRestingLiquidity    = restLiq,
                SigMarketSweep         = marketSweep,
                SigOFSequencing        = ofSeq,
                SigMarketWeakness      = mkWeak,
                SigImbalanceAuction    = imbalAuction,
            };
            return snap;
        }

        private void ComputeInterBarSignals(BarSnapshot snap)
        {
            // Delta divergence
            if (ShowDeltaDivergence)
            {
                bool atDayHigh = Math.Abs(snap.High - _dayHigh) < Instrument.MasterInstrument.TickSize;
                bool atDayLow  = Math.Abs(snap.Low  - _dayLow)  < Instrument.MasterInstrument.TickSize;
                bool divCondition = (snap.IsUpBar && snap.BarDelta < 0) || (snap.IsDownBar && snap.BarDelta > 0);
                snap.SigDeltaDivergence = divCondition
                    && (!DeltaDivergenceDayExtremeOnly || atDayHigh || atDayLow);
            }

            // Delta surge: increasing delta over 3 bars in same direction
            if (ShowDeltaSurge)
            {
                bool bullSurge = snap.BarDelta > 0 && _prevBarDelta > 0 && snap.BarDelta > _prevBarDelta && _prevBarDelta > _prevPrevBarDelta;
                bool bearSurge = snap.BarDelta < 0 && _prevBarDelta < 0 && snap.BarDelta < _prevBarDelta && _prevBarDelta < _prevPrevBarDelta;
                snap.SigDeltaSurge = bullSurge || bearSurge;
            }

            // Delta breakout: delta increased compared to prior bar in same direction
            if (ShowDeltaBreakout)
                snap.SigDeltaBreakout = (snap.BarDelta > 0 && snap.BarDelta > _prevBarDelta)
                                     || (snap.BarDelta < 0 && snap.BarDelta < _prevBarDelta);

            // Extreme delta change
            if (ShowExtremeDeltaChange)
            {
                long change = Math.Abs(snap.BarDelta - _prevBarDelta);
                snap.SigExtremeDeltaChange = change >= ExtremeDeltaChangeThreshold;
            }
            if (ShowExtremeDeltaChangePct && _prevBarDelta != 0)
            {
                long changePct = Math.Abs((snap.BarDelta - _prevBarDelta) * 100L / _prevBarDelta);
                snap.SigExtremeDeltaChangePct = changePct >= ExtremeDeltaChangePctThreshold;
            }

            // Engulfing Value Area
            if (ShowEngulfingVA && _prevVaLow != 0)
            {
                snap.SigEngulfingVABull = snap.IsUpBar  && snap.VaLow  <= _prevVaLow && snap.VaHigh >= _prevVaHigh;
                snap.SigEngulfingVABear = snap.IsDownBar && snap.VaLow <= _prevVaLow && snap.VaHigh >= _prevVaHigh;
            }

            // Orderflows Gaps: current VA entirely above or below previous VA
            if (ShowOFGaps && _prevVaLow != 0)
            {
                snap.SigOFGaps = snap.VaLow > _prevVaHigh || snap.VaHigh < _prevVaLow;
            }

            // Delta Stack: 3+ consecutive strong-directional deltas in same bar's levels
            if (ShowDeltaStack && snap.Levels != null)
            {
                int posStreak = 0, negStreak = 0;
                foreach (var lv in snap.Levels)
                {
                    posStreak = lv.Delta > MinImbalanceVolume ? posStreak + 1 : 0;
                    negStreak = lv.Delta < -MinImbalanceVolume ? negStreak + 1 : 0;
                    if (posStreak >= 3 || negStreak >= 3) { snap.SigDeltaStack = true; break; }
                }
            }

            // Imbalance Reload: same-price imbalance across consecutive bars
            // (requires prior bar data — mark for rendering from cache comparison)
            snap.SigImbalanceReload = false; // computed at render time via cache lookup

            // POC Wave requires 3 bars — computed at render time
            snap.SigPocWave = false;

            // Aligned POC: current POC same as previous bar's POC (set at render time)
        }

        // ══════════════════════════════════════════════════════════════════════
        //  RENDERING
        // ══════════════════════════════════════════════════════════════════════

        public override void OnRenderTargetChanged()
        {
            DisposeResources();
            if (RenderTarget == null) return;

            _buyBrush           = Mk(BuyColor);
            _sellBrush          = Mk(SellColor);
            _ibuyBrush          = Mk(ImbalanceBuyColor);
            _isellBrush         = Mk(ImbalanceSellColor);
            _sbuyBrush          = Mk(StackedBuyColor);
            _ssellBrush         = Mk(StackedSellColor);
            _pocBrush           = Mk(PocColor);
            _ppocBullBrush      = Mk(ProminentPocBullColor);
            _ppocBearBrush      = Mk(ProminentPocBearColor);
            _vaBullBrush        = Mk(ValueAreaBullColor);
            _vaBearBrush        = Mk(ValueAreaBearColor);
            _vaGrayBrush        = new SolidColorBrush(RenderTarget, new SharpDX.Color4(0.4f, 0.4f, 0.4f, 0.5f));
            _evaBullBrush       = Mk(EngulfingVABullColor);
            _evaBearBrush       = Mk(EngulfingVABearColor);
            _divBullBrush       = Mk(DeltaDivergenceColor);
            _divBearBrush       = Mk(DeltaDivergenceColor);
            _ratioBullBrush     = Mk(OFRatioBullColor);
            _ratioBearBrush     = Mk(OFRatioBearColor);
            _textBrush          = new SolidColorBrush(RenderTarget, SharpDX.Color4.White);
            _gridBrush          = new SolidColorBrush(RenderTarget, new SharpDX.Color4(1f, 1f, 1f, 0.10f));
            _dividerBrush       = new SolidColorBrush(RenderTarget, new SharpDX.Color4(1f, 1f, 1f, 0.5f));
            _statusBgBrush      = new SolidColorBrush(RenderTarget, new SharpDX.Color4(0f, 0f, 0f, 0.65f));
            _statusTextBrush    = new SolidColorBrush(RenderTarget, new SharpDX.Color4(0f, 0.85f, 0.3f, 1f));
            _imbalAskBrush      = new SolidColorBrush(RenderTarget, new SharpDX.Color4(0.2f, 0.6f, 1f, 1f));
            _imbalBidBrush      = new SolidColorBrush(RenderTarget, new SharpDX.Color4(1f, 0.3f, 0.3f, 1f));
            _multiImbalBullBrush = new SolidColorBrush(RenderTarget, new SharpDX.Color4(0.42f, 0.56f, 0.14f, 0.5f));  // OliveDrab
            _multiImbalBearBrush = new SolidColorBrush(RenderTarget, new SharpDX.Color4(0.82f, 0.41f, 0.12f, 0.5f));  // Chocolate

            _dw        = new SharpDX.DirectWrite.Factory();
            _cellFmt   = MkFmt((float)CellFontSize, SharpDX.DirectWrite.TextAlignment.Center);
            _statusFmt = MkFmt(11f, SharpDX.DirectWrite.TextAlignment.Leading);
            _signalFmt = MkFmt(10f, SharpDX.DirectWrite.TextAlignment.Center);
        }

        protected override void OnRender(ChartControl cc, ChartScale cs)
        {
            if (RenderTarget == null || _buyBrush == null) return;
            if (ChartBars == null || BarsArray == null || BarsArray.Length == 0) return;

            int firstBar = ChartBars.FromIndex;
            int lastBar  = ChartBars.ToIndex;
            if (BarsArray[0].Count < 1) return;

            RenderStatus(cc);

            double tickSize = Instrument.MasterInstrument.TickSize;

            for (int barIdx = firstBar; barIdx <= lastBar; barIdx++)
            {
                BarSnapshot snap;
                if (!_barCache.TryGetValue(barIdx, out snap)) continue;
                if (snap.Levels == null || snap.Levels.Count == 0) continue;

                // Compute inter-bar signals that need the cache (POC Wave, Reload, Aligned POC)
                ComputeCacheBasedSignals(snap, barIdx, tickSize);

                DrawBarSnapshot(cc, cs, tickSize, snap, isLive: false);
                DrawSignalMarkers(cc, cs, snap);
            }

            // Live bar
            if (_accumLevels.Count > 0
                && _accumBarIndex >= firstBar && _accumBarIndex <= lastBar
                && !_barCache.ContainsKey(_accumBarIndex))
            {
                if (_liveDirty)
                {
                    _liveSnap  = BuildSnapshot(_accumBarIndex, _accumLevels);
                    if (_liveSnap != null) { _liveSnap.Cvd = _cvd + _liveSnap.BarDelta; }
                    _liveDirty = false;
                }
                if (_liveSnap != null)
                    DrawBarSnapshot(cc, cs, tickSize, _liveSnap, isLive: true);
            }

            if (ShowVolumeProfile)
                DrawVolumeProfile(cc, cs, tickSize);
        }

        private void ComputeCacheBasedSignals(BarSnapshot snap, int barIdx, double tickSize)
        {
            // Aligned POC
            BarSnapshot prev;
            if (ShowAlignedPoc && _barCache.TryGetValue(barIdx - 1, out prev))
                snap.SigAlignedPoc = Math.Abs(snap.PocPrice - prev.PocPrice) < tickSize * 0.5;

            // Open POC: POC outside the previous bar(s) range
            if (ShowOpenPoc)
            {
                int lb = Math.Max(1, OpenPocLookBackBars);
                bool openBull = true, openBear = true;
                for (int k = 1; k <= lb; k++)
                {
                    BarSnapshot pk;
                    if (!_barCache.TryGetValue(barIdx - k, out pk)) { openBull = openBear = false; break; }
                    if (snap.PocPrice <= pk.High) openBull = false;
                    if (snap.PocPrice >= pk.Low)  openBear = false;
                }
                snap.SigOpenPoc = openBull || openBear;
            }

            // Prominent POC: POC acts as S/R (current POC matches a historical level)
            if (ShowProminentPoc)
            {
                int lookBack = ProminentPocLookBackBars == 0 ? Math.Min(barIdx, 50) : ProminentPocLookBackBars;
                for (int k = 1; k <= lookBack; k++)
                {
                    BarSnapshot pk;
                    if (!_barCache.TryGetValue(barIdx - k, out pk)) continue;
                    if (Math.Abs(snap.PocPrice - pk.PocPrice) < tickSize * 0.5)
                    {
                        snap.SigProminentPocBull = snap.IsUpBar;
                        snap.SigProminentPocBear = snap.IsDownBar;
                        break;
                    }
                }
            }

            // POC Wave: 3-bar setup
            if (ShowPocWave && barIdx >= 2)
            {
                BarSnapshot p1, p2;
                if (_barCache.TryGetValue(barIdx - 1, out p1) && _barCache.TryGetValue(barIdx - 2, out p2))
                {
                    // Bullish: red, green(lower POC), green(higher POC than p2)
                    bool pocWaveBull = p2.IsDownBar && p1.IsUpBar && snap.IsUpBar
                        && p1.PocPrice < p2.PocPrice && snap.PocPrice > p2.PocPrice;
                    // Bearish: green, red(higher POC), red(lower POC than p2)
                    bool pocWaveBear = p2.IsUpBar && p1.IsDownBar && snap.IsDownBar
                        && p1.PocPrice > p2.PocPrice && snap.PocPrice < p2.PocPrice;
                    snap.SigPocWave = pocWaveBull || pocWaveBear;
                }
            }

            // POC Slingshot: POC colored green on up candle or red on down (special setup)
            if (ShowPocSlingshot)
                snap.SigPocSlingshot = snap.SigProminentPocBull || snap.SigProminentPocBear;

            // Imbalance Reload: same price imbalance in previous bar
            if (ShowImbalanceReload && _barCache.TryGetValue(barIdx - 1, out prev)
                && prev.Levels != null && snap.Levels != null)
            {
                for (int si = 0; si < snap.Levels.Count && !snap.SigImbalanceReload; si++)
                {
                    long pk = PriceKey(snap.Levels[si].Price, tickSize);
                    for (int pi = 0; pi < prev.Levels.Count; pi++)
                    {
                        if (PriceKey(prev.Levels[pi].Price, tickSize) != pk) continue;
                        if (snap.IsBuyImbalance[si]  && prev.IsBuyImbalance[pi])  { snap.SigImbalanceReload = true; break; }
                        if (snap.IsSellImbalance[si] && prev.IsSellImbalance[pi]) { snap.SigImbalanceReload = true; break; }
                        break;
                    }
                }
            }

            // Inverse imbalance: buying imbalances in a down candle or selling imbalances in up candle
            if (ShowInverseImbalance)
                snap.SigInverseImbalance = (snap.IsDownBar && snap.IsBuyImbalance.Any(b => b))
                                        || (snap.IsUpBar   && snap.IsSellImbalance.Any(b => b));
        }

        private void DrawBarSnapshot(ChartControl cc, ChartScale cs,
                                     double tickSize, BarSnapshot snap, bool isLive)
        {
            if (snap.Levels == null || snap.Levels.Count == 0) return;

            float xCenter = cc.GetXByBarIndex(ChartBars, snap.BarIndex);
            float halfW   = (float)cc.GetBarPaintWidth(ChartBars) * 0.45f;
            if (halfW < 2f) return;

            float minH = CellFontSize + 4f;
            float minW = CellFontSize * 3f;
            int   n    = snap.Levels.Count;

            float tickPx = Math.Abs(cs.GetYByValue(snap.Levels[0].Price + tickSize)
                                  - cs.GetYByValue(snap.Levels[0].Price));
            tickPx = Math.Max(0.1f, tickPx);

            // Apply user-defined ticks aggregation in addition to auto-merge for readability
            int userMerge  = Math.Max(1, TicksAggregation);
            int autoMerge  = Math.Max(1, (int)Math.Ceiling(minH / (tickPx * userMerge)));
            int merge      = userMerge * autoMerge;
            bool canShowText = (tickPx * merge >= minH) && (halfW >= minW);

            // Determine cell background mode
            bool useVA = ShowValueArea && !isLive;

            for (int i = 0; i < n; i += merge)
            {
                int end = Math.Min(i + merge, n);

                long bidSum = 0, askSum = 0;
                bool hasBuyImb = false, hasSellImb = false;
                bool hasStkBuy = false, hasStkSell = false;
                bool hasPoc    = false;
                bool hasVA     = false;
                bool isEVA     = snap.SigEngulfingVABull || snap.SigEngulfingVABear;
                bool isExtrPD  = false;
                bool isExtrPLV = false;

                for (int j = i; j < end; j++)
                {
                    bidSum     += snap.Levels[j].BidVol;
                    askSum     += snap.Levels[j].AskVol;
                    hasBuyImb  |= snap.IsBuyImbalance[j];
                    hasSellImb |= snap.IsSellImbalance[j];
                    hasStkBuy  |= snap.IsStackedBuy[j];
                    hasStkSell |= snap.IsStackedSell[j];
                    isExtrPD   |= snap.IsExtrPriceDelta[j];
                    isExtrPLV  |= snap.IsExtrPriceLevelVol[j];
                    if (Math.Abs(snap.Levels[j].Price - snap.PocPrice) < tickSize * 0.5)
                        hasPoc = true;
                    double lvPrice = snap.Levels[j].Price;
                    if (lvPrice >= snap.VaLow - tickSize * 0.5 && lvPrice <= snap.VaHigh + tickSize * 0.5)
                        hasVA = true;
                }

                float yTop  = cs.GetYByValue(snap.Levels[end - 1].Price + tickSize * 0.5);
                float yBot  = cs.GetYByValue(snap.Levels[i].Price       - tickSize * 0.5);
                float cellH = Math.Max(1f, yBot - yTop);

                var leftRect  = new SharpDX.RectangleF(xCenter - halfW, yTop, halfW, cellH);
                var rightRect = new SharpDX.RectangleF(xCenter,         yTop, halfW, cellH);

                // ── Cell backgrounds ──────────────────────────────────────────
                SolidColorBrush cellBg;
                if (ShowDeltaHistogram && FootprintType == FootprintMode.Delta)
                {
                    long lvDelta = askSum - bidSum;
                    cellBg = lvDelta >= 0 ? _buyBrush : _sellBrush;
                    RenderTarget.FillRectangle(new SharpDX.RectangleF(xCenter - halfW, yTop, halfW * 2f, cellH), cellBg);
                }
                else if (FootprintType == FootprintMode.Volume)
                {
                    cellBg = _vaGrayBrush;
                    RenderTarget.FillRectangle(new SharpDX.RectangleF(xCenter - halfW, yTop, halfW * 2f, cellH), cellBg);
                }
                else
                {
                    // Standard Bid/Ask mode — left=bid(sell), right=ask(buy)
                    SolidColorBrush leftBg, rightBg;

                    if (useVA && hasVA && !hasPoc)
                    {
                        SolidColorBrush vaBg = snap.Open == snap.Close ? _vaGrayBrush
                            : snap.IsUpBar ? (isEVA ? _evaBullBrush : _vaBullBrush)
                                          : (isEVA ? _evaBearBrush : _vaBearBrush);
                        vaBg.Opacity = 0.55f;
                        RenderTarget.FillRectangle(new SharpDX.RectangleF(xCenter - halfW, yTop, halfW * 2f, cellH), vaBg);
                        vaBg.Opacity = 1f;
                    }

                    leftBg  = hasStkSell && ShowStackedImbalance ? _ssellBrush
                            : hasSellImb && ShowVolumeImbalance   ? _isellBrush
                            :                                        _sellBrush;
                    rightBg = hasStkBuy  && ShowStackedImbalance ? _sbuyBrush
                            : hasBuyImb  && ShowVolumeImbalance   ? _ibuyBrush
                            :                                        _buyBrush;

                    RenderTarget.FillRectangle(leftRect,  leftBg);
                    RenderTarget.FillRectangle(rightRect, rightBg);
                }

                // Grid + divider
                if (cellH >= 2f)
                    RenderTarget.DrawLine(new SharpDX.Vector2(xCenter - halfW, yTop),
                                          new SharpDX.Vector2(xCenter + halfW, yTop),
                                          _gridBrush, 0.5f);
                RenderTarget.DrawLine(new SharpDX.Vector2(xCenter, yTop),
                                      new SharpDX.Vector2(xCenter, yBot),
                                      _dividerBrush, 1f);

                // Numbers
                if (canShowText)
                {
                    string bidStr = FormatVol(bidSum);
                    string askStr = FormatVol(askSum);

                    // Imbalance text colouring: blue for ask imbalance, red for bid imbalance
                    SolidColorBrush leftTxt  = (hasSellImb && ShowVolumeImbalance) ? _imbalBidBrush : _textBrush;
                    SolidColorBrush rightTxt = (hasBuyImb  && ShowVolumeImbalance) ? _imbalAskBrush : _textBrush;

                    if (FootprintType == FootprintMode.Delta)
                    {
                        string dStr = FormatVol(askSum - bidSum);
                        RenderTarget.DrawText(dStr, _cellFmt,
                            new SharpDX.RectangleF(xCenter - halfW, yTop, halfW * 2f, cellH), _textBrush);
                    }
                    else if (FootprintType == FootprintMode.Volume)
                    {
                        string vStr = FormatVol(bidSum + askSum);
                        RenderTarget.DrawText(vStr, _cellFmt,
                            new SharpDX.RectangleF(xCenter - halfW, yTop, halfW * 2f, cellH), _textBrush);
                    }
                    else if (FootprintType == FootprintMode.DiagonalDelta)
                    {
                        // Show delta at each price (ask - bid at that level)
                        string ddStr = FormatVol(askSum - bidSum);
                        RenderTarget.DrawText(ddStr, _cellFmt,
                            new SharpDX.RectangleF(xCenter - halfW, yTop, halfW * 2f, cellH), _textBrush);
                    }
                    else
                    {
                        RenderTarget.DrawText(bidStr, _cellFmt, leftRect,  leftTxt);
                        RenderTarget.DrawText(askStr, _cellFmt, rightRect, rightTxt);
                    }
                }

                // POC
                if (ShowPoc && hasPoc)
                {
                    var pocRect  = new SharpDX.RectangleF(xCenter - halfW, yTop, halfW * 2f, cellH);
                    SolidColorBrush pocFill = (snap.SigProminentPocBull && ShowProminentPoc) ? _ppocBullBrush
                                           : (snap.SigProminentPocBear && ShowProminentPoc) ? _ppocBearBrush
                                           : (snap.SigPocSlingshot     && ShowPocSlingshot)
                                               ? (snap.IsUpBar ? _ppocBullBrush : _ppocBearBrush)
                                           : _pocBrush;
                    pocFill.Opacity = 0.45f;
                    RenderTarget.FillRectangle(pocRect, pocFill);
                    pocFill.Opacity = 1f;
                    RenderTarget.DrawRectangle(pocRect, pocFill, 1.5f);
                }

                // Extreme price level highlights
                if (isExtrPD && ShowExtremePriceDelta)
                {
                    var eRect = new SharpDX.RectangleF(xCenter - halfW, yTop, halfW * 2f, cellH);
                    _pocBrush.Opacity = 0.3f;
                    RenderTarget.DrawRectangle(eRect, _pocBrush, 1f);
                    _pocBrush.Opacity = 1f;
                }
            }

            // Multiple imbalance box
            if (ShowMultipleImbalance && snap.SigMultipleImbalance)
            {
                float yBarTop = cs.GetYByValue(snap.High + Instrument.MasterInstrument.TickSize);
                float yBarBot = cs.GetYByValue(snap.Low  - Instrument.MasterInstrument.TickSize);
                var boxBrush  = snap.IsUpBar ? _multiImbalBullBrush : _multiImbalBearBrush;
                RenderTarget.DrawRectangle(
                    new SharpDX.RectangleF(xCenter - halfW, yBarTop, halfW * 2f, yBarBot - yBarTop),
                    boxBrush, 1.5f);
            }

            // Delta label
            if (ShowDeltaLabel && halfW >= 10f)
            {
                float  yDelta = cs.GetYByValue(snap.Low) + 2f;
                string suffix = isLive ? "~" : "";
                string dLabel = snap.BarDelta >= 0
                    ? $"+{snap.BarDelta}{suffix}" : $"{snap.BarDelta}{suffix}";
                var dRect = new SharpDX.RectangleF(xCenter - halfW, yDelta, halfW * 2f, CellFontSize + 4f);
                RenderTarget.DrawText(dLabel, _cellFmt, dRect, _textBrush);
            }
        }

        private void DrawSignalMarkers(ChartControl cc, ChartScale cs, BarSnapshot snap)
        {
            if (snap.Levels == null) return;
            float xCenter = cc.GetXByBarIndex(ChartBars, snap.BarIndex);
            float halfW   = (float)cc.GetBarPaintWidth(ChartBars) * 0.45f;
            float tick    = Instrument.MasterInstrument.TickSize > 0
                ? Math.Abs(cs.GetYByValue(0) - cs.GetYByValue((float)Instrument.MasterInstrument.TickSize))
                : 4f;
            float spacing = tick * Math.Max(1, SignalSpacingTicks);

            float yAbove = cs.GetYByValue(snap.High) - spacing;
            float yBelow = cs.GetYByValue(snap.Low)  + spacing;

            // Delta Divergence — gold triangle
            if (ShowDeltaDivergence && snap.SigDeltaDivergence)
                DrawTriangle(xCenter, snap.IsUpBar ? yAbove : yBelow, 8f, snap.IsUpBar, _divBullBrush);

            // Orderflows Ratio
            if (ShowOFRatio)
            {
                if (snap.SigOFRatioBull) DrawTriangle(xCenter, yBelow, 8f, false, _ratioBullBrush);
                if (snap.SigOFRatioBear) DrawTriangle(xCenter, yAbove, 8f, true,  _ratioBearBrush);
            }

            // POC Shadows — diamond marker
            if (ShowPocShadows && snap.SigPocShadow)
            {
                float yShadow = cs.GetYByValue(snap.PocPrice);
                RenderTarget.DrawLine(
                    new SharpDX.Vector2(xCenter - halfW, yShadow),
                    new SharpDX.Vector2(xCenter + halfW, yShadow),
                    _pocBrush, 2f);
            }

            // Stacked Imbalance — horizontal bar
            if (ShowStackedImbalance)
            {
                for (int idx = 0; idx < snap.Levels.Count; idx++)
                {
                    if (snap.IsStackedBuy[idx])
                    {
                        float y = cs.GetYByValue(snap.Levels[idx].Price);
                        RenderTarget.DrawLine(new SharpDX.Vector2(xCenter - halfW, y),
                                              new SharpDX.Vector2(xCenter + halfW, y), _sbuyBrush, 2f);
                    }
                    if (snap.IsStackedSell[idx])
                    {
                        float y = cs.GetYByValue(snap.Levels[idx].Price);
                        RenderTarget.DrawLine(new SharpDX.Vector2(xCenter - halfW, y),
                                              new SharpDX.Vector2(xCenter + halfW, y), _ssellBrush, 2f);
                    }
                }
            }

            // Exhaustion Prints
            if (ShowExhaustionPrints)
            {
                if (snap.SigExhaustionBull) DrawTriangle(xCenter, yBelow, 6f, false, _ibuyBrush);
                if (snap.SigExhaustionBear) DrawTriangle(xCenter, yAbove, 6f, true,  _isellBrush);
            }

            // Zero Print
            if (ShowZeroPrint)
            {
                if (snap.SigZeroBuy)  DrawTriangle(xCenter, yBelow, 6f, false, _ppocBullBrush);
                if (snap.SigZeroSell) DrawTriangle(xCenter, yAbove, 6f, true,  _ppocBearBrush);
            }

            // Delta Surge
            if (ShowDeltaSurge && snap.SigDeltaSurge)
                DrawTriangle(xCenter, snap.BarDelta > 0 ? yBelow : yAbove, 7f, snap.BarDelta <= 0, _divBullBrush);

            // Extreme Bar Delta / Volume — outline bar
            if ((ShowExtremeBarDelta && snap.SigExtremeBarDelta) ||
                (ShowExtremeBarVolume && snap.SigExtremeBarVolume))
            {
                float yTop = cs.GetYByValue(snap.High);
                float yBot = cs.GetYByValue(snap.Low);
                RenderTarget.DrawRectangle(
                    new SharpDX.RectangleF(xCenter - halfW, yTop, halfW * 2f, yBot - yTop),
                    snap.BarDelta >= 0 ? _ratioBullBrush : _ratioBearBrush, 1.5f);
            }
        }

        private void DrawTriangle(float x, float y, float size,
                                   bool pointDown, SolidColorBrush brush)
        {
            if (brush == null) return;
            using (var geo = new SharpDX.Direct2D1.PathGeometry(RenderTarget.Factory))
            using (var sink = geo.Open())
            {
                if (pointDown)
                {
                    sink.BeginFigure(new SharpDX.Vector2(x, y + size), FigureBegin.Filled);
                    sink.AddLine(new SharpDX.Vector2(x - size, y - size));
                    sink.AddLine(new SharpDX.Vector2(x + size, y - size));
                }
                else
                {
                    sink.BeginFigure(new SharpDX.Vector2(x, y - size), FigureBegin.Filled);
                    sink.AddLine(new SharpDX.Vector2(x - size, y + size));
                    sink.AddLine(new SharpDX.Vector2(x + size, y + size));
                }
                sink.EndFigure(FigureEnd.Closed);
                sink.Close();
                RenderTarget.FillGeometry(geo, brush);
            }
        }

        private void DrawVolumeProfile(ChartControl cc, ChartScale cs, double tickSize)
        {
            if (_dayVolProfile.Count == 0) return;

            long maxVol = 0;
            foreach (var kv in _dayVolProfile) if (kv.Value > maxVol) maxVol = kv.Value;
            if (maxVol == 0) return;

            float chartW  = (float)cc.ActualWidth;
            float profileW = chartW * 0.08f;  // 8% of chart width
            float xStart  = VolumeProfileOnRight ? chartW - profileW : 0f;

            foreach (var kv in _dayVolProfile)
            {
                double price = kv.Key * tickSize;
                float y    = cs.GetYByValue(price);
                float barW = (float)(kv.Value * profileW / maxVol);
                float x    = VolumeProfileOnRight ? xStart : xStart + profileW - barW;
                _vaBullBrush.Opacity = 0.4f;
                RenderTarget.FillRectangle(new SharpDX.RectangleF(x, y - 1f, barW, 2f), _vaBullBrush);
                _vaBullBrush.Opacity = 1f;
            }
        }

        // ── Status overlay ────────────────────────────────────────────────────

        private void RenderStatus(ChartControl cc)
        {
            if (_statusBgBrush == null || _statusFmt == null) return;
            float w = (float)cc.ActualWidth;
            string cvdPart = ShowCvdInStatus ? $"  │  CVD: {_cvd:+#;-#;0}" : "";
            string txt = $"  OFE │ {Instrument.MasterInstrument.Name}  {BarsPeriod}"
                       + $"  │  Bars: {_barCache.Count}"
                       + cvdPart;
            RenderTarget.FillRectangle(new SharpDX.RectangleF(0f, 0f, w, 22f), _statusBgBrush);
            RenderTarget.DrawText(txt, _statusFmt,
                new SharpDX.RectangleF(4f, 3f, w - 8f, 18f), _statusTextBrush);
        }

        // ══════════════════════════════════════════════════════════════════════
        //  HELPERS
        // ══════════════════════════════════════════════════════════════════════

        private static long PriceKey(double price, double tickSize)
            => (long)Math.Round(price / tickSize);

        private string FormatVol(long v)
        {
            if (!ShortenBigNumbers || v < 1000) return v.ToString();
            if (v < 1_000_000) return $"{v / 1000.0:0.#}K";
            return $"{v / 1_000_000.0:0.#}M";
        }

        private SharpDX.Direct2D1.SolidColorBrush Mk(System.Windows.Media.Color c)
            => new SharpDX.Direct2D1.SolidColorBrush(RenderTarget,
               new SharpDX.Color4(c.R / 255f, c.G / 255f, c.B / 255f, c.A == 0 ? 1f : c.A / 255f));

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

        private void EvictOldest()
        {
            var keys = new List<int>(_barCache.Keys);
            keys.Sort();
            int toRemove = Math.Max(50, keys.Count / 10);
            for (int i = 0; i < Math.Min(toRemove, keys.Count); i++)
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

        private void DisposeResources()
        {
            void D(IDisposable x) { x?.Dispose(); }
            D(_buyBrush);  D(_sellBrush); D(_ibuyBrush);  D(_isellBrush);
            D(_sbuyBrush); D(_ssellBrush); D(_pocBrush);  D(_ppocBullBrush); D(_ppocBearBrush);
            D(_textBrush); D(_gridBrush); D(_dividerBrush);
            D(_statusBgBrush); D(_statusTextBrush);
            D(_vaBullBrush); D(_vaBearBrush); D(_vaGrayBrush);
            D(_evaBullBrush); D(_evaBearBrush);
            D(_divBullBrush); D(_divBearBrush);
            D(_ratioBullBrush); D(_ratioBearBrush);
            D(_imbalAskBrush); D(_imbalBidBrush);
            D(_multiImbalBullBrush); D(_multiImbalBearBrush);
            D(_cellFmt); D(_statusFmt); D(_signalFmt); D(_dw);

            _buyBrush  = _sellBrush  = _ibuyBrush   = _isellBrush    =
            _sbuyBrush = _ssellBrush = _pocBrush     = _ppocBullBrush  =
            _ppocBearBrush = _textBrush = _gridBrush = _dividerBrush   =
            _statusBgBrush = _statusTextBrush = _vaBullBrush = _vaBearBrush =
            _vaGrayBrush = _evaBullBrush = _evaBearBrush = _divBullBrush =
            _divBearBrush = _ratioBullBrush = _ratioBearBrush = _imbalAskBrush =
            _imbalBidBrush = _multiImbalBullBrush = _multiImbalBearBrush = null;
            _cellFmt = _statusFmt = _signalFmt = null;
            _dw = null;
        }
    }
}
