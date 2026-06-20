// OFEEngineClient.cs
// TCP client for the OFE engine bridge server.
//
// Connects to nt_bridge_server on port 7777 (configurable), reads
// 4-byte LE length-prefix JSON frames, deserialises them into OfeMessage
// objects and enqueues them for consumption by the NT8 indicator thread.
//
// On connection: sends a subscribe frame for the configured symbol.
// On disconnect: waits 5 s and reconnects indefinitely while Running is true.

using System;
using System.Collections.Concurrent;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace NinjaTrader.NinjaScript.Indicators.OFE
{
    public sealed class OFEEngineClient : IDisposable
    {
        // ── Configuration ─────────────────────────────────────────────────────

        public string Host       { get; set; } = "127.0.0.1";
        public int    Port       { get; set; } = 7777;
        public string SymbolName { get; set; } = "BTC-USD";
        public uint   SymbolId   { get; set; } = 0xB01C0001u;

        // ── State ─────────────────────────────────────────────────────────────

        public bool Running { get; private set; }
        public ConcurrentQueue<OfeMessage> Messages { get; } = new ConcurrentQueue<OfeMessage>();

        private Thread    _thread;
        private TcpClient _tcp;

        // ── Lifecycle ─────────────────────────────────────────────────────────

        public void Start()
        {
            Running = true;
            _thread = new Thread(ReaderLoop) { IsBackground = true, Name = "OFE.EngineClient" };
            _thread.Start();
        }

        public void Stop()
        {
            Running = false;
            try { _tcp?.Close(); } catch { }
            _thread?.Join(3000);
        }

        public void Dispose() => Stop();

        // ── Reader loop ───────────────────────────────────────────────────────

        private void ReaderLoop()
        {
            while (Running)
            {
                try
                {
                    _tcp = new TcpClient { NoDelay = true };
                    _tcp.Connect(Host, Port);

                    Log($"Connected to {Host}:{Port}");

                    using var stream = _tcp.GetStream();
                    SendSubscribe(stream);

                    while (Running)
                    {
                        // Read 4-byte length prefix
                        var lenBuf = ReadExact(stream, 4);
                        if (lenBuf == null) break;

                        int payloadLen = lenBuf[0]
                                       | (lenBuf[1] << 8)
                                       | (lenBuf[2] << 16)
                                       | (lenBuf[3] << 24);

                        if (payloadLen <= 0 || payloadLen > 1_048_576) break; // sanity

                        var payload = ReadExact(stream, payloadLen);
                        if (payload == null) break;

                        string json = Encoding.UTF8.GetString(payload);
                        try
                        {
                            var msg = OfeJsonParser.Parse(json);
                            if (msg.Type == OfeMessageType.Shutdown) { Running = false; break; }
                            if (msg.Type != OfeMessageType.Unknown)  Messages.Enqueue(msg);
                        }
                        catch (Exception ex) { Log($"Parse error: {ex.Message}"); }
                    }
                    Log("Stream ended — reconnecting in 5s");
                }
                catch (Exception ex)
                {
                    if (Running) Log($"{ex.Message} — retrying in 5s");
                }
                finally
                {
                    try { _tcp?.Close(); } catch { }
                    _tcp = null;
                }

                // 5-second back-off (50 × 100 ms so Running check stays responsive)
                for (int i = 0; i < 50 && Running; i++) Thread.Sleep(100);
            }
        }

        // ── Outbound: subscribe frame ─────────────────────────────────────────

        private void SendSubscribe(Stream stream)
        {
            string json    = $"{{\"msg\":\"subscribe\",\"name\":\"{SymbolName}\",\"id\":{SymbolId}}}";
            byte[] payload = Encoding.UTF8.GetBytes(json);
            int    len     = payload.Length;

            var frame = new byte[4 + len];
            frame[0] = (byte)( len        & 0xFF);
            frame[1] = (byte)((len >>  8) & 0xFF);
            frame[2] = (byte)((len >> 16) & 0xFF);
            frame[3] = (byte)((len >> 24) & 0xFF);
            Buffer.BlockCopy(payload, 0, frame, 4, len);

            stream.Write(frame, 0, frame.Length);
            stream.Flush();
            Log($"Subscribed to {SymbolName} (id={SymbolId})");
        }

        // ── ReadExact — read exactly n bytes or return null on disconnect ─────

        private static byte[] ReadExact(Stream stream, int n)
        {
            var buf = new byte[n];
            int got = 0;
            while (got < n)
            {
                int read = stream.Read(buf, got, n - got);
                if (read <= 0) return null;
                got += read;
            }
            return buf;
        }

        private static void Log(string msg) =>
            Console.WriteLine($"[OFE.Client] {msg}");
    }
}
