"""Parse MOD0/.dynamic/.rela.dyn from a flattened NSO image; dump RELATIVE relocs and find pointer tables."""
import struct, sys, json
BASE = 0x7100000000
img = open(r'C:\opencarbon\extracted\base\nso_flat\main.bin', 'rb').read()
mod0 = struct.unpack_from('<I', img, 4)[0]
assert img[mod0:mod0+4] == b'MOD0'
dyn = mod0 + struct.unpack_from('<i', img, mod0+4)[0]
tags = {}
o = dyn
while True:
    t, v = struct.unpack_from('<qQ', img, o); o += 16
    if t == 0: break
    tags.setdefault(t, v)
rela, relasz = tags[7], tags[8]
rel = {}
for i in range(relasz // 24):
    off, info, add = struct.unpack_from('<QQq', img, rela + i*24)
    if info & 0xffffffff == 0x403:  # R_AARCH64_RELATIVE
        rel[off] = add
print(f"MOD0 @{mod0:#x} dynamic @{dyn:#x} rela @{rela:#x} n_relative={len(rel)}", file=sys.stderr)
json.dump({hex(k): hex(v) for k, v in rel.items()}, open(r'C:\opencarbon\ghidra\relocs.json', 'w'))
# find runs of consecutive pointer slots
lo, hi = (int(sys.argv[1], 16) - BASE, int(sys.argv[2], 16) - BASE) if len(sys.argv) > 2 else (0, 0x111760)
keys = sorted(rel); run = []
def flush():
    if len(run) >= 16:
        tg = [rel[k] for k in run]; inrange = sum(lo <= t < hi for t in tg)
        if inrange >= len(run) // 2:
            print(f"table @{BASE+run[0]:#x} len={len(run)} in_range={inrange} first={[hex(BASE+t) for t in tg[:4]]}")
for k in keys:
    if run and k == run[-1] + 8: run.append(k)
    else: flush(); run = [k]
flush()
