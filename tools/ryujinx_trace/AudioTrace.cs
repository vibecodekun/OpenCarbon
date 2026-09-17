// opencarbon trace: records what guests hand to the audio services and what the backend finally plays.
// Output goes to %RYUJINX_AUDIO_TRACE_DIR%\<start time>, or C:\opencarbon\testresults\ryujinx_audio\<start time> when that
// testresults folder exists; otherwise tracing is off. All file I/O happens on a background thread.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;

namespace Ryujinx.Audio
{
    public static class AudioTrace
    {
        public static readonly bool Enabled;

        private const long MaxBytesPerFile = 256L << 20;

        private static readonly string _dir;
        private static readonly Stopwatch _clock = Stopwatch.StartNew();
        private static readonly BlockingCollection<Action> _work = new();
        private static readonly Dictionary<string, FileStream> _files = new();
        private static StreamWriter _log;
        private static int _nextSessionId;

        static AudioTrace()
        {
            string root = Environment.GetEnvironmentVariable("RYUJINX_AUDIO_TRACE_DIR");

            if (string.IsNullOrEmpty(root) && Directory.Exists(@"C:\opencarbon\testresults"))
            {
                root = @"C:\opencarbon\testresults\ryujinx_audio";
            }

            if (string.IsNullOrEmpty(root))
            {
                return;
            }

            _dir = Path.Combine(root, DateTime.Now.ToString("yyyyMMdd_HHmmss"));
            Directory.CreateDirectory(_dir);
            _log = new StreamWriter(Path.Combine(_dir, "events.log"), false, Encoding.UTF8);
            Enabled = true;

            new Thread(() =>
            {
                foreach (Action action in _work.GetConsumingEnumerable())
                {
                    action();
                }
            })
            { IsBackground = true, Name = "AudioTrace" }.Start();

            Log($"trace started {DateTime.Now:O}, stopwatch resolution {Stopwatch.Frequency} ticks/s");
        }

        public static double NowMs => _clock.Elapsed.TotalMilliseconds;

        public static int NextSessionId() => Interlocked.Increment(ref _nextSessionId);

        public static void Log(string text)
        {
            if (!Enabled)
            {
                return;
            }

            double now = NowMs;
            int thread = Environment.CurrentManagedThreadId;

            _work.Add(() =>
            {
                _log.WriteLine($"{now,12:F3} t{thread,-3} {text}");
                _log.Flush();
            });
        }

        // Appends a copy of data to a file in the trace folder. Returns the offset it will land at (-1 once the file
        // reached its size cap).
        private static readonly ConcurrentDictionary<string, long> _sizes = new();

        public static long Append(string name, ReadOnlySpan<byte> data)
        {
            if (!Enabled)
            {
                return -1;
            }

            long offset = 0;
            bool capped = false;
            int length = data.Length;

            _sizes.AddOrUpdate(name, _ => { offset = 0; return length; }, (_, size) =>
            {
                offset = size;
                capped = size + length > MaxBytesPerFile;
                return capped ? size : size + length;
            });

            if (capped)
            {
                return -1;
            }

            byte[] copy = data.ToArray();

            _work.Add(() =>
            {
                if (!_files.TryGetValue(name, out FileStream file))
                {
                    file = new FileStream(Path.Combine(_dir, name), FileMode.Create, FileAccess.Write);
                    _files[name] = file;
                }

                file.Write(copy);
                file.Flush();
            });

            return offset;
        }

        public static void WriteFile(string name, byte[] data)
        {
            if (Enabled)
            {
                _work.Add(() => File.WriteAllBytes(Path.Combine(_dir, name), data));
            }
        }

        // peak, non-zero sample count and the first bytes of a 16-bit PCM block
        public static string Pcm16Stats(ReadOnlySpan<byte> data)
        {
            int peak = 0;
            int nonZero = 0;

            for (int i = 0; i + 1 < data.Length; i += 2)
            {
                int sample = (short)(data[i] | data[i + 1] << 8);

                if (sample != 0)
                {
                    nonZero++;
                    peak = Math.Max(peak, Math.Abs(sample));
                }
            }

            return $"peak {peak} nonzero {nonZero}/{data.Length / 2} head {Convert.ToHexString(data[..Math.Min(32, data.Length)])}";
        }
    }
}
