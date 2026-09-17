// gbdiff.dll: exposes the ported GB core to tools/gbdiff (Python + Unicorn), which drives the original ARM64
// functions and this port with identical inputs and compares the state blocks byte for byte.
#include "app/EmuGlue.h"
#include "gb/Cartridge.h"
#include "gb/Core.h"
#include "platform/Paths.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
GbSystem g_system;
std::vector<int16_t> g_capturedSamples;
std::string g_serial;
uint8_t g_serialByte = 0;

// blargg test ROMs print through the serial port: FF01 data, FF02 = 0x81 to "send" (Carbon ignores both)
void SerialCapture(uint32_t addr, uint8_t value) {
    if ((addr & 0xffff) == 0xff01) g_serialByte = value;
    else if ((addr & 0xffff) == 0xff02 && value == 0x81) g_serial.push_back(static_cast<char>(g_serialByte));
}

void CaptureSamples(const int16_t* samples, int32_t count) {
    g_capturedSamples.insert(g_capturedSamples.end(), samples, samples + count);
}
}  // namespace

#define API extern "C" __declspec(dllexport)

// which: 0 io (.data 0x71001b1bd8), 1 core (0x7100223300), 2 cpu (0x710024d030), 3 joypSelect, 4 wramBank
API void* gbd_block(int which, uint64_t* originalAddr, uint32_t* size) {
    switch (which) {
    case 0: *originalAddr = gb::kIoBlockAddr; *size = sizeof(gb::io); return &gb::io;
    case 1: *originalAddr = gb::kCoreBlockAddr; *size = sizeof(gb::core); return &gb::core;
    case 2: *originalAddr = gb::kCpuBlockAddr; *size = sizeof(gb::cpu); return &gb::cpu;
    case 3: *originalAddr = gb::kJoypSelectAddr; *size = 1; return &gb::joypSelect;
    case 4: *originalAddr = gb::kWramBankAddr; *size = 1; return &gb::wramBank;
    default: return nullptr;
    }
}

API void gbd_reset_state() {
    gb::ResetStateToImageDefaults();
    gb::audioThreadEnabled = false;
    gb::sampleCaptureHook = CaptureSamples;
    gb::writeHook = SerialCapture;
    g_serial.clear();
}

API int32_t gbd_take_serial(char* out, int32_t max) {
    const int32_t n = static_cast<int32_t>(std::min<size_t>(g_serial.size(), static_cast<size_t>(max)));
    std::memcpy(out, g_serial.data(), static_cast<size_t>(n));
    return n;
}

API int32_t gbd_cpu_step() { return gb::CpuStep(); }
API int32_t gbd_service_interrupts() { return gb::ServiceInterrupts(); }
API uint32_t gbd_read8(uint32_t addr) { return gb::Read8(addr); }
API void gbd_write8(uint32_t addr, uint32_t value) { gb::Write8(addr, static_cast<uint8_t>(value)); }
API void gbd_lcd_step(int32_t ticks) { gb::LcdStep(ticks); }
API void gbd_timer_step(int32_t ticks) { gb::TimerStep(ticks); }
API void gbd_render_scanline() { gb::RenderScanline(); }
API void gbd_key_down(int key) { gb::KeyDown(key); }
API void gbd_key_up(int key) { gb::KeyUp(key); }
API int32_t gbd_apu_time() { return gb::apuTime; }

API void gbd_init() { gb::Init(&g_system); }
API void gbd_run_frame() { gb::RunFrame(&g_system); }
API int32_t gbd_frame_count() { return g_system.frameCount; }
API void gbd_set_frame_count(int32_t n) { g_system.frameCount = n; }
API void gbd_reset() { gb::Reset(); }
API void gbd_frame_begin() { gb::apuTime = 0; }
API int32_t gbd_run_instruction() {
    gb::RunInstruction(&g_system);
    return gb::core.frameDone;
}
API void gbd_frame_end() {
    gb::core.frameDone = 0;
    ++g_system.frameCount;
}
// gb_run_frame with an instruction budget (a game with the LCD off never reaches VBlank). Returns 1 when the
// frame completed, 0 when the budget ran out mid-frame.
API int32_t gbd_run_frame_limited(int32_t maxInstructions) {
    gb::apuTime = 0;
    for (int32_t i = 0; i < maxInstructions; ++i) {
        gb::RunInstruction(&g_system);
        if (gb::core.frameDone) {
            gbd_frame_end();
            return 1;
        }
    }
    return 0;
}

// Samples returned by Stereo_Buffer::read_samples during gb_run_frame, captured since the last call.
API int32_t gbd_take_samples(int16_t* out, int32_t max) {
    const int32_t n = static_cast<int32_t>(std::min<size_t>(g_capturedSamples.size(), static_cast<size_t>(max)));
    std::memcpy(out, g_capturedSamples.data(), static_cast<size_t>(n) * 2);
    g_capturedSamples.clear();
    return n;
}

// One gb_run_frame for the test-ROM runner. Returns 1 when the frame completed, 0 when the instruction budget ran
// out (the LCD stayed off), 2 when the next opcode is LD B,B (mooneye's result marker): regs B C D E H L are
// copied to regsOut before it executes, like the harness hook on the handler.
API int32_t gbd_run_frame_test(int32_t maxInstructions, int32_t stopAtLdBB, uint8_t* regsOut) {
    gb::apuTime = 0;
    for (int32_t i = 0; i < maxInstructions; ++i) {
        if (stopAtLdBB && gb::cpu.halted == 0 && gb::Read8(gb::cpu.pc) == 0x40) {
            const uint8_t regs[6] = {gb::cpu.b, gb::cpu.c, gb::cpu.d, gb::cpu.e, gb::cpu.h, gb::cpu.l};
            std::memcpy(regsOut, regs, 6);
            // state at the harness hook: gb_cpu_step has cleared ticks and advanced PC, the handler hasn't run
            gb::cpu.ticks = 0;
            ++gb::cpu.pc;
            return 2;
        }
        gb::RunInstruction(&g_system);
        if (gb::core.frameDone) {
            gbd_frame_end();
            return 1;
        }
    }
    return 0;
}

// Save states go through the real file code, so the tests need a directory behind save:/.
API void gbd_set_save_root(const char* dir) { Paths::SetSaveRoot(dir); }
API void gbd_save_state(int slot, int game) { gb::SaveState(&g_system, slot, game); }
API void gbd_load_state(int slot, int game) { gb::LoadState(&g_system, slot, game); }

// Entry `index` of the save-state table: file offset, the original global's address and the size. Returns 0 past
// the end.
API int32_t gbd_state_block(int index, uint32_t* fileOffset, uint64_t* originalAddr, uint32_t* size) {
    const uint8_t* data = nullptr;
    if (!gb::StateBlockInfo(index, fileOffset, &data, size)) return 0;
    const auto map = [&](const auto& block, uint64_t base) {
        const auto* begin = reinterpret_cast<const uint8_t*>(&block);
        if (data < begin || data >= begin + sizeof(block)) return false;
        *originalAddr = base + static_cast<uint64_t>(data - begin);
        return true;
    };
    return map(gb::io, gb::kIoBlockAddr) || map(gb::core, gb::kCoreBlockAddr) || map(gb::cpu, gb::kCpuBlockAddr);
}

// kind: 0 RomOnly, 1 MBC1, 3 MBC3, 5 MBC5. The buffer (ROM plus padding, since MBC3/MBC5 reads aren't
// masked to the ROM size) is copied; the cartridge frees it.
API void gbd_cart_create(int kind, const uint8_t* buffer, int32_t bufferSize, int32_t romSize, int32_t ramSize, int rtc) {
    uint8_t* copy = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(bufferSize)));
    std::memcpy(copy, buffer, static_cast<size_t>(bufferSize));
    carts::Cartridge* cart = nullptr;
    switch (kind) {
    case 0: cart = new carts::RomOnly(copy, romSize); break;
    case 1: cart = new carts::Mbc1(copy, romSize, ramSize); break;
    case 3: cart = new carts::Mbc3(copy, romSize, ramSize, rtc != 0); break;
    default: cart = new carts::Mbc5(copy, romSize, ramSize); break;
    }
    cart->systemType = static_cast<uint32_t>(cart->Read(0x143) >> 7 & 1);
    carts::current = cart;
}

