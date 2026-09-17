# opencarbon

A reverse-engineering study of Limited Run Games' **Carbon** emulation engine, as shipped in the
Nintendo Switch release of *Shantae* (GBC), and a native Windows rebuild of it (`carbon-pc`).

The full write-up (addresses, the GB/GBC core, save-state format, the startup pop, the GBA core
identification and test-ROM results) is in [FINDINGS.md](FINDINGS.md).

## Status

| Part | Status |
|---|---|
| App/menu layer, credits web applet, startup movie, pause menu, save states | working in `carbon.exe` |
| GB/GBC core (`carbon-pc/src/gb`) | proven identical to the original with `tools/gbdiff` (opcode/memory/LCD/timer/IRQ fuzzing, frame-by-frame state and audio, all 169 blargg/mooneye ROMs) |
| GBA core (libretro VBA-Next @ `252f801`) | **not integrated yet** (`carbon-pc/src/stub/GbaStubs.cpp`) |

## Third-party sources included

`carbon-pc/third_party/` holds unmodified upstream sources, byte-identical to the originals
(`.gitattributes` turns off line-ending conversion there). Each keeps its own license.

| Path | What it is | License | Used by Carbon |
|---|---|---|---|
| `Gb_Snd_Emu-0.1.4` | blargg's Gb_Snd_Emu 0.1.4 + Blip_Buffer 0.3.4, from http://www.slack.net/~ant/libs/ | LGPL 2.1 | yes, unmodified (verified by execution, see FINDINGS.md) |
| `vba-next` | libretro VBA-Next @ `252f801` (2018-02-17), from https://github.com/libretro/vba-next | GPL 2 | yes, pinned to 252f801 … 7592321^; not built into carbon-pc yet |
| `tinyxml2` | tinyxml2 11.0.0 | zlib | yes (version not pinned) |
| `stb` | `stb_image.h` 2.30, `stb_image_write.h` 1.16 | MIT / public domain | `stb_image` yes (version not pinned) |
| `glad` | glad 2.0.8 loader: `--api='gl:core=4.5' --extensions='GL_EXT_direct_state_access' c --loader` | see file headers | no, rebuild only |

Once VBA-Next is linked in, a distributed `carbon.exe` falls under the GPL 2.

## What is not in this repository

No game data, keys, firmware or BIOS are included. You need your own copy of the game.

| Local path | What it is | Where it comes from |
|---|---|---|
| `extracted/` | the game's ExeFS/RomFS and manual | your own dump, extracted with `tools/nspx` |
| `extern/webview2/pkg` | NuGet `Microsoft.Web.WebView2` 1.0.4191.47, unpacked | https://www.nuget.org/packages/Microsoft.Web.WebView2 |
| `extern/vba-next` | full VBA-Next clone, used to pin the version | https://github.com/libretro/vba-next |
| `testroms/blargg` | blargg's GB test ROMs | https://github.com/retrio/gb-test-roms @ `c240dd7` |
| `testroms/mooneye` | Mooneye Test Suite, build `mts-20260714-0944-31510e1` | https://gekkio.fi/files/mooneye-test-suite/ |
| `Ryujinx-1.1.1403/` | Ryujinx source, patched by `tools/ryujinx_trace/apply_patch.py` | Ryujinx 1.1.1403 |
| `Ghidra-Switch-Loader/` | Ghidra loader for Switch binaries | https://github.com/Adubbz/Ghidra-Switch-Loader |
| `ghidra/Carbon.rep`, `ghidra/export/*.c` | Ghidra database and decompiler output | regenerate with the commands in FINDINGS.md |

Many scripts in `tools/` have `C:\opencarbon` paths hard-coded, so clone to that location.

## Building and running (Windows)

Requires Visual Studio 2026 (C++), CMake 3.25+, and the WebView2 SDK unpacked to `extern/webview2/pkg`.

```
cmake -S carbon-pc -B carbon-pc/build -G "Visual Studio 18 2026" -A x64
cmake --build carbon-pc/build --config Release
carbon.exe --romfs extracted\base\romfs --save <dir> --html extracted\manual\<nca>\html-document
```

Options: `--skip-movie`, `--gl-debug`, and for headless checks
`--script carbon-pc/tests/<script>.txt --shots <dir> --hidden --mute --audio-dump <dir>`.

The Python tools need `unicorn`, `capstone`, `numpy` and `Pillow`. Differential tests:

```
python tools/gbdiff/gbdiff.py <cpu|cb|irq|timer|mem|lcd|frames rom>
python tools/gbdiff/run_suite_port.py
```
