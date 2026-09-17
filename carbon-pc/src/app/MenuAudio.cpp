#include "app/MenuAudio.h"
#include "app/Globals.h"
#include "platform/AudioStream.h"
#include "platform/Paths.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace menu_audio {
namespace {

constexpr int kRate = 32000;
constexpr int kFrame = 160;
constexpr float kCenterDownmix = 0.707f;  // approximation of the Switch 6ch->2ch downmix for FC

struct Wave {
    std::vector<int16_t> pcm;  // interleaved
    int channels = 2;
    int rate = 48000;
    bool Load(const char* path);
};

// @ 0x7100096f20 (RIFF/fmt/data walk) + @ 0x710009f450 (only 16-bit PCM accepted; aborts otherwise)
bool Wave::Load(const char* path) {
    FILE* f = std::fopen(Paths::Resolve(path).c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> head(0x400);
    std::fread(head.data(), 1, head.size(), f);
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, &head[o], 4); return v; };
    auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, &head[o], 2); return v; };
    if (std::memcmp(&head[0], "RIFF", 4) != 0 || std::memcmp(&head[8], "WAVE", 4) != 0) { std::fclose(f); return false; }
    size_t off = 12, dataOff = 0, dataSize = 0;
    int bits = 0;
    while (off + 8 <= head.size()) {
        uint32_t id = u32(off), len = u32(off + 4);
        if (std::memcmp(&head[off], "data", 4) == 0) { dataOff = off + 8; dataSize = len; break; }
        if (std::memcmp(&head[off], "fmt ", 4) == 0) {
            channels = u16(off + 10);
            rate = static_cast<int>(u32(off + 12));
            bits = u16(off + 22);
        }
        (void)id;
        off += 8 + len;
    }
    if (bits != 16 || dataOff == 0) { std::fclose(f); return false; }
    if (dataOff + dataSize > static_cast<size_t>(size)) dataSize = size - dataOff;
    pcm.resize(dataSize / 2);
    std::fseek(f, static_cast<long>(dataOff), SEEK_SET);
    std::fread(pcm.data(), 2, pcm.size(), f);
    std::fclose(f);
    return true;
}

struct Voice {
    const Wave* wave = nullptr;
    bool loop = false;
    bool playing = false;
    double pos = 0.0;  // in source frames
    float volL = 0.0f, volR = 0.0f, volC = 0.0f;

    void Start() { pos = 0.0; playing = true; }
    // Linear-interpolated resample to kRate; mixes into stereo float buffer.
    void Mix(float* out, int frames) {
        if (!playing || !wave || wave->pcm.empty()) return;
        const int ch = wave->channels;
        const size_t total = wave->pcm.size() / ch;
        const double step = static_cast<double>(wave->rate) / kRate;
        for (int i = 0; i < frames; ++i) {
            size_t i0 = static_cast<size_t>(pos);
            if (i0 + 1 >= total) {
                if (!loop) { playing = false; return; }
                pos -= static_cast<double>(total - 1);
                i0 = static_cast<size_t>(pos);
            }
            float t = static_cast<float>(pos - static_cast<double>(i0));
            float l = (wave->pcm[i0 * ch] * (1 - t) + wave->pcm[(i0 + 1) * ch] * t) / 32768.0f;
            float r = ch > 1 ? (wave->pcm[i0 * ch + 1] * (1 - t) + wave->pcm[(i0 + 1) * ch + 1] * t) / 32768.0f : l;
            out[i * 2] += l * volL + l * volC * kCenterDownmix;
            out[i * 2 + 1] += r * volR + l * volC * kCenterDownmix;
            pos += step;
        }
    }
};

void ThreadMain() {
    g::sfxBack = g::sfxSelect = g::sfxMove = g::sfxToggle = 0;
    g::musicRestart = 0;

    AudioStream stream;
    if (!stream.Open(kRate, 2, kFrame)) return;

    static Wave musicWave, backWave, selectWave, moveWave, toggleWave;
    musicWave.Load("rom:/assets/sounds/sndMenu.wav");
    backWave.Load("rom:/assets/sounds/sndchange.wav");
    selectWave.Load("rom:/assets/sounds/sndselect.wav");
    moveWave.Load("rom:/assets/sounds/menu_scrub_through_items3.wav");
    toggleWave.Load("rom:/assets/sounds/menu_toggle_yes_no3.wav");

    Voice music{&musicWave, true};
    music.volL = music.volR = 0.5f;
    music.Start();
    Voice back{&backWave}, select{&selectWave}, move{&moveWave}, toggle{&toggleWave};

    g::musicFadeInTarget = 1.0f;
    g::musicFadeOutTarget = 0.0f;

    std::vector<float> mix(kFrame * 2);
    std::vector<int16_t> pcm(kFrame * 2);
    for (;;) {
        // order and "only if the previous buffer was released" semantics as in the original loop
        if (g::sfxToggle && !toggle.playing) { toggle.volC = 1.0f; toggle.Start(); g::sfxToggle = 0; }
        if (g::sfxMove && !move.playing) { move.volC = 0.6f; move.Start(); g::sfxMove = 0; }
        if (g::sfxSelect && !select.playing) { select.volC = 1.0f; select.Start(); g::sfxSelect = 0; }
        if (g::sfxBack && !back.playing) { back.volC = 1.0f; back.Start(); g::sfxBack = 0; }
        if (g::musicFadeOut) {
            float v = music.volL - 0.01f;
            if (g::musicFadeOutTarget < v) music.volL = music.volR = v;
            else g::musicFadeOut = 0;
        }
        if (g::musicFadeIn) {
            float v = music.volL + 0.01f;
            if (v <= g::musicFadeInTarget) music.volL = music.volR = v;
            else g::musicFadeIn = 0;
        }

        std::fill(mix.begin(), mix.end(), 0.0f);
        music.Mix(mix.data(), kFrame);
        back.Mix(mix.data(), kFrame);
        select.Mix(mix.data(), kFrame);
        move.Mix(mix.data(), kFrame);
        toggle.Mix(mix.data(), kFrame);
        for (size_t i = 0; i < mix.size(); ++i) {
            float s = mix[i] * 32767.0f;
            pcm[i] = static_cast<int16_t>(s > 32767.0f ? 32767.0f : (s < -32768.0f ? -32768.0f : s));
        }
        stream.Write(pcm.data(), kFrame);  // blocks like WaitSystemEvent on the renderer event
    }
}

}  // namespace

void Start() {
    std::thread(ThreadMain).detach();
}

}  // namespace menu_audio
