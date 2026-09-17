// TEMPORARY stand-ins for src/gba (VBA-Next + Carbon glue), so the app layer links before the GBA core is
// integrated. Shantae is a GBC game; nothing here runs unless a .gba title is launched.
#include "app/EmuGlue.h"

namespace gba {
namespace {
uint8_t framebuffer[256 * 160 * 4];
}

void LoadRom(uint32_t*) {}
void RunFrame(uint32_t*) {}
void Exit() {}
void ClearAudioRing() {}
void Reset() {}
const uint8_t* Framebuffer() { return framebuffer; }

}  // namespace gba
