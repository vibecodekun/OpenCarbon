"""Run blargg + mooneye test ROMs through the C++ port (gbdiff.dll) with the same rules as
tools/harness/run_suite.py, then compare against the original core's results.

usage: run_suite_port.py [--workers N] [--filter substr] [--out dir] [--reference dir]
Writes <out>/results.tsv and a PNG of the final frame per ROM, then prints every ROM whose status, result detail
or final frame differs from the reference run (testresults/carbon_original by default).
"""
import argparse, ctypes, glob, os, sys, time
from multiprocessing import Pool

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = r'C:\opencarbon\testroms'
SKIP_DIRS = ('mooneye/utils', 'mooneye/manual-only', 'mooneye/madness')
MOONEYE_PASS = (3, 5, 8, 13, 21, 34)
LONG_ROMS = ('cpu_instrs.gb', 'dmg_sound.gb', 'cgb_sound.gb', 'oam_bug.gb', 'mem_timing.gb')
# The harness gives each emulated frame 20 s of Unicorn time (about 5M instructions) before calling it a hang.
FRAME_BUDGET = 5_000_000


def relname(path):
    return os.path.relpath(path, ROOT).replace(os.sep, '/')


def summarize(text):
    return ' | '.join(l.strip() for l in text.splitlines() if l.strip())[:300]


class PortGB:
    def __init__(self, rom_path):
        from gbdiff import Port, cart_params
        self.port = p = Port()
        d = p.dll
        d.gbd_run_frame_test.restype = ctypes.c_int32
        d.gbd_take_serial.restype = ctypes.c_int32
        rom = open(rom_path, 'rb').read()
        kind, ram, rtc = cart_params(rom)  # raises SystemExit for mappers Carbon lacks
        size = 0x1000000 if kind in (3, 5) else len(rom) + 0x8000
        buf = rom + bytes(size - len(rom))
        d.gbd_cart_create(kind, buf, size, len(rom), ram, rtc)
        d.gbd_init()
        self.frames = 0
        self.mooneye = None

    def run_frames(self, n):
        """Like CarbonGB.run_frames: stops early at LD B,B; returns 'hang' if a frame never reaches VBlank."""
        regs = (ctypes.c_uint8 * 6)()
        for _ in range(n):
            r = self.port.dll.gbd_run_frame_test(FRAME_BUDGET, 1 if self.mooneye is None else 0, regs)
            if r == 2:
                self.mooneye = dict(zip('BCDEHL', bytes(regs)))
                return None
            if r == 0:
                return 'hang'
            self.frames += 1
        return None

    def serial(self):
        buf = ctypes.create_string_buffer(1 << 16)
        n = self.port.dll.gbd_take_serial(buf, 1 << 16)
        return buf.raw[:n].decode('latin-1')

    def io(self):
        return self.port.read_block(0)

    def lcd_on(self):
        return bool(self.io()[5] & 0x80)

    def pc(self):
        return int.from_bytes(self.port.read_block(2)[10:12], 'little')

    def tilemap_text(self):
        vram = self.port.read_block(1)[0x248a4:0x248a4 + 0x4000]
        base = 0x1C00 if self.io()[5] & 0x08 else 0x1800
        rows = []
        for y in range(18):
            tiles = vram[base + y * 32:base + y * 32 + 20]
            rows.append(''.join(chr(t) if 0x20 <= t < 0x7F else ' ' for t in tiles).rstrip())
        return '\n'.join(rows).strip('\n')

    def save_png(self, path):
        from PIL import Image
        fb = self.port.read_block(1)[3:3 + 160 * 144 * 4]
        Image.frombytes('RGBA', (160, 144), fb).resize((480, 432), Image.NEAREST).save(path)


def run_one(job):
    rom, out_dir = job
    rel = relname(rom)
    is_mooneye = rel.startswith('mooneye/')
    max_frames = 1200 if is_mooneye else (4200 if rel.endswith(LONG_ROMS) else 2400)
    t0 = time.time()
    result = dict(rom=rel, status='?', frames=0, secs=0.0, detail='')
    try:
        try:
            gb = PortGB(rom)
        except SystemExit as e:
            result.update(status='CRASH', detail=str(e))
            return result
        while gb.frames < max_frames:
            if gb.run_frames(30) == 'hang':
                if gb.mooneye is None:
                    result.update(status='HANG', detail=f"{'LCD off' if not gb.lcd_on() else 'LCD on'}: PC={gb.pc():04x}")
                    break
            if is_mooneye:
                if gb.mooneye is not None:
                    m = gb.mooneye
                    got = (m['B'], m['C'], m['D'], m['E'], m['H'], m['L'])
                    result.update(status='PASS' if got == MOONEYE_PASS else 'FAIL',
                                  detail=' '.join(f'{k}={v:02x}' for k, v in m.items()))
                    break
            else:
                text = gb.serial() + '\n' + gb.tilemap_text()
                if 'Passed' in text or 'Failed' in text or 'failed' in text:
                    ok = 'Failed' not in text and 'failed' not in text
                    result.update(status='PASS' if ok else 'FAIL', detail=summarize(text))
                    break
        if result['status'] == '?':
            text = '' if is_mooneye else gb.serial() + '\n' + gb.tilemap_text()
            result.update(status='TIMEOUT', detail=summarize(text) or f'PC={gb.pc():04x}')
        result['frames'] = gb.frames
        gb.save_png(os.path.join(out_dir, rel.replace('/', '__') + '.png'))
    except Exception as e:
        result.update(status='CRASH', detail=f'{type(e).__name__}: {e}'[:300])
    result['secs'] = round(time.time() - t0, 1)
    return result


def load_results(path):
    rows = {}
    with open(path, encoding='utf-8') as f:
        next(f)
        for line in f:
            rom, status, frames, secs, detail = line.rstrip('\n').split('\t', 4)
            rows[rom] = dict(status=status, frames=int(frames), detail=detail)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--workers', type=int, default=12)
    ap.add_argument('--filter', default='')
    ap.add_argument('--out', default=r'C:\opencarbon\testresults\carbon_port')
    ap.add_argument('--reference', default=r'C:\opencarbon\testresults\carbon_original')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    roms = sorted(glob.glob(os.path.join(ROOT, '**', '*.gb'), recursive=True))
    roms = [r for r in roms if not relname(r).startswith(SKIP_DIRS) and args.filter in relname(r)]
    print(f'{len(roms)} ROMs, {args.workers} workers', flush=True)
    results = []
    t0 = time.time()
    with Pool(args.workers, maxtasksperchild=1) as pool:
        for r in pool.imap_unordered(run_one, [(rom, args.out) for rom in roms]):
            results.append(r)
    results.sort(key=lambda r: r['rom'])
    with open(os.path.join(args.out, 'results.tsv'), 'w', encoding='utf-8') as f:
        f.write('rom\tstatus\tframes\tsecs\tdetail\n')
        for r in results:
            f.write(f"{r['rom']}\t{r['status']}\t{r['frames']}\t{r['secs']}\t{r['detail']}\n")
    counts = {}
    for r in results:
        counts[r['status']] = counts.get(r['status'], 0) + 1
    print(f'port: {counts} in {time.time() - t0:.0f}s')

    ref = load_results(os.path.join(args.reference, 'results.tsv'))
    ref_counts = {}
    for rom, r in ref.items():
        if args.filter in rom:
            ref_counts[r['status']] = ref_counts.get(r['status'], 0) + 1
    print(f'original: {ref_counts}')
    from PIL import Image, ImageChops
    differences = 0
    for r in results:
        o = ref.get(r['rom'])
        notes = []
        if o is None:
            notes.append('not in reference run')
        else:
            if o['status'] != r['status']:
                notes.append(f"status {o['status']} -> {r['status']}")
            elif r['status'] in ('PASS', 'FAIL', 'TIMEOUT') and o['detail'] != r['detail']:
                notes.append(f"detail differs:\n      orig: {o['detail'][:150]}\n      port: {r['detail'][:150]}")
            if r['status'] == o['status'] and r['status'] != 'HANG' and o['frames'] != r['frames']:
                notes.append(f"frames {o['frames']} -> {r['frames']}")
            png = r['rom'].replace('/', '__') + '.png'
            a, b = os.path.join(args.reference, png), os.path.join(args.out, png)
            if os.path.exists(a) and os.path.exists(b) and r['status'] != 'HANG':
                if ImageChops.difference(Image.open(a).convert('RGBA'), Image.open(b).convert('RGBA')).getbbox():
                    notes.append('final frame differs')
        if notes:
            differences += 1
            print(f"  {r['rom']}: " + '; '.join(notes))
    print(f'{len(results) - differences}/{len(results)} ROMs identical to the original core (status, result, frame count, final frame)')


if __name__ == '__main__':
    main()
