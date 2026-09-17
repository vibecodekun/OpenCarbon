"""Drive Carbon's original GB/GBC core (unmodified ARM64 code) on a ROM.

usage: carbon_gb.py <rom> [--frames N] [--png out.png] [--mooneye] [--quiet]
Prints serial output (blargg), BG tile-map text, mooneye result and frame timing.
"""
import argparse, struct, sys, time, os
sys.path.insert(0, os.path.dirname(__file__))
from carbon_emu import CarbonEmu
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1

GB_WRITE8 = 0x710008D4C0
GB_INIT = 0x7100081F60
GB_RUN_FRAME = 0x71000820B0
OP_40_LD_B_B = None          # resolved from the opcode table at runtime
OPCODE_TABLE = 0x71001B2018
GB_MAPPER = 0x712ABAC020
GB_CPU = 0x710024D030        # F A C B E D L H SP PC
FRAMEBUFFER = 0x7100223303
VRAM = 0x7100247BA4          # bank-combined VRAM block; tile map at +0x1800
JOYPAD = 0x71001B1BD8
RAM_SIZES = [0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000]
CTOR_ROMONLY, CTOR_MBC1, CTOR_MBC3, CTOR_MBC5 = 0x710008A3D0, 0x710008A470, 0x710008A9A0, 0x710008B020


class CarbonGB:
    def __init__(self, rom_path):
        self.emu = e = CarbonEmu()
        self.serial = bytearray()
        self.mooneye = None
        e.watch(GB_WRITE8, self._on_write8)
        e.watch(struct.unpack('<Q', e.read(OPCODE_TABLE + 0x40 * 8, 8))[0], self._on_ld_b_b)
        for fn, err in e.run_init_array():
            if err and 'StandardAllocator' not in err:
                print(f'[init_array] {fn:#x}: {err}', file=sys.stderr)
        rom = open(rom_path, 'rb').read()
        self.rom = rom
        rom_ptr = e.alloc(len(rom))
        e.write(rom_ptr, rom)
        ctype, ramcode = rom[0x147], rom[0x149]
        ram = RAM_SIZES[ramcode] if ramcode < len(RAM_SIZES) else 0
        n = len(rom)
        # mirrors the switch in cart_load @ 0x71000880e0
        if ctype == 0x00:
            obj = e.alloc(0x68); e.call(CTOR_ROMONLY, obj, rom_ptr, n)
        elif ctype == 0x01:
            obj = e.alloc(0x1E0); e.call(CTOR_MBC1, obj, rom_ptr, n, 0)
        elif ctype in (0x02, 0x03):
            obj = e.alloc(0x1E0); e.call(CTOR_MBC1, obj, rom_ptr, n, ram)
        elif ctype in (0x0F, 0x11):
            obj = e.alloc(0xC0); e.call(CTOR_MBC3, obj, rom_ptr, n, 0, 1 if ctype == 0x0F else 0)
        elif ctype in (0x10, 0x12, 0x13):
            obj = e.alloc(0xC0); e.call(CTOR_MBC3, obj, rom_ptr, n, ram, 1 if ctype == 0x10 else 0)
        elif ctype in (0x19, 0x1C):
            obj = e.alloc(0x90); e.call(CTOR_MBC5, obj, rom_ptr, n, 0)
        elif ctype in (0x1A, 0x1B, 0x1D, 0x1E):
            obj = e.alloc(0x90); e.call(CTOR_MBC5, obj, rom_ptr, n, ram)
        else:
            raise SystemExit(f'cart type {ctype:#04x}: Carbon has no mapper for this (cart_load would return NULL and crash)')
        vt = e.u64(obj)
        cgb_flag = e.call(e.u64(vt + 8), obj, 0x143)
        e.put32(obj + 8, (cgb_flag >> 7) & 1)
        e.put64(GB_MAPPER, obj)
        self.frame_ctr = e.alloc(8)
        e.call(GB_INIT, self.frame_ctr)
        self.frames = 0

    def _on_write8(self, e):
        addr, val = e.uc.reg_read(UC_ARM64_REG_X0) & 0xFFFF, e.uc.reg_read(UC_ARM64_REG_X1) & 0xFF
        if addr == 0xFF01:
            self._sb = val
        elif addr == 0xFF02 and val == 0x81:
            self.serial.append(getattr(self, '_sb', 0))

    def _on_ld_b_b(self, e):
        if self.mooneye is None:
            reg = e.read(GB_CPU, 12)
            self.mooneye = dict(B=reg[3], C=reg[2], D=reg[5], E=reg[4], H=reg[7], L=reg[6])
            e.request_stop()

    def regs(self):
        f, a, c, b, e_, d, l, h, sp, pc = struct.unpack('<8B2H', self.emu.read(GB_CPU, 12))
        return dict(A=a, F=f, B=b, C=c, D=d, E=e_, H=h, L=l, SP=sp, PC=pc)

    def run_frames(self, n, frame_timeout_s=20):
        """Run up to n frames. Stops early if a hook requested it (e.g. mooneye LD B,B).
        Raises EmuTimeout if gb_run_frame never reaches VBlank (e.g. the ROM left the LCD off)."""
        for _ in range(n):
            self.emu.call(GB_RUN_FRAME, self.frame_ctr, timeout_us=int(frame_timeout_s * 1e6))
            if self.emu.stop_requested:
                return
            self.frames += 1

    def lcd_on(self):
        return bool(self.emu.u8(0x71001B1BDD) & 0x80)

    def tilemap_text(self):
        """Read the 20x18 visible BG tile map as ASCII (blargg's console uses tile index = char code)."""
        e = self.emu
        lcdc = e.u8(0x71001B1BDD)
        base = 0x1C00 if lcdc & 0x08 else 0x1800
        tiles = e.read(VRAM + base, 32 * 18)
        rows = []
        for y in range(18):
            rows.append(''.join(chr(t) if 0x20 <= t < 0x7F else ' ' for t in tiles[y * 32:y * 32 + 20]).rstrip())
        return '\n'.join(rows).strip('\n')

    def save_png(self, path):
        from PIL import Image
        data = self.emu.read(FRAMEBUFFER, 160 * 144 * 4)
        Image.frombytes('RGBA', (160, 144), data).resize((480, 432), Image.NEAREST).save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom')
    ap.add_argument('--frames', type=int, default=600)
    ap.add_argument('--png')
    ap.add_argument('--until-serial', help='stop early once serial output contains this text')
    args = ap.parse_args()
    gb = CarbonGB(args.rom)
    t0 = time.time()
    step = 30
    while gb.frames < args.frames:
        gb.run_frames(min(step, args.frames - gb.frames))
        s = gb.serial.decode('latin-1')
        if gb.mooneye is not None:
            break
        if args.until_serial and any(k in s for k in args.until_serial.split('|')):
            break
    dt = time.time() - t0
    print(f'frames={gb.frames} time={dt:.1f}s ({gb.frames / dt:.1f} fps)')
    print('--- serial ---'); print(gb.serial.decode('latin-1').strip())
    print('--- tilemap ---'); print(gb.tilemap_text())
    if gb.mooneye is not None:
        m = gb.mooneye
        ok = (m['B'], m['C'], m['D'], m['E'], m['H'], m['L']) == (3, 5, 8, 13, 21, 34)
        print('--- mooneye ---', 'PASS' if ok else 'FAIL', m)
    print('--- cpu ---', gb.regs())
    if gb.emu.log:
        print('--- printf ---', gb.emu.log[:10])
    if args.png:
        gb.save_png(args.png)


if __name__ == '__main__':
    main()
