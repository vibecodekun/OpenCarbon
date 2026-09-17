// Checks platform/AudioOut against the Ryujinx trace (testresults/ryujinx_audio/20260917_122613, session 4): with the
// buffer and volume calls of Carbon's GB audio thread (@ 0x7100080b40), buffer k is mixed at the SDL volumes
// 128, 2, 3, 5, 6, 7 (of 128) for k = 0..5.
#include "platform/AudioOut.h"
#include "platform/AudioStream.h"
#include <cstdio>
#include <mutex>
#include <vector>

namespace {
std::mutex g_mutex;
std::vector<int16_t> g_firstSamples;  // first sample of every block written to the device
}  // namespace

struct AudioStream::Impl {};
AudioStream::AudioStream() = default;
AudioStream::~AudioStream() = default;
bool AudioStream::Open(int, int, int) { return true; }
void AudioStream::Write(const int16_t* interleaved, int) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_firstSamples.push_back(interleaved[0]);
}

int main() {
    constexpr int kFrames = 0x800;
    constexpr int16_t kLevel = 32000;
    constexpr int kBlocks = 6;
    std::vector<int16_t> a(kFrames * 2, kLevel), b(kFrames * 2, kLevel);

    {
        AudioOut out;
        out.Open(48000, 2, kFrames);
        out.Start();
        out.Append(a.data());
        out.Append(b.data());
        out.SetVolume(0.0f);
        float volume = 0.0f;
        for (;;) {
            volume += 0.01f;
            out.SetVolume(volume);
            out.WaitEvent();
            for (int16_t* buf = out.GetReleased(); buf != nullptr; buf = out.GetReleased()) out.Append(buf);
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_firstSamples.size() >= kBlocks) break;
        }
        out.Stop();
    }

    constexpr int kMeasuredSdlVolume[kBlocks] = {128, 2, 3, 5, 6, 7};
    int failures = 0;
    for (int k = 0; k < kBlocks; ++k) {
        const float volume = static_cast<float>(g_firstSamples[k]) / kLevel;
        const int sdl = static_cast<int>(volume * 128.0f);  // (int)(volume * SDL_MIX_MAXVOLUME), as Ryujinx computes it
        const bool ok = sdl == kMeasuredSdlVolume[k];
        failures += !ok;
        std::printf("block %d: volume %.4f -> SDL %d/128, measured %d/128 %s\n", k, volume, sdl, kMeasuredSdlVolume[k],
                    ok ? "ok" : "MISMATCH");
    }
    return failures;
}
