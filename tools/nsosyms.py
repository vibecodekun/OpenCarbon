"""Symbols and relocations of a flattened NSO (extracted/base/nso_flat/*.bin), without Ghidra.

subsdk1/subsdk2/sdk export full .dynsym tables, so vtable slots (R_AARCH64_ABS64), GOT slots (GLOB_DAT/JUMP_SLOT)
and RELATIVE pointers resolve to names.

usage:
  nsosyms.py <module.bin> sym <regex>          defined/imported symbols matching a regex
  nsosyms.py <module.bin> at <hex addr>...     function (symbol) containing each address
  nsosyms.py <module.bin> ptr <hex slot>...    what a pointer slot is relocated to
  nsosyms.py <module.bin> plt <hex stub>...    symbol a PLT stub (adrp x16 / ldr x17 / br x17) jumps to
  nsosyms.py <module.bin> vtable <hex addr> <count>
"""
import bisect, re, struct, sys


class Nso:
    def __init__(self, path):
        self.img = img = open(path, 'rb').read()
        mod0 = struct.unpack_from('<I', img, 4)[0]
        dyn = mod0 + struct.unpack_from('<i', img, mod0 + 4)[0]
        tags, o = {}, dyn
        while True:
            t, v = struct.unpack_from('<qQ', img, o)
            o += 16
            if t == 0:
                break
            tags.setdefault(t, v)
        self.tags = tags
        symtab, strtab = tags[6], tags[5]
        self.syms = []  # (name, value, shndx, size)
        o = symtab
        while o < strtab:
            name, _info, _other, shndx, value, size = struct.unpack_from('<IBBHQQ', img, o)
            o += 24
            self.syms.append((img[strtab + name:img.index(b'\0', strtab + name)].decode('latin1'), value, shndx, size))
        self.by_addr = {}
        for nm, v, sh, _ in self.syms:
            if sh and v:
                self.by_addr.setdefault(v, nm)
        self.funcs = sorted((v, nm) for nm, v, sh, _ in self.syms if sh and v)
        self.func_starts = [f[0] for f in self.funcs]
        self.rel = {}  # slot -> int target (RELATIVE) or (symbol, addend)
        for rela_tag, size_tag in ((7, 8), (0x17, 2)):
            if rela_tag not in tags:
                continue
            for i in range(tags[size_tag] // 24):
                off, info, add = struct.unpack_from('<QQq', img, tags[rela_tag] + i * 24)
                kind = info & 0xFFFFFFFF
                if kind == 0x403:
                    self.rel[off] = add
                elif kind in (0x101, 0x401, 0x402):
                    self.rel[off] = (self.syms[info >> 32][0], add)

    def ptr(self, slot):
        return self.rel.get(slot)

    def describe(self, slot):
        r = self.ptr(slot)
        if r is None:
            return None
        if isinstance(r, int):
            return f'{r:#x} {self.by_addr.get(r, "")}'.strip()
        return r[0] + (f'+{r[1]:#x}' if r[1] else '')

    def containing(self, addr):
        i = bisect.bisect_right(self.func_starts, addr) - 1
        if i < 0:
            return '?'
        start, name = self.funcs[i]
        return f'{name}+{addr - start:#x}'

    def plt_target(self, stub):
        i1, i2 = struct.unpack_from('<II', self.img, stub)
        immlo, immhi = (i1 >> 29) & 3, (i1 >> 5) & 0x7FFFF
        imm = immhi << 2 | immlo
        if imm & (1 << 20):
            imm -= 1 << 21
        slot = (stub & ~0xFFF) + (imm << 12) + ((i2 >> 10) & 0xFFF) * 8
        return slot, self.describe(slot)


def main():
    n = Nso(sys.argv[1])
    cmd, args = sys.argv[2], sys.argv[3:]
    if cmd == 'sym':
        pat = re.compile(args[0])
        for nm, v, sh, sz in n.syms:
            if pat.search(nm):
                print(f'{v:#x} {sz:#x} {"defined" if sh else "import"} {nm}')
    elif cmd == 'at':
        for a in args:
            print(a, n.containing(int(a, 16)))
    elif cmd == 'ptr':
        for a in args:
            print(a, n.describe(int(a, 16)))
    elif cmd == 'plt':
        for a in args:
            slot, what = n.plt_target(int(a, 16))
            print(a, f'got {slot:#x}', what)
    elif cmd == 'vtable':
        base, count = int(args[0], 16), int(args[1])
        for i in range(count):
            print(f'+{i * 8:#05x}', n.describe(base + 16 + i * 8))


if __name__ == '__main__':
    main()
