"""Run Carbon's original ARM64 code (Switch `main` NSO, flattened) inside Unicorn.

Loads the image at 0x7100000000, applies RELATIVE/GLOB_DAT/JUMP_SLOT relocations, links every
import to a Python stub (unknown imports stop emulation with a clear message), lazily maps the huge
.bss, and exposes `call(addr, *args)` for invoking original functions directly.
"""
import struct, math, sys
from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_PROT_ALL
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ_UNMAPPED, UC_HOOK_MEM_WRITE_UNMAPPED, UC_HOOK_MEM_FETCH_UNMAPPED
from unicorn.arm64_const import *

BASE = 0x7100000000
IMAGE_PATH = r'C:\opencarbon\extracted\base\nso_flat\main.bin'
BSS_START, BSS_END = 0x71001C8000, 0x712CBB4000
STUBS = 0x7200000000
HEAP, HEAP_SIZE = 0x7300000000, 0x4000000
STACK, STACK_SIZE = 0x7400000000, 0x200000
TLS = 0x7500000000
RET_MAGIC = 0x7600000000
RET = struct.pack('<I', 0xD65F03C0)


class EmuTimeout(RuntimeError):
    pass


class CarbonEmu:
    def __init__(self, verbose=False):
        self.verbose = verbose
        self.img = open(IMAGE_PATH, 'rb').read()
        uc = self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        size = (len(self.img) + 0xFFF) & ~0xFFF
        uc.mem_map(BASE, size, UC_PROT_ALL)
        uc.mem_write(BASE, self.img)
        uc.mem_map(HEAP, HEAP_SIZE, UC_PROT_ALL)
        uc.mem_map(STACK, STACK_SIZE, UC_PROT_ALL)
        uc.mem_map(TLS, 0x10000, UC_PROT_ALL)
        uc.mem_map(RET_MAGIC, 0x1000, UC_PROT_ALL)
        uc.mem_write(RET_MAGIC, RET)
        uc.reg_write(UC_ARM64_REG_CPACR_EL1, 0x300000)
        uc.reg_write(UC_ARM64_REG_TPIDR_EL0, TLS)
        self.heap_ptr = HEAP + 0x1000
        self.alloc_sizes = {}
        self.log = []
        self._link()
        uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED | UC_HOOK_MEM_FETCH_UNMAPPED, self._unmapped)
        # local allocator functions in main (wrappers over nn::lmem) are replaced wholesale
        self.overrides = {}
        for addr, fn in [(0x710009B940, self._malloc), (0x710009B9D0, self._free), (0x710009BB00, self._calloc),
                         (0x710009BBC0, self._realloc), (0x710009BC60, self._aligned_alloc),
                         (0x710009BD00, self._malloc), (0x710009BDA0, self._malloc), (0x710009BDB0, self._malloc),
                         (0x710009BDC0, self._malloc), (0x710009BDD0, self._free), (0x710009BF00, self._free)]:
            self.override(addr, fn)

    # ---------------------------------------------------------------- linking
    def _link(self):
        img = self.img
        mod0 = struct.unpack_from('<I', img, 4)[0]
        dyn = mod0 + struct.unpack_from('<i', img, mod0 + 4)[0]
        tags = {}
        o = dyn
        while True:
            t, v = struct.unpack_from('<qQ', img, o); o += 16
            if t == 0: break
            tags.setdefault(t, v)
        self.dyn_tags = tags
        symtab, strtab = tags[6], tags[5]
        nsyms = (strtab - symtab) // 24
        syms = []
        for i in range(nsyms):
            name_off, info, other, shndx, value, sz = struct.unpack_from('<IBBHQQ', img, symtab + i * 24)
            e = img.index(b'\0', strtab + name_off)
            syms.append((img[strtab + name_off:e].decode(), shndx, value))
        imports = sorted({s[0] for s in syms if s[1] == 0 and s[0]})
        self.stub_names = imports
        self.stub_addr = {n: STUBS + i * 4 for i, n in enumerate(imports)}
        self.uc.mem_map(STUBS, (len(imports) * 4 + 0xFFF) & ~0xFFF, UC_PROT_ALL)
        self.uc.mem_write(STUBS, RET * len(imports))
        self.uc.hook_add(UC_HOOK_CODE, self._stub_hook, begin=STUBS, end=STUBS + len(imports) * 4 - 1)

        def resolve(idx):
            name, shndx, value = syms[idx]
            return BASE + value if shndx != 0 else self.stub_addr[name]

        for tag_off, tag_sz in ((7, 8), (23, 2)):
            if tag_off not in tags: continue
            start, sz = tags[tag_off], tags[tag_sz]
            for i in range(sz // 24):
                off, info, add = struct.unpack_from('<QQq', img, start + i * 24)
                typ, sym = info & 0xFFFFFFFF, info >> 32
                if typ == 0x403:
                    val = BASE + add
                elif typ in (0x401, 0x402, 0x101):
                    val = resolve(sym) + add
                else:
                    raise RuntimeError(f'unhandled reloc type {typ:#x}')
                self.uc.mem_write(BASE + off, struct.pack('<Q', val & 0xFFFFFFFFFFFFFFFF))

    def _unmapped(self, uc, access, addr, size, value, user):
        if BSS_START <= addr < BSS_END:
            self._map_bss_chunk(addr)
            return True
        pc = uc.reg_read(UC_ARM64_REG_PC)
        print(f'[emu] unmapped access {addr:#x} at pc={pc:#x}', file=sys.stderr)
        return False

    # ---------------------------------------------------------------- helpers
    def r(self, n): return self.uc.reg_read(UC_ARM64_REG_X0 + n) if n < 29 else None
    def w(self, n, v): self.uc.reg_write(UC_ARM64_REG_X0 + n, v & 0xFFFFFFFFFFFFFFFF)
    def rd(self, n): return struct.unpack('<d', struct.pack('<Q', self.uc.reg_read(UC_ARM64_REG_D0 + n)))[0]
    def wd(self, n, v): self.uc.reg_write(UC_ARM64_REG_D0 + n, struct.unpack('<Q', struct.pack('<d', v))[0])
    def rf(self, n): return struct.unpack('<f', struct.pack('<I', self.uc.reg_read(UC_ARM64_REG_D0 + n) & 0xFFFFFFFF))[0]
    def wf(self, n, v): self.uc.reg_write(UC_ARM64_REG_D0 + n, struct.unpack('<I', struct.pack('<f', v))[0])
    def _map_bss_chunk(self, addr):
        # 1 MB chunks, except the first one which starts at BSS_START (image mapping ends there)
        start = max(addr & ~0xFFFFF, BSS_START)
        end = (addr & ~0xFFFFF) + 0x100000
        self.uc.mem_map(start, end - start, UC_PROT_ALL)

    def _ensure(self, a, n):
        if a + n > BSS_START and a < BSS_END:
            mapped = [(b, e) for b, e, _ in self.uc.mem_regions()]
            p = max(a, BSS_START)
            while p < min(a + n, BSS_END):
                if not any(b <= p <= e for b, e in mapped):
                    self._map_bss_chunk(p)
                    mapped = [(b, e) for b, e, _ in self.uc.mem_regions()]
                p = (p & ~0xFFFFF) + 0x100000
    def read(self, a, n): self._ensure(a, n); return bytes(self.uc.mem_read(a, n))
    def write(self, a, data): self._ensure(a, len(data)); self.uc.mem_write(a, bytes(data))
    def u8(self, a): return self.read(a, 1)[0]
    def u16(self, a): return struct.unpack('<H', self.read(a, 2))[0]
    def u32(self, a): return struct.unpack('<I', self.read(a, 4))[0]
    def u64(self, a): return struct.unpack('<Q', self.read(a, 8))[0]
    def put32(self, a, v): self.write(a, struct.pack('<I', v & 0xFFFFFFFF))
    def put64(self, a, v): self.write(a, struct.pack('<Q', v & 0xFFFFFFFFFFFFFFFF))
    def cstr(self, a, maxlen=4096):
        b = self.read(a, maxlen)
        return b[:b.index(0)].decode('latin-1') if 0 in b else b.decode('latin-1')

    def alloc(self, size, align=16):
        size = max(size, 1)
        self.heap_ptr = (self.heap_ptr + align - 1) & ~(align - 1)
        p = self.heap_ptr
        self.heap_ptr += size
        if self.heap_ptr > HEAP + HEAP_SIZE:
            raise MemoryError('emu heap exhausted')
        self.alloc_sizes[p] = size
        self.write(p, bytes(size))
        return p

    def override(self, addr, fn):
        """Replace the function at `addr`: fn(emu) runs instead, then we return to LR."""
        self.overrides[addr] = fn
        def hook(uc, address, size, user):
            fn(self)
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))
        self.uc.hook_add(UC_HOOK_CODE, hook, begin=addr, end=addr)

    def watch(self, addr, fn):
        """Observe entry to `addr` without changing behaviour."""
        return self.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, u: fn(self), begin=addr, end=addr)

    def call(self, addr, *args, timeout_us=0):
        for i, a in enumerate(args):
            self.w(i, a)
        self.uc.reg_write(UC_ARM64_REG_SP, STACK + STACK_SIZE - 0x100)
        self.uc.reg_write(UC_ARM64_REG_X30, RET_MAGIC)
        self.stop_reason = None
        self.stop_requested = False
        try:
            self.uc.emu_start(addr, RET_MAGIC, timeout=timeout_us)
        except UcError as e:
            pc = self.uc.reg_read(UC_ARM64_REG_PC)
            raise RuntimeError(f'emulation fault {e} at pc={pc:#x}') from None
        if self.stop_reason:
            raise RuntimeError(self.stop_reason)
        pc = self.uc.reg_read(UC_ARM64_REG_PC)
        if pc != RET_MAGIC and not self.stop_requested:
            raise EmuTimeout(f'call to {addr:#x} did not return within {timeout_us / 1e6:.0f}s (pc={pc:#x})')
        return self.r(0)

    def request_stop(self):
        """Stop emulation from inside a hook; call() then returns without raising."""
        self.stop_requested = True
        self.uc.emu_stop()

    def run_init_array(self):
        """Run C++ static constructors (DT_INIT_ARRAY) the way the Switch runtime does before nnMain."""
        tags = self.dyn_tags
        if 25 not in tags: return []
        ran = []
        for i in range(tags[27] // 8):
            fn = self.u64(BASE + tags[25] + i * 8)
            try:
                self.call(fn)
                ran.append((fn, None))
            except RuntimeError as err:
                ran.append((fn, str(err)))
        return ran

    # ---------------------------------------------------------------- allocator overrides
    def _malloc(self, e): e.w(0, e.alloc(e.r(0)))
    def _free(self, e): pass
    def _calloc(self, e): e.w(0, e.alloc(e.r(0) * e.r(1)))
    def _aligned_alloc(self, e): e.w(0, e.alloc(e.r(1), max(16, e.r(0))))
    def _realloc(self, e):
        old, size = e.r(0), e.r(1)
        p = e.alloc(size)
        if old:
            n = min(e.alloc_sizes.get(old, size), size)
            e.write(p, e.read(old, n))
        e.w(0, p)

    # ---------------------------------------------------------------- import stubs
    def _stub_hook(self, uc, address, size, user):
        name = self.stub_names[(address - STUBS) // 4]
        impl = STUB_IMPLS.get(name)
        if impl is None:
            for prefix, fn in STUB_PREFIX_IMPLS:
                if name.startswith(prefix):
                    impl = fn
                    break
        if impl is None:
            self.stop_reason = f'unimplemented import {name} called from {uc.reg_read(UC_ARM64_REG_X30):#x}'
            uc.emu_stop()
            return
        impl(self)


def _printf(e):
    e.log.append(e.cstr(e.r(0)))
def _memset(e): e.write(e.r(0), bytes([e.r(1) & 0xFF]) * e.r(2)) if e.r(2) else None
def _memcpy(e): e.write(e.r(0), e.read(e.r(1), e.r(2))) if e.r(2) else None
def _memcmp(e):
    a, b = e.read(e.r(0), e.r(2)), e.read(e.r(1), e.r(2))
    e.w(0, 0 if a == b else (-1 if a < b else 1))
def _strlen(e): e.w(0, len(e.cstr(e.r(0), 1 << 16)))
def _ret0(e): e.w(0, 0)
def _ret1(e): e.w(0, 1)
def _memalign(e): e.w(0, e.alloc(e.r(1), max(16, e.r(0))))
def _math1(f): return lambda e: e.wd(0, f(e.rd(0)))
def _abort(e):
    e.stop_reason = f'program aborted (called from {e.uc.reg_read(UC_ARM64_REG_X30):#x})'
    e.uc.emu_stop()


STUB_IMPLS = {
    'memset': _memset, 'memcpy': _memcpy, 'memmove': _memcpy, 'memcmp': _memcmp, 'strlen': _strlen,
    'printf': _printf, 'memalign': _memalign,
    'log': _math1(math.log), 'cos': _math1(math.cos), 'exp2': _math1(lambda x: 2.0 ** x),
    'pow': lambda e: e.wd(0, math.pow(e.rd(0), e.rd(1))),
    'powf': lambda e: e.wf(0, math.pow(e.rf(0), e.rf(1))),
    '__cxa_guard_acquire': _ret1, '__cxa_guard_release': _ret0,
}
STUB_PREFIX_IMPLS = [
    ('_ZN2nn2os12CreateThread', _ret0), ('_ZN2nn2os11StartThread', _ret0),
    ('_ZN2nn4diag6detail9AbortImpl', _abort), ('abort', _abort),
    # libc++ iostream/locale plumbing only reached by constructors of unused stream members
    ('_ZNSt3__18ios_base4init', _ret0), ('_ZNSt3__115basic_streambuf', _ret0), ('_ZNSt3__16localeC', _ret0),
    ('_ZNSt3__16localeD', _ret0), ('_ZNKSt3__16locale9has_facet', _ret0),
]
