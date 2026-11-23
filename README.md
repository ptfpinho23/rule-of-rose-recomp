<p align="center">
  <img src="docs/logo.svg" alt="Rule of Rose PC Port" width="420">
</p>

<p align="center">
  <b>Rule of Rose (PlayStation 2) running natively on Windows and Linux, built on your PC from your own copy.</b>
</p>

<p align="center">
  <a href="#project-status">Status</a> |
  <a href="#playing">Playing</a> |
  <a href="#how-to-build">How to build</a> |
  <a href="#how-it-works">How it works</a> |
  <a href="#want-to-help">Want to help?</a> |
  <a href="#credits">Credits</a> |
  <a href="#license">License</a>
</p>

This is a static recompilation of the European release of *Rule of Rose*
(SLES-54218). The game's MIPS R5900 machine code is translated ahead of time
into C and compiled for x86-64, then linked against a runtime that stands in
for the PS2 kernel and hardware: graphics, sound, movies, disc, memory card
and pad. It is not an emulator. There is no MIPS interpreter or JIT in the
game you end up running. Models, textures, audio and movies are read from your
disc image while you play.

> [!IMPORTANT]
> This repository **does not contain the game, its executable or any of its
> assets**, and it does not explain how to get them. You need your own copy of
> the disc as an `.iso` image. Everything the build creates from it (the
> recompiled code, the game executable, your saves) is made on your PC and
> stays there. Do not share those files.

## Project status

**Early, but it boots and plays.** The game runs from power-on to the first
gameplay area with sound and movies. Nothing
beyond the first area has been played through yet.

| What | Status |
|---|---|
| Boot, language select, logo and intro movies, title screen | Works |
| New Game: opening movie with subtitles, storybook, chapter card | Works |
| First gameplay area (walking, camera, captions) | Works |
| Later areas, combat, the remaining movies | Not tested yet |
| Saving and loading at a save point | Not tested yet |
| Sound | Works, without reverb |
| Windows 10/11 x64 | Works |
| Linux x86-64 (X11, XWayland, WSLg) | Works |
| Software renderer (PS2 resolution, exact) | Works, 50 Hz with a large margin |
| GPU renderer (Vulkan 1.3, up to 8x internal resolution) | Works on the one GPU it was tested on (RTX 3090) |
| Frame interpolation up to the display refresh rate | Works with the GPU renderer, 240 Hz measured |
| Widescreen 16:9 / 21:9 / 32:9 (wider 3D view, 2D kept in proportion) | Experimental |
| Upscaling: FSR 1.0, FSR 3.1 (Windows, GPU renderer) | Works |
| Keyboard | Works |
| Xbox and PlayStation (DualShock 4, DualSense) pads | Written, not yet tried with a real controller |

The long version, with everything that is known to be missing or unverified,
is in [docs/STATUS.md](docs/STATUS.md).

## Playing

Build the two folders as described below, then start the launcher:

| Platform | Folder | Start |
|---|---|---|
| Windows 10/11 x64 | `dist/RuleOfRose-windows/` | `RuleOfRose.exe` |
| Linux x86-64 | `dist/RuleOfRose-linux/` | `./RuleOfRose` |

Point the launcher at your disc image (it checks that it is the European disc
and remembers it), choose your options and press **PLAY**. Options cover
window or fullscreen, resolution, picture shape, frame rate, renderer,
internal resolution, upscaling, image adjustments, film grain, sound, memory
card and gamepad.

Saves go to `saves/mc0/` next to the launcher. That folder is the memory card
in slot 1.

- Keyboard and gamepad mapping: [docs/CONTROLS.md](docs/CONTROLS.md)
- Running the game without the launcher, every switch: [docs/OPTIONS.md](docs/OPTIONS.md)

## How to build

Everything is built on Linux (WSL2 works) with a user-space toolchain. No root
is needed. The Windows build is cross-compiled from the same machine.

You need:

- clang 18 at `~/.local/opt/clang`, plus the glibc headers that
  `tools/fetch-debs.sh` downloads into `tools/debs/`
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (clang 18, UCRT) in
  `tools/llvm-mingw/` for the Windows build
- Qt 6.8 under `~/.local/opt/qt6/` for the launcher (Linux and
  `win64_llvm_mingw` kits)
- Vulkan headers and glslang in `tools/vulkan/` for the GPU renderer
- Python 3, CMake and Ninja
- optional: the AMD FidelityFX SDK 2.4.0 in `tools/ffx/` if you want FSR 3.1
  in the Windows package

Put your disc image in the repository root as
`Rule of Rose (Europe) (En,Fr,De,Es,It).iso`, or set `ROR_ISO=/path/to.iso`.
Then:

```sh
recomp/build.sh            # Linux game    -> build/recomp/ror
recomp/build-win.sh        # Windows game  -> build/recomp/win/ror.exe
tools/build.sh             # launcher, Linux
tools/build-win.sh         # launcher, Windows
recomp/package.sh all      # dist/RuleOfRose-linux, dist/RuleOfRose-windows
```

`recomp/build.sh` takes the game executable out of the image, runs the
recompiler over it, translates the game's VU1 program to C, and compiles the
result together with the runtime. All of that lands under `build/`, which is
ignored by git.

## How it works

- **Recompiler** (`recomp/r5900.py`, `sem.py`, `gen.py`): decodes the game's
  R5900 code and writes one C function per game function from per-instruction
  templates. The same templates also generate a reference interpreter
  (`--interp`) that is used to check the recompiled code against.
- **Runtime** (`recomp/rt/`): the parts of a PS2 the game talks to. EE kernel
  calls and threads, memory, DMA, GIF/VIF, both vector units, the Graphics
  Synthesizer, the MPEG-2 decoder, and the I/O processor services the game
  uses (disc, files, pad, memory card and its own sound driver).
- **Graphics**: a software renderer that matches the GS pixel for pixel and is
  what all regression tests run on, and a Vulkan renderer that draws the same
  primitives at a multiple of the original resolution.
- **Frame interpolation**: the game draws 25 pictures a second. The GPU
  renderer keeps each frame's draw list, pairs it with the previous one and
  draws in-between pictures from interpolated vertices, so nothing is guessed
  from pixels.
- **Widescreen**: the game's camera setup is intercepted and given a wider
  frustum. Text and other 2D drawn over 3D are kept in proportion; movies and
  menus stay 4:3.
- **Platform layer**: X11, PulseAudio and evdev on Linux; Win32, waveOut,
  XInput and raw HID on Windows.
- **Launcher** (`launcher/`): a small Qt 6 program that finds the disc image,
  holds the settings and starts the game.

The game is deterministic when run headless, and that is how changes are
tested: a scripted run from boot into gameplay must produce the same frames
and the same audio, byte for byte, on Linux and on Windows. The recipe and a
lot of detail about the game's internals are in
[recomp/NOTES.md](recomp/NOTES.md).

## Want to help?

The most useful things right now:

- Play further than the first area and report what breaks, with a save or the
  steps to get there.
- Try a real controller (Xbox, DualShock 4, DualSense) on Windows or Linux.
- Try the GPU renderer on something other than an NVIDIA card, and on Linux
  with a real Vulkan driver.
- Compare sound against a real console.

Issues and pull requests are welcome. Please do not attach or link anything
from the game itself.

## Credits

- [AMD FidelityFX Super Resolution](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
  1.0 (MIT) is ported in `recomp/rt/post.c`; FSR 3.1 is loaded from AMD's own
  signed libraries when present.
- [Cormorant Garamond](https://github.com/CatharsisFonts/Cormorant) (SIL Open
  Font License) is the launcher's typeface.
- [Qt](https://www.qt.io/) 6 (LGPL v3) for the launcher.
- The PS2 homebrew and emulation communities, whose public documentation of
  the hardware made this possible.

*Rule of Rose* is the property of its respective owners. This project is not
affiliated with or endorsed by them.

## License

The code in this repository is under the [MIT License](LICENSE). Third-party
pieces keep their own licences, listed above.
