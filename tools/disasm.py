import sys, capstone
img = open(r'C:\opencarbon\extracted\base\nso_flat\main.bin','rb').read()
BASE = 0x7100000000
a = int(sys.argv[1], 16); a = a - BASE if a >= BASE else a
n = int(sys.argv[2]) if len(sys.argv) > 2 else 60
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
for off in range(a, a + n * 4, 4):
    r = list(md.disasm(img[off:off+4], BASE + off))
    print(f"{BASE+off:#x}: " + (f"{r[0].mnemonic:7s} {r[0].op_str}" if r else f".word {int.from_bytes(img[off:off+4],'little'):#010x}"))
