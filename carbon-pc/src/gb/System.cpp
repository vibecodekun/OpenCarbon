// GbSystem: the EmuGlue entry points the app layer calls (gb_init, gb_run_frame, save states, ...).
#include "app/EmuGlue.h"
#include "gb/Cartridge.h"
#include "gb/Core.h"
#include "platform/Paths.h"
#include "platform/Platform.h"
#include "stb_image_write.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <vector>

namespace gb {
namespace {

std::string SlotDir(int game, int slot) {
    return "save:/" + std::to_string(game) + "/" + std::to_string(slot) + "/";
}

std::string StatePath(int slot, int game) {
    return SlotDir(game, slot) + carts::current->title + ".stt";
}

// Save-state file layout (0x1A2AD bytes; the file is created 0x20000 bytes long)
struct StateBlock {
    uint32_t offset;
    uint8_t* data;
    uint32_t size;
};

std::vector<StateBlock> StateBlocks() {
    return {
        {0x0032, &cpu.f, 0x0c},                          // registers F A C B E D L H SP PC
        {0x003e, &io.ly, 0x98},                          // LY .. OBJ palette RAM
        {0x00d6, &cpu.intFlags, 2},                      // IF, IE
        {0x00d8, cpu.wram, 0x8000},
        {0x80d8, core.vram, 0x4000},
        {0xc0d8, core.hram, 0x7f},
        {0xc157, core.dmgTileMap, 0x8000},
        {0x14157, core.dmgTileCache, 0x6000},
        {0x1a157, core.oamMirror, 0xa0},
        {0x1a1f7, core.oam, 0xa0},
        {0x1a297, &cpu.div, 4},                          // DIV TAC TIMA TMA
        {0x1a29b, reinterpret_cast<uint8_t*>(&core.mbc1BankLow), 0x0c},  // MBC1 bank globals
        {0x1a2a7, &core.mbc5Dirty, 6},                   // MBC5 bank globals
    };
}
constexpr uint32_t kStateSize = 0x1a2ad;

}  // namespace

bool StateBlockInfo(int index, uint32_t* fileOffset, const uint8_t** data, uint32_t* size) {
    const std::vector<StateBlock> blocks = StateBlocks();
    if (index < 0 || index >= static_cast<int>(blocks.size())) return false;
    *fileOffset = blocks[index].offset;
    *data = blocks[index].data;
    *size = blocks[index].size;
    return true;
}

namespace {

// @ 0x710008d850
void WriteState(int slot, int game) {
    const std::string path = StatePath(slot, game);
    Platform::CreateDirectory(("save:/" + std::to_string(game)).c_str());
    Platform::CreateDirectory(SlotDir(game, slot).c_str());

    std::vector<uint8_t> file(0x20000, 0);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    std::snprintf(reinterpret_cast<char*>(file.data()), 0x32, "%04d-%02d-%02d %02d:%02d", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);
    for (const StateBlock& b : StateBlocks()) std::memcpy(file.data() + b.offset, b.data, b.size);

    const std::string resolved = Paths::Resolve(path);
    std::error_code ec;
    const bool exists = std::filesystem::exists(resolved, ec);
    {
        std::fstream out(resolved, std::ios::binary | std::ios::in | std::ios::out);
        if (!out.is_open()) out.open(resolved, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            std::fprintf(stderr, "save state: cannot write %s\n", resolved.c_str());
            return;
        }
        out.write(reinterpret_cast<const char*>(file.data()), exists ? kStateSize : static_cast<std::streamsize>(file.size()));
    }

    const std::string preview = Paths::Resolve(SlotDir(game, slot) + carts::current->title + ".prv");
    stbi_write_png(preview.c_str(), 160, 144, 4, core.framebuffer, 160 * 4);
    Platform::Commit("save");
}

// @ 0x710008e210. A missing file leaves everything untouched.
void ReadState(int slot, int game) {
    std::ifstream in(Paths::Resolve(StatePath(slot, game)), std::ios::binary);
    if (!in.is_open()) return;
    for (const StateBlock& b : StateBlocks()) {
        in.seekg(b.offset);
        in.read(reinterpret_cast<char*>(b.data), b.size);
        in.clear();
    }
}

}  // namespace

// @ 0x7100081f60
void Init(GbSystem* sys) {
    core.isCgb = 1;  // the only writer: DMG games run in CGB mode with blank palettes
    CpuReset();
    VideoReset();    // runs after the CPU reset's LCDC write, so the LCD starts switched off
    DmgIoDefaults();
    sys->frameCount = 0;
    AudioThreadStart();
    AudioSetup();
    ApuConstruct();
    ApuConnectOutputs();
    AudioRingClear();
}

// One iteration of gb_run_frame's loop.
void RunInstruction(GbSystem* sys) {
    apuTime = CpuStep();
    apuTime += ServiceInterrupts();
    const int32_t t = apuTime >> (cpu.doubleSpeed & 0x1f);
    LcdStep(t);
    AudioEndInstruction(t);
    AudioPumpFrameSamples(sys->frameCount);
    TimerStep(apuTime);
}

// @ 0x71000820b0. Everything advances in lock-step per instruction; the frame ends at VBlank, so a game
// that keeps the LCD off never finishes a frame.
void RunFrame(GbSystem* sys) {
    apuTime = 0;
    while (core.frameDone == 0) RunInstruction(sys);
    core.frameDone = 0;
    ++sys->frameCount;
}

// @ 0x7100082220
void SaveState(GbSystem*, int slot, int game) {
    WriteState(slot & 0xff, game);
}

// @ 0x7100082230. The APU isn't part of the state: it keeps playing with its current registers, and only the
// oscillator outputs are re-derived from NR51.
void LoadState(GbSystem*, int slot, int game) {
    AudioRingClear();
    ReadState(slot & 0xff, game);
    ApuApplyStereo();
}

// @ 0x7100082280: CPU registers and a fresh APU. The APU outputs aren't reconnected, so game audio stays
// silent until the next launch.
void Reset() {
    CpuReset();
    ApuConstruct();
}

// @ 0x7100082060
void Shutdown() {
    ApuConstruct();
    AudioThreadStop();
}

void ClearAudioRing() {
    AudioRingClear();
}

// @ 0x7100080ef0
void LoadSlotPreviews(GbSystem* sys, const std::string& title, int game) {
    static const char* const kEmpty[3] = {"Empty Slot 1", "Empty Slot 2", "Empty Slot 3"};
    static const char* const kUsed[3] = {"Slot 1", "Slot 2", "Slot 3"};
    for (int i = 0; i < 3; ++i) {
        sys->slots[i].title = kEmpty[i];
        sys->slots[i].empty = 1;
    }
    Platform::CreateDirectory(("save:/" + std::to_string(game)).c_str());
    for (int i = 0; i < 3; ++i) Platform::CreateDirectory(SlotDir(game, i).c_str());
    Platform::Commit("save");

    for (int i = 0; i < 3; ++i) {
        std::ifstream in(Paths::Resolve(SlotDir(game, i) + title + ".stt"), std::ios::binary);
        if (!in.is_open()) continue;
        char stamp[0x33] = {};
        in.read(stamp, 0x32);
        SaveSlotInfo& s = sys->slots[i];
        s.timestamp = stamp;
        s.title = kUsed[i];
        s.empty = 0;
        s.previewPath = SlotDir(game, i) + title + ".prv";
    }
}

const uint8_t* Framebuffer() {
    return core.framebuffer;
}

}  // namespace gb
