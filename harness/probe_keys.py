#!/usr/bin/env /usr/bin/python3
"""Find which WIPI key codes advance the title screen: press each code, see if the frame changes."""
import os, sys, hashlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from host import Game, jarr, ROOT
from run_frames import rgb565_png

g = Game(os.path.join(ROOT,'work/apk/lib/armeabi/libdestinia_jni.so'), os.path.join(ROOT,'work/apk/assets'), os.path.join(ROOT,'work/savedata'))
g.init(); buf = jarr(g.e, bytes(192000))
def step(n=1):
    for _ in range(n): g.run(buf)
    return bytes(g.e.handles[buf][1])
step(60)
base = step(5)
def sig(b): return hashlib.md5(b).hexdigest()[:8]
# the title has an animated blinking "TAP TO START": compare against a few idle frames to know the idle set
idle = {sig(step(1)) for _ in range(40)}
print('idle signatures:', len(idle))
for code in [0x11,0x15,0x2,0xb,0x1,0xa,0xc,0xd,0xe,0xf,0x10,0x14,0x25,0x26,0x27,0x28,0x29,0x2a,0x2b,0x2c,0x3,0x4,0x5,0x6,0x7,0x8,0x9,0x1b]:
    g.key(code, True); step(2); g.key(code, False); b = step(6)
    ch = sig(b) not in idle
    print(f'key {code:#x}: {"CHANGED" if ch else "same"}')
    if ch:
        rgb565_png(b, os.path.join(ROOT, f'work/frames/key_{code:02x}.png'))
        break
