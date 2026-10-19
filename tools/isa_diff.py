#!/usr/bin/python3 -I
"""Differential test of the C interpreter against Unicorn, one instruction at a time.
Every distinct instruction word found in the game library's code is executed on several random register states.
Usage: isa_diff.py libdestinia_jni.so symbols_raw.txt port/isatest [trials] [seed]"""
import sys, struct, random, subprocess, zlib, os, tempfile
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UcError, UC_ERR_FETCH_UNMAPPED
from unicorn.arm_const import *

so = open(sys.argv[1], 'rb').read(); isatest = sys.argv[3]
TRIALS = int(sys.argv[4]) if len(sys.argv) > 4 else 8
rng = random.Random(int(sys.argv[5]) if len(sys.argv) > 5 else 1)
CODE, DATA, DSIZE = 0x10000000, 0x20000000, 0x2000

def pattern(n, salt): return bytes((i * 167 + (i >> 8) * 31 + salt) & 255 for i in range(n))
CODE_PAT, DATA_PAT = pattern(0x200, 13), pattern(DSIZE, 77)

# ---- distinct instruction words in code ----
funcs = []
for ln in open(sys.argv[2]):
    p = ln.split()
    if len(p) >= 8 and p[3] == 'FUNC' and int(p[2]) > 0: funcs.append((int(p[1], 16) & ~1, int(p[2])))
cs = Cs(CS_ARCH_ARM, CS_MODE_THUMB); words = {}
for addr, size in sorted(funcs):
    code = so[addr:addr + size]; pool = set()
    for i in cs.disasm(code, addr):
        if i.mnemonic == 'ldr' and 'pc' in i.op_str:
            try: pool.add(((i.address + 4) & ~3) + int(i.op_str.split('#')[-1].strip(' ]'), 0))
            except ValueError: pass
    for i in cs.disasm(code, addr):
        if any(i.address in (p, p + 2) for p in pool): continue
        w = struct.unpack('<%dH' % (i.size // 2), so[i.address:i.address + i.size])
        words.setdefault(w, (i.address, i.mnemonic + ' ' + i.op_str))
cases_w = sorted(words)
print(len(cases_w), 'distinct instruction words', file=sys.stderr)

def rand_regs():
    r = []
    for k in range(16):
        x = rng.random()
        if k < 8 or k == 13:
            if k == 13 or x < 0.5: v = DATA + 0x400 + rng.randrange(0, 0x1000, rng.choice([1, 2, 4]))
            elif x < 0.75: v = rng.randrange(0, 64)
            else: v = rng.choice([0, 0xffffffff, 0x80000000, 0x7fffffff, rng.getrandbits(32)])
            if k == 13: v &= ~3
        else: v = rng.choice([0, 1, 0xffffffff, 0x80000000, rng.getrandbits(32), DATA + 0x800])
        r.append(v)
    r[15] = 0
    return r

cases = []
for w in cases_w:
    for _ in range(TRIALS): cases.append((w, rand_regs(), rng.getrandbits(4)))
tmp = tempfile.mkdtemp()
with open(tmp + '/c.bin', 'wb') as f:
    f.write(struct.pack('<I', len(cases)))
    for w, regs, fl in cases:
        f.write(struct.pack('<2H', w[0], w[1] if len(w) > 1 else 0)); f.write(struct.pack('<16I', *regs)); f.write(bytes([fl]))
subprocess.run([isatest, tmp + '/c.bin', tmp + '/r.bin'], check=True)
res = open(tmp + '/r.bin', 'rb').read(); REC = 1 + 64 + 1 + 1 + 4

# ---- reference ----
uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB); uc.mem_map(CODE, 0x1000); uc.mem_map(DATA, DSIZE)
REGS = [UC_ARM_REG_R0 + i for i in range(13)] + [UC_ARM_REG_SP, UC_ARM_REG_LR]
bad = 0; shown = 0; by_mn = {}
for k, (w, regs, fl) in enumerate(cases):
    o = res[k * REC:(k + 1) * REC]; cf = o[0]
    uc.mem_write(CODE, CODE_PAT); uc.mem_write(CODE + 0x100, struct.pack('<%dH' % len(w), *w)); uc.mem_write(DATA, DATA_PAT)
    uc.ctl_remove_cache(CODE + 0x100, CODE + 0x108)
    uc.reg_write(UC_ARM_REG_CPSR, (fl << 28) | 0x20 | 0x10)
    for i, rr in enumerate(REGS): uc.reg_write(rr, regs[i])
    try: uc.emu_start((CODE + 0x100) | 1, 0, count=1); ufault = 0
    except UcError as e: ufault = 0 if e.errno == UC_ERR_FETCH_UNMAPPED else 1       # branch into unmapped memory = the branch itself worked
    if cf != ufault:
        why = 'fault-mismatch C=%d U=%d' % (cf, ufault)
    elif cf:
        continue
    else:
        why = None
        cr = struct.unpack('<16I', o[1:65]); cfl = o[65]; cthumb = o[66]; ccrc = struct.unpack('<I', o[67:71])[0]
        ur = [uc.reg_read(rr) for rr in REGS] + [uc.reg_read(UC_ARM_REG_PC)]
        cpsr = uc.reg_read(UC_ARM_REG_CPSR); ufl = cpsr >> 28 & 15; uthumb = cpsr >> 5 & 1
        ucrc = zlib.crc32(bytes(uc.mem_read(DATA, DSIZE)))
        for i in range(15):
            if cr[i] != ur[i]: why = 'r%d C=%08x U=%08x' % (i, cr[i], ur[i]); break
        if not why and (cr[15] & ~1) != (ur[15] & ~1): why = 'pc C=%08x U=%08x' % (cr[15], ur[15])
        if not why and cfl != ufl: why = 'flags C=%x U=%x' % (cfl, ufl)
        if not why and cthumb != uthumb: why = 'thumb C=%d U=%d' % (cthumb, uthumb)
        if not why and ccrc != ucrc: why = 'memory differs'
    if why:
        bad += 1; mn = words[w][1].split()[0]; by_mn[mn] = by_mn.get(mn, 0) + 1
        if shown < 25:
            shown += 1; print('MISMATCH', ' '.join('%04x' % x for x in w), words[w][1], '@%x' % words[w][0], why, 'regs=' + ' '.join('%x' % x for x in regs[:8]), 'fl=%x' % fl)
nf = sum(1 for k in range(len(cases)) if res[k * REC] == 0)
print('%d cases, %d ran without fault, %d mismatches' % (len(cases), nf, bad), by_mn)
sys.exit(1 if bad else 0)
