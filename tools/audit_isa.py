#!/usr/bin/python3 -I
"""Static audit: which Thumb-1 encoding classes (top 10 bits) occur in the game library's code, with a sample address each.
Usage: audit_isa.py libdestinia_jni.so symbols_raw.txt > classes.json"""
import sys, json, struct
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
so = open(sys.argv[1], 'rb').read()
funcs = []
for ln in open(sys.argv[2]):
    p = ln.split()
    if len(p) >= 8 and p[3] == 'FUNC' and int(p[2]) > 0:
        funcs.append((int(p[1], 16) & ~1, int(p[2])))
cs = Cs(CS_ARCH_ARM, CS_MODE_THUMB); cs.detail = False
classes = {}
for addr, size in sorted(funcs):
    code = so[addr:addr + size]; pool = set()
    for i in cs.disasm(code, addr):
        if i.mnemonic == 'ldr' and 'pc' in i.op_str:        # literal-pool word(s) are data, skip them when sweeping
            try:
                imm = int(i.op_str.split('#')[-1].strip(' ]'), 0); pool.add(((i.address + 4) & ~3) + imm)
            except ValueError: pass
    for i in cs.disasm(code, addr):
        if any(i.address in (p, p + 2) for p in pool): continue
        if i.size == 4: key = '110'
        else:
            op = struct.unpack('<H', so[i.address:i.address + 2])[0]; key = '%03x' % ((op >> 6) if (op >> 10) == 0x10 else (op >> 8))
        c = classes.setdefault(key, {'n': 0, 'mn': set(), 'ex': i.address})
        c['n'] += 1; c['mn'].add(i.mnemonic)
for c in classes.values(): c['mn'] = sorted(c['mn'])
json.dump(classes, sys.stdout, indent=0)
print(len(classes), 'classes', file=sys.stderr)
