"""Make Ghidra C output for clang/libc++ code readable.

- Folds libc++ short-string construction (byte/qword assignment runs into a local) into
  `VAR = "text";` comments.
- Drops `if ((x & 1) != 0) { operator_delete(...); }` string-destructor blocks.
- Drops pure local declarations.
- Annotates 32-bit hex constants that look like IEEE floats: 0x44480000 /*800.0f*/.

usage: simplify_c.py in.c out.c
"""
import re, struct, sys

DECL = re.compile(r'^\s+(?:undefined\d?|byte|char|ulong|long|uint|int|short|ushort|float|double|bool|string|undefined8|undefined4|undefined2|undefined1)\s*\*?\s*(\w+)\s*(?:\[(\d+)\])?;\s*$')
ASSIGN = re.compile(r'^\s+(\w+)(?:\[(\w+)\])?\s*=\s*(-?(?:0x[0-9a-fA-F]+|\d+|\'.\'));\s*$')
ELEM = {'byte': 1, 'char': 1, 'undefined1': 1, 'string': 1, 'ulong': 8, 'long': 8, 'undefined8': 8,
        'uint': 4, 'int': 4, 'undefined4': 4, 'undefined2': 2, 'short': 2, 'ushort': 2}


def parse_val(v):
    if v.startswith("'"):
        return ord(v[1])
    return int(v, 0)


def try_string(buf):
    if not buf:
        return None
    b0 = buf.get(0)
    if b0 is None or b0 & 1 or b0 == 0:
        return None
    n = b0 >> 1
    if n > 22:
        return None
    chars = []
    for i in range(1, n + 1):
        c = buf.get(i)
        if c is None or not (32 <= c < 127):
            return None
        chars.append(chr(c))
    return ''.join(chars)


def float_note(m):
    v = int(m.group(0), 16)
    if 0x30000000 <= v <= 0x46ffffff or 0xb0000000 <= v <= 0xc6ffffff:
        f = struct.unpack('<f', struct.pack('<I', v))[0]
        if 1e-4 <= abs(f) <= 1e6:
            return f'{m.group(0)} /*{f:.6g}f*/'
    return m.group(0)


def simplify(text):
    out = []
    for func in re.split(r'(?=^// ==== )', text, flags=re.M):
        lines = func.split(chr(10))
        sizes = {}
        for l in lines:
            md = DECL.match(l)
            if md:
                sizes[md.group(1)] = ELEM.get(l.split()[0], 1)
        res = []
        block = []   # consecutive assignment lines: (line, var, byte_base, width, value)

        def flush():
            nonlocal block
            if not block:
                return
            bufs = {}
            for line, var, base, width, val in block:
                b = bufs.setdefault(var, {})
                for k in range(width):
                    b[base + k] = (val >> (8 * k)) & 0xFF
            decoded = {}
            for var, b in bufs.items():
                lo = min(b)
                s_ = try_string({k - lo: v for k, v in b.items()})
                if s_ is not None:
                    decoded[var] = (s_, lo)
            emitted = set()
            for line, var, base, width, val in block:
                if var in decoded:
                    if var not in emitted:
                        indent = re.match(r'^\s*', line).group(0)
                        s_, lo = decoded[var]
                        res.append(f'{indent}{var}{"" if lo == 0 else "+" + str(lo)} = "{s_}";')
                        emitted.add(var)
                else:
                    res.append(line)
            block = []

        i = 0
        while i < len(lines):
            l = lines[i]
            if DECL.match(l):
                i += 1
                continue
            if re.match(r'^\s+if \(\(\(?(?:\(byte\))?\w+(?:\[\w+\])?\s*&\s*1\) != 0\) \{\s*$', l) and i + 2 < len(lines) and 'operator_delete' in lines[i + 1] and lines[i + 2].strip() == '}':
                i += 3
                continue
            ma = ASSIGN.match(l)
            if ma and ma.group(1) in sizes:
                var, idx, val = ma.group(1), ma.group(2), parse_val(ma.group(3))
                elem = sizes[var]
                if idx is not None:
                    idx = int(idx, 0) if re.match(r'^(0x[0-9a-fA-F]+|\d+)$', idx) else None
                    if idx is None:
                        flush(); res.append(l); i += 1; continue
                base = (idx or 0) * elem
                width = elem if (idx is not None or elem > 1) else 8
                if val < 0:
                    val += 1 << (8 * width)
                block.append((l, var, base, width, val))
                i += 1
                continue
            flush()
            res.append(re.sub(r'0x[0-9a-f]{8}', float_note, l))
            i += 1
        flush()
        out.append(chr(10).join(res))
    return ''.join(out)


if __name__ == '__main__':
    src = open(sys.argv[1], encoding='utf-8').read()
    open(sys.argv[2], 'w', encoding='utf-8').write(simplify(src))
