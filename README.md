<div align="center">

<a href="icon.jpg"><img src="icon.jpg" alt="Destinia Nintendo Switch" width="160"></a>

# destinia-nx

**Destinia for Nintendo Switch & PC**

An unofficial clean-room Nintendo Switch port and compatibility layer for the classic 2011 Action RPG  
**Destinia** (Gamevil, v1.06).

[![Switch](https://img.shields.io/badge/Nintendo_Switch-Homebrew-E60012?style=for-the-badge&logo=nintendoswitch&logoColor=white)](#)
[![Version](https://img.shields.io/badge/Version-0.7.3-4C8BF5?style=for-the-badge)](#)
[![Architecture](https://img.shields.io/badge/Architecture-ARMv5_Thumb_%2F_AArch64-0091BD?style=for-the-badge&logo=arm&logoColor=white)](#)
[![License](https://img.shields.io/badge/License-MIT-green?style=for-the-badge)](#)

**English** | [Русский](README_RU.md)

</div>

---

## About

`destinia-nx` is a native clean-room C port and compatibility layer that executes the ARM Thumb engine (`libdestinia_jni.so`) of **Destinia** (v1.06, 2011, GAMEVIL) on Nintendo Switch (Horizon OS) and PC.

Instead of running an Android emulator or virtual machine, the project embeds a lightweight interpreter with High-Level Emulation (HLE) of the Android and JNI platform services (framebuffer presentation, file I/O, audio streaming, input processing, and timing). Critical rendering hotspots (weapon blitters, alpha blending, screen darkeners) are replaced with verified native C routines for optimal performance.

**This repository and its releases contain no game code or assets.** You must supply your own legally acquired copy of `DESTINIA_v1_06.apk`. The native library inside the APK is verified at launch by SHA-256 (`8464fc90edab73ee67bf8528aa1d40a5a2a229a888ca8c8a14e9fbc74640d829`).

---

## Features & Improvements

- **Native 60 FPS Performance**: Optimized C blitters for sprite rendering (`Grp_drawWPNImage`, `Grp_drawWPNImage2`, `Grp_darkScreen`, `Grp_pxlMultiply`) ensure stable 60 frames per second without CPU throttling.
- **Console ARPG HUD Redesign**: Removed legacy mobile touch D-pads and virtual buttons when playing with a controller.
- **Two-Page Shift Hotbar (Hold L)**:
  - Quick-slot arc redesigned into a console-friendly horizontal hotbar.
  - Holding **L** seamlessly shifts between **Quick Slots 1–5** and **6–10** with matching button glyphs (`[ZL], [Y], [X], [R], [ZR]` $\leftrightarrow$ `[L+ZL], [L+Y], [L+X], [L+R], [L+ZR]`).
- **Full Controller & Touchscreen Support**: Native handling of Joy-Cons, Nintendo Switch Pro Controller, and capacitive touchscreen (direct menu tapping and virtual analog stick drag).
- **HD Rumble / Haptics**: Calibrated pulse haptics on attacks, critical strikes, incoming damage, and hotbar shifts.
- **Hardware VSync & Scaling**: Sharp integer-scaled 400×240 pixel art output at 720p (Handheld) and 1080p (Docked) with zero tearing.
- **Suspend & Resume Safety**: Full sleep/wake handling (`SDL_APP_WILLENTERBACKGROUND`); audio and vibration safely stop when entering the Home menu or putting the console to sleep.
- **Offline Single-Player**: Legacy network billing and server prompts are cleanly bypassed without hanging or requiring an internet connection.

---

## Controls

### Field & Combat

| Input | Action |
| :--- | :--- |
| **Left Stick / D-Pad** | Move Hero / Navigate Menus |
| **A** | Confirm / Attack / Interact / Talk |
| **B** | Cancel / Character Menu (in field) |
| **Minus (-)** | Toggle Mini Map |
| **Plus (+)** | System Menu (Save / Settings) |
| **Right Stick** | Move Virtual Pointer |
| **RS (R3 click)** | Capture screenshot (`shots/shot_NNNN.ppm`) |
| **Touchscreen** | Direct touch in menus / Drag left half of screen to walk |

### Quick Slots Hotbar

The quick slots use a two-page system. Hold **L** to switch to Page 2:

| Input | Page 1 (Neutral) | Page 2 (Holding L) |
| :--- | :--- | :--- |
| **ZL** | Quick Slot 5 (Potion / Consumable) | Quick Slot 10 (Consumable 2) |
| **Y** | Quick Slot 1 (Skill) | Quick Slot 6 (Skill) |
| **X** | Quick Slot 2 (Skill) | Quick Slot 7 (Skill) |
| **R** | Quick Slot 3 (Skill) | Quick Slot 8 (Skill) |
| **ZR** | Quick Slot 4 (Skill) | Quick Slot 9 (Skill) |
| **LS (L3 click)** | Quick Slot 5 (Direct access) | Quick Slot 10 (Direct access) |

---

## Installation & Running

### Requirements
- A Nintendo Switch running custom firmware (**Atmosphère** 1.0.0+).
- Legally acquired `DESTINIA_v1_06.apk` (Gamevil, 2011).

### Setup
1. Download the latest release from the [Releases](https://github.com/malinkin-s/destinia-nx/releases) page.
2. Copy the files to your microSD card so the folder structure looks like this:
   ```text
   sdmc:/switch/destinia/
   ├── destinia.nro
   └── DESTINIA_v1_06.apk
   ```

### Launching

You can run the game in two ways:

1. **Via Homebrew Menu**: Launch the Homebrew Menu (hold **R** on any installed game for full RAM access, or open Album) and select **Destinia**.
2. **Via Home Menu Forwarder (Recommended)**:
   - Install the included `Destinia [0100D3571A000000] [v0].nsp` using DBI, Tinfoil, TinWoo, or Sphaira (*"Install Forwarder"*).
   - Launch **Destinia** directly from the Nintendo Switch HOME menu.
   - When updating the game in the future, you only need to replace `destinia.nro` on your SD card.

---

## Build

### Nintendo Switch (.nro)

Building for Nintendo Switch uses Docker with devkitPro `devkita64`:

```bash
# Clean build
./tools/build_switch.sh clean

# Build NRO (outputs to port/destinia.nro)
./tools/build_switch.sh -j$(nproc)

# Package release zip
./tools/package.sh 0.7.3
```

### PC (Linux / macOS)

Requirements: GCC/Clang, SDL2, zlib, libm.

```bash
# Build desktop binary
make -C port -j$(nproc)

# Run game
./port/destinia --apk DESTINIA_v1_06.apk
```

### Regression Tests (Golden Frames)

To verify pixel-exact emulation against the Unicorn reference:

```bash
./port/tests/golden.sh DESTINIA_v1_06.apk
```

---

## Repository Layout

```text
destinia-nx/
├── port/
│   ├── src/               # Clean-room C interpreter, HLE dispatch, native blitters
│   ├── Makefile           # PC Linux build
│   ├── Makefile.switch    # Nintendo Switch libnx build
│   └── tests/             # Bit-exact regression tests
├── tools/
│   ├── build_switch.sh    # Docker devkitA64 compilation wrapper
│   ├── package.sh         # Release packaging script
│   └── build_forwarder.sh # Home Menu forwarder builder
├── docs/                  # Golden hashes and HLE platform specifications
├── harness/               # Unicorn reference verification scripts
├── icon.jpg               # Application icon
├── LICENSE                # MIT License
└── NOTICE.txt             # Third-party notices and licenses
```

---

## Credits

- **Original Game**: GAMEVIL Inc. (now Com2uS Holdings).
- **Nintendo Switch Port & Reverse Engineering**: [malinkin-s](https://github.com/malinkin-s) and contributors.
- **Reference & Inspiration**: aks796 ([a8retry_nx](https://github.com/aks796/a8retry_nx), android32).
- **Libraries & Toolchains**: [devkitPro](https://devkitpro.org/) (devkitA64, libnx), [libSDL2](https://www.libsdl.org/), SDL2_mixer.

---

## Contributing

Pull requests and bug reports are welcome. Please ensure that:
1. No copyrighted game code or binary assets are committed.
2. The golden regression test (`./port/tests/golden.sh`) passes with 700/700 frames identical.

---

## Disclaimer

This is an unofficial fan-made project and is **not** affiliated with, endorsed by, or sponsored by Nintendo or GAMEVIL / Com2uS Holdings.  
*Destinia* and all related characters and trademarks belong to their respective owners.  
**No copyrighted game code, binary libraries, or assets are distributed in this repository.**
