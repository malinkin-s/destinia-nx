#!/usr/bin/env /usr/bin/python3
"""Run N frames of Destinia on the host and dump framebuffer PNGs.

  run_frames.py [--frames N] [--every K] [--out DIR] [--trace] [--script FILE]

script lines: "<frame> touch X Y T" (T: 0 down,1 up,2 move,3 menu)
"""
import argparse, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from host import Game, jarr, ROOT
from PIL import Image

W, H = 400, 240

def rgb565_png(buf, path):
    img = Image.new('RGB', (W, H))
    px = img.load()
    for i in range(W * H):
        v = buf[2 * i] | (buf[2 * i + 1] << 8)
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        px[i % W, i // W] = ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
    img.save(path)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--frames', type=int, default=30)
    ap.add_argument('--every', type=int, default=10)
    ap.add_argument('--out', default=os.path.join(ROOT, 'work', 'frames'))
    ap.add_argument('--save', default=os.path.join(ROOT, 'work', 'savedata'))
    ap.add_argument('--manifest', help='write md5 of every frame to this file')
    ap.add_argument('--all', action='store_true', help='dump every frame')
    ap.add_argument('--trace', action='store_true')
    ap.add_argument('--script')
    ap.add_argument('--tracef', default='', help='comma list of internal function names to trace')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    so = os.path.join(ROOT, 'work/apk/lib/armeabi/libdestinia_jni.so')
    assets = os.path.join(ROOT, 'work/apk/assets')
    g = Game(so, assets, a.save, trace=a.trace)
    if a.tracef: g.e.trace_funcs(a.tracef.split(','))
    events = {}
    if a.script:
        for ln in open(a.script):
            p = ln.split()
            if len(p) == 5 and p[1] == 'touch': events.setdefault(int(p[0]), []).append(('touch',) + tuple(map(int, p[2:])))
            if len(p) == 4 and p[1] == 'warp': events.setdefault(int(p[0]), []).append(('warp', int(p[2]), int(p[3])))
            if len(p) == 4 and p[1] == 'key': events.setdefault(int(p[0]), []).append(('key', int(p[2], 0), int(p[3])))
    t0 = time.time()
    print('initGame ->', g.init(), f'({time.time()-t0:.2f}s)')
    buf = jarr(g.e, bytes(192000))
    import hashlib
    manifest = []
    for f in range(a.frames):
        for ev in events.get(f, []):
            if ev[0] == 'touch': g.touch(*ev[1:])
            elif ev[0] == 'warp': g.warp(ev[1], ev[2])
            else: g.key(ev[1], bool(ev[2]))
        t1 = time.time()
        sleep = g.run(buf)
        manifest.append(f'{f} {hashlib.md5(bytes(g.e.handles[buf][1])).hexdigest()}')
        if a.all: rgb565_png(bytes(g.e.handles[buf][1]), os.path.join(a.out, f'frame_{f:04d}.png'))
        bn = g.banner()
        if f % a.every == 0 or f == a.frames - 1:
            data = bytes(g.e.handles[buf][1])
            rgb565_png(data, os.path.join(a.out, f'frame_{f:04d}.png'))
            nz = sum(1 for b in data if b)
            print(f'frame {f}: jniRun->{sleep}ms banner={bn} nonzero_bytes={nz} ({time.time()-t1:.3f}s)')
        if sleep == 0:
            print('game requested exit'); break
    if a.manifest: open(a.manifest, 'w').write('\n'.join(manifest) + '\n')
    print('unimplemented imports:', sorted(g.e.unimpl))
    with open(os.path.join(a.out, 'trace.log'), 'w') as fh: fh.write('\n'.join(g.e.log_lines))

if __name__ == '__main__':
    main()
