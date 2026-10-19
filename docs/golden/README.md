Golden reference for bit-exact testing (frames are 400x240 RGB565, md5 of the 192000-byte buffer).
`newgame3_700f.md5` was produced by the Unicorn harness (harness/run_frames.py) and is reproduced exactly
by the C interpreter (port/tests/golden.sh): 700/700 frames identical. The input script is `newgame3.txt`.
