// Interface between the app layer and the emulator cores. Implemented by src/gb (decompiled GB/GBC core)
// and src/gba (VBA-Next + Carbon glue). Names follow the Ghidra labels.
#pragma once
#include <cstdint>
#include <string>

struct SaveSlotInfo {               // 0x68 bytes
    int32_t empty = 1;              // +0x00  1 = "Empty Slot N"
    std::string title;              // +0x08  "Slot N" / "Empty Slot N"
    std::string unused20;           // +0x20
    std::string timestamp;          // +0x38  "%04d-%02d-%02d %02d:%02d" read from the .stt header
    std::string previewPath;        // +0x50  save:/<game>/<slot>/<TITLE>.prv
};

struct GbSystem {                   // 0x140 bytes @ 0x712abac010 (ctor @ 0x7100080ea0)
    int32_t frameCount = 0;         // +0x00 incremented by gb_run_frame
    int32_t lShoulderHeld = 0;      // +0x04 set while L/ZL is held in-game (never read by the GB core)
    SaveSlotInfo slots[3];          // +0x08, +0x70, +0xd8
};

namespace carts {
// Cartridge object from cart_load (@ 0x71000880e0). systemType: 0 GB, 1 GBC, 2 GBA, 3 NES.
struct Cartridge;
extern Cartridge* current;                      // gb_mapper @ 0x712abac020
Cartridge* Load(const std::string& romPath);
uint32_t SystemType(const Cartridge* c);        // field +0x08
const std::string& Title(const Cartridge* c);   // field +0x28 (header title, used for save paths)
void FreeBuffers(Cartridge* c);                 // vtable[5] (+0x28)
void Destroy(Cartridge* c);                     // base dtor @ 0x7100087db0 + delete
}  // namespace carts

namespace gb {
void Init(GbSystem* sys);                        // gb_init @ 0x7100081f60
void RunFrame(GbSystem* sys);                    // gb_run_frame @ 0x71000820b0
void KeyDown(int key);                           // @ 0x71000856c0  0=R 1=L 2=U 3=D 4=A 5=B 6=Select 7=Start
void KeyUp(int key);                             // @ 0x7100085730
void SaveState(GbSystem* sys, int slot, int game);   // GbSystem_SaveState @ 0x7100082220 -> gb_save_state
void LoadState(GbSystem* sys, int slot, int game);   // GbSystem_LoadState @ 0x7100082230
void Reset();                                    // GbSystem_Reset @ 0x7100082280 (CPU + APU only)
void Shutdown();                                 // GbSystem_Shutdown @ 0x7100082060
void ClearAudioRing();                           // @ 0x71000822a0
void LoadSlotPreviews(GbSystem* sys, const std::string& title, int game);  // @ 0x7100080ef0
const uint8_t* Framebuffer();                    // gb_framebuffer_rgba @ 0x7100223303, 160x144 RGBA
}  // namespace gb

namespace gba {
void LoadRom(uint32_t* sys);                     // GbaSystem_LoadRom @ 0x710007ea90
void RunFrame(uint32_t* sys);                    // gba_run_frame @ 0x710007ec80
void Exit();                                     // @ 0x710007ebe0: write save:/<title>.stt, CPUCleanUp, stop audio
void ClearAudioRing();                           // @ 0x710007ec50
void Reset();                                    // VBA-Next CPUReset @ 0x71000030d0
const uint8_t* Framebuffer();                    // @ 0x71001c80e8, 256x160 RGBA
}  // namespace gba
