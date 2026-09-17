#include "gb/State.h"
#include <cstring>

namespace gb {

#define CHECK_OFFSET(Block, field, base, addr) \
    static_assert(offsetof(Block, field) == (addr) - (base), #Block "::" #field " is not at its original address")

CHECK_OFFSET(IoBlock, ly, kIoBlockAddr, 0x71001b1bda);
CHECK_OFFSET(IoBlock, lcdc, kIoBlockAddr, 0x71001b1bdd);
CHECK_OFFSET(IoBlock, stat, kIoBlockAddr, 0x71001b1be6);
CHECK_OFFSET(IoBlock, hdmaSrc, kIoBlockAddr, 0x71001b1bec);
CHECK_OFFSET(IoBlock, hdmaLength, kIoBlockAddr, 0x71001b1bf0);
CHECK_OFFSET(IoBlock, bgPalette, kIoBlockAddr, 0x71001b1bf2);
CHECK_OFFSET(IoBlock, objPalette, kIoBlockAddr, 0x71001b1c32);
static_assert(sizeof(IoBlock) == 0x71001b1c72 - kIoBlockAddr);

CHECK_OFFSET(CoreBlock, framebuffer, kCoreBlockAddr, 0x7100223303);
CHECK_OFFSET(CoreBlock, dmgTileCache, kCoreBlockAddr, 0x7100239b03);
CHECK_OFFSET(CoreBlock, oamMirror, kCoreBlockAddr, 0x710023fb03);
CHECK_OFFSET(CoreBlock, dmgTileMap, kCoreBlockAddr, 0x710023fba3);
CHECK_OFFSET(CoreBlock, isCgb, kCoreBlockAddr, 0x7100247ba3);
CHECK_OFFSET(CoreBlock, vram, kCoreBlockAddr, 0x7100247ba4);
CHECK_OFFSET(CoreBlock, lineColorIndex, kCoreBlockAddr, 0x710024bba4);
CHECK_OFFSET(CoreBlock, lineBgPriority, kCoreBlockAddr, 0x710024be24);
CHECK_OFFSET(CoreBlock, dmgPaletteRgb, kCoreBlockAddr, 0x710024bec4);
CHECK_OFFSET(CoreBlock, lcdTickAccum, kCoreBlockAddr, 0x710024bed0);
CHECK_OFFSET(CoreBlock, frameDone, kCoreBlockAddr, 0x710024bed5);
CHECK_OFFSET(CoreBlock, mbc1BankLow, kCoreBlockAddr, 0x710024bed8);
CHECK_OFFSET(CoreBlock, mbc1Mode, kCoreBlockAddr, 0x710024bee1);
CHECK_OFFSET(CoreBlock, mbc5Dirty, kCoreBlockAddr, 0x710024bee4);
CHECK_OFFSET(CoreBlock, mbc5RamBank, kCoreBlockAddr, 0x710024bee8);
CHECK_OFFSET(CoreBlock, oam, kCoreBlockAddr, 0x710024bef0);
CHECK_OFFSET(CoreBlock, hram, kCoreBlockAddr, 0x710024bf90);
static_assert(sizeof(CoreBlock) == 0x710024c010 - kCoreBlockAddr);

CHECK_OFFSET(CpuBlock, sp, kCpuBlockAddr, 0x710024d038);
CHECK_OFFSET(CpuBlock, halted, kCpuBlockAddr, 0x710024d03c);
CHECK_OFFSET(CpuBlock, ticks, kCpuBlockAddr, 0x710024d040);
CHECK_OFFSET(CpuBlock, rst38Marker, kCpuBlockAddr, 0x710024d048);
CHECK_OFFSET(CpuBlock, key1, kCpuBlockAddr, 0x710024d04c);
CHECK_OFFSET(CpuBlock, divAccum, kCpuBlockAddr, 0x710024d050);
CHECK_OFFSET(CpuBlock, div, kCpuBlockAddr, 0x710024d054);
CHECK_OFFSET(CpuBlock, timaPeriod, kCpuBlockAddr, 0x710024d05c);
CHECK_OFFSET(CpuBlock, intFlags, kCpuBlockAddr, 0x710024d060);
CHECK_OFFSET(CpuBlock, wram, kCpuBlockAddr, 0x710024d062);
static_assert(sizeof(CpuBlock) == 0x7100255062 - kCpuBlockAddr);

IoBlock io;
CoreBlock core;
CpuBlock cpu;
uint8_t joypSelect;
uint8_t wramBank;

void ResetStateToImageDefaults() {
    std::memset(&io, 0, sizeof(io));
    io.joypadButtons = 0xff;
    io.hdmaLength = 0x00ff;
    std::memset(&core, 0, sizeof(core));
    std::memset(&cpu, 0, sizeof(cpu));
    joypSelect = 0;
    wramBank = 1;
}

namespace {
struct InitDefaults {
    InitDefaults() { ResetStateToImageDefaults(); }
} initDefaults;
}  // namespace

}  // namespace gb
