"""Decompress Switch NSO files into flat memory images (text|ro|data laid out at their vaddrs)."""
import struct, sys, os

def lz4_block(src, out_size):
    dst = bytearray(); i = 0; n = len(src)
    while i < n:
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255: break
        dst += src[i:i+lit]; i += lit
        if i >= n: break
        off = src[i] | (src[i+1] << 8); i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255: break
        ml += 4
        start = len(dst) - off
        if off >= ml:
            dst += dst[start:start+ml]
        else:
            for k in range(ml): dst.append(dst[start+k])
    assert len(dst) == out_size, (len(dst), out_size)
    return bytes(dst)

def load(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'NSO0'
    flags = struct.unpack_from('<I', d, 0xC)[0]
    segs = []
    for idx, (ho, co) in enumerate([(0x10, 0x60), (0x20, 0x64), (0x30, 0x68)]):
        foff, voff, size = struct.unpack_from('<III', d, ho)
        csize = struct.unpack_from('<I', d, co)[0]
        raw = d[foff:foff+csize]
        data = lz4_block(raw, size) if flags & (1 << idx) else raw[:size]
        segs.append((voff, data))
    bss = struct.unpack_from('<I', d, 0x3C)[0]
    build_id = d[0x40:0x60].rstrip(b'\0').hex()
    end = max(v + len(x) for v, x in segs)
    img = bytearray(end)
    for v, x in segs: img[v:v+len(x)] = x
    info = dict(text=(segs[0][0], len(segs[0][1])), ro=(segs[1][0], len(segs[1][1])),
                data=(segs[2][0], len(segs[2][1])), bss=bss, build_id=build_id)
    return bytes(img), info

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    for name in sorted(os.listdir(src)):
        p = os.path.join(src, name)
        with open(p, 'rb') as f:
            if f.read(4) != b'NSO0': continue
        img, info = load(p)
        open(os.path.join(dst, name + '.bin'), 'wb').write(img)
        print(f"{name:8s} size={len(img):#x} text={info['text'][0]:#x}+{info['text'][1]:#x} ro={info['ro'][0]:#x}+{info['ro'][1]:#x} data={info['data'][0]:#x}+{info['data'][1]:#x} bss={info['bss']:#x} build_id={info['build_id']}")
