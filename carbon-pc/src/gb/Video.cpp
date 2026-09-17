#include "app/EmuGlue.h"
#include "gb/Cartridge.h"
#include "gb/Core.h"
#include <cstring>

namespace gb {
namespace {

// @ 0x710019433c: BGP colour 0-3 as RGB (only used by the DMG renderer)
constexpr uint8_t kDmgGreens[4][3] = {{0xe0, 0xf8, 0xd0}, {0x88, 0xc0, 0x70}, {0x34, 0x68, 0x56}, {0x08, 0x18, 0x20}};

// RGB555 little-endian palette entry -> RGBA8888 with bit replication, as the renderers do it inline.
void PutPixel(uint8_t* rgba, uint8_t lo, uint8_t hi) {
    const uint16_t c = static_cast<uint16_t>(lo | hi << 8);
    rgba[0] = static_cast<uint8_t>(lo << 3 | (lo >> 2 & 7));
    rgba[1] = static_cast<uint8_t>((c >> 2 & 0xf8) | (c >> 7 & 7));
    rgba[2] = static_cast<uint8_t>((hi & 0x7c) << 1 | (hi >> 4 & 7));
    rgba[3] = 0xff;
}

uint8_t* LineStart() {
    return &CoreByte(kFramebufferOffset + static_cast<size_t>(io.ly) * 0x280);
}

// HDMA / GDMA source read: anything below C000 goes through the cartridge, including VRAM sources.
uint8_t DmaSourceRead(uint32_t addr) {
    if ((addr >> 14 & 3) < 3) return carts::current->Read(addr);
    return static_cast<uint8_t>(WramRead(addr));
}

void DmaCopy16() {
    for (int i = 0; i < 16; ++i) {
        const uint8_t v = DmaSourceRead(io.hdmaSrc);
        Write8(io.hdmaDst, v);
        ++io.hdmaDst;
        ++io.hdmaSrc;
    }
}

// @ 0x71000858d0
void CgbRenderBg() {
    const uint32_t mapBase = (io.lcdc & 8) ? 0x1c00 : 0x1800;
    const uint32_t y = static_cast<uint32_t>(io.ly) + io.scy;
    const uint32_t row2 = (y & 7) << 1;
    const uint32_t mapRow = (y & 0xf8) << 2;
    const bool unsignedTiles = (io.lcdc >> 4 & 1) != 0;
    uint8_t* out = LineStart();
    for (uint32_t x = 0; x < 0xa0; ++x, out += 4) {
        const uint32_t sx = io.scx + x;
        const uint32_t mapIndex = mapBase | mapRow | (sx & 0xf8) >> 3;
        const uint8_t attr = core.vram[mapIndex | 0x2000];
        uint32_t px = sx & 7;
        if (attr & 0x20) px ^= 7;
        uint32_t r2 = row2;
        if (attr & 0x40) r2 ^= 0xe;
        const uint8_t tile = core.vram[mapIndex];
        uint32_t tileAddr = unsignedTiles ? (static_cast<uint32_t>(attr & 8) << 10 | static_cast<uint32_t>(tile) << 4 | r2)
                                          : ((tile ^ 0x80u) * 0x10 + 0x800 | static_cast<uint32_t>(attr >> 3 & 1) << 13 | r2);
        const uint32_t shift = px ^ 7;
        const uint32_t colour = (attr & 7u) << 2 | (core.vram[tileAddr] >> shift & 1) | (core.vram[tileAddr | 1] >> shift & 1) << 1;
        core.lineBgPriority[x] = attr >> 7;
        core.lineColorIndex[x] = static_cast<int32_t>(colour | 0x20);
        PutPixel(out, io.bgPalette[colour << 1], io.bgPalette[colour << 1 | 1]);
    }
}

// @ 0x7100085c90 (also has the DMG-mode variant inline: no flips, no tile bank)
void CgbRenderWindow() {
    const bool cgb = core.isCgb != 0;
    const int64_t left = static_cast<int64_t>(io.wx) - 7;
    if (!(left < 0xa0 && io.wy <= io.ly)) return;
    const uint32_t wxu = io.wx;
    const uint32_t mapBase = (io.lcdc & 0x40) ? 0x1c00 : 0x1800;
    const uint32_t tileRow = io.windowLine & 7;
    const uint32_t mapRow = (io.windowLine & 0xf8) * 4u;
    const bool unsignedTiles = (io.lcdc >> 4 & 1) != 0;
    uint8_t* out = LineStart();
    ++io.windowLine;
    for (int64_t x = 0; x < 0xa0; ++x, out += 4) {
        if (x < left) continue;
        const uint32_t col = static_cast<uint32_t>(-static_cast<int32_t>(wxu) + 7 + static_cast<int32_t>(x));
        if (static_cast<int32_t>(col) < 0) continue;
        const uint32_t mapIndex = (mapBase + mapRow + (col >> 3)) & 0xffff;
        const uint8_t attr = core.vram[mapIndex | 0x2000];
        const uint8_t tile = core.vram[mapIndex];
        uint32_t px = col & 7;
        uint32_t row = tileRow;
        uint32_t tileAddr = unsignedTiles ? static_cast<uint32_t>(tile) << 4 : (tile ^ 0x80u) * 0x10 + 0x800;
        if (cgb) {
            if (attr & 0x20) px ^= 7;
            if (attr & 0x40) row ^= 7;
            tileAddr |= static_cast<uint32_t>(attr >> 3 & 1) << 13;
        }
        const size_t index = static_cast<size_t>(tileAddr) + static_cast<size_t>(row) * 2;
        const uint32_t shift = 7 - px & 0x1f;
        const uint32_t colour = (attr & 7u) << 2 | (core.vram[index] >> shift & 1) | (CoreByte(offsetof(CoreBlock, vram) + index + 1) >> shift & 1) << 1;
        core.lineBgPriority[x] = attr >> 7;
        core.lineColorIndex[x] = static_cast<int32_t>(colour | 0x20);
        PutPixel(out, io.bgPalette[colour << 1], io.bgPalette[colour << 1 | 1]);
    }
}

// @ 0x71000860e0. Up to 10 sprites in OAM order, lower OAM index wins per pixel.
void CgbRenderSprites() {
    struct Entry { uint8_t y, x, tile, attr; } found[10] = {};
    int32_t colour[160];
    uint8_t priority[160] = {};
    std::memset(colour, 0xff, sizeof(colour));

    const bool cgb = core.isCgb != 0;
    const int32_t height = (io.lcdc & 4) ? 16 : 8;
    const uint32_t ly = io.ly;
    int count = 0;
    for (uint32_t i = 1;; ++i) {
        const uint8_t* e = &core.oam[(i - 1) * 4];
        const int32_t top = e[0] - 16;
        if (top <= static_cast<int32_t>(ly) && static_cast<int32_t>(ly) < top + height) found[count++] = {e[0], e[1], e[2], e[3]};
        if (i > 0x27 || count >= 10) break;
    }

    for (int k = 0; k < count; ++k) {
        const Entry& s = found[k];
        const uint32_t top = static_cast<uint32_t>(s.y - 0x10) & 0xff;
        uint32_t row = ly - top;
        if (s.attr & 0x40) row = static_cast<uint32_t>(height) + top + ~ly;
        uint32_t tileNum = s.tile;
        if (io.lcdc >> 2 & 1) {  // 16 px tall: second half uses tile|1, first half tile&0xfe
            tileNum = (row & 0xf8) ? (s.tile | 1u) : (s.tile & 0xfeu);
            row &= 7;
        }
        uint32_t tileAddr = tileNum << 4;
        if (cgb && (s.attr & 8)) tileAddr |= 0x2000;
        const size_t index = static_cast<size_t>((row & 0xff) << 1) + tileAddr;
        const uint8_t lo = core.vram[index];
        const uint8_t hi = CoreByte(offsetof(CoreBlock, vram) + (index | 1));
        const uint8_t prio = s.attr >> 7;

        // The function also has a DMG branch (OBP0/OBP1 colours, X-ordered ownership). It is unreachable because
        // gb_init forces CGB mode, and is not ported.
        if (!cgb) continue;
        const uint32_t pal = (s.attr & 7u) << 2;
        const bool xflip = (s.attr >> 5 & 1) != 0;
        for (uint32_t n = 0; n < 8; ++n) {
            const uint8_t x = static_cast<uint8_t>(s.x - 8 + n);
            if (x >= 0xa0 || colour[x] != -1) continue;
            const uint32_t bit = xflip ? n : 7 - n;
            const uint32_t c = (lo >> bit & 1) | (hi >> bit & 1) << 1;
            if (c != 0) colour[x] = static_cast<int32_t>(c | pal);
            priority[x] = prio;
        }

        // The original composites the whole line after every sprite; the result equals doing it once, but
        // keeping the loop here keeps the framebuffer identical at every step.
        uint8_t* out = LineStart();
        for (uint32_t x = 0; x < 0xa0; ++x, out += 4) {
            if (((priority[x] == 0 && core.lineBgPriority[x] == 0) || (core.lineColorIndex[x] & 3) == 0) && colour[x] != -1) {
                const uint32_t c = static_cast<uint32_t>(colour[x]) & 0x1f;
                PutPixel(out, io.objPalette[c << 1], io.objPalette[c << 1 | 1]);
            }
        }
    }
}

}  // namespace

// @ 0x71000871f0
uint8_t VramRead(uint32_t addr) {
    if ((addr & 0xe000) == 0x8000) return core.vram[addr & 0x1fff | static_cast<uint32_t>(io.vbk) << 13];
    if ((addr >> 5 & 0x7ff) > 0x7f4) return 0;
    return CoreByte(kOamOffset + (addr & 0xff));
}

// @ 0x7100087260
void VramWrite(uint32_t addr, uint8_t value) {
    const uint32_t page = addr >> 12 & 0xf;
    if (page - 8 < 2) {
        core.vram[addr & 0x1fff | static_cast<uint32_t>(io.vbk) << 13] = value;
        if ((addr >> 11 & 0x1f) > 0x12) {  // 9800-9FFF: tile map copy for the DMG renderer
            core.dmgTileMap[(addr & 0x3ff) + ((addr & 0xffff) >> 10 & 1) * 0x4000] = value;
            return;
        }
        // 8000-97FF: re-decode this tile row for the DMG renderer (always from bank 0)
        const uint32_t pair = addr & 0x1ffe;
        const uint8_t lo = core.vram[pair];
        const uint8_t hi = core.vram[pair | 1];
        uint8_t* p = &core.dmgTileCache[(addr >> 1 & 7) * 8 + (pair >> 4) * 0x40];
        for (int n = 0; n < 8; ++n) p[n] = static_cast<uint8_t>((hi >> (7 - n) & 1) << 1 | (lo >> (7 - n) & 1));
    } else if (page == 0xf && ((addr + 0x200) & 0xffff) < 0xa0) {
        const uint32_t i = addr & 0xff;
        core.oam[i] = value;
        core.oamMirror[(addr & 3) + (i & 0xfc)] = value;
    }
}

// @ 0x7100087700
uint8_t VideoIoRead(uint16_t addr) {
    switch (addr) {
    case 0xff40: return io.lcdc;
    case 0xff41: return io.stat;
    case 0xff42: return io.scy;
    case 0xff43: return io.scx;
    case 0xff44: return io.ly;
    case 0xff45: return io.lyc;
    case 0xff47: return io.bgp;
    case 0xff48: return io.obp0;
    case 0xff49: return io.obp1;
    case 0xff4a: return io.wy;
    case 0xff4b: return io.wx;
    case 0xff4d: return cpu.key1;
    case 0xff4f: return io.vbk;
    case 0xff51: return static_cast<uint8_t>(io.hdmaSrc >> 8);
    case 0xff52: return static_cast<uint8_t>(io.hdmaSrc);
    case 0xff53: return static_cast<uint8_t>(io.hdmaDst >> 8);
    case 0xff54: return static_cast<uint8_t>(io.hdmaDst);
    case 0xff55: return static_cast<uint8_t>((io.hdmaLength & 0x7f | static_cast<uint32_t>(io.hdmaActive) << 7) ^ 0x80);
    case 0xff68: return io.bcps;
    case 0xff69: return io.bgPalette[io.bcps & 0x3f];
    case 0xff6a: return io.ocps;
    case 0xff6b: return io.objPalette[io.ocps & 0x3f];
    default: return 0;
    }
}

// @ 0x71000878c0
void VideoIoWrite(uint16_t addr, uint8_t value) {
    switch (addr) {
    case 0xff40: io.lcdc = value; return;
    case 0xff41: io.stat = static_cast<uint8_t>((io.stat & 0x87) | (value & 0xf8)); return;
    case 0xff42: io.scy = value; return;
    case 0xff43: io.scx = value; return;
    case 0xff44: io.windowLine = 0; io.ly = 0; return;
    case 0xff45: io.lyc = value; return;
    case 0xff46:  // OAM DMA: 160 bytes through the full memory map, instantly
        for (uint32_t i = 0; i < 0xa0; ++i) {
            const uint32_t src = static_cast<uint32_t>(value) * 0x100 + i;
            const uint8_t v = Read8(src);
            core.oam[i] = v;
            core.oamMirror[(src & 3) + (src >> 2 & 0x3f) * 4] = v;
        }
        return;
    case 0xff47:
        std::memcpy(&core.dmgPaletteRgb[0], kDmgGreens[value & 3], 3);
        std::memcpy(&core.dmgPaletteRgb[3], kDmgGreens[value >> 2 & 3], 3);
        std::memcpy(&core.dmgPaletteRgb[6], kDmgGreens[value >> 4 & 3], 3);
        std::memcpy(&core.dmgPaletteRgb[9], kDmgGreens[value >> 6 & 3], 3);
        io.bgp = value;
        return;
    case 0xff48: io.obp0 = value; return;
    case 0xff49: io.obp1 = value; return;
    case 0xff4a: io.wy = value; return;
    case 0xff4b: io.wx = value; return;
    case 0xff4d: cpu.key1 = value; return;
    case 0xff4f: io.vbk = value & 1; return;
    case 0xff51: io.hdmaSrc = static_cast<uint16_t>(value << 8 | (io.hdmaSrc & 0xff)); return;
    case 0xff52: io.hdmaSrc = static_cast<uint16_t>((io.hdmaSrc & 0xff00 | value) & 0xfff0); return;
    case 0xff53: io.hdmaDst = static_cast<uint16_t>((value << 8 | (io.hdmaDst & 0xff)) & 0x1fff | 0x8000); return;
    case 0xff54: io.hdmaDst = static_cast<uint16_t>((io.hdmaDst & 0xff00 | value) & 0xfff0); return;
    case 0xff55:
        io.hdmaLength = value & 0x7f;
        if (!(value & 0x80) && io.hdmaActive == 0) {  // GDMA: (length + 1) * 16 bytes at once
            uint32_t block = 0;
            bool more;
            do {
                DmaCopy16();
                more = block < static_cast<uint32_t>(io.hdmaLength & 0x7f);
                ++block;
            } while (more);
            io.hdmaLength = 0xff;
            io.hdmaActive = 0;
            return;
        }
        if (io.hdmaActive != 0 && !(value & 0x80)) {
            io.hdmaActive = 0;
            return;
        }
        io.hdmaActive = 1;
        return;
    case 0xff68: io.bcps = value; return;
    case 0xff69:
        io.bgPalette[io.bcps & 0x3f] = value;
        if (io.bcps & 0x80) io.bcps = static_cast<uint8_t>((io.bcps + 1) & 0xbf);
        return;
    case 0xff6a: io.ocps = value; return;
    case 0xff6b:
        io.objPalette[io.ocps & 0x3f] = value;
        if (io.ocps & 0x80) io.ocps = static_cast<uint8_t>((io.ocps + 1) & 0xbf);
        return;
    default:
        return;
    }
}

// @ 0x71000873b0. Fixed timings: mode 2 = 80, mode 3 = 172, HBlank = 204 ticks, 456 per line, 10 VBlank lines
// (LY 144..152, then 0 while still in VBlank). STAT interrupts fire on every matching step (no blocking).
void LcdStep(int32_t ticks) {
    int32_t accum = core.lcdTickAccum + ticks;
    const uint32_t mode = io.stat & 3;
    core.lcdMode = static_cast<uint8_t>(mode);
    const uint32_t stat = io.stat;

    if (!(io.lcdc & 0x80)) {
        io.stat &= 0xfc;
        io.ly = 0;
        core.lcdMode = 0;
        io.windowLine = 0;
        core.lcdTickAccum = 0;
        return;
    }

    if (mode == 1) {
        core.lcdTickAccum = accum - 456;
        if (accum >= 456) {
            uint32_t s = (io.lyc != static_cast<uint8_t>(io.ly + 1)) ? (stat & 0xfffffffbu) : (stat | 4);
            io.stat = static_cast<uint8_t>(s);
            if ((~s & 0x44) == 0) cpu.intFlags |= 0xe2;
            if (io.ly == 0) {
                if (s >> 5 & 1) cpu.intFlags |= 0xe2;
                io.stat = static_cast<uint8_t>((io.stat & 0xfc) | 2);
                io.ly = 0;
                return;
            }
            const bool last = io.ly == 0x98;
            ++io.ly;
            accum = core.lcdTickAccum;
            if (last) {
                const uint32_t s2 = io.lyc != 0 ? (s & 0xfffffffbu) : (s | 4);
                io.windowLine = 0;
                io.ly = 0;
                io.stat = static_cast<uint8_t>(s2);
                if ((~s2 & 0x44) == 0) {
                    cpu.intFlags |= 0xe2;
                    return;
                }
            }
        }
    } else if (mode == 2) {
        if (accum > 0x4f) {
            core.lcdTickAccum = accum - 80;
            io.stat |= 3;
            return;
        }
    } else if (mode == 3) {
        core.lcdTickAccum = accum - 172;
        if (accum > 0xab) {
            if (io.stat >> 3 & 1) cpu.intFlags |= 0xe2;
            io.stat &= 0xfc;
            RenderScanline();
            accum = core.lcdTickAccum;
            if (io.hdmaActive != 0) {
                if ((io.hdmaLength & 0x7f) == 0) io.hdmaActive = 0;
                DmaCopy16();
                --io.hdmaLength;
                return;
            }
        }
    } else if (accum > 0xcb) {
        core.lcdTickAccum = accum - 204;
        ++io.ly;
        const uint32_t s = (io.lyc != io.ly) ? (stat & 0xfffffffbu) : (stat | 4);
        if ((~s & 0x44) == 0) cpu.intFlags |= 0xe2;
        if (io.ly == 0x90) {
            if (s >> 4 & 1) cpu.intFlags |= 0xe2;
            io.stat = static_cast<uint8_t>((s & 0xfc) | 1);
            cpu.intFlags |= 0xe1;
            core.frameDone = 1;
            return;
        }
        if (s >> 5 & 1) cpu.intFlags |= 0xe2;
        io.stat = static_cast<uint8_t>((s & 0xfc) | 2);
        return;
    }
    core.lcdTickAccum = accum;
}

// @ 0x7100085800. BG off (LCDC bit 0 clear) skips the BG entirely, even in CGB mode.
void RenderScanline() {
    if (io.lcdc & 1) {
        if (core.isCgb == 0) {
            // dmg_render_bg @ 0x7100085b30: unreachable (gb_init forces CGB) and not ported
        } else {
            CgbRenderBg();
        }
    }
    if ((io.lcdc >> 5 & 1) && io.wy <= io.ly) {
        if (core.isCgb == 0) {
            // dmg_render_window @ 0x7100085f70: unreachable, not ported
        } else {
            CgbRenderWindow();
        }
    }
    if (io.lcdc >> 1 & 1) {
        if (core.isCgb != 0) CgbRenderSprites();
        // dmg_render_sprites @ 0x7100086c00: unreachable, not ported
    }
}

// @ 0x7100085750
void VideoReset() {
    std::memset(core.framebuffer, 0xff, sizeof(core.framebuffer));
    std::memset(core.dmgTileCache, 0, sizeof(core.dmgTileCache));
    std::memset(core.oamMirror, 0, sizeof(core.oamMirror));
    std::memset(core.dmgTileMap, 0, sizeof(core.dmgTileMap));
    std::memset(&io.ly, 0, sizeof(IoBlock) - offsetof(IoBlock, ly));
}

}  // namespace gb
