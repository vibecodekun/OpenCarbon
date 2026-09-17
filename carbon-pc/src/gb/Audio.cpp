// GB audio: blargg's Gb_Snd_Emu 0.1.4 (Gb_Apu + Stereo_Buffer, used unmodified from upstream) plus Carbon's
// sample ring and output thread.
//
// Pipeline (all per CPU instruction inside gb_run_frame):
//   Gb_Apu::end_frame -> Stereo_Buffer::end_frame -> read_samples (once >= 1024 samples are buffered)
//   -> append to an 0x2000-frame ring (only after frame 30, dropped when it would overflow)
//   audio_thread_main drains up to 2048 frames per 48 kHz output buffer.
#include "gb/Core.h"
#pragma warning(push)
#pragma warning(disable : 4100 5054)  // upstream Gb_Snd_Emu headers
#include "Gb_Apu.h"
#include "Multi_Buffer.h"
#pragma warning(pop)
#include "platform/AudioOut.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace gb {

int32_t apuTime = 0;  // 0x7100222118
bool audioThreadEnabled = true;
void (*sampleCaptureHook)(const int16_t*, int32_t) = nullptr;

namespace {

Stereo_Buffer stereoBuffer;                          // 0x7100222000
unsigned char apuStorage[sizeof(Gb_Apu)];            // 0x710024c010
bool apuBuilt = false;
int16_t sampleScratch[0x800];                        // 0x710022211c

constexpr int32_t kRingFrames = 0x2000;
std::mutex ringMutex;                                // (the original shares these without locking)
int16_t ring[kRingFrames * 2];                       // malloc(0x8000) @ 0x7100223138
int32_t ringFill = kRingFrames;                      // 0x7100223124, in frames
uint32_t appendedFrames[2] = {};                     // 0x7100223128, 0x710022312c (never read)

std::atomic<bool> threadRunning{false};              // 0x7100223120
std::thread audioThread;
float outputVolume = 0.0f;                           // 0x7100223130
std::atomic<int> fadeIn{0};                          // 0x710021f5d8
std::atomic<int> fadeOut{0};                         // 0x710021f5dc (never set by the GB core)

// ---- the audio thread's two memalign(0x1000, 0x2000) blocks
//
// Carbon never clears them: the memset before each allocation zeroes the *previous*, leaked allocation. They are
// freed when the thread ends, and the allocator can hand the same blocks back on the next launch. So a launch
// plays whatever that memory held for its first two buffers: heap leftovers, or the last audio of an earlier
// session. That is the pop at game start.
//
// The rebuild can't reproduce the Switch heap, so it replays what the trace measured (Ryujinx 1.1.1403: boot, menu,
// SHANTAE, then exit and relaunch five times; testresults/ryujinx_audio/20260917_122613, sessions 4-9):
//   launch 1-3: two fresh blocks each (captures 0-5)
//   launch 4-5: A reuses the previous launch's B, B is fresh (captures 6, 7)
//   launch 6:   A reuses the previous launch's A, B is fresh (capture 8)
// Later launches repeat launch 6's pattern, with silent fresh blocks. A reused block keeps what that buffer held at
// the end of the earlier session; the trace confirms this for every reuse.
struct HeapCaptureRun {
    int capture;
    uint16_t offset, length;
    uint32_t poolOffset;
};
#include "gb/AudioHeapCapture.inc"

constexpr size_t kBlockBytes = 0x2000;
using HeapBlock = std::array<int16_t, kBlockBytes / 2>;

struct AudioBlocks {
    HeapBlock* a = nullptr;
    HeapBlock* b = nullptr;
};

enum class BlockSource { Capture, PreviousA, PreviousB };
struct BlockPick {
    BlockSource source;
    int capture;  // with BlockSource::Capture; -1 = silent
};
constexpr std::array<std::array<BlockPick, 2>, 6> kLaunchBlocks = {{
    {{{BlockSource::Capture, 0}, {BlockSource::Capture, 1}}},
    {{{BlockSource::Capture, 2}, {BlockSource::Capture, 3}}},
    {{{BlockSource::Capture, 4}, {BlockSource::Capture, 5}}},
    {{{BlockSource::PreviousB, -1}, {BlockSource::Capture, 6}}},
    {{{BlockSource::PreviousB, -1}, {BlockSource::Capture, 7}}},
    {{{BlockSource::PreviousA, -1}, {BlockSource::Capture, 8}}},
}};

std::vector<std::unique_ptr<HeapBlock>> heapBlocks;
AudioBlocks previousBlocks;
int launches = 0;

HeapBlock* FreshBlock(int capture) {
    heapBlocks.push_back(std::make_unique<HeapBlock>());
    HeapBlock* block = heapBlocks.back().get();
    block->fill(0);
    if (capture >= 0) {
        auto* bytes = reinterpret_cast<unsigned char*>(block->data());
        for (const HeapCaptureRun& run : kHeapCaptureRuns)
            if (run.capture == capture) std::memcpy(bytes + run.offset, kHeapCaptureBytes + run.poolOffset, run.length);
    }
    return block;
}

HeapBlock* PickBlock(const BlockPick& pick) {
    switch (pick.source) {
    case BlockSource::PreviousA: return previousBlocks.a;
    case BlockSource::PreviousB: return previousBlocks.b;
    default: return FreshBlock(pick.capture);
    }
}

AudioBlocks AllocateAudioBlocks() {
    const size_t launch = static_cast<size_t>(launches++);
    std::array<BlockPick, 2> picks = kLaunchBlocks[std::min(launch, kLaunchBlocks.size() - 1)];
    if (launch >= kLaunchBlocks.size()) picks[1].capture = -1;
    AudioBlocks blocks;
    blocks.a = PickBlock(picks[0]);
    blocks.b = PickBlock(picks[1]);
    return blocks;
}

void FreeAudioBlocks(const AudioBlocks& blocks) {
    previousBlocks = blocks;
}

// @ 0x7100080b40
void AudioThreadMain() {
    constexpr int kFrames = 0x800;
    AudioOut out;
    out.Open(48000, 2, kFrames);  // (the original aborts when no AudioOut can be opened)
    out.Start();

    const AudioBlocks blocks = AllocateAudioBlocks();
    out.Append(blocks.a->data());  // mixed at once, at the default volume 1.0
    out.Append(blocks.b->data());
    constexpr float kTarget = 1.0f, kFloor = 0.0f;  // 0x710021f5d0 / 0x710021f5d4
    out.SetVolume(0.0f);

    while (threadRunning.load()) {
        if (fadeIn.load()) {
            outputVolume += 0.01f;
            if (kTarget < outputVolume) {
                fadeIn = 0;
                outputVolume = kTarget;
            }
            out.SetVolume(outputVolume);
        }
        if (fadeOut.load()) {
            outputVolume -= 0.01f;
            if (outputVolume < kFloor) {
                fadeOut = 0;
                outputVolume = 0.0f;
            }
            out.SetVolume(outputVolume);
        }
        out.WaitEvent();
        for (int16_t* buf = out.GetReleased(); buf != nullptr; buf = out.GetReleased()) {
            {
                std::lock_guard<std::mutex> lock(ringMutex);
                const int32_t n = std::min(ringFill, kFrames);
                std::memcpy(buf, ring, static_cast<size_t>(n) * 4);  // the rest of the buffer keeps stale samples
                ringFill -= n;
                if (ringFill > 0) std::memmove(ring, ring + n * 2, static_cast<size_t>(ringFill) * 4);
            }
            out.Append(buf);
        }
    }
    FreeAudioBlocks(blocks);
    out.Stop();
}

}  // namespace

Gb_Apu& Apu() {
    if (!apuBuilt) ApuConstruct();
    return *std::launder(reinterpret_cast<Gb_Apu*>(apuStorage));
}

Stereo_Buffer& StereoBuf() { return stereoBuffer; }

uint8_t ApuRead(uint16_t addr) {
    return static_cast<uint8_t>(Apu().read_register(apuTime, addr));
}

void ApuWrite(uint16_t addr, uint8_t value) {
    Apu().write_register(apuTime, addr, value);
}

// The original runs the constructor again on the live object (gb_init, Reset, Shutdown). The constructor
// clears every oscillator's output pointers, so callers that don't reconnect them leave the APU silent.
void ApuConstruct() {
    new (apuStorage) Gb_Apu();
    apuBuilt = true;
}

namespace {
// Gb_Apu keeps its oscillator table and register mirror private. An explicit instantiation may name private
// members, which hands this file member pointers without editing the upstream header.
template <typename Tag, typename Tag::type Member>
struct PrivateAccess {
    friend typename Tag::type Get(Tag) { return Member; }
};
struct ApuOscs {
    using type = Gb_Osc* (Gb_Apu::*)[Gb_Apu::osc_count];
    friend type Get(ApuOscs);
};
struct ApuRegs {
    using type = std::uint8_t (Gb_Apu::*)[Gb_Apu::register_count];
    friend type Get(ApuRegs);
};
template struct PrivateAccess<ApuOscs, &Gb_Apu::oscs>;
template struct PrivateAccess<ApuRegs, &Gb_Apu::regs>;
}  // namespace

// @ 0x71000834a0. Not part of Gb_Snd_Emu 0.1.4; it matches apply_stereo from the later Gb_Apu, except that an
// oscillator moved to another buffer while sounding clears that whole buffer instead of offsetting -last_amp.
// NR51 is used without the NR52 power mask that register writes apply.
void ApuApplyStereo() {
    Gb_Apu& apu = Apu();
    const int nr51 = (apu.*Get(ApuRegs{}))[0xff25 - Gb_Apu::start_addr];
    for (int i = Gb_Apu::osc_count; --i >= 0;) {
        Gb_Osc& osc = *(apu.*Get(ApuOscs{}))[i];
        const int bits = nr51 >> i;
        Blip_Buffer* const out = osc.outputs[(bits >> 3 & 2) | (bits & 1)];
        if (osc.output == out) continue;
        if (osc.last_amp) {
            osc.last_amp = 0;
            if (osc.output) osc.output->clear(true);
        }
        osc.output = out;
    }
}

void ApuConnectOutputs() {
    Apu().output(stereoBuffer.center(), stereoBuffer.left(), stereoBuffer.right());
}

void AudioSetup() {
    stereoBuffer.set_sample_rate(48000, 250);
    stereoBuffer.clock_rate(0x400000);
}

void AudioEndInstruction(int32_t ticks) {
    const bool stereo = Apu().end_frame(ticks);
    stereoBuffer.end_frame(ticks, stereo);
}

void AudioPumpFrameSamples(int32_t frameCount) {
    if (stereoBuffer.center()->samples_avail() < 1024) return;
    const int32_t n = static_cast<int32_t>(stereoBuffer.read_samples(sampleScratch, 0x800));
    if (sampleCaptureHook) sampleCaptureHook(sampleScratch, n);
    if (frameCount <= 0x1e) return;
    std::lock_guard<std::mutex> lock(ringMutex);
    if (ringFill + n < kRingFrames) {  // n counts samples, the fill counts frames
        std::memcpy(ring + ringFill * 2, sampleScratch, static_cast<size_t>(n) * 2);
        ringFill += n / 2;
        appendedFrames[0] += static_cast<uint32_t>(n / 2);
        appendedFrames[1] += static_cast<uint32_t>(n / 2);
    }
}

void AudioRingClear() {  // @ 0x71000822a0
    std::lock_guard<std::mutex> lock(ringMutex);
    std::memset(ring, 0, sizeof(ring));
    ringFill = kRingFrames;
}

void AudioThreadStart() {
    AudioThreadStop();
    if (!audioThreadEnabled) return;
    threadRunning = true;
    outputVolume = 0.0f;
    fadeIn = 1;
    audioThread = std::thread(AudioThreadMain);
}

void AudioThreadStop() {
    threadRunning = false;
    if (audioThread.joinable()) audioThread.join();
}

}  // namespace gb
