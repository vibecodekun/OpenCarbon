"""Differential tests: Carbon's original GB core (ARM64, in Unicorn) vs the C++ port (gbdiff.dll).

Both sides get byte-identical state blocks and inputs; after each call the blocks, return values and captured
audio are compared. See carbon-pc/src/gb/State.h for the block layout (it mirrors the original globals).

usage:
  gbdiff.py cpu  [--cases N] [--ops 00,3e,...] [--seed S]   random states through gb_cpu_step
  gbdiff.py cb   [--cases N] [--ops ...]                     CB-prefixed opcodes
  gbdiff.py irq  [--cases N]                                 gb_service_interrupts
  gbdiff.py mem  [--cases N]                                 gb_read8 / gb_write8 over the whole address space
  gbdiff.py lcd  [--cases N]                                 lcd_step incl. scanline rendering and HDMA
  gbdiff.py timer [--cases N]                                gb_timer_step
  gbdiff.py frames <rom> [--frames N] [--keys script]        gb_init + gb_run_frame, compared every frame
               [--save-at F --load-at L]                      + GbSystem_SaveState after frame F, GbSystem_LoadState
                                                               before frame L (the port writes the file; the
                                                               original's gb_load_state reads it)
"""
import argparse, ctypes, os, random, struct, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'harness'))
from carbon_emu import CarbonEmu, EmuTimeout  # noqa: E402

DLL_PATH = os.path.join(HERE, '..', '..', 'carbon-pc', 'build', 'Release', 'gbdiff.dll')

GB_CPU_STEP = 0x710008E7F0
GB_SERVICE_INTERRUPTS = 0x71000853E0
GB_READ8 = 0x710008D630
GB_WRITE8 = 0x710008D4C0
LCD_STEP = 0x71000873B0
GB_TIMER_STEP = 0x71000948E0
GB_INIT = 0x7100081F60
GB_RUN_FRAME = 0x71000820B0
GB_KEY_DOWN = 0x71000856C0
GB_KEY_UP = 0x7100085730
GB_MAPPER = 0x712ABAC020
GB_APU_TIME = 0x7100222118
GBSYSTEM_LOAD_STATE = 0x7100082230
GB_LOAD_STATE_FILE = 0x710008E210
READ_SAMPLES_RET = 0x7100082188  # instruction after the StereoBuffer_read_samples call in gb_run_frame
SAMPLE_SCRATCH = 0x710022211C
CTORS = {0: 0x710008A3D0, 1: 0x710008A470, 3: 0x710008A9A0, 5: 0x710008B020}
CTOR_SIZE = {0: 0x68, 1: 0x1E0, 3: 0xC0, 5: 0x90}

# (name, block, offset, size) for readable diffs; arrays are reported as name[index]
FIELDS = [
    ('joypadButtons', 0, 0x00, 1), ('io_1bd9', 0, 0x01, 1), ('LY', 0, 0x02, 1), ('LYC', 0, 0x03, 1), ('io_1bdc', 0, 0x04, 1),
    ('LCDC', 0, 0x05, 1), ('SCY', 0, 0x06, 1), ('SCX', 0, 0x07, 1), ('WY', 0, 0x08, 1), ('WX', 0, 0x09, 1),
    ('BGP', 0, 0x0a, 1), ('OBP0', 0, 0x0b, 1), ('OBP1', 0, 0x0c, 1), ('windowLine', 0, 0x0d, 1), ('STAT', 0, 0x0e, 1),
    ('BCPS', 0, 0x0f, 1), ('OCPS', 0, 0x10, 1), ('VBK', 0, 0x11, 1), ('hdmaActive', 0, 0x12, 1), ('io_1beb', 0, 0x13, 1),
    ('hdmaSrc', 0, 0x14, 2), ('hdmaDst', 0, 0x16, 2), ('hdmaLength', 0, 0x18, 2), ('bgPalette', 0, 0x1a, 0x40),
    ('objPalette', 0, 0x5a, 0x40),
    ('imeRequest', 1, 0, 1), ('ime', 1, 1, 1), ('eiDelay', 1, 2, 1), ('framebuffer', 1, 3, 0x16800),
    ('dmgTileCache', 1, 0x16803, 0x6000), ('oamMirror', 1, 0x1c803, 0xa0), ('dmgTileMap', 1, 0x1c8a3, 0x8000),
    ('isCgb', 1, 0x248a3, 1), ('vram', 1, 0x248a4, 0x4000), ('lineColorIndex', 1, 0x288a4, 0x280),
    ('lineBgPriority', 1, 0x28b24, 0xa0), ('dmgPaletteRgb', 1, 0x28bc4, 12), ('lcdTickAccum', 1, 0x28bd0, 4),
    ('lcdMode', 1, 0x28bd4, 1), ('frameDone', 1, 0x28bd5, 1), ('core_bed6', 1, 0x28bd6, 2), ('mbc1BankLow', 1, 0x28bd8, 4),
    ('mbc1BankHigh', 1, 0x28bdc, 4), ('mbc1RamEnable', 1, 0x28be0, 1), ('mbc1Mode', 1, 0x28be1, 1), ('core_bee2', 1, 0x28be2, 2),
    ('mbc5Dirty', 1, 0x28be4, 1), ('mbc5RamEnable', 1, 0x28be5, 1), ('mbc5BankLow', 1, 0x28be6, 1),
    ('mbc5BankHigh', 1, 0x28be7, 1), ('mbc5RamBank', 1, 0x28be8, 1), ('core_bee9', 1, 0x28be9, 7), ('oam', 1, 0x28bf0, 0xa0),
    ('hram', 1, 0x28c90, 0x7f), ('core_c00f', 1, 0x28d0f, 1),
    ('F', 2, 0, 1), ('A', 2, 1, 1), ('C', 2, 2, 1), ('B', 2, 3, 1), ('E', 2, 4, 1), ('D', 2, 5, 1), ('L', 2, 6, 1), ('H', 2, 7, 1),
    ('SP', 2, 8, 2), ('PC', 2, 10, 2), ('halted', 2, 12, 1), ('cpu_d03d', 2, 13, 3), ('ticks', 2, 0x10, 4), ('debugLog', 2, 0x14, 1),
    ('cpu_d045', 2, 0x15, 3), ('rst38Marker', 2, 0x18, 4), ('KEY1', 2, 0x1c, 1),
    ('doubleSpeed', 2, 0x1d, 1), ('cpu_d04e', 2, 0x1e, 2), ('divAccum', 2, 0x20, 4), ('DIV', 2, 0x24, 1), ('TAC', 2, 0x25, 1),
    ('TIMA', 2, 0x26, 1), ('TMA', 2, 0x27, 1), ('timaAccum', 2, 0x28, 4), ('timaPeriod', 2, 0x2c, 4), ('IF', 2, 0x30, 1),
    ('IE', 2, 0x31, 1), ('wram', 2, 0x32, 0x8000),
    ('joypSelect', 3, 0, 1), ('wramBank', 4, 0, 1),
]


def field_name(block, offset):
    for name, b, off, size in FIELDS:
        if b == block and off <= offset < off + size:
            return name if size <= 4 else f'{name}[{offset - off:#x}]'
    return f'block{block}+{offset:#x}'


class Port:
    def __init__(self):
        self.dll = d = ctypes.CDLL(os.path.abspath(DLL_PATH))
        d.gbd_block.restype = ctypes.c_void_p
        d.gbd_block.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint32)]
        for name in ('gbd_cpu_step', 'gbd_service_interrupts', 'gbd_frame_count', 'gbd_apu_time'):
            getattr(d, name).restype = ctypes.c_int32
        d.gbd_read8.restype = ctypes.c_uint32
        d.gbd_take_samples.restype = ctypes.c_int32
        d.gbd_cart_create.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int]
        d.gbd_reset_state()
        self.blocks = []
        for i in range(5):
            addr, size = ctypes.c_uint64(), ctypes.c_uint32()
            ptr = d.gbd_block(i, ctypes.byref(addr), ctypes.byref(size))
            self.blocks.append((ptr, addr.value, size.value))

    def read_block(self, i):
        ptr, _, size = self.blocks[i]
        return ctypes.string_at(ptr, size)

    def write_block(self, i, data, offset=0):
        ptr, _, _ = self.blocks[i]
        ctypes.memmove(ptr + offset, data, len(data))


class Differ:
    def __init__(self, rom=None, cart_kind=0, ram_size=0, rtc=0, seed=1):
        self.rng = random.Random(seed)
        self.emu = e = CarbonEmu()
        for fn, err in e.run_init_array():
            if err and 'StandardAllocator' not in err:
                print(f'[init_array] {fn:#x}: {err}', file=sys.stderr)
        self.port = Port()
        self.samples_orig = bytearray()
        e.watch(READ_SAMPLES_RET, self._on_read_samples)
        if rom is None:
            rom = bytes(self.rng.getrandbits(8) for _ in range(0x8000))
        self.make_cart(rom, cart_kind, ram_size, rtc)

    def _on_read_samples(self, e):
        n = e.r(0) & 0xFFFFFFFF
        if n & 0x80000000:
            n -= 1 << 32
        if n > 0:
            self.samples_orig += e.read(SAMPLE_SCRATCH, n * 2)

    def make_cart(self, rom, kind, ram_size, rtc, buffer_size=None):
        e = self.emu
        # MBC3/MBC5 don't mask ROM reads, so both sides get the same zero-padded buffer to read past the end
        buffer_size = buffer_size or (0x1000000 if kind in (3, 5) else len(rom) + 0x8000)
        buf = bytes(rom) + bytes(buffer_size - len(rom))
        rom_ptr = e.alloc(buffer_size)
        e.write(rom_ptr, buf)
        obj = e.alloc(CTOR_SIZE[kind])
        if kind == 0:
            e.call(CTORS[0], obj, rom_ptr, len(rom))
        elif kind == 3:
            e.call(CTORS[3], obj, rom_ptr, len(rom), ram_size, rtc)
        else:
            e.call(CTORS[kind], obj, rom_ptr, len(rom), ram_size)
        vt = e.u64(obj)
        e.put32(obj + 8, (e.call(e.u64(vt + 8), obj, 0x143) >> 7) & 1)
        e.put64(GB_MAPPER, obj)
        self.port.dll.gbd_cart_create(kind, buf, buffer_size, len(rom), ram_size, rtc)

    # ---- state transfer
    def orig_block(self, i):
        _, addr, size = self.port.blocks[i]
        return self.emu.read(addr, size)

    def write_both(self, i, data, offset=0):
        _, addr, _ = self.port.blocks[i]
        self.emu.write(addr + offset, data)
        self.port.write_block(i, data, offset)

    def sync_port_from_orig(self):
        for i in range(5):
            self.port.write_block(i, self.orig_block(i))

    def diff(self, limit=12):
        out = []
        for i in range(5):
            a, b = self.orig_block(i), self.port.read_block(i)
            if a == b:
                continue
            j = 0
            while j < len(a) and len(out) < limit:
                if a[j] != b[j]:
                    out.append((field_name(i, j), a[j], b[j]))
                j += 1
        return out

    # ---- random state
    def randomize_all(self):
        r = self.rng
        # random, but within invariants the original maintains: VBK is 0/1 (FF4F masks it), CGB mode is
        # forced by gb_init, the WRAM bank is 1..7
        io = bytearray(r.getrandbits(8) for _ in range(0x9a))
        io[0x11] &= 1
        self.write_both(0, bytes(io))
        core = bytearray(os.urandom(0x28d10))
        core[0x248a3] = 1
        core[0x28be7] &= 2   # mbc5BankHigh only ever holds (value & 1) << 1
        self.write_both(1, bytes(core))
        cpu = bytearray(os.urandom(0x8032))
        cpu[12] = 0            # halted
        cpu[0x14] = 0          # debugLog (would print)
        self.write_both(2, bytes(cpu))
        self.write_both(3, bytes([r.getrandbits(8)]))
        self.write_both(4, bytes([r.randint(1, 7)]))


def cmd_cpu(args, cb=False):
    d = Differ(seed=args.seed)
    d.emu.call(GB_INIT, d.emu.alloc(8))
    d.port.dll.gbd_init()
    d.randomize_all()
    d.sync_port_from_orig()
    r = d.rng
    ops = [int(x, 16) for x in args.ops.split(',')] if args.ops else list(range(256))
    if not cb:
        ops = [o for o in ops if o != 0xCB]
    bad = {}
    t0 = time.time()
    total = 0
    for op in ops:
        for case in range(args.cases):
            if case % 25 == 0:
                d.randomize_all()
            cpu = bytearray(d.orig_block(2)[:0x32])
            wram_bank = r.randint(1, 7)
            d.write_both(4, bytes([wram_bank]))

            def ptr():
                k = r.random()
                if k < 0.7:
                    return r.randint(0xC000, 0xDFFF)
                if k < 0.9:
                    return r.randint(0xFF80, 0xFFFE)
                return r.getrandbits(16)
            pc = r.randint(0xC000, 0xDFF0)
            for k, v in enumerate(r.getrandbits(8) for _ in range(8)):
                cpu[k] = v
            struct.pack_into('<HH', cpu, 8, r.randint(0xC002, 0xDFFE) if r.random() < 0.8 else ptr(), pc)
            for reg_off in (2, 4, 6):  # BC DE HL
                if r.random() < 0.8:
                    struct.pack_into('<H', cpu, reg_off, ptr())
            cpu[12] = 1 if r.random() < 0.05 else 0
            cpu[0x14] = 0
            d.write_both(2, bytes(cpu[:0x32]))
            # opcode + operands at PC (C000-CFFF and D000-DFFF with the current bank)
            code = bytes([0xCB if cb else op, op if cb else r.getrandbits(8), r.getrandbits(8), r.getrandbits(8)])
            for k, byte in enumerate(code):
                a = pc + k
                idx = (a - 0xC000) if a < 0xD000 else ((a & 0xFFF) | wram_bank << 12)
                d.write_both(2, bytes([byte]), 0x32 + idx)
            d.emu.put32(GB_APU_TIME, 0)
            before = d.orig_block(2)[:0x32]
            try:
                t_orig = d.emu.call(GB_CPU_STEP) & 0xFFFFFFFF
            except Exception as ex:
                print(f'op {op:02x}: original raised {ex}')
                d.sync_port_from_orig()
                continue
            t_port = d.port.dll.gbd_cpu_step() & 0xFFFFFFFF
            total += 1
            diffs = d.diff()
            if t_orig != t_port:
                diffs.insert(0, ('return', t_orig, t_port))
            if diffs:
                bad.setdefault(op, 0)
                bad[op] += 1
                if bad[op] <= 2:
                    regs = struct.unpack_from('<8BHHB', before)
                    names = 'F A C B E D L H SP PC halted'.split()
                    state = ' '.join(f'{n}={v:02x}' for n, v in zip(names, regs))
                    print(f"{'CB ' if cb else ''}{op:02x} case {case}: {state} code={code.hex()}")
                    for name, a, b in diffs:
                        print(f'    {name}: orig={a:#x} port={b:#x}')
                d.sync_port_from_orig()
    dt = time.time() - t0
    print(f'{total} cases in {dt:.0f}s; mismatching {"CB " if cb else ""}opcodes: ' +
          (' '.join(f'{o:02x}({n})' for o, n in sorted(bad.items())) or 'none'))
    return 1 if bad else 0


class Report:
    def __init__(self, label, max_prints=5):
        self.label, self.max_prints = label, max_prints
        self.total = self.bad = 0
        self.t0 = time.time()

    def check(self, d, desc, ret_orig=None, ret_port=None):
        self.total += 1
        diffs = d.diff()
        if ret_orig is not None and ret_orig != ret_port:
            diffs.insert(0, ('return', ret_orig, ret_port))
        if diffs:
            self.bad += 1
            if self.bad <= self.max_prints:
                print(f'{self.label}: {desc}')
                for name, a, b in diffs:
                    print(f'    {name}: orig={a:#x} port={b:#x}')
            d.sync_port_from_orig()
        return not diffs

    def done(self):
        print(f'{self.label}: {self.total} cases in {time.time() - self.t0:.0f}s, {self.bad} mismatching')
        return 1 if self.bad else 0


def new_differ(args, cart='romonly'):
    kinds = {'romonly': (0, 0), 'mbc1': (1, 0x2000), 'mbc3': (3, 0x8000), 'mbc5': (5, 0x20000)}
    kind, ram = kinds[cart]
    rng = random.Random(args.seed)
    rom = bytearray(rng.getrandbits(8) for _ in range(0x40000 if kind else 0x8000))
    d = Differ(rom=bytes(rom), cart_kind=kind, ram_size=ram, seed=args.seed)
    d.emu.call(GB_INIT, d.emu.alloc(8))
    d.port.dll.gbd_init()
    d.randomize_all()
    d.sync_port_from_orig()
    return d


def set_cpu_fields(d, **fields):
    offsets = {'F': (0, 'B'), 'A': (1, 'B'), 'SP': (8, 'H'), 'PC': (10, 'H'), 'halted': (12, 'B'), 'IF': (0x30, 'B'),
               'IE': (0x31, 'B'), 'TAC': (0x25, 'B'), 'TIMA': (0x26, 'B'), 'TMA': (0x27, 'B'), 'DIV': (0x24, 'B'),
               'divAccum': (0x20, 'i'), 'timaAccum': (0x28, 'i'), 'doubleSpeed': (0x1d, 'B'), 'debugLog': (0x14, 'B')}
    for k, v in fields.items():
        off, fmt = offsets[k]
        d.write_both(2, struct.pack('<' + fmt, v), off)


def cmd_irq(args):
    d = new_differ(args)
    r, rep = d.rng, Report('irq')
    for case in range(args.cases):
        if case % 50 == 0:
            d.randomize_all()
        d.write_both(1, bytes([r.getrandbits(1), r.getrandbits(1), r.getrandbits(1)]), 0)  # imeRequest ime eiDelay
        set_cpu_fields(d, IF=r.getrandbits(8), IE=r.getrandbits(8), halted=r.getrandbits(1), debugLog=0,
                       SP=r.choice([r.randint(0xC002, 0xDFFE), r.randint(0xFF82, 0xFFFE), 0xFF10, 0x0000]),
                       PC=r.getrandbits(16))
        d.emu.put32(GB_APU_TIME, 0)
        a = d.emu.call(GB_SERVICE_INTERRUPTS) & 0xFFFFFFFF
        b = d.port.dll.gbd_service_interrupts() & 0xFFFFFFFF
        rep.check(d, f'case {case}', a, b)
    return rep.done()


def cmd_mem(args):
    status = 0
    for cart in ('romonly', 'mbc1', 'mbc3', 'mbc5'):
        d = new_differ(args, cart)
        r, rep = d.rng, Report(f'mem/{cart}')
        for case in range(args.cases):
            if case % 200 == 0:
                d.randomize_all()
                set_cpu_fields(d, debugLog=0)
            k = r.random()
            if k < 0.35:
                addr = r.randint(0xFF00, 0xFF7F)
            elif k < 0.45:
                addr = r.randint(0xFE00, 0xFFFF)
            elif k < 0.7:
                addr = r.choice([r.randint(0x0000, 0x7FFF), r.randint(0xA000, 0xBFFF)])
            else:
                addr = r.getrandbits(16)
            d.emu.put32(GB_APU_TIME, 0)
            if r.random() < 0.5:
                value = r.getrandbits(8)
                if cart != 'romonly' and addr < 0x2000 and r.random() < 0.5:
                    value = 0x0A  # enable cartridge RAM more often
                d.emu.call(GB_WRITE8, addr, value)
                d.port.dll.gbd_write8(addr, value)
                rep.check(d, f'write {addr:04x} = {value:02x}')
            else:
                a = d.emu.call(GB_READ8, addr) & 0xFF
                b = d.port.dll.gbd_read8(addr) & 0xFF
                rep.check(d, f'read {addr:04x}', a, b)
        status |= rep.done()
    return status


def cmd_timer(args):
    d = new_differ(args)
    r, rep = d.rng, Report('timer')
    for case in range(args.cases):
        set_cpu_fields(d, TAC=r.getrandbits(8), TIMA=r.getrandbits(8), TMA=r.getrandbits(8), DIV=r.getrandbits(8),
                       divAccum=r.randint(0, 300), timaAccum=r.randint(0, 1100), IF=r.getrandbits(8))
        ticks = r.choice([4, 8, 12, 16, 20, 24, 32, r.randint(0, 3000)])
        d.emu.call(GB_TIMER_STEP, ticks)
        d.port.dll.gbd_timer_step(ticks)
        rep.check(d, f'case {case} ticks {ticks}')
    return rep.done()


def cmd_lcd(args):
    d = new_differ(args)
    r, rep = d.rng, Report('lcd')
    for case in range(args.cases):
        if case % 20 == 0:
            d.randomize_all()
            set_cpu_fields(d, debugLog=0)
        io = bytearray(d.orig_block(0)[:0x1a])
        io[2] = r.randint(0, 143) if r.random() < 0.8 else r.randint(0, 255)          # LY
        io[3] = r.choice([io[2], r.getrandbits(8)])                                    # LYC
        io[5] = r.getrandbits(8) | (0x80 if r.random() < 0.95 else 0)                  # LCDC
        io[0x0d] = r.randint(0, 143)                                                   # windowLine
        io[0x11] &= 1
        io[0x12] = 1 if r.random() < 0.2 else 0                                        # hdmaActive
        struct.pack_into('<HHH', io, 0x14, r.getrandbits(16), (r.getrandbits(16) & 0x1FFF) | 0x8000, r.getrandbits(16))
        d.write_both(0, bytes(io))
        d.write_both(1, struct.pack('<i', r.randint(0, 460)), 0x28bd0)                 # lcdTickAccum
        ticks = r.choice([4, 8, 12, 24, 80, 172, 204, 456, r.randint(0, 600)])
        d.emu.put32(GB_APU_TIME, 0)
        d.emu.call(LCD_STEP, ticks)
        d.port.dll.gbd_lcd_step(ticks)
        rep.check(d, f'case {case} LY={io[2]} STAT={io[0x0e]:02x} LCDC={io[5]:02x} ticks={ticks}')
    return rep.done()


RAM_SIZES = [0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000]
KEY_NAMES = {'right': 0, 'left': 1, 'up': 2, 'down': 3, 'a': 4, 'b': 5, 'select': 6, 'start': 7}


def cart_params(rom):
    """Mapper choice as in cart_load @ 0x71000880e0 (constructors only, like the harness)."""
    ctype, ramcode = rom[0x147], rom[0x149]
    ram = RAM_SIZES[ramcode] if ramcode < len(RAM_SIZES) else 0
    if ctype == 0x00: return 0, 0, 0
    if ctype == 0x01: return 1, 0, 0
    if ctype in (0x02, 0x03): return 1, ram, 0
    if ctype in (0x0F, 0x11): return 3, 0, 1 if ctype == 0x0F else 0
    if ctype in (0x10, 0x12, 0x13): return 3, ram, 1 if ctype == 0x10 else 0
    if ctype in (0x19, 0x1C): return 5, 0, 0
    if ctype in (0x1A, 0x1B, 0x1D, 0x1E): return 5, ram, 0
    raise SystemExit(f'cart type {ctype:#04x} has no mapper in Carbon')


def parse_keys(spec):
    """'120=start*6,300=a*4' -> {frame: [(key, down)]}"""
    events = {}
    for item in filter(None, spec.split(',')):
        frame, rest = item.split('=')
        name, _, length = rest.partition('*')
        frame, length, key = int(frame), int(length or 4), KEY_NAMES[name.lower()]
        events.setdefault(frame, []).append((key, True))
        events.setdefault(frame + length, []).append((key, False))
    return events


class InstructionTracer:
    """Steps the port one gb_run_frame iteration each time the original enters gb_cpu_step, comparing as it goes."""

    def __init__(self, d, quick=True):
        self.d, self.quick = d, quick
        self.count = 0
        self.mismatch = None
        self.last_regs = None
        self.hook = d.emu.watch(GB_CPU_STEP, self._on_step)

    def _on_step(self, e):
        d = self.d
        if self.count > 0:
            d.port.dll.gbd_run_instruction()
            regs_o = d.orig_block(2)[:0x32]
            regs_p = d.port.read_block(2)[:0x32]
            if regs_o != regs_p or (not self.quick and d.diff(1)):
                self.mismatch = (self.count, d.diff(16), self.last_regs)
                e.request_stop()
                return
        self.last_regs = d.orig_block(2)[:12]
        self.count += 1

    def remove(self):
        self.d.emu.uc.hook_del(self.hook)


def cmd_frames(args):
    rom = open(args.rom, 'rb').read()
    kind, ram, rtc = cart_params(rom)
    keys = parse_keys(args.keys)

    def fresh():
        d = Differ(rom=rom, cart_kind=kind, ram_size=ram, rtc=rtc, seed=0)
        counter = d.emu.alloc(8)
        d.emu.call(GB_INIT, counter)
        d.port.dll.gbd_init()
        return d, counter

    def apply_keys(d, frame):
        for key, down in keys.get(frame, []):
            d.emu.call(GB_KEY_DOWN if down else GB_KEY_UP, key)
            (d.port.dll.gbd_key_down if down else d.port.dll.gbd_key_up)(key)

    d, counter = fresh()
    after_init = d.diff()
    if after_init:
        print('after gb_init:')
        for name, a, b in after_init:
            print(f'    {name}: orig={a:#x} port={b:#x}')
        return 1
    if (args.save_at < 0) != (args.load_at < 0) or args.load_at >= 0 and args.load_at <= args.save_at:
        raise SystemExit('--save-at and --load-at go together, with the load after the save')
    save_dir = tempfile.mkdtemp(prefix='gbdiff_state_')
    d.port.dll.gbd_set_save_root(save_dir.encode())
    state_file = os.path.join(save_dir, '0', '1', '.stt')  # game 0, slot 1; the test cartridge has no title

    def orig_read_state(e):
        """gb_load_state without the ARM iostreams: the same table the port reads, into the original globals."""
        data = open(state_file, 'rb').read()
        i = 0
        off, addr, size = ctypes.c_uint32(), ctypes.c_uint64(), ctypes.c_uint32()
        while d.port.dll.gbd_state_block(i, ctypes.byref(off), ctypes.byref(addr), ctypes.byref(size)):
            e.write(addr.value, data[off.value:off.value + size.value])
            i += 1
    d.emu.override(GB_LOAD_STATE_FILE, orig_read_state)

    t0 = time.time()
    audio_frames = 0
    loud_after_load = 0
    for frame in range(args.frames):
        apply_keys(d, frame)
        if frame == args.load_at:
            d.emu.call(GBSYSTEM_LOAD_STATE, 0, 1, 0)
            d.port.dll.gbd_load_state(1, 0)
            diffs = d.diff()
            if diffs:
                print(f'after GbSystem_LoadState before frame {frame}: MISMATCH')
                for name, a, b in diffs:
                    print(f'    {name}: orig={a:#x} port={b:#x}')
                return 1
            print(f'loaded the frame-{args.save_at} state before frame {frame}: identical')
        try:
            d.emu.call(GB_RUN_FRAME, counter, timeout_us=int(args.timeout * 1e6))
            done_orig = True
        except EmuTimeout:
            done_orig = False
        done_port = d.port.dll.gbd_run_frame_limited(3_000_000 if done_orig else 1) == 1
        if not done_orig:
            print(f'frame {frame}: original did not finish (LCD off?); stopping')
            break
        diffs = d.diff() if done_port else [('frame completed', 1, 0)]
        if d.emu.u32(counter) != d.port.dll.gbd_frame_count():
            diffs.append(('frameCount', d.emu.u32(counter), d.port.dll.gbd_frame_count()))
        buf = (ctypes.c_int16 * 0x10000)()
        n = d.port.dll.gbd_take_samples(buf, 0x10000)
        samples_port = bytes(buf)[:n * 2]
        if samples_port != bytes(d.samples_orig):
            k = next((i for i in range(min(len(samples_port), len(d.samples_orig))) if samples_port[i] != d.samples_orig[i]), None)
            diffs.append(('audio samples', len(d.samples_orig) // 2, n))
            if k is not None:
                diffs.append((f'first differing sample byte {k}', d.samples_orig[k], samples_port[k]))
        elif n:
            audio_frames += 1
        d.samples_orig.clear()
        if frame >= args.load_at >= 0 and samples_port:
            loud_after_load = max(loud_after_load, max(abs(v) for v in struct.unpack(f'<{n}h', samples_port)))
        if diffs:
            print(f'frame {frame}: MISMATCH')
            for name, a, b in diffs:
                print(f'    {name}: orig={a:#x} port={b:#x}')
            if args.trace:
                print(f'tracing frame {frame} instruction by instruction...')
                t, tc = fresh()
                for f in range(frame):
                    apply_keys(t, f)
                    t.emu.call(GB_RUN_FRAME, tc)
                    t.port.dll.gbd_run_frame_limited(3_000_000)
                apply_keys(t, frame)
                tracer = InstructionTracer(t)
                t.port.dll.gbd_frame_begin()
                try:
                    t.emu.call(GB_RUN_FRAME, tc, timeout_us=int(args.timeout * 1e6))
                except EmuTimeout:
                    pass
                if tracer.mismatch:
                    count, tdiffs, prev = tracer.mismatch
                    names = 'F A C B E D L H SP PC'.split()
                    pr = struct.unpack('<8BHH', prev)
                    print(f'  first divergence after instruction {count} (state before it: ' +
                          ' '.join(f'{nm}={v:02x}' for nm, v in zip(names, pr)) + ')')
                    for name, a, b in tdiffs:
                        print(f'    {name}: orig={a:#x} port={b:#x}')
                else:
                    print('  registers matched at every instruction; the difference is in state compared only per frame')
            return 1
        if frame == args.save_at:
            d.port.dll.gbd_save_state(1, 0)
        if frame % 50 == 49:
            print(f'frame {frame + 1}: identical ({(frame + 1) / (time.time() - t0):.1f} fps, {audio_frames} frames with audio)')
    print(f'{args.rom}: {frame + 1} frames identical (state blocks and audio samples)')
    if args.load_at >= 0:
        print(f'peak sample after the load: {loud_after_load}')
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('frames')
    p.add_argument('rom')
    p.add_argument('--frames', type=int, default=300)
    p.add_argument('--keys', default='')
    p.add_argument('--timeout', type=float, default=30)
    p.add_argument('--trace', action='store_true')
    p.add_argument('--save-at', type=int, default=-1)
    p.add_argument('--load-at', type=int, default=-1)
    for name, cases in (('cpu', 150), ('cb', 150), ('irq', 5000), ('mem', 5000), ('timer', 5000), ('lcd', 3000)):
        p = sub.add_parser(name)
        p.add_argument('--cases', type=int, default=cases)
        p.add_argument('--ops', default='')
        p.add_argument('--seed', type=int, default=1)
    args = ap.parse_args()
    handlers = {'cpu': cmd_cpu, 'cb': lambda a: cmd_cpu(a, cb=True), 'irq': cmd_irq, 'mem': cmd_mem,
                'timer': cmd_timer, 'lcd': cmd_lcd, 'frames': cmd_frames}
    sys.exit(handlers[args.cmd](args))


if __name__ == '__main__':
    main()
