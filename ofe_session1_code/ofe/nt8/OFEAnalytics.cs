// OFEAnalytics.cs
// NinjaTrader 8 indicator — OFE Live Analytics overlay.
//
// Connects to the btc_live_analytics C++ TCP server (port 9000) and renders:
//   • VWAP              — solid gold horizontal line on price panel
//   • ±1σ bands         — dashed cyan horizontal lines
//   • ±2σ bands         — dashed steel-blue horizontal lines
//   • Delta + CVD stats — text box (bottom-left corner)
//   • Signals           — green ▲ (long) / red ▼ (short) arrows at signal bar
//
// ─── Installation ────────────────────────────────────────────────────────────
//   1. Copy to: Documents\NinjaTrader 8\bin\Custom\Indicators\OFEAnalytics.cs
//   2. NinjaTrader: Tools → Edit NinjaScript → Indicators → Compile
//   3. Open a BTC-USD chart → right-click → Indicators → Add → "OFE Analytics"
//   4. Set ServerHost if C++ engine runs on another machine (default: 127.0.0.1)
//   5. Run btc_live_analytics on the C++ side: ./build/btc_live_analytics
//
// ─── Wire protocol (newline-delimited JSON) ───────────────────────────────────
//   Tick (per trade):
//     {"t":"tick","price":63400.00,"side":"BUY","vol_btc":0.001,
//      "vwap":63388.78,"dist":11.22,"cvd":15239880,
//      "sd1_hi":63392.38,"sd1_lo":63385.19,"sd2_hi":63395.98,"sd2_lo":63381.59}
//   Bar (on bar close, every 60s):
//     {"t":"bar","n":5,"open":63380.00,"high":63410.00,"low":63370.00,
//      "close":63400.00,"vol_btc":0.16,"delta":-123456,"cvd":15239880,
//      "vwap":63388.78,"sd1_hi":63392.38,"sd1_lo":63385.19,
//      "sd2_hi":63395.98,"sd2_lo":63381.59,"signal":"VWAP_REACTION_LONG"}
//   Shutdown: {"t":"shutdown"}

#region Using declarations

using System;
using System.Collections.Concurrent;
using System.ComponentModel;
using System.ComponentModel.DataAnnotations;
using System.Globalization;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Windows.Media;
using NinjaTrader.Gui;
using NinjaTrader.Gui.Chart;
using NinjaTrader.NinjaScript;

#endregion

namespace NinjaTrader.NinjaScript.Indicators
{
    // ── Message types ─────────────────────────────────────────────────────────

    enum OFEMsgType { Tick, Bar, Shutdown, Unknown }

    struct OFEMsg
    {
        public OFEMsgType Type;
        // shared
        public double Price, Vwap, Dist, VolBtc;
        public long   Cvd;
        public double Sd1Hi, Sd1Lo, Sd2Hi, Sd2Lo;
        // tick-only
        public string Side;
        // bar-only
        public int    BarNum, Delta;
        public double Open, High, Low, Close;
        public string Signal;
    }

    // ── Indicator ─────────────────────────────────────────────────────────────

    [Gui.CategoryOrder("OFE Connection", 1)]
    [Gui.CategoryOrder("Display",        2)]
    public class OFEAnalytics : Indicator
    {
        // ── NinjaScript Properties ────────────────────────────────────────────
        // These appear in the indicator dialog when the user right-clicks
        // the indicator on the chart.

        [NinjaScriptProperty]
        [Display(Name = "Server Host", Order = 1, GroupName = "OFE Connection",
                 Description = "Host where btc_live_analytics is running.")]
        public string ServerHost { get; set; }

        [NinjaScriptProperty]
        [Range(1, 65535)]
        [Display(Name = "Server Port", Order = 2, GroupName = "OFE Connection",
                 Description = "TCP port for the OFE analytics broadcast server.")]
        public int ServerPort { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show ±1σ/±2σ Bands", Order = 1, GroupName = "Display")]
        public bool ShowBands { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Stats Overlay",  Order = 2, GroupName = "Display")]
        public bool ShowStats { get; set; }

        [NinjaScriptProperty]
        [Display(Name = "Show Signal Arrows",  Order = 3, GroupName = "Display")]
        public bool ShowArrows { get; set; }

        // ── Internal state ────────────────────────────────────────────────────

        // Latest VWAP / band values (written by TCP thread, read by NT thread)
        private volatile double _vwap  = 0;
        private volatile double _sd1Hi = 0, _sd1Lo = 0;
        private volatile double _sd2Hi = 0, _sd2Lo = 0;
        private volatile int    _delta = 0;
        private volatile long   _cvd   = 0;
        private volatile string _side  = "";
        private volatile double _lastPrice = 0;

        private readonly ConcurrentQueue<OFEMsg> _queue = new ConcurrentQueue<OFEMsg>();

        private Thread         _reader;
        private TcpClient      _tcp;
        private volatile bool  _running;
        private int            _arrowSeq = 0;

        private System.Windows.Threading.DispatcherTimer _refreshTimer;

        // ── NT lifecycle ──────────────────────────────────────────────────────

        protected override void OnStateChange()
        {
            if (State == State.SetDefaults)
            {
                Name        = "OFE Analytics";
                Description = "OFE real-time analytics: VWAP, delta, CVD, signal arrows.";
                Calculate   = Calculate.OnEachTick;
                IsOverlay   = true;
                IsAutoScale = false;
                DrawOnPricePanel = true;
                IsSuspendedWhileInactive = false;

                // Default property values shown in indicator dialog
                ServerHost = "127.0.0.1";
                ServerPort = 9000;
                ShowBands  = true;
                ShowStats  = true;
                ShowArrows = true;
            }
            else if (State == State.DataLoaded)
            {
                _running = true;
                _reader  = new Thread(ReadLoop) { IsBackground = true, Name = "OFE.Reader" };
                _reader.Start();

                // Refresh timer — ensures lines update even when NT feed is quiet
                _refreshTimer = new System.Windows.Threading.DispatcherTimer
                {
                    Interval = TimeSpan.FromMilliseconds(500)
                };
                _refreshTimer.Tick += (_, __) =>
                {
                    Drain();
                    if (_vwap > 0) ForceRefresh();
                };
                _refreshTimer.Start();
            }
            else if (State == State.Terminated)
            {
                _refreshTimer?.Stop();
                _running = false;
                try { _tcp?.Close(); } catch { }
                _reader?.Join(2000);
            }
        }

        protected override void OnBarUpdate()
        {
            Drain();
            if (_vwap <= 0) return;

            // ── VWAP line ─────────────────────────────────────────────────────
            Draw.HorizontalLine(this, "OFE_VWAP", _vwap, Brushes.Gold,
                DashStyleHelper.Solid, 2);

            // ── Band lines ────────────────────────────────────────────────────
            if (ShowBands)
            {
                Draw.HorizontalLine(this, "OFE_SD1H", _sd1Hi, Brushes.CadetBlue,
                    DashStyleHelper.Dash, 1);
                Draw.HorizontalLine(this, "OFE_SD1L", _sd1Lo, Brushes.CadetBlue,
                    DashStyleHelper.Dash, 1);
                Draw.HorizontalLine(this, "OFE_SD2H", _sd2Hi, Brushes.SteelBlue,
                    DashStyleHelper.DashDot, 1);
                Draw.HorizontalLine(this, "OFE_SD2L", _sd2Lo, Brushes.SteelBlue,
                    DashStyleHelper.DashDot, 1);
            }

            // ── Stats overlay ─────────────────────────────────────────────────
            if (ShowStats)
            {
                string zone  = ZoneLabel(_lastPrice, _vwap, _sd1Hi, _sd1Lo, _sd2Hi, _sd2Lo);
                string delta = _delta >= 0 ? $"+{_delta:#,##0}" : $"{_delta:#,##0}";
                string cvd   = _cvd   >= 0 ? $"+{_cvd:#,##0}"  : $"{_cvd:#,##0}";
                string last  = string.IsNullOrEmpty(_side) ? "" : $"  {_side}  ${_lastPrice:F2}";

                string txt = $"─── OFE Analytics ───\n" +
                             $"VWAP  ${_vwap:F2}  [{zone}]\n" +
                             $"+1σ   ${_sd1Hi:F2}   -1σ  ${_sd1Lo:F2}\n" +
                             $"+2σ   ${_sd2Hi:F2}   -2σ  ${_sd2Lo:F2}\n" +
                             $"Δ Bar {delta,14}\n" +
                             $"CVD   {cvd,14}{last}";

                Draw.TextFixed(this, "OFE_STATS", txt, TextPosition.BottomLeft,
                    Brushes.White,
                    new SimpleFont("Consolas", 11),
                    Brushes.Transparent,
                    Brushes.Black,
                    80);
            }
        }

        // ── Drain message queue ────────────────────────────────────────────────
        // Must run on the NT/WPF thread (called from OnBarUpdate or timer Tick).

        private void Drain()
        {
            OFEMsg m;
            while (_queue.TryDequeue(out m))
            {
                if (m.Type == OFEMsgType.Tick)
                {
                    _vwap      = m.Vwap;
                    _sd1Hi     = m.Sd1Hi; _sd1Lo = m.Sd1Lo;
                    _sd2Hi     = m.Sd2Hi; _sd2Lo = m.Sd2Lo;
                    _cvd       = m.Cvd;
                    _side      = m.Side;
                    _lastPrice = m.Price;
                }
                else if (m.Type == OFEMsgType.Bar)
                {
                    _vwap  = m.Vwap;
                    _sd1Hi = m.Sd1Hi; _sd1Lo = m.Sd1Lo;
                    _sd2Hi = m.Sd2Hi; _sd2Lo = m.Sd2Lo;
                    _delta = m.Delta;
                    _cvd   = m.Cvd;
                    _lastPrice = m.Close;

                    // Draw signal arrow at the current chart bar
                    if (ShowArrows && m.Signal != null &&
                        m.Signal != "NONE" && m.Signal.Length > 0 && CurrentBar >= 0)
                    {
                        bool isLong = m.Signal.EndsWith("LONG");
                        string tag  = "OFE_SIG_" + (++_arrowSeq);
                        string lbl  = ShortSignal(m.Signal);

                        if (isLong)
                        {
                            Draw.ArrowUp(this, tag, false, 0,
                                Low[0] - TickSize * 5, Brushes.LimeGreen);
                            Draw.Text(this, tag + "T", lbl, 0,
                                Low[0] - TickSize * 14, Brushes.LimeGreen);
                        }
                        else
                        {
                            Draw.ArrowDown(this, tag, false, 0,
                                High[0] + TickSize * 5, Brushes.OrangeRed);
                            Draw.Text(this, tag + "T", lbl, 0,
                                High[0] + TickSize * 14, Brushes.OrangeRed);
                        }
                    }
                }
            }
        }

        // ── TCP reader thread ──────────────────────────────────────────────────

        private void ReadLoop()
        {
            while (_running)
            {
                try
                {
                    _tcp = new TcpClient();
                    _tcp.Connect(ServerHost, ServerPort);
                    _tcp.NoDelay = true;

                    Print("[OFE] Connected to " + ServerHost + ":" + ServerPort);

                    using var ns     = _tcp.GetStream();
                    using var reader = new StreamReader(ns, Encoding.UTF8);

                    string line;
                    while (_running && (line = reader.ReadLine()) != null)
                    {
                        if (line.Length == 0) continue;
                        try
                        {
                            var msg = Parse(line);
                            if (msg.Type == OFEMsgType.Shutdown) { break; }
                            if (msg.Type != OFEMsgType.Unknown)  { _queue.Enqueue(msg); }
                        }
                        catch (Exception ex) { Print("[OFE] Parse error: " + ex.Message); }
                    }
                    Print("[OFE] Stream ended — reconnecting in 5s");
                }
                catch (Exception ex)
                {
                    if (_running) Print("[OFE] " + ex.Message + " — retrying in 5s");
                }
                finally
                {
                    try { _tcp?.Close(); } catch { }
                    _tcp = null;
                }

                // 5-second back-off before reconnect (50 × 100 ms)
                for (int i = 0; i < 50 && _running; i++) Thread.Sleep(100);
            }
        }

        // ── Minimal JSON parser ────────────────────────────────────────────────
        // Parses the fixed-format JSON produced by btc_live_analytics.
        // No external dependencies — works on .NET Framework 4.8 (NT8's runtime).

        private static OFEMsg Parse(string j)
        {
            var m = new OFEMsg();
            string t = Str(j, "t");
            switch (t)
            {
                case "tick":
                    m.Type   = OFEMsgType.Tick;
                    m.Price  = Dbl(j, "price");
                    m.Side   = Str(j, "side");
                    m.VolBtc = Dbl(j, "vol_btc");
                    m.Vwap   = Dbl(j, "vwap");
                    m.Dist   = Dbl(j, "dist");
                    m.Cvd    = (long)Dbl(j, "cvd");
                    m.Sd1Hi  = Dbl(j, "sd1_hi"); m.Sd1Lo = Dbl(j, "sd1_lo");
                    m.Sd2Hi  = Dbl(j, "sd2_hi"); m.Sd2Lo = Dbl(j, "sd2_lo");
                    break;
                case "bar":
                    m.Type   = OFEMsgType.Bar;
                    m.BarNum = (int)Dbl(j, "n");
                    m.Open   = Dbl(j, "open");  m.High  = Dbl(j, "high");
                    m.Low    = Dbl(j, "low");   m.Close = Dbl(j, "close");
                    m.VolBtc = Dbl(j, "vol_btc");
                    m.Delta  = (int)Dbl(j, "delta");
                    m.Cvd    = (long)Dbl(j, "cvd");
                    m.Vwap   = Dbl(j, "vwap");
                    m.Sd1Hi  = Dbl(j, "sd1_hi"); m.Sd1Lo = Dbl(j, "sd1_lo");
                    m.Sd2Hi  = Dbl(j, "sd2_hi"); m.Sd2Lo = Dbl(j, "sd2_lo");
                    m.Signal = Str(j, "signal");
                    m.Price  = m.Close;
                    break;
                case "shutdown":
                    m.Type = OFEMsgType.Shutdown;
                    break;
                default:
                    m.Type = OFEMsgType.Unknown;
                    break;
            }
            return m;
        }

        private static double Dbl(string j, string key)
        {
            int i = j.IndexOf("\"" + key + "\":", StringComparison.Ordinal);
            if (i < 0) return 0;
            int s = i + key.Length + 3;
            while (s < j.Length && j[s] == ' ') s++;
            int e = s;
            while (e < j.Length && j[e] != ',' && j[e] != '}') e++;
            return double.TryParse(j.Substring(s, e - s).Trim(),
                NumberStyles.Float, CultureInfo.InvariantCulture, out double v) ? v : 0;
        }

        private static string Str(string j, string key)
        {
            int i = j.IndexOf("\"" + key + "\":\"", StringComparison.Ordinal);
            if (i < 0) return "";
            int s = i + key.Length + 4;
            int e = j.IndexOf('"', s);
            return e < 0 ? "" : j.Substring(s, e - s);
        }

        // ── Display helpers ────────────────────────────────────────────────────

        private static string ZoneLabel(double price, double vwap,
            double sd1Hi, double sd1Lo, double sd2Hi, double sd2Lo)
        {
            if (vwap <= 0) return "---";
            if (price >= sd2Hi) return "+2σ extended";
            if (price >= sd1Hi) return "+1σ above VWAP";
            if (price <= sd2Lo) return "-2σ extended";
            if (price <= sd1Lo) return "-1σ below VWAP";
            return Math.Abs(price - vwap) < 1.0 ? "AT VWAP"
                 : (price > vwap ? "above VWAP" : "below VWAP");
        }

        private static string ShortSignal(string s)
        {
            switch (s)
            {
                case "VWAP_REACTION_LONG":  return "▲ REACT";
                case "VWAP_REACTION_SHORT": return "▼ REACT";
                case "VWAP_ROTATION_LONG":  return "▲ ROTAT";
                case "VWAP_ROTATION_SHORT": return "▼ ROTAT";
                default:                    return s;
            }
        }
    }
}

#region NinjaScript generated code — do not edit manually

namespace NinjaTrader.NinjaScript.Indicators
{
    public partial class Indicator : NinjaTrader.Gui.NinjaScript.IndicatorRenderBase
    {
        private OFEAnalytics[] cacheOFEAnalytics;

        public OFEAnalytics OFEAnalytics(string serverHost, int serverPort,
            bool showBands, bool showStats, bool showArrows)
            => OFEAnalytics(Input, serverHost, serverPort,
                            showBands, showStats, showArrows);

        public OFEAnalytics OFEAnalytics(ISeries<double> input,
            string serverHost, int serverPort,
            bool showBands, bool showStats, bool showArrows)
        {
            if (cacheOFEAnalytics != null)
                foreach (var c in cacheOFEAnalytics)
                    if (c != null &&
                        c.ServerHost == serverHost &&
                        c.ServerPort == serverPort &&
                        c.ShowBands  == showBands  &&
                        c.ShowStats  == showStats  &&
                        c.ShowArrows == showArrows &&
                        c.EqualsInput(input))
                        return c;

            return CacheIndicator<OFEAnalytics>(
                new OFEAnalytics
                {
                    ServerHost = serverHost,
                    ServerPort = serverPort,
                    ShowBands  = showBands,
                    ShowStats  = showStats,
                    ShowArrows = showArrows
                },
                input, ref cacheOFEAnalytics);
        }
    }
}

namespace NinjaTrader.NinjaScript.MarketAnalyzerColumns
{
    public partial class MarketAnalyzerColumn : MarketAnalyzerColumnBase
    {
        public Indicators.OFEAnalytics OFEAnalytics(string serverHost, int serverPort,
            bool showBands, bool showStats, bool showArrows)
            => indicator.OFEAnalytics(Input, serverHost, serverPort,
                                      showBands, showStats, showArrows);
    }
}

namespace NinjaTrader.NinjaScript.Strategies
{
    public partial class Strategy : NinjaTrader.Gui.NinjaScript.StrategyRenderBase
    {
        public Indicators.OFEAnalytics OFEAnalytics(string serverHost, int serverPort,
            bool showBands, bool showStats, bool showArrows)
            => indicator.OFEAnalytics(Input, serverHost, serverPort,
                                      showBands, showStats, showArrows);
    }
}
#endregion
