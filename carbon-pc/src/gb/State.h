// Carbon GB/GBC core state.
//
// The original keeps everything in loose globals. These packed blocks reproduce their exact layout (original
// address noted per field, offsets checked in State.cpp), so that
//   - save states copy the same contiguous byte ranges,
//   - out-of-range accesses the original performs (e.g. FEA0-FEFF writes landing in HRAM) hit the same bytes,
//   - tools/gbdiff can compare the port against the original code byte for byte.
#pragma once
#include <cstddef>
#include <cstdint>

namespace gb {

#pragma pack(push, 1)

// .data @ 0x71001b1bd8
struct IoBlock {
    uint8_t joypadButtons;      // 0x71001b1bd8  bit clear = held: 0 R, 1 L, 2 U, 3 D, 4 A, 5 B, 6 Select, 7 Start
    uint8_t _1bd9;
    uint8_t ly;                 // 0x71001b1bda  start of the 0x98-byte video block copied by save states
    uint8_t lyc;                // 0x71001b1bdb
    uint8_t _1bdc;
    uint8_t lcdc;               // 0x71001b1bdd
    uint8_t scy;                // 0x71001b1bde
    uint8_t scx;                // 0x71001b1bdf
    uint8_t wy;                 // 0x71001b1be0
    uint8_t wx;                 // 0x71001b1be1
    uint8_t bgp;                // 0x71001b1be2
    uint8_t obp0;               // 0x71001b1be3
    uint8_t obp1;               // 0x71001b1be4
    uint8_t windowLine;         // 0x71001b1be5  window row counter, +1 per rendered window line
    uint8_t stat;               // 0x71001b1be6
    uint8_t bcps;               // 0x71001b1be7
    uint8_t ocps;               // 0x71001b1be8
    uint8_t vbk;                // 0x71001b1be9
    uint8_t hdmaActive;         // 0x71001b1bea
    uint8_t _1beb;
    uint16_t hdmaSrc;           // 0x71001b1bec  FF51 high / FF52 low
    uint16_t hdmaDst;           // 0x71001b1bee  FF53 high / FF54 low
    uint16_t hdmaLength;        // 0x71001b1bf0  FF55 low 7 bits (initial value 0x00FF)
    uint8_t bgPalette[0x40];    // 0x71001b1bf2
    uint8_t objPalette[0x40];   // 0x71001b1c32
};

// .bss @ 0x7100223300
struct CoreBlock {
    uint8_t imeRequest;               // 0x7100223300
    uint8_t ime;                      // 0x7100223301
    uint8_t eiDelay;                  // 0x7100223302
    uint8_t framebuffer[160 * 144 * 4];  // 0x7100223303  RGBA8888
    uint8_t dmgTileCache[0x6000];     // 0x7100239b03  decoded 2bpp pixels, 0x40 per tile (DMG renderer only)
    uint8_t oamMirror[0xa0];          // 0x710023fb03  copy of OAM kept by OAM writes and DMA (saved, unused)
    uint8_t dmgTileMap[0x8000];       // 0x710023fba3  copies of 9800-9BFF / 9C00-9FFF (DMG renderer only)
    uint8_t isCgb;                    // 0x7100247ba3  forced to 1 by gb_init
    uint8_t vram[0x4000];             // 0x7100247ba4  bank 0, bank 1
    int32_t lineColorIndex[160];      // 0x710024bba4  per-pixel BG/window palette<<2|colour, |0x20
    uint8_t lineBgPriority[0xa0];     // 0x710024be24  per-pixel BG attribute bit 7
    uint8_t dmgPaletteRgb[12];        // 0x710024bec4  BGP expanded through the green palette table
    int32_t lcdTickAccum;             // 0x710024bed0
    uint8_t lcdMode;                  // 0x710024bed4
    uint8_t frameDone;                // 0x710024bed5
    uint8_t _bed6[2];
    int32_t mbc1BankLow;              // 0x710024bed8  (save states copy 0x710024bed8..bee3)
    int32_t mbc1BankHigh;             // 0x710024bedc
    uint8_t mbc1RamEnable;            // 0x710024bee0
    uint8_t mbc1Mode;                 // 0x710024bee1
    uint8_t _bee2[2];
    uint8_t mbc5Dirty;                // 0x710024bee4  (save states copy 0x710024bee4..bee9)
    uint8_t mbc5RamEnable;            // 0x710024bee5
    uint8_t mbc5BankLow;              // 0x710024bee6
    uint8_t mbc5BankHigh;             // 0x710024bee7  holds (value & 1) << 1: bank 256+ maps to +512
    uint8_t mbc5RamBank;              // 0x710024bee8
    uint8_t _bee9[7];
    uint8_t oam[0xa0];                // 0x710024bef0
    uint8_t hram[0x7f];               // 0x710024bf90
    uint8_t _c00f;
};

// .bss @ 0x710024d030
struct CpuBlock {
    uint8_t f, a, c, b, e, d, l, h;   // 0x710024d030  little-endian pairs AF BC DE HL
    uint16_t sp;                      // 0x710024d038
    uint16_t pc;                      // 0x710024d03a
    uint8_t halted;                   // 0x710024d03c
    uint8_t _d03d[3];
    int32_t ticks;                    // 0x710024d040  4 ticks per M-cycle
    uint8_t debugLog;                 // 0x710024d044  prints "not implemented" for illegal opcodes
    uint8_t _d045[3];
    int32_t rst38Marker;              // 0x710024d048  cleared by RST 38, never read
    uint8_t key1;                     // 0x710024d04c  FF4D
    uint8_t doubleSpeed;              // 0x710024d04d
    uint8_t _d04e[2];
    int32_t divAccum;                 // 0x710024d050
    uint8_t div;                      // 0x710024d054  (save states copy div..tma)
    uint8_t tac;                      // 0x710024d055
    uint8_t tima;                     // 0x710024d056
    uint8_t tma;                      // 0x710024d057
    int32_t timaAccum;                // 0x710024d058
    int32_t timaPeriod;               // 0x710024d05c
    uint8_t intFlags;                 // 0x710024d060  IF
    uint8_t intEnable;                // 0x710024d061  IE
    uint8_t wram[0x8000];             // 0x710024d062
};

#pragma pack(pop)

extern IoBlock io;          // 0x71001b1bd8
extern CoreBlock core;      // 0x7100223300
extern CpuBlock cpu;        // 0x710024d030
extern uint8_t joypSelect;  // 0x710024cff8  FF00 select bits
extern uint8_t wramBank;    // 0x71001b2818  FF70, initial 1

// Original base addresses, for tools that compare against the Switch binary.
inline constexpr uint64_t kIoBlockAddr = 0x71001b1bd8;
inline constexpr uint64_t kCoreBlockAddr = 0x7100223300;
inline constexpr uint64_t kCpuBlockAddr = 0x710024d030;
inline constexpr uint64_t kJoypSelectAddr = 0x710024cff8;
inline constexpr uint64_t kWramBankAddr = 0x71001b2818;

// Byte access relative to a block, for the original's unchecked indexing that crosses field boundaries.
inline uint8_t& CoreByte(size_t offset) { return reinterpret_cast<uint8_t*>(&core)[offset]; }
inline constexpr size_t kOamOffset = offsetof(CoreBlock, oam);
inline constexpr size_t kFramebufferOffset = offsetof(CoreBlock, framebuffer);

void ResetStateToImageDefaults();  // .data initial values, zeroed .bss (what a fresh process has)

}  // namespace gb
