"""Run blargg + mooneye test ROMs through Carbon's original GB core in parallel.

usage: run_suite.py [--workers N] [--filter substr] [--out dir]
Writes <out>/results.tsv and a PNG of the final frame per ROM.
"""
import argparse, os, sys, time, glob
from multiprocessing import Pool
sys.path.insert(0, os.path.dirname(__file__))

ROOT = r'C:\opencarbon\testroms'
SKIP_DIRS = ('mooneye/utils', 'mooneye/manual-only', 'mooneye/madness')
MOONEYE_PASS = (3, 5, 8, 13, 21, 34)
LONG_ROMS = ('cpu_instrs.gb', 'dmg_sound.gb', 'cgb_sound.gb', 'oam_bug.gb', 'mem_timing.gb')


def relname(path):
    return os.path.relpath(path, ROOT).replace(os.sep, '/')


def summarize(text):
    return ' | '.join(l.strip() for l in text.splitlines() if l.strip())[:300]


def run_one(job):
    rom, out_dir = job
    rel = relname(rom)
    is_mooneye = rel.startswith('mooneye/')
    max_frames = 1200 if is_mooneye else (4200 if rel.endswith(LONG_ROMS) else 2400)
    t0 = time.time()
    result = dict(rom=rel, status='?', frames=0, secs=0.0, detail='')
    try:
        from carbon_gb import CarbonGB
        from carbon_emu import EmuTimeout
        try:
            gb = CarbonGB(rom)
        except SystemExit as e:
            result.update(status='CRASH', detail=str(e))
            return result
        while gb.frames < max_frames:
            try:
                gb.run_frames(30)
            except EmuTimeout as t:
                if gb.mooneye is None:
                    result.update(status='HANG', detail=f"{'LCD off' if not gb.lcd_on() else 'LCD on'}: {t} PC={gb.regs()['PC']:04x}")
                    break
            if is_mooneye:
                if gb.mooneye is not None:
                    m = gb.mooneye
                    got = (m['B'], m['C'], m['D'], m['E'], m['H'], m['L'])
                    result.update(status='PASS' if got == MOONEYE_PASS else 'FAIL',
                                  detail=' '.join(f'{k}={v:02x}' for k, v in m.items()))
                    break
            else:
                text = gb.serial.decode('latin-1') + '\n' + gb.tilemap_text()
                if 'Passed' in text or 'Failed' in text or 'failed' in text:
                    ok = 'Failed' not in text and 'failed' not in text
                    result.update(status='PASS' if ok else 'FAIL', detail=summarize(text))
                    break
        if result['status'] == '?':
            text = '' if is_mooneye else gb.serial.decode('latin-1') + '\n' + gb.tilemap_text()
            result.update(status='TIMEOUT', detail=summarize(text) or f"PC={gb.regs()['PC']:04x}")
        result['frames'] = gb.frames
        gb.save_png(os.path.join(out_dir, rel.replace('/', '__') + '.png'))
    except Exception as e:
        result.update(status='CRASH', detail=f'{type(e).__name__}: {e}'[:300])
    result['secs'] = round(time.time() - t0, 1)
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--workers', type=int, default=12)
    ap.add_argument('--filter', default='')
    ap.add_argument('--out', default=r'C:\opencarbon\testresults\carbon_original')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    roms = sorted(glob.glob(os.path.join(ROOT, '**', '*.gb'), recursive=True))
    roms = [r for r in roms if not relname(r).startswith(SKIP_DIRS) and args.filter in relname(r)]
    print(f'{len(roms)} ROMs, {args.workers} workers', flush=True)
    results = []
    t0 = time.time()
    with Pool(args.workers) as pool:
        for r in pool.imap_unordered(run_one, [(rom, args.out) for rom in roms]):
            results.append(r)
            print(f"[{len(results):3d}/{len(roms)}] {r['status']:7s} {r['rom']}  ({r['frames']} fr, {r['secs']}s) {r['detail'][:90]}", flush=True)
    results.sort(key=lambda r: r['rom'])
    tsv = os.path.join(args.out, 'results.tsv')
    mode = 'a' if args.filter and os.path.exists(tsv) else 'w'
    with open(tsv, mode, encoding='utf-8') as f:
        if mode == 'w':
            f.write('rom\tstatus\tframes\tsecs\tdetail\n')
        for r in results:
            f.write(f"{r['rom']}\t{r['status']}\t{r['frames']}\t{r['secs']}\t{r['detail']}\n")
    counts = {}
    for r in results:
        counts[r['status']] = counts.get(r['status'], 0) + 1
    print(f'done in {time.time() - t0:.0f}s: {counts}')


if __name__ == '__main__':
    main()
