import re, sys
# usage: strs.py file [minlen]  -> prints "offset\tstring"
d = open(sys.argv[1], 'rb').read(); n = int(sys.argv[2]) if len(sys.argv) > 2 else 6
for m in re.finditer(rb'[\x20-\x7e\t]{%d,}' % n, d):
    sys.stdout.write('%08x\t%s\n' % (m.start(), m.group().decode('ascii')))
