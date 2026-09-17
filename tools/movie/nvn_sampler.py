"""Which filter does the startup movie's sampler get? Runs the NVN driver's own code (it lives in the sdk module) in
Unicorn with the calls Carbon's movie renderer makes (@ 0x71001071ec..0x7100107238: SetDefaults, SetWrapMode(7,7,7),
Initialize), then builds the GPU sampler descriptor the way nvnSamplerPoolRegisterSampler does, and decodes it with
Ryujinx's Maxwell rules (src/Ryujinx.Graphics.Gpu/Image/SamplerDescriptor.cs).

The driver functions are found by name through the table nvnDeviceGetProcAddress searches (sdk @ 0x45fb00: 516 names,
offsets @ 0xac3d90 into a pool @ 0xac06df, function pointers at the slot the GOT entry 0xc6edb8 points to).
"""
import os, struct, sys
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_UNMAPPED
from unicorn.arm64_const import UC_ARM64_REG_CPACR_EL1, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_X0

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from nsosyms import Nso  # noqa: E402

SDK = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'extracted', 'base', 'nso_flat', 'sdk.bin')
PROC_COUNT, PROC_NAME_OFFSETS, PROC_NAME_POOL, PROC_TABLE_GOT = 0x204, 0xac3d90, 0xac06df, 0xc6edb8
DESCRIPTOR_BUILDER = 0x485ab0  # called by nvnSamplerPoolRegisterSampler with (device, sampler + 8, out[32])


def nvn_procs(n):
    table = n.ptr(PROC_TABLE_GOT)
    procs = {}
    for i in range(PROC_COUNT):
        off = struct.unpack_from('<I', n.img, PROC_NAME_OFFSETS + 4 * i)[0]
        name = n.img[PROC_NAME_POOL + off:n.img.index(b'\0', PROC_NAME_POOL + off)].decode()
        procs['nvn' + name] = n.ptr(table + 8 * i)
    return procs


def main():
    n = Nso(SDK)
    procs = nvn_procs(n)
    # sanity check of the lookup: nvnDeviceGetSeparateSamplerHandle is "sbfiz x0, x1, #20, #32; ret" (id << 20)
    assert n.img[procs['nvnDeviceGetSeparateSamplerHandle']:][:8] == bytes.fromhex('207c6c93c0035fd6')

    base = 0x10000000
    img = bytearray(n.img)
    for slot, r in n.rel.items():
        if isinstance(r, int):
            struct.pack_into('<Q', img, slot, base + r)
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(base, ((len(img) + 0xFFFF) & ~0xFFFF) + 0x1000000)
    uc.mem_write(base, bytes(img))
    stack, heap, ret = 0x80000000, 0x90000000, 0x7F000000
    for region in (stack, heap, ret):
        uc.mem_map(region, 0x100000)
    uc.mem_write(ret, struct.pack('<I', 0xD4200000))
    uc.reg_write(UC_ARM64_REG_CPACR_EL1, 0x300000)
    faults = []
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, lambda u, access, addr, size, value, data: faults.append(addr) or False)

    def call(fn, *args):
        for i, a in enumerate(args):
            uc.reg_write(UC_ARM64_REG_X0 + i, a)
        uc.reg_write(UC_ARM64_REG_SP, stack + 0xF0000)
        uc.reg_write(UC_ARM64_REG_LR, ret)
        uc.emu_start(base + fn, ret)
        if faults:
            raise SystemExit(f'unmapped access at {faults[0]:#x}')

    builder, sampler, device, out = heap, heap + 0x1000, heap + 0x2000, heap + 0x5000
    call(procs['nvnSamplerBuilderSetDefaults'], builder)
    call(procs['nvnSamplerBuilderSetDevice'], builder, device)
    call(procs['nvnSamplerBuilderSetWrapMode'], builder, 7, 7, 7)
    call(procs['nvnSamplerInitialize'], sampler, builder)
    min_f, mag_f, ws, wt, wr = struct.unpack_from('<5i', uc.mem_read(builder, 0x20), 8)
    print(f'builder after SetDefaults + SetWrapMode(7,7,7): minFilter {min_f}, magFilter {mag_f}, wrap {ws} {wt} {wr}')
    call(DESCRIPTOR_BUILDER, device, sampler + 8, out)
    w0, w1, w2, w3 = struct.unpack('<4I', uc.mem_read(out, 16))
    print(f'GPU sampler descriptor: {w0:#010x} {w1:#010x} {w2:#010x} {w3:#010x}')
    mag = {1: 'NEAREST', 2: 'LINEAR'}[w1 & 3]
    minf = {1: 'NEAREST', 2: 'LINEAR'}[(w1 >> 4) & 3]
    mip = {1: 'NONE', 2: 'NEAREST', 3: 'LINEAR'}[(w1 >> 6) & 3]
    modes = ['REPEAT', 'MIRRORED_REPEAT', 'CLAMP_TO_EDGE', 'CLAMP_TO_BORDER', 'CLAMP', 'MIRROR_CLAMP_TO_EDGE',
             'MIRROR_CLAMP_TO_BORDER', 'MIRROR_CLAMP']
    print(f'decoded: mag {mag}, min {minf}, mip {mip}, address {modes[w0 & 7]} / {modes[(w0 >> 3) & 7]} / {modes[(w0 >> 6) & 7]}')


if __name__ == '__main__':
    main()
