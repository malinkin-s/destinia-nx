# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`destinia-nx`: a clean-room Nintendo Switch (and PC) port of Destinia (Gamevil, 2011, Android v1.06). It does not recompile the game. A 64-bit NRO embeds an ARMv5/Thumb-1 interpreter that runs the original `libdestinia_jni.so` read from the player's own `DESTINIA_v1_06.apk` at runtime. **No game code or assets may ever be committed or shipped** (`.gitignore` blocks `*.apk`, `*.so`, `work/`, `ref/`, `dist/*.zip|nro`). The library's SHA-256 is checked at launch (`LIB_SHA256` in `port/src/main.c`, bypass with `--skip-check`).

## Commands

```bash
# PC build (SDL2, zlib; SDL2_mixer optional, otherwise audio_stub.c = silent)
make -C port -j$(nproc)                       # -> port/destinia
./port/destinia --apk DESTINIA_v1_06.apk      # run; add --headless --frames N --script FILE for scripted runs

# Switch NRO (runs devkitpro/devkita64 in Docker; build.sh is a wrapper)
./tools/build_switch.sh -j$(nproc)            # -> port/destinia.nro (pass `clean` to clean)
./tools/package.sh 1.0.0                      # release zip in dist/, aborts if APK data is found in the NRO
./tools/build_forwarder.sh                    # NSP/NSZ forwarder via hacBrewPack (VERSION is hard-coded in the script)

# Regression test (the only test gate): must print "PASS: 700/700 frames identical"
./port/tests/golden.sh [APK]

# Interpreter ISA differential test vs Unicorn (needs /usr/bin/python3 with unicorn+capstone, not the mise python)
make -C port isatest
/usr/bin/python3 -I tools/isa_diff.py <libdestinia_jni.so> docs/symbols_raw.txt port/isatest [trials] [seed]
```

Other `destinia` flags: `--hle`, `--trace`, `--profile`, `--dump`, `--manifest FILE` (per-frame md5s), `--save DIR`, `--hide-pad/--show-pad`, `--tour*`, `--portals`. Python tooling in `harness/` (Unicorn reference runner `run_frames.py`, `host.py`) and `tools/` must run with `/usr/bin/python3`. Input scripts are lines of `<frame> key <code> <0|1>` / `<frame> touch X Y T` (see `harness/scripts/`).

## Architecture

The guest library is a WIPI-emulation layer: the entire platform boundary is 28 libc imports, 12 JNI entry points and a few Java callbacks (full map in `docs/platform_interface.md`). Rendering is software into a 400x240 RGB565 framebuffer; `jniRun` returns the delay to the next frame in ms (0 = quit).

`port/src/`:
- `cpu.c` / `guest.h`: Thumb-1 + ARM interpreter and paged guest memory. The guest address map (`LIB_BASE`, `HEAP_BASE`, stubs, JNI tables) deliberately mirrors the Unicorn harness so pointer values and frame hashes are comparable.
- `hle.c`: implements the libc imports and a fake JNIEnv/JavaVM as trap addresses in C.
- `game.h`: host-independent API (`game_load/init/run/key/touch/frame`, `GameHost` callbacks for assets/sound/vibration/clock). `main.c` is the SDL2 frontend (video, audio, gamepad/touch, HUD overlay, CLI, SHA check); `apk.c` reads assets from the zip; `audio.c` uses SDL2_mixer.
- `native.c`: two kinds of hooks keyed by guest symbol name (the `.so` keeps 611 symbols):
  - `DEFS`: native C replacements for hot blitters (`Grp_drawWPNImage`, `Grp_darkScreen`, `_Grp_drawAura16`, `Grp_closeupScreen2`, ...) for 60 FPS. New ones: profile first (`PROFILE_FROM/TO ... --profile N` on a combat script), then check with `--hle 40` (verify mode, slow: ~30 min for 4800 frames) or the faster manifest diff against `--hle -1`. In verify mode (`game_set_natives(N)`) each is checked against the real guest code.
  - `PATCHES`: behavior changes (`MMain_drawTouchButton` hides the touch pad, `GameUI_drawStatus` / `GameUI_drawQuickSlot` draw the console HUD/hotbar).
- Input quirks (details in `NOTES.md`/`docs/platform_interface.md`): direction keys are 0xd up/0xe down/0xf left/0x10 right; the game drops movement when any direction key is released so the rest are re-pressed, and a dummy key tap is inserted on direction changes to avoid triggering a dash (same key twice within 300 ms). Key 0x11 is OK.
- Ads, IAP and network are no-op host stubs, except the Special Shop: `Comm_buyCash` is hooked and Coins are bought with Gold (`economy_*` in `native.c`, numbers in `docs/economy.md`). `gametxt_en.tdt` is rewritten when read in `host_asset_read`; strings are matched by exact content, so a changed APK text simply stops matching.
- Host UI helpers in `main.c` that read guest state: `menu_tab_step` (L/R tabs; `GameUI` = `*(LIB_BASE+0x441d4)`, `+0x78` focus depth, `+0x7a` tab*4096, `+0x7c` menu state word), `resume_walk` (re-presses held direction when the hero idles), `native_draw_title_overlay` (title menu selection = `*(LIB_BASE+0x4439c)+0xcc`). The game starts walking only on a key *press*, never on a held key.
- Debug env vars (headless): `HERODBG=<frame>` per-frame hero/menu state, `SETGOLD=<frame>:<n>`, `GOLDDBG=<frame>`, `PROFILE_FROM/TO` with `--profile`, `FRAME_STATS`. Script events: `field` (warp into the field), `button <pad button> <0|1>`. Guest struct offsets found this way are only valid for this exact library build.

## Workflow rules

- **Bit-exactness is the invariant.** Any change to `cpu.c`, `hle.c`, or a `DEFS` native must keep `golden.sh` at 700/700 (`docs/golden/newgame3_700f.md5`, produced by the Unicorn reference). Patches that intentionally alter output (HUD, hidden pad) are gated so the golden script still passes; check this when touching `PATCHES`.
- Never put APK-derived data in the repo; `work/` and `ref/` are scratch for extracted assets/decompilation and are git-ignored.
- The Switch build can only be verified compile-wise here; console behavior is tested by the user (logs go in `logs_nx/`).
- Staged workflow requested by the user: finish one stage fully, describe the next stage and ask before starting it, report done/works/next afterwards.
- The release version is set in several places (the `package.sh` argument and `VERSION` in `tools/build_forwarder.sh`); keep them in sync when bumping.
- `NOTES.md` (Russian) holds the original reverse-engineering notes; `README_RU.md` mirrors `README.md`, so update both when changing user-facing docs.
