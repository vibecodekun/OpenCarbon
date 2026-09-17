// Cartridge objects created by cart_load (@ 0x71000880e0). Vtable order matches the original:
// [0] Write, [1] Read, [2] LoadSram, [3] FlushSram, [4] SetStatePath, [5] FreeBuffers.
#pragma once
#include <cstdint>
#include <string>

namespace carts {

struct Cartridge {  // base object 0x40 bytes, ctor @ 0x7100087d80, dtor @ 0x7100087db0
    virtual void Write(uint32_t addr, uint8_t value) = 0;
    virtual uint8_t Read(uint32_t addr) = 0;
    virtual void LoadSram(const std::string& path) = 0;
    virtual void FlushSram() = 0;
    virtual void SetStatePath(const std::string& path) = 0;
    virtual void FreeBuffers() = 0;
    virtual ~Cartridge() = default;  // frees only the two strings, like the base dtor

    uint32_t systemType = 0;  // +0x08  0 GB, 1 GBC (header 0x143 bit 7), 2 GBA, 3 NES
    std::string statePath;    // +0x10  save:/<TITLE>.stt
    std::string title;        // +0x28  header title 0x134..0x13F
};

// 0x00: ROM access masked to 0x7fff, including A000-BFFF reads.  ctor @ 0x710008a3d0
struct RomOnly final : Cartridge {
    RomOnly(uint8_t* rom, int32_t romSize);
    void Write(uint32_t, uint8_t) override {}
    uint8_t Read(uint32_t addr) override;
    void LoadSram(const std::string&) override {}
    void FlushSram() override {}
    void SetStatePath(const std::string&) override {}
    void FreeBuffers() override;

    uint8_t* rom;             // +0x58
    int32_t romSize;          // +0x60
};

// 0x01-0x03. Bank registers live in globals (core.mbc1*).  ctor @ 0x710008a470
struct Mbc1 final : Cartridge {
    Mbc1(uint8_t* rom, int32_t romSize, int32_t ramSize);
    void Write(uint32_t addr, uint8_t value) override;
    uint8_t Read(uint32_t addr) override;
    void LoadSram(const std::string& path) override;
    void FlushSram() override;
    void SetStatePath(const std::string& path) override { mapperStatePath = path; }
    void FreeBuffers() override;

    int32_t romSize;          // +0x58
    int32_t ramSize;          // +0x5c
    std::string sramPath;     // +0x60
    bool sramLoaded = false;  // +0x78  flushes only happen once a load succeeded
    bool sramDirty = false;   // +0x79
    uint8_t* sram;            // +0x80
    std::string mapperStatePath;  // +0x40
    uint8_t* rom;             // +0x1d8
};

// 0x0F-0x13. LoadSram is an empty function in the original, so SRAM is never loaded or flushed.
// ctor @ 0x710008a9a0
struct Mbc3 final : Cartridge {
    Mbc3(uint8_t* rom, int32_t romSize, int32_t ramSize, bool hasRtc);
    void Write(uint32_t addr, uint8_t value) override;
    uint8_t Read(uint32_t addr) override;
    void LoadSram(const std::string&) override {}
    void FlushSram() override;
    void SetStatePath(const std::string& path) override { mapperStatePath = path; }
    void FreeBuffers() override;

    uint8_t* rom;             // +0x40
    int32_t romSize;          // +0x48
    int32_t ramSize;          // +0x4c
    bool sramLoaded = false;  // +0x50  never set
    std::string sramPath;     // +0x58
    std::string mapperStatePath;  // +0x70
    bool sramDirty = false;   // +0x88
    uint8_t* sram;            // +0x90
    bool ramEnable = false;   // +0x98
    int32_t romBank = 1;      // +0x9c
    uint8_t ramSelect = 0;    // +0xa0  0-3 RAM bank, 8-12 RTC register (value % 13)
    bool hasRtc;              // +0xa1
    uint8_t rtc[5] = {};      // +0xa2  seconds, minutes, hours, day low, day high/halt/carry
    uint8_t rtcLatched[5] = {};  // +0xa7
    int64_t rtcLastTime = 0;  // +0xb0
    uint8_t latchPrev = 0;    // +0xb8

private:
    void AdvanceRtc(int64_t now);
    void LatchRtc();          // @ 0x710008ad50
};

// 0x19-0x1E. Bank registers live in globals (core.mbc5*).  ctor @ 0x710008b020
struct Mbc5 final : Cartridge {
    Mbc5(uint8_t* rom, int32_t romSize, int32_t ramSize);
    void Write(uint32_t addr, uint8_t value) override;
    uint8_t Read(uint32_t addr) override;
    void LoadSram(const std::string& path) override;
    void FlushSram() override;
    void SetStatePath(const std::string& path) override { mapperStatePath = path; }
    void FreeBuffers() override;

    std::string mapperStatePath;  // +0x40
    uint8_t* rom;             // +0x58
    int32_t romSize;          // +0x60
    int32_t ramSize;          // +0x64
    bool sramLoaded = false;  // +0x68
    std::string sramPath;     // +0x70
    uint8_t* sram;            // +0x88
};

// .gba -> type 2 (the GBA core loads rom:/game.gba itself), .nes -> type 3 placeholder.
struct SystemOnly final : Cartridge {
    explicit SystemOnly(uint32_t type) { systemType = type; }
    void Write(uint32_t, uint8_t) override {}
    uint8_t Read(uint32_t) override { return 0; }
    void LoadSram(const std::string&) override {}
    void FlushSram() override {}
    void SetStatePath(const std::string&) override {}
    void FreeBuffers() override {}
};

extern Cartridge* current;  // gb_mapper @ 0x712abac020

// The two file helpers the mappers use (@ 0x7100088da0 / 0x7100088fc0).
bool LoadSramFile(uint8_t* buffer, int32_t size, const std::string& path);
void SaveSramFile(const uint8_t* buffer, int32_t size, const std::string& path);

}  // namespace carts
