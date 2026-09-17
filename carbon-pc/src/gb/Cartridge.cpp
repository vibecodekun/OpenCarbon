#include "gb/Cartridge.h"
#include "app/EmuGlue.h"
#include "gb/State.h"
#include "platform/Paths.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>

namespace carts {

Cartridge* current = nullptr;

namespace {

// @ 0x7100194368, indexed by header byte 0x149 without a bounds check. Entries past 5 are whatever
// .rodata follows (a CRC table among other things), kept so odd headers allocate the same sizes.
constexpr int32_t kRamSizeTable[256] = {
    0, 2048, 8192, 32768, 131072, 65536, 1647866963, 1667196001,
    7631713, 1698264147, 1885692792, 1852795252, 134479872, 150999052, 34737932, 6,
    2069185569, 855452, 370546176, 589508880, 774778403, 921088, 202116096, 4368,
    218959104, 196631, 327684, 458758, 589832, 720906, 983053, 1245201,
    1769495, 2293791, 3342379, 4390971, 6488147, 8585331, 12779683, 16908515,
    259, 0, 16842752, 33685761, 50528770, 67371779, 84214788, 1285,
    131073, 262147, 458757, 851977, 1638417, 3211297, 6357057, 12648577,
    25231617, 50397697, 100729857, 201394177, 402722817, 805380097, 1610694657, 32768,
    16842752, 50528770, 84214788, 117900806, 151586824, 185272842, 218958860, -1,
    0, 4, 2, 6, 0, 1, 2, 3,
    4, 0, 1, 0, 5, 6, 0, 1996959894,
    -301047508, -1727442502, 124634137, 1886057615, -379345611, -1637575261, 249268274, 2044508324,
    -522852066, -1747789432, 162941995, 2125561021, -407360249, -1866523247, 498536548, 1789927666,
    -205950648, -2067906082, 450548861, 1843258603, -187386543, -2083289657, 325883990, 1684777152,
    -43845254, -1973040660, 335633487, 1661365465, -99664541, -1928851979, 997073096, 1281953886,
    -715111964, -1570279054, 1006888145, 1258607687, -770865667, -1526024853, 901097722, 1119000684,
    -608450090, -1396901568, 853044451, 1172266101, -589951537, -1412350631, 651767980, 1373503546,
    -925412992, -1076862698, 565507253, 1454621731, -809855591, -1195530993, 671266974, 1594198024,
    -972236366, -1324619484, 795835527, 1483230225, -1050600021, -1234817731, 1994146192, 31158534,
    -1731059524, -271249366, 1907459465, 112637215, -1614814043, -390540237, 2013776290, 251722036,
    -1777751922, -519137256, 2137656763, 141376813, -1855689577, -429695999, 1802195444, 476864866,
    -2056965928, -228458418, 1812370925, 453092731, -2113342271, -183516073, 1706088902, 314042704,
    -1950435094, -54949764, 1658658271, 366619977, -1932296973, -69972891, 1303535960, 984961486,
    -1547960204, -725929758, 1256170817, 1037604311, -1529756563, -740887301, 1131014506, 879679996,
    -1385723834, -631195440, 1141124467, 855842277, -1442165665, -586318647, 1342533948, 654459306,
    -1106571248, -921952122, 1466479909, 544179635, -1184443383, -832445281, 1591671054, 702138776,
    -1328506846, -942167884, 1504918807, 783551873, -1212326853, -1061524307, -306674912, -1698712650,
    62317068, 1957810842, -355121351, -1647151185, 81470997, 1943803523, -480048366, -1805370492,
    225274430, 2053790376, -468791541, -1828061283, 167816743, 2097651377, -267414716, -2029476910,
    503444072, 1762050814, -144550051, -2140837941, 426522225, 1852507879, -19653770, -1982649376,
    282753626, 1742555852, -105259153, -1900089351, 397917763, 1622183637, -690576408, -1580100738,
    953729732, 1340076626, -776247311, -1497606297, 1068828381, 1219638859, -670225446, -1358292148,
};

// operator new[] in the original (contents uninitialised); zeroed here
uint8_t* AllocBuffer(int32_t size) {
    return static_cast<uint8_t*>(std::calloc(static_cast<size_t>(static_cast<uint32_t>(size)) + 1, 1));
}

std::string Extension(const std::string& path) {
    size_t dot = path.rfind('.');
    return dot == std::string::npos ? std::string() : path.substr(dot);
}

bool PathIsNes(const std::string& path) {  // @ 0x7100087e00 (case-sensitive)
    const std::string ext = Extension(path);
    return ext == ".nes" || ext == ".NES";
}

bool PathIsGba(const std::string& path) {  // @ 0x7100087f70 (lower-cases the extension first)
    std::string ext = Extension(path);
    for (char& c : ext)
        if (static_cast<unsigned char>(c - 'A') < 26) c = static_cast<char>(c + 0x20);
    return ext == ".gba";
}

// ifstream opened at end (tellg = size), read one byte at a time, as in cart_load.
uint8_t* ReadRom(const std::string& path, int32_t& size) {
    std::ifstream in(Paths::Resolve(path), std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        size = 0;
        std::cout << "Rom could not be open. This probably won't work.";
        return AllocBuffer(0x8000);
    }
    size = static_cast<int32_t>(static_cast<std::streamoff>(in.tellg()));
    uint8_t* data = AllocBuffer(size);
    in.seekg(0);
    for (int32_t i = 0; i < size; ++i) {
        char c = 0;
        in.read(&c, 1);
        data[i] = static_cast<uint8_t>(c);
    }
    return data;
}

}  // namespace

bool LoadSramFile(uint8_t* buffer, int32_t size, const std::string& path) {
    std::ifstream in(Paths::Resolve(path), std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        if (size != 0) std::memset(buffer, 0, static_cast<uint32_t>(size));
        return true;  // missing file: start blank, flushes enabled
    }
    if (static_cast<int32_t>(static_cast<std::streamoff>(in.tellg())) != size) {
        if (size != 0) std::memset(buffer, 0, static_cast<uint32_t>(size));
        return false;  // wrong size: start blank and never save
    }
    in.seekg(0);
    for (int32_t i = 0; static_cast<uint32_t>(i) < static_cast<uint32_t>(size); ++i) {
        char c = 0;
        in.read(&c, 1);
        buffer[i] = static_cast<uint8_t>(c);
    }
    return true;
}

void SaveSramFile(const uint8_t* buffer, int32_t size, const std::string& path) {
    std::ofstream out(Paths::Resolve(path), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return;
    for (int32_t i = 0; static_cast<uint32_t>(i) < static_cast<uint32_t>(size); ++i)
        out.write(reinterpret_cast<const char*>(buffer + i), 1);
}

// ---------------------------------------------------------------------------------------------- RomOnly

RomOnly::RomOnly(uint8_t* romData, int32_t size) : rom(romData), romSize(size) {}

uint8_t RomOnly::Read(uint32_t addr) {  // @ 0x710008a430
    return rom[addr & 0x7fff];
}

void RomOnly::FreeBuffers() {  // @ 0x710008a410
    std::free(rom);
}

// ---------------------------------------------------------------------------------------------- MBC1

Mbc1::Mbc1(uint8_t* romData, int32_t size, int32_t ram) : romSize(size), ramSize(ram), rom(romData) {
    gb::core.mbc1BankLow = 1;  // the other MBC1 globals keep their values from the previous cartridge
    sram = AllocBuffer(ram);
}

void Mbc1::Write(uint32_t addr, uint8_t value) {  // @ 0x710008a630
    const uint32_t region = addr >> 13 & 7;
    if (region == 0) {
        if ((value & 0xf) != 10 && (gb::core.mbc1RamEnable & static_cast<uint8_t>(sramLoaded)) != 0 && sramDirty) {
            FlushSram();
            sramDirty = false;
        }
        gb::core.mbc1RamEnable = (value & 0xf) == 10;
        return;
    }
    if ((addr >> 14 & 3) == 0) {  // 2000-3FFF
        gb::core.mbc1BankLow = value & 0x1f;
        if ((value & 0x1f) == 0) gb::core.mbc1BankLow = 1;
        return;
    }
    if (region < 3) {  // 4000-5FFF
        gb::core.mbc1BankHigh = value & 3;
    } else if (static_cast<int16_t>(addr) < 0) {  // A000-BFFF
        if ((addr & 0xe000) == 0xa000 && gb::core.mbc1RamEnable != 0 && ramSize != 0) {
            uint32_t index;
            if (gb::core.mbc1Mode == 0)
                index = static_cast<uint32_t>(ramSize - 1) & addr & 0x1fff;
            else
                index = (addr & 0x1fff | static_cast<uint32_t>(gb::core.mbc1BankHigh) << 13) & static_cast<uint32_t>(ramSize - 1);
            sram[index] = value;
            sramDirty = true;
        }
    } else {  // 6000-7FFF
        if (value == 1) gb::core.mbc1Mode = 1;
        else if (value == 0) gb::core.mbc1Mode = 0;
    }
}

uint8_t Mbc1::Read(uint32_t addr) {  // @ 0x710008a7b0
    if (static_cast<int16_t>(addr) >= 0) {
        uint32_t index = addr & 0x3fff;
        if ((addr & 0xffff) >> 14 & 1) {
            uint32_t bank = static_cast<uint32_t>(gb::core.mbc1BankLow);
            if (gb::core.mbc1Mode == 0) bank |= static_cast<uint32_t>(gb::core.mbc1BankHigh) << 5;
            index = (index | bank << 14) & static_cast<uint32_t>(romSize - 1);
        }
        return rom[index];
    }
    if ((addr & 0xe000) == 0xa000 && gb::core.mbc1RamEnable != 0) {
        if (ramSize == 0) return 0;
        uint32_t index;
        if (gb::core.mbc1Mode == 0)
            index = static_cast<uint32_t>(ramSize - 1) & addr & 0x1fff;
        else
            index = (addr & 0x1fff | static_cast<uint32_t>(gb::core.mbc1BankHigh) << 13) & static_cast<uint32_t>(ramSize - 1);
        return sram[index];
    }
    return 0;
}

void Mbc1::LoadSram(const std::string& path) {  // @ 0x710008a870
    sramLoaded = false;
    sramPath = path;
    if (LoadSramFile(sram, ramSize, sramPath)) sramLoaded = true;
}

void Mbc1::FlushSram() {  // @ 0x710008a910
    if (sramLoaded) SaveSramFile(sram, ramSize, sramPath);
}

void Mbc1::FreeBuffers() {  // @ 0x710008a600
    std::free(rom);
    std::free(sram);
}

// ---------------------------------------------------------------------------------------------- MBC3

Mbc3::Mbc3(uint8_t* romData, int32_t size, int32_t ram, bool rtcPresent)
    : rom(romData), romSize(size), ramSize(ram), hasRtc(rtcPresent) {
    sram = AllocBuffer(rtcPresent ? ram + 0x30 : ram);
}

void Mbc3::AdvanceRtc(int64_t now) {
    uint8_t dayHigh = rtc[4];
    if (rtcLastTime < now && (dayHigh & 0x40) == 0) {
        const uint32_t delta = static_cast<uint32_t>(static_cast<int32_t>(now) - static_cast<int32_t>(rtcLastTime));
        rtcLastTime = now;
        if (delta == 0) return;
        uint32_t v = rtc[0] + delta;
        rtc[0] = static_cast<uint8_t>(v % 60);
        if (v < 60) return;
        v = v / 60 + rtc[1];
        rtc[1] = static_cast<uint8_t>(v % 60);
        if (v < 60) return;
        v = v / 60 + rtc[2];
        rtc[2] = static_cast<uint8_t>(v % 24);
        if (v < 24) return;
        v = (rtc[3] | static_cast<uint32_t>(dayHigh & 1) << 8) + v / 24;
        uint8_t high = static_cast<uint8_t>((dayHigh & 0xfe) | (v >> 8 & 1));
        if (v >= 0x200) high |= 0x80;
        rtc[3] = static_cast<uint8_t>(v);
        rtc[4] = high;
    } else {
        rtcLastTime = now;
    }
}

void Mbc3::LatchRtc() {  // @ 0x710008ad50
    AdvanceRtc(static_cast<int64_t>(std::time(nullptr)));
    std::memcpy(rtcLatched, rtc, sizeof(rtc));
}

void Mbc3::Write(uint32_t addr, uint8_t value) {  // @ 0x710008aa90
    const uint32_t region = addr >> 13 & 7;
    if (region == 0) {
        if ((value & 0xf) != 10 && (static_cast<uint8_t>(ramEnable) & static_cast<uint8_t>(sramLoaded)) != 0 && sramDirty) {
            FlushSram();
            sramDirty = false;
        }
        ramEnable = (value & 0xf) == 10;
        return;
    }
    if ((addr >> 14 & 3) == 0) {  // 2000-3FFF
        romBank = (value & 0x7f) == 0 ? 1 : value & 0x7f;
        return;
    }
    if (region < 3) {  // 4000-5FFF
        ramSelect = static_cast<uint8_t>(value % 13);
        return;
    }
    if (static_cast<int16_t>(addr) >= 0) {  // 6000-7FFF
        if ((value & 1) && latchPrev == 0 && hasRtc) LatchRtc();
        latchPrev = value & 1;
        return;
    }
    if ((addr & 0xe000) != 0xa000 || !ramEnable) return;
    if (ramSelect < 4) {
        if (ramSize == 0) return;
        sram[(addr & 0x1fff | static_cast<uint32_t>(ramSelect) << 13) & static_cast<uint32_t>(ramSize - 1)] = value;
        sramDirty = true;
        return;
    }
    if (static_cast<uint8_t>(ramSelect - 8) > 4 || !hasRtc) return;
    AdvanceRtc(static_cast<int64_t>(std::time(nullptr)));
    rtc[ramSelect - 8] = value;
}

uint8_t Mbc3::Read(uint32_t addr) {  // @ 0x710008ae60
    if (static_cast<int16_t>(addr) >= 0) {
        uint32_t index = addr & 0x3fff;
        if ((addr & 0xffff) >> 14 & 1) index |= static_cast<uint32_t>(romBank) << 14;  // not masked to ROM size
        return rom[index];
    }
    if ((addr & 0xe000) != 0xa000 || !ramEnable) return 0xff;
    if (ramSelect > 3) {
        if (static_cast<uint8_t>(ramSelect - 8) < 5 && hasRtc) return rtcLatched[ramSelect - 8];
        return 0xff;
    }
    if (ramSize == 0) return 0xff;
    return sram[(addr & 0x1fff | static_cast<uint32_t>(ramSelect) << 13) & static_cast<uint32_t>(ramSize - 1)];
}

void Mbc3::FlushSram() {  // @ 0x710008af90
    if (sramLoaded) SaveSramFile(sram, ramSize, sramPath);
}

void Mbc3::FreeBuffers() {  // @ 0x710008aa60
    std::free(rom);
    std::free(sram);
}

// ---------------------------------------------------------------------------------------------- MBC5

Mbc5::Mbc5(uint8_t* romData, int32_t size, int32_t ram) : rom(romData), romSize(size), ramSize(ram) {
    gb::core.mbc5Dirty = 0;
    gb::core.mbc5BankLow = 1;
    gb::core.mbc5BankHigh = 0;
    sram = AllocBuffer(ram);
}

void Mbc5::Write(uint32_t addr, uint8_t value) {  // @ 0x710008b0e0
    const uint32_t region = addr >> 13 & 7;
    if (region == 0) {
        if (gb::core.mbc5Dirty != 0 && (value & 0xf) != 10 && (gb::core.mbc5RamEnable & static_cast<uint8_t>(sramLoaded)) != 0) {
            FlushSram();
            gb::core.mbc5Dirty = 0;
        }
        gb::core.mbc5RamEnable = (value & 0xf) == 10;
        return;
    }
    if ((addr >> 12 & 0xf) < 3) {  // 2000-2FFF
        gb::core.mbc5BankLow = value;
        return;
    }
    if ((addr >> 14 & 3) == 0) {  // 3000-3FFF
        gb::core.mbc5BankHigh = static_cast<uint8_t>((value & 1) << 1);
        return;
    }
    if (region < 3) {  // 4000-5FFF
        gb::core.mbc5RamBank = value & 0xf;
        return;
    }
    if ((addr & 0xe000) == 0xa000 && gb::core.mbc5RamEnable != 0 && ramSize != 0) {
        sram[(addr & 0x1fff | static_cast<uint32_t>(gb::core.mbc5RamBank) << 13) & static_cast<uint32_t>(ramSize - 1)] = value;
        gb::core.mbc5Dirty = 1;
    }
}

uint8_t Mbc5::Read(uint32_t addr) {  // @ 0x710008b1f0
    if (static_cast<int16_t>(addr) >= 0) {
        uint32_t index = addr & 0x3fff;
        if ((addr & 0xffff) >> 14 & 1) {
            const uint16_t bank = static_cast<uint16_t>(gb::core.mbc5BankLow | gb::core.mbc5BankHigh << 8);
            index |= static_cast<uint32_t>(bank) << 14;  // not masked to ROM size
        }
        return rom[index];
    }
    if ((addr & 0xe000) == 0xa000 && gb::core.mbc5RamEnable != 0) {
        if (ramSize == 0) return 0xff;
        return sram[(addr & 0x1fff | static_cast<uint32_t>(gb::core.mbc5RamBank) << 13) & static_cast<uint32_t>(ramSize - 1)];
    }
    return 0xff;
}

void Mbc5::LoadSram(const std::string& path) {  // @ 0x710008b280
    sramLoaded = false;
    sramPath = path;
    if (LoadSramFile(sram, ramSize, sramPath)) sramLoaded = true;
}

void Mbc5::FlushSram() {  // @ 0x710008b320
    if (sramLoaded) SaveSramFile(sram, ramSize, sramPath);
}

void Mbc5::FreeBuffers() {  // @ 0x710008b0b0
    std::free(rom);
    std::free(sram);
}

// ---------------------------------------------------------------------------------------------- cart_load

Cartridge* Load(const std::string& romPath) {  // @ 0x71000880e0
    if (PathIsNes(romPath)) {
        int32_t size = 0;
        std::free(ReadRom(romPath, size));  // the NES placeholder object keeps the data but never runs
        return new SystemOnly(3);
    }
    if (PathIsGba(romPath)) return new SystemOnly(2);

    int32_t romSize = 0;
    uint8_t* rom = ReadRom(romPath, romSize);

    char headerTitle[16] = {};
    std::memcpy(headerTitle, rom + 0x134, 12);
    const uint8_t cartType = rom[0x147];
    const int32_t ramSize = kRamSizeTable[rom[0x149]];

    char buf[0x20];
    std::snprintf(buf, 0x1f, "save:/%s.sav", headerTitle);
    const std::string savPath = buf;
    std::snprintf(buf, 0x1f, "save:/%s.stt", headerTitle);
    const std::string sttPath = buf;
    std::snprintf(buf, 0x11, "%s", headerTitle);
    const std::string title = buf;

    if (std::memcmp(headerTitle, "PM_CRYSTAL", 11) == 0) {  // hard-coded game patch: JP $6385
        rom[0x658c] = 0xc3;
        rom[0x658d] = 0x85;
        rom[0x658e] = 0x63;
    }

    Cartridge* cart = nullptr;
    switch (cartType) {
    case 0x00: cart = new RomOnly(rom, romSize); break;
    case 0x01: cart = new Mbc1(rom, romSize, 0); break;
    case 0x02:
    case 0x03: cart = new Mbc1(rom, romSize, ramSize); break;  // 0x03 has a battery, but no .sav is loaded
    case 0x0f: cart = new Mbc3(rom, romSize, 0, true); break;
    case 0x10: cart = new Mbc3(rom, romSize, ramSize, true); cart->LoadSram(savPath); break;
    case 0x11: cart = new Mbc3(rom, romSize, 0, false); break;
    case 0x12: cart = new Mbc3(rom, romSize, ramSize, false); break;
    case 0x13: cart = new Mbc3(rom, romSize, ramSize, false); cart->LoadSram(savPath); break;
    case 0x19:
    case 0x1c: cart = new Mbc5(rom, romSize, 0); break;
    case 0x1a:
    case 0x1d: cart = new Mbc5(rom, romSize, ramSize); break;
    case 0x1b:
    case 0x1e: cart = new Mbc5(rom, romSize, ramSize); cart->LoadSram(savPath); break;
    default:
        // The original leaves the object NULL and crashes on the next virtual call.
        std::fprintf(stderr, "cart_load: cartridge type 0x%02x has no mapper in Carbon (the original crashes here)\n", cartType);
        std::abort();
    }

    cart->SetStatePath(sttPath);
    cart->systemType = static_cast<uint32_t>(cart->Read(0x143) >> 7 & 1);
    cart->title = title;
    cart->statePath = sttPath;
    return cart;
}

uint32_t SystemType(const Cartridge* c) { return c->systemType; }
const std::string& Title(const Cartridge* c) { return c->title; }
void FreeBuffers(Cartridge* c) { c->FreeBuffers(); }
void Destroy(Cartridge* c) { delete c; }

}  // namespace carts
