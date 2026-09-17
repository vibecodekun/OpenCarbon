#include "app/EmuGlue.h"
#include "gb/Cartridge.h"
#include "gb/Core.h"

namespace gb {

// @ 0x710008d630
uint8_t Read8(uint32_t addr) {
    switch (addr >> 12 & 0xf) {
    case 8:
    case 9:
        return VramRead(addr);
    case 0xc:
    case 0xd:
    case 0xe:
        return static_cast<uint8_t>(WramRead(addr));
    case 0xf:
        break;
    default:
        return carts::current->Read(addr);
    }

    const uint32_t a = addr & 0xffff;
    const uint32_t page = a >> 8 & 0xf;
    if (page == 0xe) return CoreByte(kOamOffset + (a & 0xff));  // FE00-FEFF, FEA0+ runs into HRAM
    if (page != 0xf) return static_cast<uint8_t>(WramRead(addr));
    if (a == 0xff0f) return cpu.intFlags;
    if (a == 0xffff) return cpu.intEnable;
    if ((addr >> 7 & 0x1ff) >= 0x1ff) return core.hram[a & 0x7f];

    switch (a >> 4 & 0xf) {
    case 0:
        if (static_cast<uint32_t>((addr & 0xf) - 4) < 4) return TimerRead(static_cast<uint16_t>(addr));
        if ((addr & 0xf) != 0) return 0;  // FF01-FF03, FF08-FF0E read as 0
        {
            const uint32_t inv = ~static_cast<uint32_t>(joypSelect);
            if ((inv >> 4 & 1) == 0) return static_cast<uint8_t>((io.joypadButtons >> 4 | 0xfffffff0u) & inv);
            if (((inv & 0xff) >> 5 & 1) != 0) return static_cast<uint8_t>(inv);  // both groups selected
            return static_cast<uint8_t>((io.joypadButtons | 0xfffffff0u) & inv);
        }
    case 1:
    case 2:
    case 3:
        return ApuRead(static_cast<uint16_t>(a));
    case 4:
    case 5:
    case 6:
        return VideoIoRead(static_cast<uint16_t>(addr));
    case 7:
        return static_cast<uint8_t>(WramRead(addr));
    default:
        return 0;
    }
}

void (*writeHook)(uint32_t addr, uint8_t value) = nullptr;

// @ 0x710008d4c0
void Write8(uint32_t addr, uint8_t value) {
    if (writeHook) writeHook(addr, value);
    switch (addr >> 12 & 0xf) {
    case 8:
    case 9:
        VramWrite(addr, value);
        return;
    case 0xc:
    case 0xd:
    case 0xe:
        WramWrite(addr, value);
        return;
    case 0xf:
        break;
    default:
        carts::current->Write(addr, value);
        return;
    }

    const uint32_t a = addr & 0xffff;
    const uint32_t page = a >> 8 & 0xf;
    if (page == 0xe) {
        CoreByte(kOamOffset + (a & 0xff)) = value;
        return;
    }
    if (page != 0xf) {
        WramWrite(addr, value);
        return;
    }
    if (a == 0xff0f) {
        cpu.intFlags = value;
        return;
    }
    if (a == 0xffff) {
        cpu.intEnable = value;
        return;
    }
    if ((addr >> 7 & 0x1ff) >= 0x1ff) {
        core.hram[a & 0x7f] = value;
        return;
    }

    switch (a >> 4 & 0xf) {
    case 0:
        if (static_cast<uint32_t>((addr & 0xf) - 4) < 4) {
            TimerWrite(static_cast<uint16_t>(addr), value);
            return;
        }
        if ((addr & 0xf) == 0) {
            joypSelect = value;
            return;
        }
        [[fallthrough]];  // FF01-FF03, FF08-FF0E go to the APU, which ignores them
    case 1:
    case 2:
    case 3:
        ApuWrite(static_cast<uint16_t>(a), value);
        return;
    case 4:
    case 5:
    case 6:
        VideoIoWrite(static_cast<uint16_t>(addr), value);
        return;
    case 7:
        WramWrite(addr, value);
        return;
    default:
        return;
    }
}

uint16_t Read16(uint32_t addr) {
    const uint8_t lo = Read8(addr);
    const uint8_t hi = Read8(addr + 1);
    return static_cast<uint16_t>(lo | hi << 8);
}

void Write16(uint32_t addr, uint32_t value) {
    Write8(addr, static_cast<uint8_t>(value));
    Write8(addr + 1, static_cast<uint8_t>(value >> 8));
}

// @ 0x7100094b00: C000-FDFF (bank 1 area switched by FF70), FF70 itself
uint32_t WramRead(uint32_t addr) {
    if (((addr + 0x4000) >> 9 & 0x7f) < 0x1f) {
        uint32_t index = addr & 0x1fff;
        if (index > 0xfff) index = addr & 0xfff | static_cast<uint32_t>(wramBank) << 12;
        return cpu.wram[index & 0xffff];
    }
    return (addr & 0xffff) == 0xff70 ? wramBank : 0xffffffffu;
}

// @ 0x7100094a90
void WramWrite(uint32_t addr, uint8_t value) {
    if (((addr + 0x4000) >> 9 & 0x7f) > 0x1e) {
        if ((addr & 0xffff) == 0xff70) {
            wramBank = value & 7;
            if (wramBank == 0) wramBank = 1;
        }
        return;
    }
    uint32_t index = addr & 0x1fff;
    if (index > 0xfff) index = addr & 0xfff | static_cast<uint32_t>(wramBank) << 12;
    cpu.wram[index & 0xffff] = value;
}

// @ 0x7100094a20
uint8_t TimerRead(uint16_t addr) {
    switch (addr) {
    case 0xff04: return cpu.div;
    case 0xff05: return cpu.tima;
    case 0xff06: return cpu.tma;
    case 0xff07: return cpu.tac;
    default: return 0;
    }
}

// @ 0x71000949b0
void TimerWrite(uint16_t addr, uint8_t value) {
    switch (addr) {
    case 0xff04: cpu.div = 0; break;
    case 0xff05: cpu.tima = value; break;
    case 0xff06: cpu.tma = value; break;
    case 0xff07: cpu.tac = value; break;
    default: break;
    }
}

// @ 0x71000948e0. DIV advances at most once per call; TIMA reloads immediately on overflow.
void TimerStep(int32_t ticks) {
    static constexpr int32_t kTimaPeriods[4] = {1024, 16, 64, 256};  // @ 0x7100193e90
    cpu.divAccum += ticks;
    if (cpu.divAccum > 0xff) {
        ++cpu.div;
        cpu.divAccum -= 0x100;
    }
    if ((cpu.tac >> 2 & 1) == 0) return;
    cpu.timaPeriod = kTimaPeriods[cpu.tac & 3];
    cpu.timaAccum += ticks;
    while (cpu.timaPeriod <= cpu.timaAccum) {
        for (;;) {
            ++cpu.tima;
            cpu.timaAccum -= cpu.timaPeriod;
            if (cpu.tima == 0) break;
            if (cpu.timaAccum < cpu.timaPeriod) return;
        }
        cpu.intFlags |= 0xe4;
        cpu.tima = cpu.tma;
    }
}

// @ 0x710008d230: DMG power-on I/O values. gb_init forces CGB mode first, so this never runs.
void DmgIoDefaults() {
    if (core.isCgb != 0) return;
    joypSelect = 0xcf;
    ApuWrite(0xff02, 0x7c);
    TimerWrite(0xff04, 0xab);
    TimerWrite(0xff05, 0);
    TimerWrite(0xff06, 0);
    TimerWrite(0xff07, 0);
    cpu.intFlags = 0xe1;
    static constexpr struct { uint16_t addr; uint8_t value; } kApu[] = {
        {0xff10, 0x80}, {0xff11, 0xbf}, {0xff12, 0xf3}, {0xff14, 0xbf}, {0xff16, 0x3f}, {0xff17, 0x00},
        {0xff19, 0xbf}, {0xff1a, 0x7f}, {0xff1b, 0xff}, {0xff1c, 0x9f}, {0xff1e, 0xbf}, {0xff20, 0xff},
        {0xff21, 0x00}, {0xff22, 0x00}, {0xff23, 0xbf}, {0xff24, 0x77}, {0xff25, 0xf3}, {0xff26, 0xf1},
    };
    for (const auto& w : kApu) ApuWrite(w.addr, w.value);
    VideoIoWrite(0xff40, 0x91);
    VideoIoWrite(0xff42, 0);
    VideoIoWrite(0xff43, 0);
    VideoIoWrite(0xff45, 0);
    VideoIoWrite(0xff47, 0xfc);
    VideoIoWrite(0xff48, 0xff);
    VideoIoWrite(0xff49, 0xff);
    VideoIoWrite(0xff4a, 0);
    VideoIoWrite(0xff4b, 0);
    cpu.intEnable = 0;
}

}  // namespace gb

// ---- joypad (@ 0x71000856c0 / 0x7100085730), part of the EmuGlue API
namespace gb {

void KeyDown(int key) {
    const uint32_t bit = 1u << (static_cast<uint32_t>(key) & 0x1f);
    const uint32_t old = io.joypadButtons;
    io.joypadButtons &= static_cast<uint8_t>(~bit);
    const uint8_t group = key < 4 ? joypSelect >> 4 : joypSelect >> 5;
    if ((group & 1) == 0 && (bit & old) != 0 && cpu.intEnable != 0) cpu.intFlags |= 0x10;
}

void KeyUp(int key) {
    io.joypadButtons |= static_cast<uint8_t>(1u << (static_cast<uint32_t>(key) & 0x1f));
}

}  // namespace gb
