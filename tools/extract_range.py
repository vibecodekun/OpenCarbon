"""Extract the functions of an address range from a Ghidra ExportAll.java dump (all.c).

usage: extract_range.py all.c out.c <start-hex> <end-hex>
"""
import re, sys

src, out, start, end = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4], 16)
marker = re.compile(r'^// ==== (\S+) @ ([0-9a-fA-F]+) size=(\d+)')
keep = False
count = 0
with open(src, encoding='utf-8', errors='replace') as f, open(out, 'w', encoding='utf-8') as o:
    for line in f:
        m = marker.match(line)
        if m:
            addr = int(m.group(2), 16)
            keep = start <= addr < end
            count += keep
        if keep:
            o.write(line)
print(f'{count} functions -> {out}')
