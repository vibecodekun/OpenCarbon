import sys, capstone, collections
from capstone.arm64 import ARM64_OP_IMM
img = open(r'C:\opencarbon\extracted\base\nso_flat\main.bin','rb').read()
TEXT_END = 0x111760
want = {int(x,0) for x in sys.argv[1:]}
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); md.detail = True
hits = collections.defaultdict(list)
# decode 4 bytes at a time (fixed width) so data islands don't desync us
for off in range(0, TEXT_END, 4):
    for ins in md.disasm(img[off:off+4], off):
        for op in ins.operands:
            if op.type == ARM64_OP_IMM and op.imm in want:
                hits[op.imm].append((off, ins.mnemonic, ins.op_str))
for k in sorted(hits):
    print(f"== {k:#x} ({k}) : {len(hits[k])} hits")
    for off, m, s in hits[k][:40]:
        print(f"   0x71{off+0x00000000:08x}  {m} {s}")
