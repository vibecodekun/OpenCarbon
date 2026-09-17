// Internal interface of the decompiled GB/GBC core. Names follow the Ghidra labels; definitions note the
// original address. The app-facing API is app/EmuGlue.h.
#pragma once
#include "gb/State.h"
#include <cstdint>

class Gb_Apu;
class Stereo_Buffer;
struct GbSystem;

namespace gb {

// ---- memory map (Memory.cpp)
uint8_t Read8(uint32_t addr);                    // gb_read8   @ 0x710008d630
void Write8(uint32_t addr, uint8_t value);       // gb_write8  @ 0x710008d4c0
uint16_t Read16(uint32_t addr);                  // gb_read16  @ 0x710008d7f0
void Write16(uint32_t addr, uint32_t value);     // gb_write16 @ 0x710008d820
uint32_t WramRead(uint32_t addr);                // wram_read  @ 0x7100094b00 (0xFFFFFFFF when unmapped)
void WramWrite(uint32_t addr, uint8_t value);    // wram_write @ 0x7100094a90
uint8_t TimerRead(uint16_t addr);                // timer_read @ 0x7100094a20
void TimerWrite(uint16_t addr, uint8_t value);   // timer_write @ 0x71000949b0
void TimerStep(int32_t ticks);                   // gb_timer_step @ 0x71000948e0
void DmgIoDefaults();                            // @ 0x710008d230 (returns at once in CGB mode)

// ---- frame loop (System.cpp)
void RunInstruction(GbSystem* sys);              // one iteration of gb_run_frame's loop

// ---- CPU (Cpu.cpp)
int32_t CpuStep();                               // gb_cpu_step @ 0x710008e7f0
int32_t ServiceInterrupts();                     // gb_service_interrupts @ 0x71000853e0
void CpuReset();                                 // @ 0x710008e780: HLE post-boot register state

// ---- video (Video.cpp)
uint8_t VramRead(uint32_t addr);                 // vram_read  @ 0x71000871f0 (8000-9FFF, FE00-FE9F)
void VramWrite(uint32_t addr, uint8_t value);    // vram_write @ 0x7100087260
uint8_t VideoIoRead(uint16_t addr);              // video_io_read  @ 0x7100087700
void VideoIoWrite(uint16_t addr, uint8_t value); // video_io_write @ 0x71000878c0
void LcdStep(int32_t ticks);                     // lcd_step @ 0x71000873b0
void RenderScanline();                           // gb_render_scanline @ 0x7100085800
void VideoReset();                               // @ 0x7100085750

// ---- audio (Audio.cpp): blargg Gb_Snd_Emu 0.1.4 objects and the output ring
Gb_Apu& Apu();                                   // gb_apu @ 0x710024c010
Stereo_Buffer& StereoBuf();                      // gb_stereo_buffer @ 0x7100222000
extern int32_t apuTime;                          // gb_apu_time @ 0x7100222118
uint8_t ApuRead(uint16_t addr);                  // GbApu_read_register @ 0x7100084160
void ApuWrite(uint16_t addr, uint8_t value);     // GbApu_write_register @ 0x7100083890
void ApuConstruct();                             // GbApu_ctor @ 0x7100083430 (in place, keeps nothing)
void ApuApplyStereo();                           // GbApu_apply_stereo @ 0x71000834a0 (outputs from NR51)
void ApuConnectOutputs();                        // GbApu_output(center, left, right)
void AudioSetup();                               // Stereo_Buffer 48000 Hz / 250 ms, clock 4194304
void AudioEndInstruction(int32_t ticks);         // GbApu_end_frame + StereoBuffer_end_frame
void AudioPumpFrameSamples(int32_t frameCount);  // the read_samples + ring append inside gb_run_frame
void AudioRingClear();                           // @ 0x71000822a0
void AudioThreadStart();                         // audio_thread_main @ 0x7100080b40
void AudioThreadStop();

// test hooks (tools/gbdiff)
extern bool audioThreadEnabled;
extern void (*writeHook)(uint32_t addr, uint8_t value);  // every Write8 call
extern void (*sampleCaptureHook)(const int16_t* samples, int32_t count);
bool StateBlockInfo(int index, uint32_t* fileOffset, const uint8_t** data, uint32_t* size);  // save-state table

}  // namespace gb
