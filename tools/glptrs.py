"""Recover GL/EGL function-pointer globals: x0=&"name"; bl getter; str x0,[page+off]."""
import capstone, collections, sys, json
B = 0x7100000000
REL = {int(k, 16): int(v, 16) for k, v in json.load(open('C:/opencarbon/ghidra/relocs.json')).items()}
img = open(r'C:\opencarbon\extracted\base\nso_flat\main.bin', 'rb').read()
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); md.detail = False
def cstr(a):
    a -= B
    if not (0 <= a < len(img)): return None
    e = img.find(b'\0', a)
    s = img[a:e]
    return s.decode() if s and all(32 < c < 127 for c in s) else None
out = {}
getters = collections.Counter()
for lo, hi in [(0xa2860, 0xcbf60), (0xf0910, 0x102360)]:
    ins = []
    for off in range(lo, hi, 4):
        r = list(md.disasm(img[off:off + 4], B + off))
        ins.append((B + off, r[0].mnemonic, r[0].op_str) if r else (B + off, '', ''))
    reg = {}
    pending = None  # (name, getter)
    for a, m, o in ins:
        ops = [x.strip() for x in o.split(',')]
        if m == 'adrp':
            reg[ops[0]] = int(ops[1].lstrip('#'), 16)
        elif m == 'add' and len(ops) == 3 and ops[0] == ops[1] and ops[0] in reg and ops[2].startswith('#'):
            reg[ops[0]] = reg[ops[0]] + int(ops[2].lstrip('#'), 16)
            if ops[0] == 'x0':
                reg['x0_str'] = cstr(reg['x0'])
        elif m == 'ldr' and len(ops) == 3 and ops[0].startswith('x') and ops[1].lstrip('[') == ops[0] and ops[0] in reg:
            slot = reg[ops[0]] + int(ops[2].rstrip(']').lstrip('#'), 16) - B
            if slot in REL:
                reg[ops[0]] = B + REL[slot]
            else:
                reg.pop(ops[0], None)
        elif m == 'bl':
            name = reg.get('x0_str')
            pending = (name, int(ops[0].lstrip('#'), 16)) if name else None
            reg.pop('x0_str', None)
        elif m == 'str' and pending and ops[0] == 'x0':
            base = ops[1].lstrip('[').rstrip(']')
            disp = int(ops[2].rstrip(']').lstrip('#'), 16) if len(ops) > 2 else 0
            if base in reg:
                out[reg[base] + disp] = pending[0]
                getters[pending[1]] += 1
            pending = None
with open(r'C:\opencarbon\ghidra\gl_ptrs.tsv', 'w') as f:
    for addr, name in sorted(out.items()):
        f.write(f'{addr:#x}\t{name}\n')
print(len(out), 'pointer globals; getters:', {hex(k): v for k, v in getters.items()})
