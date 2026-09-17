"""Checks carbon.exe's startup-movie captures (movie_NNN.png from a --script run) against renderings computed from an
independent decode, for each candidate the original could use: the BT.601 or BT.709 limited-range table from
Carbon's movie renderer (@ 0x710019589c / 0x710019591c), sampled NEAREST or LINEAR (clamp to edge) onto 1280x720.

usage: cmp_frames.py <shots dir> [--mp4 extracted/base/romfs/assets/splash/wf_logo.mp4] [--ffmpeg ffmpeg]
"""
import argparse, glob, os, re, struct, subprocess
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
W, H, CANVAS_W, CANVAS_H = 1920, 1080, 1280, 720


def f32(bits):
    return struct.unpack('<f', struct.pack('<I', bits))[0]


# (src_bias, mat3 columns) exactly as stored in main.bin; the shader computes rgb[i] = dot(yuv - bias, column[i])
TABLES = {
    'BT.601': ((f32(0x3d808081), f32(0x3f008081), f32(0x3f008081)),
               ((f32(0x3f950a81), 0.0, f32(0x3fcc4a9d)), (f32(0x3f950a81), f32(0xbec89514), f32(0xbf501ea8)),
                (f32(0x3f950a81), f32(0x40011a56), 0.0))),
}


def load_tables(main_bin):
    d = open(main_bin, 'rb').read()
    out = {}
    for name, addr in (('BT.601', 0x19589c), ('BT.709', 0x19591c)):
        v = struct.unpack_from('<16f', d, addr)
        out[name] = (np.array(v[0:3], np.float32), np.array([v[4:7], v[8:11], v[12:15]], np.float32))
    return out


def sample(plane, out_w, out_h, linear):
    ph, pw = plane.shape
    sx = (np.arange(out_w) + 0.5) / out_w * pw
    sy = (np.arange(out_h) + 0.5) / out_h * ph
    if not linear:
        return plane[np.floor(sy).astype(int)][:, np.floor(sx).astype(int)]
    sx, sy = sx - 0.5, sy - 0.5
    x0, y0 = np.floor(sx).astype(int), np.floor(sy).astype(int)
    fx, fy = (sx - x0).astype(np.float32), (sy - y0).astype(np.float32)
    x1, y1 = np.clip(x0 + 1, 0, pw - 1), np.clip(y0 + 1, 0, ph - 1)
    x0, y0 = np.clip(x0, 0, pw - 1), np.clip(y0, 0, ph - 1)
    rows = plane[y0] * (1 - fy)[:, None] + plane[y1] * fy[:, None]
    return rows[:, x0] * (1 - fx) + rows[:, x1] * fx


def render(yuv420, table, linear):
    y = yuv420[:W * H].reshape(H, W).astype(np.float32) / 255
    u = yuv420[W * H:W * H * 5 // 4].reshape(H // 2, W // 2).astype(np.float32) / 255
    v = yuv420[W * H * 5 // 4:].reshape(H // 2, W // 2).astype(np.float32) / 255
    s = np.stack([sample(p, CANVAS_W, CANVAS_H, linear) for p in (y, u, v)], -1)
    bias, cols = table
    rgb = (s - bias) @ cols.T
    return np.clip(np.round(rgb * 255), 0, 255).astype(int)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('shots')
    ap.add_argument('--mp4', default=os.path.join(HERE, '..', '..', 'extracted', 'base', 'romfs', 'assets', 'splash', 'wf_logo.mp4'))
    ap.add_argument('--main', default=os.path.join(HERE, '..', '..', 'extracted', 'base', 'nso_flat', 'main.bin'))
    ap.add_argument('--ffmpeg', default='ffmpeg')
    args = ap.parse_args()
    tables = load_tables(args.main)
    shots = sorted(glob.glob(os.path.join(args.shots, 'movie_*.png')))
    if not shots:
        raise SystemExit('no movie_NNN.png captures (run carbon.exe with --script and --shots)')
    for shot in shots:
        n = int(re.search(r'movie_(\d+)', shot).group(1))
        raw = subprocess.run([args.ffmpeg, '-v', 'error', '-i', args.mp4, '-vf', f'select=eq(n\\,{n})', '-vsync', '0',
                              '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'yuv420p', '-'],
                             capture_output=True, check=True).stdout
        yuv = np.frombuffer(raw, np.uint8)
        got = np.asarray(Image.open(shot))[..., :3].astype(int)
        results = []
        for name, table in tables.items():
            for linear in (False, True):
                d = np.abs(render(yuv, table, linear) - got)
                results.append((d.mean(), d.max(), (d.max(-1) > 1).sum(), f'{name} {"LINEAR " if linear else "NEAREST"}'))
        best = min(results)
        line = '  '.join(f'{label}: mean {m:.3f} max {mx:3d} >1:{cnt:6d}' for m, mx, cnt, label in results)
        print(f'frame {n:3d}: best {best[3]} | {line}')


if __name__ == '__main__':
    main()
