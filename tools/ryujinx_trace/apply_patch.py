"""Adds the opencarbon audio trace hooks to the Ryujinx 1.1.1403 source tree (C:\\opencarbon\\Ryujinx-1.1.1403).
The trace class itself is src/Ryujinx.Audio/AudioTrace.cs. Safe to re-run: already-patched files are skipped."""
import os, sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'Ryujinx-1.1.1403', 'src')
MARK = 'opencarbon trace'


def patch(rel, edits):
    path = os.path.join(ROOT, rel)
    raw = open(path, 'rb').read()
    bom = raw.startswith(b'\xef\xbb\xbf')
    s = raw.decode('utf-8-sig')
    if 'AudioTrace' in s:
        print('already patched:', rel)
        return
    for old, new in edits:
        n = s.count(old)
        if n != 1:
            raise SystemExit(f'{rel}: expected one match, found {n}: {old[:80]!r}')
        s = s.replace(old, new)
    open(path, 'wb').write((b'\xef\xbb\xbf' if bom else b'') + s.encode('utf-8'))
    print('patched:', rel)


patch('Ryujinx.Audio/Backends/Common/HardwareDeviceSessionOutputBase.cs', [
    ('using System.Runtime.CompilerServices;', 'using System;\nusing System.Runtime.CompilerServices;\nusing System.Text;'),
    ('''        public uint RequestedChannelCount { get; }
''', '''        public uint RequestedChannelCount { get; }

        // opencarbon trace
        public readonly int TraceId = AudioTrace.NextSessionId();
        private int _traceAppendCount;
'''),
    ('''            RequestedChannelCount = requestedChannelCount;
        }''', '''            RequestedChannelCount = requestedChannelCount;

            AudioTrace.Log($"session {TraceId} created: {GetType().Name} {requestedSampleRate} Hz, {requestedChannelCount} ch, {requestedSampleFormat}");
        }'''),
    ('''            MemoryManager.Read(buffer.DataPointer, data);

            return data;''', '''            MemoryManager.Read(buffer.DataPointer, data);

            if (AudioTrace.Enabled)
            {
                TraceGuestBuffer(buffer, data);
            }

            return data;'''),
    ('''        protected ulong GetSampleCount(AudioBuffer buffer)''', '''        // opencarbon trace: the bytes as the guest handed them over, plus the guest memory around the first few buffers
        private void TraceGuestBuffer(AudioBuffer buffer, byte[] data)
        {
            int index = _traceAppendCount++;
            long offset = AudioTrace.Append($"s{TraceId}_guest.pcm", data);

            AudioTrace.Log($"session {TraceId} guest buffer #{index} tag 0x{buffer.BufferTag:x} ptr 0x{buffer.DataPointer:x} size {buffer.DataSize} file offset {offset} {AudioTrace.Pcm16Stats(data)}");

            if (index < 4)
            {
                const ulong PageSize = 0x1000;
                const ulong Margin = 0x40000;

                ulong start = (buffer.DataPointer & ~(PageSize - 1)) - Margin;
                ulong end = ((buffer.DataPointer + buffer.DataSize + PageSize - 1) & ~(PageSize - 1)) + Margin;
                byte[] window = new byte[end - start];
                StringBuilder unmapped = new();

                for (ulong page = start; page < end; page += PageSize)
                {
                    try
                    {
                        MemoryManager.Read(page, window.AsSpan((int)(page - start), (int)PageSize));
                    }
                    catch (Exception)
                    {
                        unmapped.Append($" 0x{page:x}");
                    }
                }

                AudioTrace.WriteFile($"s{TraceId}_guest{index}_mem_{start:x}.bin", window);
                AudioTrace.Log($"session {TraceId} guest buffer #{index} memory window 0x{start:x}..0x{end:x} saved{(unmapped.Length > 0 ? ", unreadable pages:" + unmapped : "")}");
            }
        }

        protected ulong GetSampleCount(AudioBuffer buffer)'''),
])

patch('Ryujinx.Audio/Backends/CompatLayer/CompatLayerHardwareDeviceSession.cs', [
    ('''            _userChannelCount = userChannelCount;
        }''', '''            _userChannelCount = userChannelCount;

            AudioTrace.Log($"session {TraceId} (compat, user {userSampleFormat} {userChannelCount} ch) wraps session {realSession.TraceId}"); // opencarbon trace
        }'''),
])

patch('Ryujinx.Audio/Common/AudioDeviceSession.cs', [
    ('''        public bool AppendBuffer(AudioBuffer buffer)
        {''', '''        // opencarbon trace
        private int TraceId => (_hardwareDeviceSession as Backends.Common.HardwareDeviceSessionOutputBase)?.TraceId ?? -1;

        public bool AppendBuffer(AudioBuffer buffer)
        {
            AudioTrace.Log($"session {TraceId} AppendBuffer tag 0x{buffer.BufferTag:x} ptr 0x{buffer.DataPointer:x} size {buffer.DataSize} state {_state}");
'''),
    ('''        public ResultCode Start()
        {''', '''        public ResultCode Start()
        {
            AudioTrace.Log($"session {TraceId} Start (volume {_volume:R})");
'''),
    ('''        public ResultCode Stop()
        {''', '''        public ResultCode Stop()
        {
            AudioTrace.Log($"session {TraceId} Stop");
'''),
    ('''        public void SetVolume(float volume)
        {''', '''        public void SetVolume(float volume)
        {
            AudioTrace.Log($"session {TraceId} SetVolume {volume:R} state {_state}");
'''),
    ('''                uint bufferIndex = (_releasedBufferIndex - _bufferReleasedCount) % Constants.AudioDeviceBufferCountMax;

                buffer = _buffers[bufferIndex];
''', '''                uint bufferIndex = (_releasedBufferIndex - _bufferReleasedCount) % Constants.AudioDeviceBufferCountMax;

                buffer = _buffers[bufferIndex];

                AudioTrace.Log($"session {TraceId} guest took released tag 0x{buffer.BufferTag:x}");
'''),
])

patch('Ryujinx.Audio.Backends.SDL2/SDL2HardwareDeviceSession.cs', [
    ('''                // SDL2 left the responsibility to the user to clear the buffer.
                streamSpan.Clear();

                return;''', '''                // SDL2 left the responsibility to the user to clear the buffer.
                streamSpan.Clear();

                TraceOutput(streamSpan, 0, maxFrameCount);

                return;'''),
    ('''                SDL_MixAudioFormat(stream, pStreamSrc, _nativeSampleFormat, (uint)samples.Length, (int)(_driver.Volume * _volume * SDL_MIX_MAXVOLUME));
            }''', '''                SDL_MixAudioFormat(stream, pStreamSrc, _nativeSampleFormat, (uint)samples.Length, (int)(_driver.Volume * _volume * SDL_MIX_MAXVOLUME));
            }

            TraceOutput(streamSpan, frameCount, maxFrameCount);'''),
    ('''        public override ulong GetPlayedSampleCount()''', '''        // opencarbon trace: what SDL is given to play, after volume
        private int _traceCallbacks;

        private void TraceOutput(ReadOnlySpan<byte> stream, int frames, int maxFrames)
        {
            if (!AudioTrace.Enabled)
            {
                return;
            }

            long offset = AudioTrace.Append($"s{TraceId}_output.pcm", stream);
            int callback = _traceCallbacks++;
            int sdlVolume = (int)(_driver.Volume * _volume * SDL_MIX_MAXVOLUME);

            if (callback < 4000 || frames < maxFrames)
            {
                AudioTrace.Log($"session {TraceId} sdl callback #{callback} frames {frames}/{maxFrames} sdl volume {sdlVolume}/{SDL_MIX_MAXVOLUME} output offset {offset} {AudioTrace.Pcm16Stats(stream)}");
            }
        }

        public override ulong GetPlayedSampleCount()'''),
    ('''                    Logger.Info?.Print(LogClass.Audio, $"New audio stream setup with a target sample count of {_sampleCount}");''', '''                    Logger.Info?.Print(LogClass.Audio, $"New audio stream setup with a target sample count of {_sampleCount}");

                    AudioTrace.Log($"session {TraceId} SDL stream opened: {_sampleCount} frames per callback, started {_started}");'''),
    ('''        public override void SetVolume(float volume)
        {
            _volume = volume;
        }''', '''        public override void SetVolume(float volume)
        {
            _volume = volume;

            AudioTrace.Log($"session {TraceId} SDL volume {volume:R}");
        }'''),
    ('''                SDL2AudioBuffer driverBuffer = new(buffer.DataPointer, GetSampleCount(buffer));''', '''                AudioTrace.Log($"session {TraceId} SDL queue ptr 0x{buffer.DataPointer:x} {buffer.Data.Length} bytes, ring {_ringBuffer.Length} bytes before");

                SDL2AudioBuffer driverBuffer = new(buffer.DataPointer, GetSampleCount(buffer));'''),
])

patch('Ryujinx.Audio.Backends.OpenAL/OpenALHardwareDeviceSession.cs', [
    ('''                AL.BufferData(driverBuffer.BufferId, _targetFormat, buffer.Data, (int)RequestedSampleRate);''', '''                AL.BufferData(driverBuffer.BufferId, _targetFormat, buffer.Data, (int)RequestedSampleRate);

                AudioTrace.Log($"session {TraceId} OpenAL queue {buffer.Data.Length} bytes gain {_volume * _driver.Volume:R} offset {AudioTrace.Append($"s{TraceId}_queued.pcm", buffer.Data)}"); // opencarbon trace'''),
    ('''        public override void SetVolume(float volume)
        {
            _volume = volume;''', '''        public override void SetVolume(float volume)
        {
            AudioTrace.Log($"session {TraceId} OpenAL volume {volume:R}"); // opencarbon trace

            _volume = volume;'''),
])

patch('Ryujinx.Audio.Backends.SoundIo/SoundIoHardwareDeviceSession.cs', [
    ('''            _ringBuffer.Read(samples, 0, samples.Length);
''', '''            _ringBuffer.Read(samples, 0, samples.Length);

            AudioTrace.Log($"session {TraceId} SoundIO write {frameCount}/{maxFrameCount} frames volume {_volume:R} offset {AudioTrace.Append($"s{TraceId}_output_prevolume.pcm", samples)}"); // opencarbon trace
'''),
])

patch('Ryujinx.HLE/Loaders/Processes/ProcessLoaderHelper.cs', [
    ('''                Logger.Info?.Print(LogClass.Loader, $"Loading image {index} at 0x{nsoBase[index]:x16}...");''', '''                Logger.Info?.Print(LogClass.Loader, $"Loading image {index} at 0x{nsoBase[index]:x16}...");

                Ryujinx.Audio.AudioTrace.Log($"image {index} at 0x{nsoBase[index]:x16}: text 0x{executables[index].Text.Length:x} bss 0x{executables[index].BssSize:x}"); // opencarbon trace'''),
])
