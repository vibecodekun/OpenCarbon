# Carbon Engine (Limited Run Games) — Shantae GBC (Switch) teardown

Target: `Shantae [0100430013120000][v0][US]` + update `v65536`. All addresses are for `main`
loaded at `0x7100000000` (Ghidra project `ghidra/Carbon.gpr`, program `/main`).

## TL;DR

Carbon is **not** a RetroArch-style frontend. It is a single custom C++ app (`Carbon.nss`, built from
`C:\development\Carbon\Carbon-Gb\NX64\Release\`) that statically contains:

- a menu/skin layer driven by `rom:/gamedef.xml` (tinyxml2, stb_image, desktop OpenGL 4.5 via EGL),
- an in-house **GB/GBC core** (~80 KB of code, all global state, table-driven SM83 interpreter),
- a **GBA core that is libretro VBA-Next** (a GPLv2 VBA-M fork), pinned to a commit between 252f801 (2018-02-17) and 7592321 (2018-06-03). It runs the HLE BIOS and is ~480 KB of code,
- a stub "NES" system type,
- blargg's **Gb_Snd_Emu 0.1.4 + Blip_Buffer** for GB audio.

Credits list "Carbon Emulation Engine" as a single LRG developer. Neither the credits page nor the legal info
mentions VBA-Next/VBA-M, Gb_Snd_Emu or the SameBoy-derived shader.

### Display filters (pause menu, `gb_present_frame` @ 0x7100097520)
| Filter | Shader | Output |
|---|---|---|
| Sharp | plain sprite | 800×720 at x=225 (15 px left of center) |
| LCD | `lcd.fs` | 800×720 at x=225 |
| Native | plain sprite | 640×576 at (320,72), centered |
| GameBoy Color | — | no draw path; **unreachable** because the menu wraps the setting after 2. A cut feature, matching the unused `gbc.fs` |

## Package contents

| Item | Notes |
|---|---|
| `exefs/main` | Carbon itself (text 0x111760, bss 0x2C9ECB60 ≈ 750 MB static arena) |
| `exefs/sdk` | nnSdk |
| `exefs/subsdk0` | NVIDIA GLSLC (runtime GLSL compiler) |
| `exefs/subsdk1` | NVIDIA OpenGL driver |
| `exefs/subsdk2` | NintendoSDK_movie 10.4.1 (plays `wf_logo.mp4`) |
| `romfs/gamedef.xml` | skin + title list; `engine` attr is parsed but the core is picked by file extension/header |
| `romfs/ShantaeB3ae.bin` | original GBC ROM (MBC5+RAM+BATT, 4 MB) |
| `romfs/game2.bin` | "GBA Enhanced": **5-byte patch** of the same ROM (see below) |
| `romfs/assets/shaders/*.fs` | Loaded: `lcd.fs` (SameBoy LCD shader), `sprite.vs/fs`, `fade.fs`, `particles.fs`. **Never loaded:** `gbc.fs` (GBC color matrix), `dot.fs` (libretro dot), `emulation.fs` (also has a syntax error) |
| Update v65536 | **every NSO and RomFS file is byte-identical to v0**; only `main.npdm` changed |

### The "GBA Enhanced" ROM
- `0x138..0x13A`: title `SHANTAE` → `SHANLRG`. Save files are named after the header title, so this gives it its own `.sav`. The header checksum is left invalid, which the emulator doesn't check.
- `0x397B`: `CB 20` (`sla b`) → `06 02` (`ld b,2`). Shantae's boot code stores `(A==$11) | (B<<1)` into `$FFFE`, so this forces "CGB-on-GBA" detection. The emulator itself has no GBA-mode feature for GB games.

## System selection — `cart_load` @ 0x71000880e0
- `.gba` → GBA object (type 2). `.nes`/`.NES` → placeholder object (type 3).
- Anything else → GB (type 0) or GBC (type 1) from header byte `0x143 & 0x80`.
- The ROM is read **one byte at a time** through `std::istream::read`. If the ROM is missing, it prints `Rom could not be open. This probably won't work.` and continues with 32 KB of zeros.
- Mapper chosen by header `0x147`:

| 0x147 | Class | ctor | battery `.sav` loaded? |
|---|---|---|---|
| 00 | RomOnly | 0x710008a3d0 | – |
| 01,02,03 | MBC1 | 0x710008a470 | **no, not even for 03 (MBC1+RAM+BATT)** |
| 0F,10,11,12,13 | MBC3 (RTC flag for 0F/10) | 0x710008a9a0 | **never**: cart_load calls vtable[2] for 10/13, but `MBC3_load_sram` @ 0x710008af80 is an empty function, so the "loaded" flag stays 0 and SRAM is never flushed either |
| 19–1E | MBC5 | 0x710008b020 | 1B, 1E |
| anything else | – | – | returns NULL → crash on first virtual call |

SRAM file helpers (not functions in the Ghidra project): load @ 0x7100088da0, save @ 0x7100088fc0.
- **Missing `.sav`:** SRAM is zeroed and loading counts as successful, so flushes are enabled and the file gets created.
- **`.sav` with the wrong size:** SRAM is zeroed and loading counts as *failed*, which disables every later flush. That game never saves again until the file is removed.
- The file is read and written one byte at a time through iostreams.
- `0x149` indexes the RAM size table @ 0x7100194368 without a bounds check. Entries past 5 are unrelated .rodata.

- Hard-coded game hack: if title == `PM_CRYSTAL`, it patches ROM `0x658C` = `C3 85 63` (`JP $6385`).
- Save paths: `save:/<TITLE>.sav`, states under `save:/<game>/<slot>/<TITLE>…` plus a PNG thumbnail.

## Host loop — `app_main_loop` @ 0x71000e9bb0
State machine (menu = 0, in-game = 4, pause fades = 5/6, …). In state 4 it runs **exactly one emulated
frame per iteration** (`gb_run_frame` or `gba_run_frame`), draws, then `eglSwapBuffers`.
`eglSwapInterval(1)` means the GB runs at the display's 60 Hz instead of 59.7275 Hz (**+0.46% fast**).
The EGL context is desktop OpenGL 4.5 core (`EGL_OPENGL_API`), not GLES.

## GB/GBC core

### Frame loop — `gb_run_frame` @ 0x71000820b0
```
while (!gb_frame_done):
    t  = gb_cpu_step()                 // one instruction
    t += gb_service_interrupts()       // +20 ticks if an IRQ was taken
    lcd_step(t >> double_speed)
    stereo = Gb_Apu::end_frame(t >> double_speed)
    Stereo_Buffer::end_frame(t >> double_speed, stereo)
    n = Stereo_Buffer::read_samples(tmp, 0x800)   // per instruction!
    if frame > 30 and ring_fill + n < 0x2000: append to ring (else drop)
    gb_timer_step(t)                   // timer sees full-speed ticks
frame_counter++
```
Everything, including audio synthesis, runs in lock-step **per CPU instruction**.
The time unit is "ticks" at 4194304 Hz: 4 ticks = 1 M-cycle.

### CPU — `gb_cpu_step` @ 0x710008e7f0, table @ 0x71001b2018
- Registers are a global struct @ 0x710024d030: `F A C B E D L H SP PC`. Ticks @ 0x710024d040.
- Flow: fetch at PC. If halted, add 4 ticks. Otherwise PC++ and call `gb_opcode_table[op]`, and the handler adds its fixed cost.
- 245 unique handlers. **Opcode `00` (NOP) shares the illegal-opcode stub** (`D3 DB DD E3 E4 EB EC ED F4 FC FD`), which prints `not implemented` only if a debug flag is set.
- `CB` @ 0x7100092190 uses a single `switch` over all 256 CB opcodes.
- `STOP` @ 0x710008efb0: in CGB mode it toggles double speed if KEY1 bit 0 is set. In double speed it doesn't skip the padding byte and adds no ticks. There is no real low-power stop.
- `EI` delay goes through `gb_ime_request`/`gb_ei_delay` (0x7100223300..02). HALT bug not emulated.
  - `DI` is delayed the same way. `RETI` @ 0x7100085660 enables interrupts immediately.
- Flag and timing details confirmed by fuzzing every opcode against the original (see "Windows rebuild"):
  - DAA tests `C || adjusted > 0x9F` using the value *after* the +6 step.
  - `POP AF` masks F with 0xF0.
  - `BIT` keeps C and F's low nibble.
  - `SLA` keeps F's low nibble; the other CB shifts and rotates clear it.
  - `STOP` in double-speed mode doesn't advance PC. `RST 38` also zeroes an unused int @ 0x710024d048.
- **Decompiler caveat:** Ghidra silently drops ARM `bfi` bit-field inserts. Its C for the ADC handlers shows no carry flag, although the machine code sets it (C from bit 8). Flag logic must be checked against the disassembly or by execution.

### Interrupts — `gb_service_interrupts` @ 0x71000853e0
Vectors 40/48/50/58/60 are correct and cost 20 ticks. **Priority order is wrong**: it checks
VBlank → **Joypad** → STAT → Serial → **Timer** (hardware: VBlank → STAT → Timer → Serial → Joypad).
*Verified by executing the original function in the harness:* with STAT+Timer+Serial+Joypad pending it vectors to $60 (Joypad), and with Timer+Serial pending it vectors to $58 (Serial).
Any pending enabled IRQ clears HALT, even when IME=0.

### Memory map — `gb_read8` @ 0x710008d630 / `gb_write8` @ 0x710008d4c0
Dispatches on `addr >> 12`:
- 0000–7FFF and A000–BFFF → `gb_mapper` vtable (`[0]` write, `[1]` read).
- 8000–9FFF → VRAM. C000–FDFF and FF70 → WRAM with CGB banking.
- FE00–FEFF → OAM @ 0x710024bef0. FF80–FFFE → HRAM @ 0x710024bf90.
- FF0F → IF @ 0x710024d060, FFFF → IE @ 0x710024d061. FF00 → joypad.
- FF04–07 → timer. FF10–3F → `Gb_Apu`. FF40–6F → video.

### Timer — `gb_timer_step` @ 0x71000948e0
DIV increments every 256 ticks. TIMA periods are `{1024, 16, 64, 256}` ticks. Overflow reloads TMA immediately
(no 1-cycle delay) and sets `IF |= 0xE4`.

### LCD — `lcd_step` @ 0x71000873b0
- Scanline state machine: mode 2 = 80 ticks, mode 3 = **fixed** 172 ticks, HBlank = 204 ticks (456 per line). 144 visible + 10 VBlank lines = 70224 ticks per frame.
- LY 152 wraps to 0 while still in VBlank (approximates the line-153 quirk). VBlank sets `gb_frame_done`.
- STAT IRQs are level-less (no STAT blocking).
- The whole line renders at HBlank entry via `gb_render_scanline` @ 0x7100085800.
- HDMA copies 16 bytes per HBlank. The source is read through the mapper when `< 0xC000`, which means a VRAM source goes to the cart, not VRAM.
- `gb_init` hard-sets `gb_is_cgb = 1` (its only writer), so the DMG render paths are dead code here.
- With LCDC bit 0 clear, the BG layer is **not drawn at all**, even in CGB mode (hardware: it only changes priority).
- Framebuffer @ 0x7100223303 is 160×144 RGBA8888. CGB RGB555 is expanded by bit replication; color correction exists only in `gbc.fs`.
- Writing FF44 resets LY to 0. OAM DMA (FF46) and GDMA copy instantly.

### Audio — blargg Gb_Snd_Emu 0.1.4 + Blip_Buffer
- `GbApu_write_register` @ 0x7100083890, `GbApu_read_register` @ 0x7100084160, `GbApu_end_frame` @ 0x7100083830. Stereo_Buffer @ 0x7100222000.
- These match 0.1.4 almost line for line: the `other_synth.offset(time, ±30, …)` volume-change trick, `output_select = (bits>>3&2)|(bits&1)`, wave RAM split into nibbles, `end_frame` returning `stereo_found`.
- Setup in `gb_init` @ 0x7100081f60: 48000 Hz, 250 ms buffer, clock rate 4194304.
- Output: `audio_thread_main` @ 0x7100080b40 (core 1) uses `nn::audio::AudioOut` 48 kHz stereo s16. Two 8192-byte buffers are recycled.
  - On each release it copies `min(ring_fill, 2048)` frames. The rest of the buffer is **not cleared on underrun**, so stale audio replays.
  - The ring holds 8192 frames and starts "full" (silence cushion). The producer drops samples when full, which acts as crude rate matching for the 60 Hz vs 59.73 Hz mismatch.
  - The frame loop reads from Stereo_Buffer only once ≥1024 samples are buffered, and appends to the ring only after frame 30.
  - The fill check compares the fill (in frames) against the read count (in samples).
  - **The two AudioOut buffers are never cleared, which causes the pop at game start.** Each `memalign(0x1000, 0x2000)` is followed by a `memset` of the *previous*, leaked allocation. Both buffers are appended right after `StartAudioOut`, before the first `SetAudioOutVolume(0.0)`. They're freed when the thread ends, and the allocator can hand them back on the next launch. Details under "Start-up pop" below.
  - Game audio fades in by +0.01 volume per released buffer, about 4.3 s from silence.
- **The GB audio code is unmodified Gb_Snd_Emu 0.1.4 / Blip_Buffer 0.3.4.** *Verified by execution:* the upstream sources, compiled for Windows, reproduce Carbon's `read_samples` output sample for sample over 2400 frames of Shantae.
- **Pause-menu RESET GAME silences game audio until the next launch.**
  - `GbSystem_Reset` @ 0x7100082280 re-runs the `Gb_Apu` constructor in place and never calls `output()` again.
  - The constructor clears all oscillator output pointers.
  - `gb_init` and `GbSystem_Shutdown` run the constructor too, but `gb_init` reconnects the outputs.
  - *Verified by execution:* after the reset, 600 frames of Shantae produce only silence in both the original and the port.
- There is a **second, unused APU implementation** @ 0x71000800f0..0x7100080b40 (state @ 0x710021f2c0..0x710021f3d5).
  - `gb_init` and reset initialise it (@ 0x71000804a0), and reset clears its 0x4000-byte buffer @ 0x710021b2c0.
  - No register write or output path ever reaches it.

### Start-up pop (measured on Ryujinx with the audio trace, `tools/ryujinx_trace`)
Traces: `testresults/ryujinx_audio/20260917_122613` (boot → menu → SHANTAE, then exit and relaunch five times; GB sessions 4–9) and `20260917_122728` (artwork gallery first; session 4).
- **Order of events** (every launch):
  1. `Start` (volume 1.0).
  2. Append buffer A. Ryujinx's SDL2 session opens its output stream inside that call (~9 ms), and the first callback mixes A at **128/128**.
  3. Append B, then `SetVolume(0)` and `SetVolume(0.01)`. These arrive 0.2–3 ms after A was already mixed.
  4. A is released at once, refilled from the ring and appended, then `SetVolume(0.02)`.
  5. Each later callback, ~42.7 ms apart, takes the next buffer at the volume current then and releases it. Buffer k ≥ 1 plays at 0.01·(k+1), i.e. SDL 2, 3, 5, 6, 7 … /128.
  6. The guest's wait clears the buffer event, so there's one fade step per release.
- **What A and B contain** is whatever that heap memory held. Their first two buffers per launch:

| Launch (session) | A | B |
|---|---|---|
| 1 (s4) | fresh: free-list header `0x0ca4c000, 1`, then a page of `0x01` (peak 16384) | fresh, peak 32640 |
| 2 (s5) | fresh, peak 8320 | fresh, peak 32688 |
| 3 (s6) | fresh, high-entropy bytes, peak 32768 | fresh, zeros |
| 4 (s7) | reuses s6's B (silence) | fresh, peak 20496 |
| 5 (s8) | reuses s7's B | fresh, peak 16384 |
| 6 (s9) | reuses s8's A | fresh, peak 2356 |
| 1 after artwork (run 2) | fresh, different pointer data, peak 32726 | fresh, peak 26160 |

  Every reused block held exactly what that buffer had at the end of the earlier session. So relaunching after music would replay its last 43 ms at full volume.
- **Hardware caveat:** on a Switch the loudness of A depends on whether the audio server mixes before `SetVolume(0)` arrives. That can't be measured without a console. The rebuild follows Ryujinx.
- **Rebuild:**
  - `platform/AudioOut` reproduces the measured session semantics: the first append is mixed at once, later buffers are taken per period at the current volume and released when taken, and waiting clears the event.
  - `gb/Audio.cpp` runs the original thread code on it. The blocks come from a heap stand-in that replays the table above (captures exported to `gb/AudioHeapCapture.inc` by `export_startup_buffers.py`). Launches after the sixth repeat launch 6's pattern with silent fresh blocks.
  - *Verified:* the rebuild's first buffer matches Ryujinx's final output byte for byte on all six launches, and the second buffer equals B × 0.02f. Ryujinx then rounds that to SDL's 2/128, which isn't ported. `audio_out_test` checks the volume schedule for buffers 0–5.

### Behaviour confirmed by running the original code (Unicorn harness, see Tooling)
- **DMG-only ROMs render solid black.**
  - `gb_init` forces CGB mode and the video reset zeroes both CGB palette RAMs. There is no boot ROM to install a DMG compatibility palette.
  - Example: mooneye `acceptance/instr/daa.gb` (header 0x143=00) passes, but every framebuffer pixel is (0,0,0).
  - CGB-aware ROMs render normally. blargg `cpu_instrs` 03 matches the dev's Windows screenshot pixel for pixel. The dev's grayscale Super Mario Land video must come from a build with the DMG path active.
- **With the LCD off, a frame never ends.**
  - `gb_run_frame` loops until `lcd_step` sets `gb_frame_done` at LY=144. With LCDC bit 7 clear, `lcd_step` just resets LY/mode.
  - While a game keeps the LCD off, the host loop (vsync, input polling, OS notifications) is stalled and emulation runs unthrottled.
- **HLE boot state:** A=11 F=80 BC=0000 DE=FF56 HL=000D SP=FFFE PC=0100 (`FUN_710008e780`). The DMG I/O defaults routine `FUN_710008d230` returns immediately because CGB mode is forced first.
- Shantae (`ShantaeB3ae.bin`) boots to the WayForward/Bozon copyright screen and then a gray "LIMITEDRUNGAMES" screen. The shipped ROM is LRG's revision, not the 2002 Capcom-published original.
- Input (`Input_Update` @ 0x710009fe60): Npad buttons plus both analog sticks as digital directions (±32 threshold). `gb_key_down/up` @ 0x71000856c0/0x7100085730. Holding the 0x70 button combo opens the pause menu.

### MBC5 (Shantae) — vtable @ 0x71001b1fb0
- `[0]` write @ 0x710008b0e0, `[1]` read @ 0x710008b1f0, `[2]` load_sram(path) @ 0x710008b280, `[3]` flush_sram @ 0x710008b320, `[4]` set_state_path @ 0x710008b3a0, `[5]` free ROM/SRAM buffers @ 0x710008b0b0 (misnamed `MBC5_save_sram` in the early label files).
- Bank, RAM-enable and dirty state are **globals** @ 0x710024bee4..e9, not object members.
- **SRAM is written to disk only when the game disables cart RAM** (write ≠ 0x0A to 0000–1FFF while dirty, via vtable `[3]` flush). *Correction:* vtable `[5]` (called on exit-to-menu and on the HOME-close request) only frees the ROM/SRAM buffers, so nothing is flushed on exit. Unsaved SRAM changes are lost if the game hasn't disabled RAM yet.
- **Bug:** the 3000–3FFF write stores `(v&1)<<1` as the bank high byte, so bank 256+ maps to **+512**, and ROM reads are never masked against ROM size (out-of-bounds read on >4 MB MBC5 ROMs). Harmless for Shantae (max bank 255). *Verified by execution:* on a synthetic 8 MB MBC5 ROM, bank 256 reads tag 8112 and bank 261 reads 0, i.e. garbage from past the end of the ROM buffer.
- The rumble bit isn't separated from the RAM bank number.

### Save states — write @ 0x710008d850, load @ 0x710008e210
The file is 0x1A2AD bytes:

| Offset | Size | Contents |
|---|---|---|
| 0x0000 | 0x32 | timestamp string |
| 0x0032 | 0x0C | CPU registers |
| 0x003E | 0x98 | video register block @ 0x71001b1bda |
| 0x00D6 | 2 | IF, IE |
| 0x00D8 | 0x8000 | WRAM |
| 0x80D8 | 0x4000 | VRAM |
| 0xC0D8 | 0x7F | HRAM |
| 0xC157 | 0x8000 | renderer buffer @ 0x710023fba3 |
| 0x14157 | 0x6000 | renderer buffer @ 0x7100239b03 |
| 0x1A157 | 0xA0 | sprite cache |
| 0x1A1F7 | 0xA0 | OAM |
| 0x1A297 | 4 | DIV, TAC, TIMA, TMA (@ 0x710024d054) |
| 0x1A29B | 0x0C | MBC1 bank globals @ 0x710024bed8..bee3 (*correction:* not "LCD accumulators") |
| 0x1A2A7 | 6 | MBC5 bank globals @ 0x710024bee4..bee9 |

- Paths: `save:/<game>/<slot>/<TITLE>.stt` and `.prv` (a 160×144 PNG of the framebuffer). `GbSystem_LoadSlotPreviews` reads the first 0x32 bytes of each `.stt` for the slot timestamp.
- A new state file is created 0x20000 bytes long, and 0x1A2AD bytes are written at offset 0.
- WRAM sits directly after IE at 0x710024d062.
- MBC3's bank registers are object members, so they aren't in the state.

**Not saved:** APU state, cartridge SRAM, IME/EI-delay/HALT, double-speed flag, DIV/TIMA sub-tick counters.

**Loading keeps the sound running.** `GbSystem_LoadState` @ 0x7100082230 clears the audio ring, reads the file, then calls `GbApu_apply_stereo` @ 0x71000834a0.
- *Correction:* this function was labelled `Gb_Apu::reset`, and the port reset the APU on load. That zeroes every register including NR52, so the game's next NR51 write disconnected all four channels and a loaded game stayed silent. The user noticed the difference on Ryujinx, where audio carries on after a load.
- `GbApu_apply_stereo` isn't in Gb_Snd_Emu 0.1.4. It resembles `apply_stereo` from the later Gb_Apu: from osc 3 down to 0, the output is re-derived from NR51 alone, with no NR52 power mask.
- If an oscillator's output changes while `last_amp != 0`, the original zeroes `last_amp` and clears that **whole** old Blip_Buffer (`Blip_Buffer::clear(true)` @ 0x7100082360) instead of offsetting by −last_amp.
- Because the registers aren't in the state, the channels keep their pre-load notes until the game's sound driver (whose state is in WRAM) writes new ones.
- *Verified by execution:* `gbdiff.py frames ShantaeB3ae.bin --frames 1900 --save-at 1300 --load-at 1600`. The original's `GbSystem_LoadState` and the port's run on the same state file, and state blocks and audio samples stay identical through frame 1900, with the title music still playing (peak 13677).

## GBA core = VBA-M (identified, version not yet pinned)
Evidence:
- **`CPUIsGBABios`** @ 0x7100003000: accepts `.gba .agb .bin .bios .rom` in that order via `strcasecmp`, the same list as VBA-M's function.
- **`armInsnTable`/`thumbInsnTable`** @ 0x71001a72d0 (4096 + 1024 entries, 709 unique handlers in 0x7100006b60–0x7100078590). The dispatch in `armExecute` @ ~0x7100006440 indexes `((op>>16)&0xFF0)|((op>>4)&0xF)`.
- After each instruction it runs VBA-M's `codeTicksAccessSeq32` logic (ROM region 8–D, `busPrefetchCount & 1`, `> 0xFF`), then `cpuTotalTicks += clockTicks` vs `cpuNextEvent`.
- **`myROM`** (VBA-M's replacement BIOS) @ 0x71001a7018, directly before the instruction tables.

Startup `FUN_710007ea90` mirrors VBA-M's SDL frontend:
- `CPULoadRom("rom:/game.gba")` @ 0x7100000a90. The path is hard-coded, not taken from gamedef.
- `flashSetSize(0x10000)`, optional `rtcEnable` @ 0x710007fab0, `doMirroring` @ 0x7100000be0.
- A 0x9600-byte buffer (240×160×4 framebuffer?) and an emulation thread @ 0x710007e800 (core 1).
- **`CPUInit(NULL, false)`** @ 0x7100002a40 means no BIOS file (HLE BIOS). Then `CPUReset` @ 0x71000030d0.
- State path `save:/<title>.stt`.

Other:
- The ~3–5 KB zero-caller functions at 0x7100051f50–0x710006abd0 are most likely VBA-M's per-mode scanline renderers.
- `gba_run_frame` @ 0x710007ec80, `gba_present_frame` @ 0x71000977f0.
- Implication for the port: build this part from matching upstream VBA-M source and diff for LRG changes, rather than decompiling 480 KB.

### Pinned: libretro **VBA-Next** @ 252f801 (2018-02-17) … 7592321^ (2018-06-03)
Confirmed against `extern/vba-next` history:
- **`bus_t`** in src/gba.h is `{ reg[45], busPrefetch, busPrefetchEnable, busPrefetchCount, armNextPC }`, matching the layout at 0x71001d4158. The core is one file with a single `static int clockTicks`.
- The 114-entry `saveGameStruct` @ 0x71001b1320 matches VBA-Next's table entry for entry.
- **Lower bound, has `252f801` (2018-02-17):** `thumb47` (BX) @ 0x71000732f0 stores `clockTicks = CLOCKTICKS_UPDATE_TYPE32P` on the ARM-switch path. Before the fix the value was discarded.
- **Upper bound, lacks `7592321` (2018-06-03, OP_RSB):**
  - RSBS @ 0x7100013980 computes carry/overflow with the same operand roles as SUBS @ 0x710000fd20 (buggy pre-fix flags).
  - Also lacks `6d7b472` (same day): no `rm += 4` for Rm=PC in register shifts (AND/ADD handlers @ 0x7100006e50, 0x7100015960).
  - Also lacks `5243b2d` (2019-09-24): THUMB POP/STMIA/LDMIA end with `clockTicks = …`.
- The only core change inside the window is `2412114` (2018-05-10, one `if (x1 >= sizeX) goto skipLine;` in affine BG rendering), not yet resolved.
- `CPUIsGBABios` still existed in VBA-Next at that time (removed 2026-05). The sound save table `gba_state` is absent, most likely linker-stripped, so Carbon's GBA states would not include sound.
- Known consequence: Carbon inherits pre-fix VBA-Next bugs (RSBS carry/overflow flags, PC-as-Rm register shifts, THUMB block-transfer timing).

### Earlier VBA-M comparison (superseded by the VBA-Next pin)
- `myROM` (173 words) matches every 2.1.x tag byte for byte. `CPUIsGBABios` matches v2.1.4 line for line.
- **THUMB timing predates VBA-M commit 6f2e320d (2020-07-08).** `thumbBC`/`thumbC0`/`thumbC8` @ 0x7100075600/0x7100076210/0x7100076810 end with `clockTicks = N + wait`, not `+=`.
- **Global layout doesn't match upstream VBA-M.**
  - Carbon has one shared `clockTicks` @ 0x71001d4130, read by the ARM loop and written by THUMB handlers. Upstream kept separate `static int clockTicks` in GBA-arm.cpp and GBA-thumb.cpp from 2009 to 2024.
  - Carbon lays out `reg[45]` @ 0x71001d4158, then `busPrefetch`/`busPrefetchEnable` @ …420c, `busPrefetchCount` @ …4210 and `armNextPC` @ …4214 as one contiguous block. In v2.1.4, `reg[45]` is followed by `map[256]`, and `armNextPC`/`busPrefetchCount` are defined in other places or files.
  - This points to a single-TU VBA-M fork with a `bus` struct, most likely libretro **VBA-Next** (unconfirmed, source not fetched yet).
- Carbon rewrote `utilLoad` @ 0x710007ede0 (plain fopen, no zip/gz; stores the 12-byte title from 0xA0 for save naming) and drops e-Reader support.
- GBA audio: Carbon's own thread @ 0x710007e800. AudioOut 48 kHz stereo, 0x3000-byte buffers, up to 2400 frames copied per release from a malloc(0x9600) ring.

A retail GBA BIOS (16384 B, MD5 a860e8c0b6d573d191e4ec7db1b1e4f6) is in `gba_bios/`.
Test ROMs: `testroms/blargg` (retrio/gb-test-roms) and `testroms/mooneye` (gekkio.fi build mts-20260714-0944-31510e1). Carbon doesn't use one, but it's useful as a reference and for testing real-BIOS mode.

## App / menu layer (decompiled to C++ in `carbon-pc/src/app` + `carbon-pc/src/gfx`)
Every function below has a C++ counterpart annotated with the original address. The code type-checks under MSVC (`cl /Zs /W4`); it hasn't been linked or run yet.

### Frameworks it is built on
- **LearnOpenGL "Breakout" 2D framework:** `Shader`, `Texture2D`, `ResourceManager` (std::map by name) and `SpriteRenderer`.
  - Carbon adds a vec4 colour, a model matrix T·Rz·S (rotation about the top-left corner), `CreateEmptyTexture` (uploads an uninitialised buffer) and a per-frame `Generate(..., linear)`.
- **TBO text renderer:** fonts are `rom:/assets/fonts/<name>.tga` (uncompressed 24-bit atlas) + `.bin` (raw `FileHeader{texw,texh,ascent,descent,linegap,norm...,GlyphInfo[256]}`, 0x3820 bytes).
  - Glyph quads are pushed to a texture buffer and expanded from `gl_VertexID` by the two embedded `#version 140` shaders.
  - There is **one global glyph table**, so every renderer uses the last font loaded.
  - Text coordinates are bottom-up; sprite coordinates are top-down on a 1280×720 canvas.
- Menu audio: `nn::audio` AudioRenderer (32 kHz, 160-sample frames, 6-ch final mix "MainAudioOut").
- Controller input: `nns` sample-framework style.

### App states (`App_Run` @ 0x71000e9bb0, `App_UpdateInput` @ 0x71000ea3e0)
| # | State | Notes |
|---|---|---|
| 0 | Main menu | titles…, EXTRAS, CREDITS. **Hard-coded for two titles:** selection 0/1 launch, 2 opens Extras, 3 opens Credits |
| 2 | Credits | offline web applet with `creditsMenu/splashData@document` (details below) |
| 3 | Artwork gallery | 16 images; L/R page, right-stick-X zoom (±stick/8, width 600–2200), left-stick pan, hold A hides the UI; draws and swaps on its own |
| 4 | In game | one emulated frame per vsync |
| 5 | Pause | Resume / Load / Save / Reset / Filter / Exit. Game tint fades to 0.2. Menu slides via `pauseSlideX` (−24/frame to −720) |
| 6, 13 | Two extra submenus | unreachable from the visible menu; 6 jumps to {9, 10, 12, 11}; 13 cycles option0, filter, `gbaBackground` |
| 9 | Exit to menu | GB: shutdown + free cart. GBA: write `save:/<title>.stt` |
| 10 | Reset | GB: CPU + APU only; GBA: `CPUReset` |
| 14 | Launch transition | sprite slides out, fade to black, then `cart_load` + `gb_init` |
| 15 | Return transition | never entered |

### Credits (web applet, `ShowCredits` @ 0x71000eb800)
- `ShowOfflineHtmlPage("credits.htdocs/sw.html")` runs with these arguments, read from the disassembly (the decompiler loses them):
  - background kind 1;
  - footer, pointer, boot loading icon and touch on contents **off**;
  - JS extension and web audio **on**.
- The call blocks the app until the applet closes.
- The page (HtmlDocument NCA) is a Star Wars-style crawl:
  - It animates `top` from −10 px to −14000 px over 52 s, in Bubblegum Sans / Nunito Bold with a red `-webkit-text-stroke` on the headings.
  - `<meta http-equiv="Refresh" content="52;url=http://localhost/">` navigates to the applet's callback URL after 52 s, which closes the applet.
  - `sw.js` calls `nx.footer.unsetAssign` for B and X and swallows keydown, so **the credits can't be skipped**: they always run the full 52 s.

### Input
- `Input_Update` @ 0x710009fe60 builds the Carbon button mask from `nn::hid` (see `src/app/Input.h`):
  - d-pad 0x1/0x2/0x4/0x8; ZL→0x50, ZR→0x30, L→0x40, R→0x20; A/B/X/Y → 0x100/0x200/0x400/0x800; Plus → 0x1000; **Minus unmapped**.
  - Left stick → 0x100000–0x800000, right stick → 0x10000–0x80000.
  - Stick bits are computed from the **previous** frame's stick values (one-frame lag).
- **GB mapping:** Switch **B → GB A**, Switch **A and Y → GB B**, X → Select, + → Start.
  - Up/Down/Right follow the held state; Left only reacts on the press edge.
  - ZL+ZR (all of 0x70 held) opens the pause menu. Holding L/ZL sets `GbSystem+4`, which nothing reads.
- **GBA mapping:** A→A, B→B, +→Start, L→L, R→R; **Select unmapped**. Any of L/ZL/ZR (0x50) opens the pause menu, so the GBA L button can't be used in games.

### Menus and presentation (`Renderer` @ App+0x1d0)
- Splash: only **Splash1** is shown, for 6 s of wall-clock time. Splash2 is loaded but never drawn. The WayForward logo is the `wf_logo.mp4` movie played during EGL init (next section).
- Main menu layout:
  - MenuBackground full screen, drawn twice (plain, then through the `emulation`/particles shader with a `time` uniform).
  - MenuSprite slides from x=1800 → 320 at 36 px/frame, then fades in (+0.015).
  - GameLogo (30,30) 367.16×190.95.
  - **B_Button** hint (30,660), gated by `Button_B@enabled`; A_Button is loaded but never drawn.
  - DescriptionBar (−20,620).
  - Title list x=90/140 from y=460, 40 px steps; preview (62,404) inside GameBorder.
- Pause menu:
  - Option text at y=400…100 (x 180/220).
  - Save slots (700, 150/300/450) with thumbnails `save:/<game>/<slot>/<TITLE>.prv`.
  - **The selected slot box blinks:** hue-table bytes are used without /255.
  - **Slot text stays invisible until the fade is exactly 1.0:** `(int)` of each fade component.
  - Slot 0's small text is flushed one frame late.
  - The confirm-save preview re-uploads the live GB framebuffer.
- In-game:
  - For the first 600 frames a "pause menu" hint shows at (1080,660), fading over the last 60.
  - **Nothing is drawn for the first 60 frames.**
  - Filters: Sharp (800×720 at x=225), LCD (same, `lcd.fs`), Native (640×576 centred). GameBoy Color is unreachable.
- GBA presenter uploads the 256×160 frame but only draws `GameBackground`; the GBA frame itself is never drawn.

### Startup movie (`MoviePlayer_Play` @ 0x710009bf50)
`Graphics_InitEGL` first plays `rom:/assets/splash/wf_logo.mp4`, before it creates Carbon's own vi layer and EGL context.
- **The file:** 1920×1080 H.264 High at 30 fps (122 frames), stereo AAC-LC at 48 kHz (192 frames), 4.09 s, muxed by Lavf54.59.107. It has no VUI colour description.
- **The player:** the NintendoSDK movie sample player on NintendoSDK_movie 10.4.1 (subsdk2: Android stagefright MPEG4Extractor plus NVIDIA decoders). It has an NVN video renderer and an AudioOut audio renderer, sharing a media clock. If `nn::fs::OpenFile` fails, nothing is played.
- **Nothing reads input, so the movie can't be skipped.**
- **Startup:**
  - A wait for 5 s of buffered data ends at once for a file, because `NuMediaExtractor::getBufferedRange` returns BAD_VALUE unless the source is caching.
  - It then waits up to 5 s on an event, then up to 1 s (200 × 5 ms) until 6 audio and 4 video frames are decoded.
- **Timing:**
  - The video renderer anchors the media clock to its first frame, so that frame shows at once. Every audio buffer re-anchors the clock to the audio position, and later frames are shown when the clock reaches their timestamps.
  - A frame ≥ 40001 µs late is dropped (`MovieVideoRenderer_CheckFrameTime` @ 0x7100105a10).
- **Edit lists are ignored.** The extractor's `elst` handler reads the track's duration and sample rate, but they are parsed later (`edts` precedes `mdia`), so it sets no encoder delay.
  - Video timestamps start at 66.7 ms (the B-frame offset the edit list would remove).
  - The 1600 AAC priming frames (33 ms) are played.
- **End:** `MoviePlayer_Play` checks every 100 ms whether both renderers reached end of stream, then `MoviePlayer_Destroy` @ 0x710009d640 tears down the player and its layer. The screen is black until Carbon's first swap, which comes after the rest of `Renderer::Init`, the artwork loading and `App::Init`.
- **Drawing** (`MovieNvnRenderer_*` @ 0x7100106b50, 0x7100108270, 0x7100107e10):
  - A 1280×720 RGBA8 NVN window is cleared to (0.5, 0.5, 0.5, 1).
  - A triangle-strip quad is scaled horizontally by (video aspect ÷ 16:9), which is full screen here.
  - NV12 is copied into R8 + RG8 textures (display area only), then the fragment shader applies `(yuv − src_bias) * src_xform` with a table picked by the decoder's `nv12-colorspace` (tables @ 0x710019589c): 0/4 BT.601 limited, 1 BT.601 full, 2 BT.709 limited, 3 BT.709 full.
- **Sampler: bilinear, no mip, clamp to edge.** *Verified by execution* (`tools/movie/nvn_sampler.py`).
  - The renderer calls only `nvnSamplerBuilderSetDefaults`, `SetWrapMode(7,7,7)` and `nvnSamplerInitialize`. The only reference to `SetMinMagFilter` in main is the pointer loader storing it.
  - The NVN driver lives in the `sdk` module. `nvnDeviceGetProcAddress` (sdk @ 0x45fb00) binary-searches 516 names, and the tool resolves the functions through that same table.
  - `SetDefaults` (sdk @ 0x4679a0) writes 1 to min and mag filter. The descriptor builder (sdk @ 0x485ab0, used by `nvnSamplerPoolRegisterSampler`) maps them as mag = `(v & 1) + 1`, min = `(v & 1) + 1`, mip = `((v >> 1) & 3) + 1`.
  - Running that code gives descriptor words `0x00026092 0x00000262`. Decoded with Ryujinx's `SamplerDescriptor` rules, that is mag LINEAR, min LINEAR, mip NONE and address mode CLAMP_TO_EDGE. So NVN's filter value 1 means linear.
- **Colour space: BT.601 limited range** (`nv12-colorspace` 0). Traced through every writer in subsdk2 (symbols from its .dynsym, `tools/nsosyms.py`):
  1. `movie::convertMessageToMediaData`+0x378 sets `nv12-colorspace` = `hw-buf-ColorSpace ror 5` if that is 1–4, otherwise 0. When the key is absent, main's local stays 0.
  2. `hw-buf-ColorSpace` is set only by `ACodec::sendFormatChange`, from `SfNvnUtil::getNativeBufColorSpace` = `(fmt & ~0xFF) == 0x100 ? fmt & 0xE0 : 0`, where `fmt` is `SfNvnUtil+0x358`.
  3. +0x358 is written by the `SfNvnUtil` constructor and `setParameter`, with the OMX output port's `eColorFormat`. The only 0x1xx formats in the module are Nintendo's 0x106/0x111 and NVIDIA's 0x106–0x10F mapping, none with bits in 0xE0.
  4. `SfNvnUtil::setColorAspect` ORs in 0x20/0x40/0x60/0x80. Its only caller, `ACodec::allocateOutputMetaDataBuffers`+0x2d0, skips it when `color-range`, `color-standard` and `color-transfer` are all -1.
  5. `ACodec::configureCodec` fills those three from the configure format, or -1 when absent. The format is Carbon's track configuration. `movie::convertMetaDataToMediaData` adds those keys only from MetaData `cRng`/`cPrm`/`tFun`/`cMtx`, and the only code that sets those MetaData keys is `MatroskaExtractor::getColorInformation` (plus an AMessage→MetaData converter). The MP4 extractor never does, and `wf_logo.mp4` has no `colr` box anyway.
- **Audio:** AudioOut at full volume.

### Audio
- Menu music: `sndMenu.wav` loops at 0.5. It fades out (−0.01 per 5 ms) on launch and fades **in to 0.7** on returning, so it comes back louder than it started.
- SFX: sndchange (back), sndselect (select), menu_scrub_through_items3 (move, 0.6), menu_toggle_yes_no3 (toggle).
  - Only the left source channel is routed, to the centre output.
  - A sound only restarts once its previous playback has ended.
- Loading a save state clears the emulator audio ring and re-applies the APU's stereo routing; the APU itself keeps playing (see Save states). Pause clears the ring every frame.

### Save/exit behaviour
- Pause → Load loads immediately, with no confirmation. Save goes through CONFIRM SAVE. Both always call the **GB** save-state functions, even for GBA games.
- HOME-menu close with a GB game writes a hidden **slot 4** state (`save:/0/4/<TITLE>.stt`) that is never loaded, then frees the cart without flushing SRAM.
  - `App_Run` then still calls `App_UpdateInput`, which reads the now-NULL `gb_mapper` while in game (NULL+8).
  - The Switch terminates the process once `LeaveExitRequestHandlingSection` returns, so the crash never shows.
- `gb_init` resets the CPU (writing LCDC = 0x91), then runs the video reset, which zeroes the whole video block. The LCD therefore starts **off**, and the game has to enable it.
- WRAM, HRAM, OAM, VRAM, the timer, IF/IE and the WRAM bank are *not* cleared between launches, so the second title starts with the first title's leftovers.
- GBA: `save:/<title>.stt` is written on exit and read on launch (auto-resume).

### Assets (verified against gamedef.xml + every `rom:/` string in the binary)
- **Referenced but missing:** `assets/ingameTopMenu.png` (gamedef `TopPauseMenu`; the load fails silently and the texture is never drawn), `game.gba` (hard-coded GBA path).
- **Never referenced:**
  - Shaders: `gbc.fs`, `dot.fs`, `emulation.fs`.
  - Sounds: `MenuMusic.wav` (18 MB), `menu_scrub_through_items.wav`, `menu_scrub_through_items2.wav`, `menu_toggle_yes_no.wav`.
  - UI images: `a_button.png`, `b_button.png`, `menuDialog.png`, `border.png`.
  - Fonts: `HarmattanBold_24.h`, `Jewels_28/32/48` (.bin/.tga).
  - Artwork: `BonusArtwork_P01–16.png` (the non-`2_` set), `artwork0–6.png`, `artworkBG.png`, `artworkBG1.png`, `artworkUI.png`, `artworkUI-Smal2l.png`, `Untitled-1.png`.
- Loaded but never shown: `splash/splash2.png`.

### NES "placeholder"
0x7100094bf0–0x7100097120 (~10 KB) is a partial NES core: CPU bus and mapper write handlers, plus a Blip_Buffer-based APU. Only reachable through `.nes` files.

## Test-ROM results for the ORIGINAL Carbon GB core
Run through `tools/harness/run_suite.py` against the unmodified ARM64 code. Per-ROM details and final-frame PNGs are in `testresults/carbon_original/`. Overall: **36 PASS / 85 FAIL / 12 TIMEOUT / 29 HANG / 7 CRASH** out of 169.

| Suite | Pass | Fail | Timeout | Hang | Crash | Interpretation |
|---|---|---|---|---|---|---|
| blargg cpu_instrs (11 + combined) | 12 | 0 | 0 | 0 | 0 | Instruction semantics are solid |
| blargg instr_timing | 0 | 1 | | | | Per-instruction tick accounting fails the timer-based check |
| blargg interrupt_time | 1 | 0 | | | | |
| blargg mem_timing / mem_timing-2 | 0 | 7 | 1 | | | No sub-instruction memory access timing |
| blargg halt_bug | 0 | 1 | | | | HALT bug not emulated |
| blargg oam_bug | 2 | 7 | | | | OAM corruption bug not emulated |
| blargg dmg_sound | 1 | 6 | 6 | | | Gb_Snd_Emu 0.1.4 lacks most hardware quirks |
| blargg cgb_sound | 0 | 8 | 5 | | | same |
| mooneye acceptance (all subdirs) | 11 | 35 | 0 | 28 | 0 | Timing-accurate tests. 28 hang with the LCD off (frame never ends) |
| mooneye mbc1 | 7 | 6 | | | | Mode-1 banking drops upper bits; RAM tests fail |
| mooneye mbc2 | 0 | 0 | | | 7 | No MBC2 mapper; cart_load returns NULL |
| mooneye mbc5 | 2 | 6 | | | | Bank number not masked to ROM size: only 32/64 Mbit pass. Tests never select bank ≥256 (0x3000 written as 0xFE) |
| mooneye misc | 0 | 7 | | 1 | | CGB-specific checks |

MBC1 detail (`MBC1_read` @ 0x710008a7b0): upper 2 bits are used only in mode 0. In mode 1 the switchable area uses just the low 5 bits, and 0000–3FFF is always bank 0.

## Tooling / how to reproduce

### Running the original code: `tools/harness`
- `carbon_emu.py` loads `extracted/base/nso_flat/main.bin` into **Unicorn** (ARM64) at 0x7100000000:
  - applies RELATIVE/GLOB_DAT/JUMP_SLOT relocations and runs `.init_array`;
  - maps the 750 MB `.bss` lazily;
  - links every import to a Python stub (unimplemented ones stop with the symbol name);
  - replaces the local malloc family.
- `carbon_gb.py <rom> [--frames N] [--png out.png]` builds the cartridge object the way `cart_load` does, then calls the original `gb_init` and `gb_run_frame`.
  - Observers: `gb_write8` for blargg serial output (Carbon itself ignores FF01/FF02) and `op_40` (LD B,B) for mooneye results.
- `run_suite.py [--workers N] [--filter s]` runs blargg + mooneye in parallel and writes `testresults/<name>/results.tsv` plus a final-frame PNG per ROM.
- Speed is roughly 7–20 emulated frames/s per process.
- `tools/nspx` — LibHac 0.19 extractor: `dotnet tools/nspx/bin/Release/net8.0/nspx.dll <prod.keys> <title.keys> <outdir> <base.nsp> [update.nsp]`. Set `NSPX_MANUAL=<dir>` to also dump Manual NCAs. Use Windows-style paths.
- `tools/nso2bin.py` — flattens NSOs to `extracted/base/nso_flat/*.bin`. `tools/relocs.py` — parses MOD0/.rela.dyn and finds pointer tables (writes `ghidra/relocs.json`).
- `tools/disasm.py <addr> [n]` (capstone), `tools/immscan.py <imm…>`, `tools/strs.py`, `tools/sm83names.py`.
- `tools/nsosyms.py <module.bin> sym|at|ptr|plt|vtable …` resolves symbols, containing functions, relocated pointer slots, PLT stubs and vtables in the other modules (sdk, subsdk1/2 export full .dynsym tables).
- `tools/ryujinx_trace/` is an audio trace for the Ryujinx 1.1.1403 source in `Ryujinx-1.1.1403/`.
  - `apply_patch.py` adds the hooks and `AudioTrace.cs` is the trace class. Build with `dotnet build src/Ryujinx -c Release -r win-x64`.
  - Each run writes `testresults/ryujinx_audio/<time>/`: an event log, the guest buffers as appended, a memory window around each session's first four buffers, and the backend's final mixed output.
  - `analyze.py` lists sessions and converts the dumps to WAV. `export_startup_buffers.py` exports buffer captures for the rebuild.
- `tools/movie/nvn_sampler.py` runs the NVN driver's sampler code from `sdk` in Unicorn and decodes the GPU descriptor. `tools/movie/cmp_frames.py <shots>` checks `movie_NNN.png` captures against BT.601/BT.709 × nearest/linear renderings of an ffmpeg decode.
- Ghidra-Switch-Loader is built with plain `javac` against Ghidra 12.1.2 jars (the only JDK is 25, which its Gradle 8.10 can't run). Build output is in `tools/switchloader/ext`, installed to `%APPDATA%\ghidra\ghidra_12.1.2_PUBLIC\Extensions`.
- Label, then export:
  `analyzeHeadless C:\opencarbon\ghidra Carbon -process main -noanalysis -scriptPath C:\opencarbon\tools\ghidra_scripts -preScript ApplyLabels.java ghidra/labels_core.tsv ghidra/labels_opcodes.tsv ghidra/labels_mappers.tsv -postScript ExportAll.java C:/opencarbon/ghidra/export`
  This produces `ghidra/export/all.c` (every function decompiled), `functions.tsv` and `strings_xrefs.tsv`.

## Windows rebuild (`carbon-pc`)
A native Windows build of Carbon that behaves like the Switch binary, bugs included. Status as of 2026-09-17:

| Part | Source | Status |
|---|---|---|
| App/menu layer (`src/app`, `src/gfx`) | decompiled | runs; menus, artwork, credits, pause menu, filters, save/load states verified by scripted runs |
| Offline web applet (credits) | WebView2 (Chromium) | the page plays inside the window with the Switch arguments (no input, blocks, closes on the `http://localhost/` callback after 52.6 s) |
| Startup movie (`wf_logo.mp4`) | Media Foundation + the original shaders and sampler state (`MoviePlayer_win32.cpp`) | unskippable; 122 frames presented, 0 dropped. `tools/movie/cmp_frames.py`: captures match an ffmpeg decode rendered with BT.601 limited + bilinear within ±1 (nearest is off by up to 91, BT.709 by up to 10). Audio samples match ffmpeg (±1) apart from the 1600 priming frames, which are kept as on the Switch |
| Win32 platform (`src/platform/win32`) | new | WGL 4.5 core into a 1280×720 canvas, 60 Hz pacing on any monitor, keyboard + XInput, WASAPI |
| GB/GBC core + mappers (`src/gb`) | decompiled | **identical to the original** (below) |
| GB audio | Gb_Snd_Emu 0.1.4, unmodified | identical samples |
| GB audio output | original thread code on `platform/AudioOut` (Ryujinx-measured AudioOut timing) + replayed heap captures | start-up pop identical to the Ryujinx trace on six launches |
| GBA core | VBA-Next @ `252f801` | not integrated yet (`src/stub/GbaStubs.cpp`) |
| DMG renderers (`dmg_render_*`) | – | not ported: unreachable because `gb_init` forces CGB mode |

Deliberate differences from the Switch build:
- **Exit request:** `App::Run` returns right after the exit-request close instead of dereferencing the NULL cartridge.
- **Missing images:** the texture loader zero-initialises width and height when an image fails to load.
- **Allocations:** the ring buffer is mutex-protected, and ROM/SRAM buffers are zeroed.
- **Startup movie:** it plays after the window and GL context exist (the Switch plays it on its own layer first). The movie's presents don't count as `--script` frames. With `--script`, every 30th movie frame is captured as `movie_NNN.png`. `--skip-movie` leaves it out.

### Verification (`tools/gbdiff`)
The core's state lives in packed blocks laid out like the original globals: io @ 0x71001b1bd8, core @ 0x7100223300, cpu @ 0x710024d030. `gbdiff.dll` exposes them, and `gbdiff.py` runs the original ARM64 functions in Unicorn and the port on byte-identical inputs, then compares every byte.

| Test | Cases | Result |
|---|---|---|
| `gbdiff.py cpu`: every opcode from random states through `gb_cpu_step` | 153,000 | identical |
| `gbdiff.py cb`: every CB opcode | 102,400 | identical |
| `irq`, `timer` | 10,000 each | identical |
| `mem`: `gb_read8`/`gb_write8` with RomOnly, MBC1, MBC3, MBC5 | 160,000 | identical |
| `lcd`: `lcd_step` including scanline rendering, sprites, window, HDMA | 163,000 | identical |
| `frames`: Shantae, 2400 frames with button input; GBA Enhanced ROM, 600 frames | – | state blocks and audio samples identical every frame |
| `frames --save-at 1300 --load-at 1600`: the port writes a state, and both sides load it through `GbSystem_LoadState` (the original's file read is replaced by the same table) | 1900 frames | identical, music continues after the load |
| `run_suite_port.py`: all 169 blargg/mooneye ROMs | – | status, result, frame count and final-frame PNG identical to `testresults/carbon_original` (the port runs the suite in 8 s) |

Bugs this found in the port during development: the decompiled `SLA`/`BIT` flags, a 4-byte field at 0x710024d048, and the BG colour-index cache @ 0x710024bba4 being `int[160]` rather than bytes.

Commands:
- Build: `cmake -S carbon-pc -B carbon-pc/build -G "Visual Studio 18 2026" -A x64`, then `cmake --build carbon-pc/build --config Release`.
- Run: `carbon.exe --romfs extracted/base/romfs --save <dir> --html extracted/manual/<nca>/html-document`.
- Scripted checks: `carbon.exe ... --script carbon-pc/tests/<script>.txt --shots <dir> --hidden --mute --audio-dump <dir>`.
- Differential tests: `python tools/gbdiff/gbdiff.py <cpu|cb|irq|timer|mem|lcd|frames rom>` and `python tools/gbdiff/run_suite_port.py`.
