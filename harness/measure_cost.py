#!/usr/bin/env /usr/bin/python3
"""Estimate guest instructions per jniRun frame in different game states (basic-block counting)."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from host import Game, jarr, ROOT
from unicorn import UC_HOOK_BLOCK

ev = {}
for ln in open(os.path.join(ROOT, 'harness/scripts/newgame4.txt')):
    p = ln.split(); ev.setdefault(int(p[0]), []).append((int(p[2], 0), int(p[3])))
g = Game(os.path.join(ROOT,'work/apk/lib/armeabi/libdestinia_jni.so'), os.path.join(ROOT,'work/apk/assets'), os.path.join(ROOT,'work/savedata_cost'))
g.init(); buf = jarr(g.e, bytes(192000))
cnt = [0, 0]
def blk(uc, addr, size, ud): cnt[0] += size; cnt[1] += 1
h = g.e.uc.hook_add(UC_HOOK_BLOCK, blk)
res = []
for f in range(1500):
    for c, d in ev.get(f, []): g.key(c, bool(d))
    cnt[0] = cnt[1] = 0
    g.run(buf)
    res.append((f, cnt[0], cnt[1]))
import statistics
for name, lo, hi in [('prelogo',1,40),('title',45,70),('menu',90,125),('charsel',135,195),('cutscene',210,600),('field/battle',900,1500)]:
    seg=[b for f,b,n in res if lo<=f<hi]
    print(f'{name:13s} frames {lo}-{hi}: bytes executed/frame median {statistics.median(seg):9.0f}  max {max(seg):9d}  (~instr median {statistics.median(seg)/3:9.0f}, max {max(seg)/3:9.0f})')
