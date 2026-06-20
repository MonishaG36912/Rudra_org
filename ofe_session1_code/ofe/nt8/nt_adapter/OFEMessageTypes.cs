// OFEMessageTypes.cs
// Shared message structs, enums, and a minimal JSON parser for the OFE engine protocol.
//
// Protocol: 4-byte LE uint32 length-prefix + UTF-8 JSON payload
//
// Engine → NT8 frames:
//   bar_close — after each 60-second bar closes (includes full price levels)
//   vwap      — after each tick, rate-limited to 10 Hz
//   signal    — when a signal fires
//
// NT8 → Engine frames:
//   subscribe — on connect and reconnect

using System;
using System.Collections.Generic;
using System.Globalization;

namespace NinjaTrader.NinjaScript.Indicators.OFE
{
    // ── Enumerations ──────────────────────────────────────────────────────────

    public enum OfeMessageType { BarClose, Vwap, Signal, Shutdown, Unknown }
    public enum OfeSignalDir   { Long = 0, Short = 1, Neutral = 2 }
    public enum OfeProfileShape { D = 0, P = 1, B = 2, Thin = 3, Unclassified = 4 }

    // ── Price level inside a bar_close frame ──────────────────────────────────

    public struct OfePriceLevel
    {
        public double Price;
        public int    BidVol;
        public int    AskVol;
        public int    TotalVol;
        public bool   IsBuyImbalance;
        public bool   IsSellImbalance;
        public bool   IsStackedBuy;
        public bool   IsStackedSell;
        public bool   IsZeroPrint;
        public bool   IsPoc;
        public bool   IsCot;

        public int Delta => AskVol - BidVol;
    }

    // ── bar_close message ─────────────────────────────────────────────────────

    public class OfeBarCloseMsg
    {
        public long            Ts;          // nanosecond timestamp
        public uint            SymbolId;
        public int             BarNum;
        public double          O, H, L, C;
        public long            Vol;         // total volume (satoshi units)
        public int             Delta;
        public long            Cvd;
        public double          Poc;         // bar-level POC price
        public double          SPoc;        // session POC price
        public double          Vah, Val;    // session value area
        public double          Vwap;
        public double          B1p, B1m;   // ±1σ
        public double          B2p, B2m;   // ±2σ
        public OfeProfileShape Shape;
        public List<OfePriceLevel> Levels  = new List<OfePriceLevel>();

        public bool HasBuyImbalance  => Levels.Exists(l => l.IsBuyImbalance);
        public bool HasSellImbalance => Levels.Exists(l => l.IsSellImbalance);
        public double VolBtc         => Vol / 1e8;
    }

    // ── vwap message ─────────────────────────────────────────────────────────

    public struct OfeVwapMsg
    {
        public long   Ts;
        public double Vwap;
        public double B1p, B1m;   // ±1σ
        public double B2p, B2m;   // ±2σ
        public double B3p, B3m;   // ±3σ
    }

    // ── signal message ────────────────────────────────────────────────────────

    public struct OfeSignalMsg
    {
        public long         Ts;
        public string       Type;      // e.g. "VWAP_REACTION_LONG"
        public OfeSignalDir Dir;
        public int          Strength;  // 1–4
        public double       Price;
        public double       Score;     // composite score (Pulse only)

        public bool IsLong  => Dir == OfeSignalDir.Long;
        public bool IsShort => Dir == OfeSignalDir.Short;
    }

    // ── Discriminated union carrier ───────────────────────────────────────────

    public class OfeMessage
    {
        public OfeMessageType   Type;
        public OfeBarCloseMsg   BarClose;
        public OfeVwapMsg       Vwap;
        public OfeSignalMsg     Signal;
    }

    // ── Minimal JSON parser ───────────────────────────────────────────────────
    // Hand-rolled to avoid NuGet dependencies on NT8's .NET 4.8 runtime.
    // Handles the fixed-format JSON produced by nt_bridge_server.

    public static class OfeJsonParser
    {
        public static OfeMessage Parse(string json)
        {
            var msg = new OfeMessage { Type = OfeMessageType.Unknown };
            string msgType = Str(json, "msg");

            switch (msgType)
            {
                case "bar_close": msg.Type = OfeMessageType.BarClose;  msg.BarClose = ParseBarClose(json); break;
                case "vwap":      msg.Type = OfeMessageType.Vwap;      msg.Vwap     = ParseVwap(json);     break;
                case "signal":    msg.Type = OfeMessageType.Signal;    msg.Signal   = ParseSignal(json);   break;
                case "shutdown":  msg.Type = OfeMessageType.Shutdown;  break;
            }
            return msg;
        }

        private static OfeBarCloseMsg ParseBarClose(string j)
        {
            var m = new OfeBarCloseMsg
            {
                Ts      = (long)Dbl(j, "ts"),
                SymbolId= (uint)Dbl(j, "sym"),
                BarNum  = (int) Dbl(j, "n"),
                O       = Dbl(j, "o"),   H = Dbl(j, "h"),
                L       = Dbl(j, "l"),   C = Dbl(j, "c"),
                Vol     = (long)Dbl(j, "vol"),
                Delta   = (int) Dbl(j, "delta"),
                Cvd     = (long)Dbl(j, "cvd"),
                Poc     = Dbl(j, "poc"),
                SPoc    = Dbl(j, "spoc"),
                Vah     = Dbl(j, "vah"),  Val = Dbl(j, "val"),
                Vwap    = Dbl(j, "vwap"),
                B1p     = Dbl(j, "b1p"), B1m = Dbl(j, "b1m"),
                B2p     = Dbl(j, "b2p"), B2m = Dbl(j, "b2m"),
                Shape   = (OfeProfileShape)(int)Dbl(j, "shape"),
            };

            // Parse the levels array: locate [...] after "levels":
            int arrStart = j.IndexOf("\"levels\":[", StringComparison.Ordinal);
            if (arrStart >= 0)
            {
                arrStart += 9; // skip "levels":
                int arrEnd = j.LastIndexOf(']');
                if (arrEnd > arrStart)
                    m.Levels = ParseLevels(j, arrStart, arrEnd);
            }
            return m;
        }

        private static List<OfePriceLevel> ParseLevels(string j, int start, int end)
        {
            var list = new List<OfePriceLevel>(64);
            int i = start;
            while (i < end)
            {
                int ob = j.IndexOf('{', i);
                if (ob < 0 || ob >= end) break;
                int cb = j.IndexOf('}', ob);
                if (cb < 0) break;

                string lj = j.Substring(ob, cb - ob + 1);
                list.Add(new OfePriceLevel
                {
                    Price           = Dbl(lj, "px"),
                    BidVol          = (int)Dbl(lj, "bv"),
                    AskVol          = (int)Dbl(lj, "av"),
                    TotalVol        = (int)Dbl(lj, "tv"),
                    IsBuyImbalance  = Bool(lj, "ib"),
                    IsSellImbalance = Bool(lj, "is"),
                    IsStackedBuy    = Bool(lj, "sb"),
                    IsStackedSell   = Bool(lj, "ss"),
                    IsZeroPrint     = Bool(lj, "zp"),
                    IsPoc           = Bool(lj, "poc"),
                    IsCot           = Bool(lj, "cot"),
                });
                i = cb + 1;
            }
            return list;
        }

        private static OfeVwapMsg ParseVwap(string j) => new OfeVwapMsg
        {
            Ts  = (long)Dbl(j, "ts"),
            Vwap= Dbl(j, "vwap"),
            B1p = Dbl(j, "b1p"), B1m = Dbl(j, "b1m"),
            B2p = Dbl(j, "b2p"), B2m = Dbl(j, "b2m"),
            B3p = Dbl(j, "b3p"), B3m = Dbl(j, "b3m"),
        };

        private static OfeSignalMsg ParseSignal(string j) => new OfeSignalMsg
        {
            Ts       = (long)Dbl(j, "ts"),
            Type     = Str(j, "type"),
            Dir      = (OfeSignalDir)(int)Dbl(j, "dir"),
            Strength = (int)Dbl(j, "str"),
            Price    = Dbl(j, "px"),
            Score    = Dbl(j, "score"),
        };

        // ── Primitive extractors ──────────────────────────────────────────────

        internal static double Dbl(string j, string key)
        {
            int i = j.IndexOf("\"" + key + "\":", StringComparison.Ordinal);
            if (i < 0) return 0;
            int s = i + key.Length + 3;
            while (s < j.Length && j[s] == ' ') s++;
            int e = s;
            while (e < j.Length && j[e] != ',' && j[e] != '}' && j[e] != ']') e++;
            return double.TryParse(j.Substring(s, e - s).Trim(),
                NumberStyles.Float, CultureInfo.InvariantCulture, out double v) ? v : 0;
        }

        internal static string Str(string j, string key)
        {
            int i = j.IndexOf("\"" + key + "\":\"", StringComparison.Ordinal);
            if (i < 0) return "";
            int s = i + key.Length + 4;
            int e = j.IndexOf('"', s);
            return e < 0 ? "" : j.Substring(s, e - s);
        }

        internal static bool Bool(string j, string key)
        {
            int i = j.IndexOf("\"" + key + "\":", StringComparison.Ordinal);
            if (i < 0) return false;
            int s = i + key.Length + 3;
            while (s < j.Length && j[s] == ' ') s++;
            return s + 3 < j.Length && j[s] == 't';   // "true" starts with 't'
        }
    }
}
