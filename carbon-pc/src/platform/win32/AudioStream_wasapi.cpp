// WASAPI shared-mode implementation of platform/AudioStream.h. Windows converts the stream's rate/channel
// count to the device mix format (AUTOCONVERTPCM), so callers keep the Switch rates (32 kHz menu, 48 kHz cores).
// Without a device (--mute, none present, or unplugged) Write() still blocks in real time so callers keep
// their pacing. --audio-dump tees every stream into a WAV file.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include "platform/AudioStream.h"
#include "platform/Platform.h"
#include "platform/win32/Win32Platform.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

std::atomic<int> g_streamCount{0};

class WavDump {
public:
    ~WavDump() {
        if (file_) std::fclose(file_);
    }
    void Open(const std::string& dir, int index, int rate, int channels) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string path = dir + "/audio" + std::to_string(index) + "_" + std::to_string(rate) + "hz.wav";
        file_ = std::fopen(path.c_str(), "wb");
        if (!file_) return;
        rate_ = rate;
        channels_ = channels;
        name_ = path;
        WriteHeader();
        std::printf("audio: dumping stream %d to %s\n", index, path.c_str());
    }
    void Append(const int16_t* samples, int frames) {
        if (!file_) return;
        if (dataBytes_ == 0) std::printf("audio: first sample of %s at t=%.3f\n", name_.c_str(), Platform::MonotonicNs() / 1e9);
        std::fwrite(samples, sizeof(int16_t) * channels_, static_cast<size_t>(frames), file_);
        dataBytes_ += static_cast<uint32_t>(frames * channels_ * sizeof(int16_t));
        WriteHeader();  // keep the file valid: the process may end without destructors
        std::fseek(file_, 0, SEEK_END);
    }

private:
    void WriteHeader() {
        const auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, file_); };
        const auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, file_); };
        std::fseek(file_, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, file_);
        u32(36 + dataBytes_);
        std::fwrite("WAVEfmt ", 1, 8, file_);
        u32(16);
        u16(1);
        u16(static_cast<uint16_t>(channels_));
        u32(static_cast<uint32_t>(rate_));
        u32(static_cast<uint32_t>(rate_ * channels_ * 2));
        u16(static_cast<uint16_t>(channels_ * 2));
        u16(16);
        std::fwrite("data", 1, 4, file_);
        u32(dataBytes_);
    }

    FILE* file_ = nullptr;
    std::string name_;
    int rate_ = 0;
    int channels_ = 0;
    uint32_t dataBytes_ = 0;
};

}  // namespace

struct AudioStream::Impl {
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    HANDLE event = nullptr;
    UINT32 bufferFrames = 0;
    int sampleRate = 0;
    int channels = 0;
    int framesPerBuffer = 0;
    ULONGLONG nextRetryMs = 0;
    bool comInitialized = false;
    bool mute = false;
    int64_t silentStartNs = 0;  // real-time schedule while no device is open
    int64_t silentFrames = 0;
    WavDump dump;

    bool Init();
    void Release();
    void WaitSilently(int frames);
};

bool AudioStream::Impl::Init() {
    if (mute) return false;
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) return false;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr)) return false;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    if (FAILED(hr)) return false;

    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(channels);
    format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(channels * 2);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    // Room for two caller buffers (the Switch cores recycle two AudioOut buffers), at least 40 ms.
    const int64_t frames = std::max<int64_t>(2 * framesPerBuffer, sampleRate / 25);
    const REFERENCE_TIME duration = frames * 10'000'000 / sampleRate;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                            duration, 0, &format, nullptr);
    if (FAILED(hr)) return false;
    if (!event) event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(client->SetEventHandle(event)) || FAILED(client->GetBufferSize(&bufferFrames)) ||
        FAILED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render))))
        return false;
    silentStartNs = 0;
    return SUCCEEDED(client->Start());
}

void AudioStream::Impl::Release() {
    if (client) client->Stop();
    if (render) render->Release();
    if (client) client->Release();
    if (device) device->Release();
    render = nullptr;
    client = nullptr;
    device = nullptr;
}

void AudioStream::Impl::WaitSilently(int frames) {
    if (silentStartNs == 0) {
        silentStartNs = Platform::MonotonicNs();
        silentFrames = 0;
    }
    silentFrames += frames;
    Platform::SleepUntilNs(silentStartNs + silentFrames * 1'000'000'000 / sampleRate);
}

AudioStream::AudioStream() : impl(std::make_unique<Impl>()) {}

AudioStream::~AudioStream() {
    impl->Release();
    if (impl->event) CloseHandle(impl->event);
    if (impl->comInitialized) CoUninitialize();
}

bool AudioStream::Open(int sampleRate, int channels, int framesPerBuffer) {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    impl->comInitialized = SUCCEEDED(hr);
    impl->sampleRate = sampleRate;
    impl->channels = channels;
    impl->framesPerBuffer = framesPerBuffer;

    const Platform::Win32Options& options = Platform::GetWin32Options();
    impl->mute = options.mute;
    if (!options.audioDumpDir.empty()) impl->dump.Open(options.audioDumpDir, g_streamCount++, sampleRate, channels);

    if (!impl->Init()) {
        if (!impl->mute)
            std::fprintf(stderr, "audio: no WASAPI output for %d Hz/%d ch; continuing silently\n", sampleRate, channels);
        impl->Release();
        impl->nextRetryMs = GetTickCount64() + 2000;
    }
    return true;  // a silent stream still paces its caller
}

void AudioStream::Write(const int16_t* interleaved, int frames) {
    impl->dump.Append(interleaved, frames);
    while (frames > 0) {
        UINT32 padding = 0;
        if (!impl->render || FAILED(impl->client->GetCurrentPadding(&padding))) {
            // no device, or it went away (e.g. headphones unplugged): retry the default device every 2 s
            impl->Release();
            const ULONGLONG now = GetTickCount64();
            const bool retry = !impl->mute && now >= impl->nextRetryMs;
            if (!retry || !impl->Init()) {
                if (retry) impl->nextRetryMs = now + 2000;
                impl->Release();
                impl->WaitSilently(frames);
                return;
            }
            continue;
        }
        const UINT32 available = impl->bufferFrames - padding;
        if (available == 0) {
            WaitForSingleObject(impl->event, 100);
            continue;
        }
        const UINT32 n = std::min<UINT32>(available, static_cast<UINT32>(frames));
        BYTE* dst = nullptr;
        if (FAILED(impl->render->GetBuffer(n, &dst))) continue;
        std::memcpy(dst, interleaved, static_cast<size_t>(n) * impl->channels * sizeof(int16_t));
        impl->render->ReleaseBuffer(n, 0);
        interleaved += static_cast<size_t>(n) * impl->channels;
        frames -= static_cast<int>(n);
    }
}
