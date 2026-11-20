# Zone of the Enders Recompiled

A static recompilation of *Zone of the Enders* (PlayStation 2, USA, SLUS-20148) into native PC code, with a Vulkan
renderer and a set of modern extras: higher resolutions, widescreen, antialiasing, a settings menu inside the game,
and the Japanese voices with English, French, German or Italian subtitles.

> This project contains **no game data, no game code and no keys**. You need your own copy of the game (USA disc).
> The download holds a translator and a compiler: on the first start it translates **your** disc's program to C++
> and compiles it on your PC.

> **AI disclosure:**  AI assistance yes (Claude Code by Anthropic).

---

## The first port of ps2recomp-option-p

This port is first and foremost the first game of **ps2recomp-option-p**, my fork of the ps2-port-engine toolkit
([ps2recomp-option-p](https://github.com/almost-drowned-rat/ps2recomp-option-p)).

**What option P does differently:** the game's program is translated to C++ and the translation is *checked* -
run side by side with an interpreter of the PS2's processor on the game's own data, or proven equal to a form that
was. The PS2's vector unit programs and the sound processor's modules are translated ahead of time too. The
translation and the compile run on your own PC from your own disc image, so the download carries no game code.

**Comparison** with an independently developed recompilation of the same game: Yaz-Myst's
[ZOE-Recompiled](https://github.com/Yaz-Myst/ZOE-Recompiled) v1.1.0, its latest version, built from source. I measured
both on 2026-11-19 in one session on the same machine (laptop with an RTX 3050 Ti), each at the PS2's own resolution,
windowed, 60 Hz; nothing else running. CPU in cores busy (means); GPU in milliseconds per frame (read for the whole
GPU, desktop included):

| | Zone of the Enders Recompiled | ZOE-Recompiled v1.1.0 |
|---|---|---|
| CPU, fight (first battle) | 1.12 | 1.72 |
| CPU, in-engine cutscene (hangar) | 1.09 | 1.61 |
| CPU, title movie | 0.37 | 0.86 |
| GPU, fight | 3.5 ms | 2.0 ms |
| GPU, in-engine cutscene | 3.5 ms | 2.3 ms |
| GPU, title movie | 0.8 ms | 0.8 ms |

Both hold the game's 60 pictures a second in every scene. On a four-core machine (the Steam Deck's count) the CPU
results come out alike: 29-57 % less for this port. Since that measurement this port's GPU time in the fight went down
by about a fifth (textures written by the game itself are re-decoded only where they changed).

**Only a benchmark, not a competition.** I measure against ZOE-Recompiled because it is the best way to see what
option P brings - not to rank anyone's work. Its renderer is leaner on the GPU than mine; both projects are young
and will change, and these are numbers from one machine and one scene set.

---

## What you need

- Windows 10 or 11, 64-bit, a processor with AVX2, a graphics card with a **Vulkan 1.3** driver
- A disc image of **Zone of the Enders (USA, SLUS-20148)** that you made from your own disc (ISO)
- Optional: a disc image of the Japanese release (**SLPM-65019**) for the Japanese voices
- About 1 GB of free space and ~10 minutes for the first start (the translation and the compile run once)

## Getting started

1. Download `ZOE.exe` from the [Releases](../../releases) page and put it in a folder you can write to (not
   *Program Files*) - the game keeps its saves and settings beside it.
2. Start it. A small window asks for your disc image (and, if you like, the Japanese one) and checks them.
3. Press **Go**. The first start translates and compiles the game; after that it starts straight away.

Your disc images are only read, never changed or deleted.

## Features

### Native port, no emulator
- The PS2's program is translated to C++ ahead of time and compiled for the PC - no CPU emulation at run time.
- The PS2's vector units and the game's sound processor run as translated code too; the GS is drawn with
  **Vulkan**, window, input and sound through **SDL3**.

### RECOMP OPTIONS - in the game's own OPTIONS menu
The OPTIONS screen shows VIBRATION, SOUND, **RECOMP OPTIONS** and EXIT. RECOMP OPTIONS opens pages drawn in the
game's own style:

| Page | Lines |
|---|---|
| POST PROCESSING | antialiasing (FXAA, SMAA, SSAA 2x / 4x), anisotropic filter, bloom + strength, upscaling (bilinear / FSR 1) |
| DISPLAY | display mode (window / fullscreen / borderless), render resolution (standard sizes up to 5x), aspect ratio (4:3, 16:9, 16:10, 21:9 - the picture widened, never stretched), field of view |
| FRAME RATE | V-sync, frame pacing, FPS counter |
| SHADERS | shader compile mode, shader notice |
| LANGUAGE | subtitles in cutscenes / in gameplay, voices (English / Japanese), subtitle language |

Up / down chooses a line, left / right changes it, **Triangle** sets a line back to its default.

### Languages and the Japanese voices
- Subtitles in **English, French, German or Italian** - all four are on the US disc already.
- With your Japanese disc image set, **VOICES** offers the original Japanese voice cast. The European subtitles are
  moved onto the Japanese lines' timing; the movies with speech come in their Japanese version.

### Controls
- A controller works as a DualShock 2, **with rumble**.
- Keyboard (by key position, the same on every layout): W A S D left stick, arrow keys D-pad, Enter / Backspace
  Start / Select, I / J / K / L Triangle / Square / Cross / Circle, U / O L1 / R1, Z / P L2 / R2.
- Alt+Enter switches between window and fullscreen, F1 shows the FPS counter.

### Texture packs
Put a texture pack in a folder inside `textures` beside the game; TEXTURES in RECOMP OPTIONS chooses it.

## Files beside the game

| File | What it is |
|---|---|
| `pc_settings.ini` | your settings and where your disc images are |
| `memcard\` | your memory card: the game's saves |
| `*.log` | what the last run did - please attach it to a bug report |

## Known issues

- This is an early release. If something looks or sounds wrong, please open an issue with the log - I will try to
  fix each one as fast as I can.
- With the Japanese voices, the European subtitles follow the Japanese lines automatically; in a few scenes a line
  may come a little early or late.

## Building from source

This repository holds the port's own code (`games/zoe_us/port`: the game's hooks, the RECOMP OPTIONS pages, the
Japanese voices) and its configuration; the toolkit is
[ps2recomp-option-p](https://github.com/almost-drowned-rat/ps2recomp-option-p) (the `engine` submodule). The release
is built with the toolkit's `tools/player_release.py`; the translation itself runs from your disc image.

## Credits and licences

- The toolkit, ps2recomp-option-p, is my fork of [ps2-port-engine](https://github.com/danielgomesvieira2000/ps2-port-engine).
- Fonts and libraries: Liberation Sans (SIL Open Font License 1.1), SDL3 (zlib), stb_truetype and stb_image (MIT /
  public domain), pl_mpeg (MIT), miniz (MIT), xxHash (BSD-2-Clause), bcdec (MIT), minimp3 (CC0), AMD FidelityFX FSR 1
  (MIT), SMAA (MIT). Their licence texts are in the toolkit's `LICENSES` folder.
- Test results of the real console: [ps2autotests](https://github.com/unknownbrackets/ps2autotests) (ISC).
- Some addresses of the game's functions were first named by another recompilation of the same game,
  ZOE-Recompiled; only those facts were taken, no code.

This project is licensed under the GNU General Public License v3.0 (see `LICENSE`).

*Zone of the Enders* is a trademark of its owners. This project is not affiliated with, endorsed by or sponsored by
the game's publisher or developer.
