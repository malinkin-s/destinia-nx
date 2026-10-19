#!/usr/bin/env /usr/bin/python3
"""contact.py OUT.png frame_dir N1 N2 ... : montage of selected frames (3 per row)."""
import sys, os
from PIL import Image
out, d, nums = sys.argv[1], sys.argv[2], sys.argv[3:]
ims = [Image.open(os.path.join(d, f'frame_{int(n):04d}.png')) for n in nums]
cols = 3; rows = (len(ims) + cols - 1) // cols
m = Image.new('RGB', (400 * cols, 240 * rows))
for i, im in enumerate(ims): m.paste(im, ((i % cols) * 400, (i // cols) * 240))
m.save(out)
