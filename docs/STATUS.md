# Status in detail

## What has been verified

Scripted runs with screenshots cover: language select, memory-card check, logo
and intro movies, title screen, the New Game opening movie with subtitles, the
3D storybook, the chapter card, the first gameplay scene and the path leading
uphill from it.

- The Windows and Linux builds produce byte-identical frames and audio over
  that run.
- Both hold the game's 50 Hz in a real window with sound, with a large margin
  (the first gameplay scene runs at about 150 fps unthrottled on the
  development machine).
- The GPU renderer, frame interpolation (`--fps display`), widescreen at 16:9
  and 21:9 and FSR 3.1 have been run over the same stretch on one GPU, an RTX
  3090.

## Not tested yet

- **The rest of the game.** Later areas, combat, the remaining movies and
  saving or loading at a save point.
- **Real controllers.** Gamepad input has only been fed in synthetically
  through the mapping code (buttons and analog walking reproduce the keyboard
  runs exactly), and the DualShock 4 / DualSense report decoding has only been
  checked against constructed reports and the Linux kernel and SDL sources.
  Device detection, hot-plug, Bluetooth and vibration have never run against
  hardware.
- **Other GPUs.** Nothing but the RTX 3090 on Windows. On Linux the GPU
  renderer has only run on a CPU Vulkan driver.
- **By hand:** the Alt+Enter / F11 fullscreen toggle and `--aspect stretch` in
  a real window; 32:9 on the GPU renderer.

## Known problems

- Sound is mixed without reverb. Loudness and panning have not been compared
  with a console.
- With the GPU renderer and `--wide`, the start of the walk in the first scene
  drops below full speed for about a second (many textures change at once).
  The software renderer is not affected.
- With `--wide` the picture changes shape between 3D scenes (wide) and movies
  or menus (4:3). In the tested stretch this happens while the screen is
  black. Later areas may show objects popping in at the sides or 2D elements
  that are not centred.
- FSR 3.1 at `--internal 2` does not hold 240 Hz with frame interpolation on
  the test machine (about 200). Its output varies slightly from run to run at
  `--internal 1`.
- `--bloom` is accepted but too weak to see in this game.
- DualShock 3 is not supported on Windows. Old DirectInput-only joysticks are
  not picked up. There is no button remapping, and the game has one player:
  several connected pads all act as that one pad.

## Things that are not bugs

At `--internal 2` and above the GPU renderer shows detail a PS2 picture hides:
bark and leaf textures (the game uses no mip-mapping), its translucent foliage
layers, and its film grain, whose specks stay crisp. That is the game's own
content at a higher resolution. `--film-grain` turns the grain down.

## Not implemented

Nothing in the tested stretch needs these: mip-mapping, dithering, reading the
frame buffer back to the CPU, MPEG-1 video (the game's movies are MPEG-2).

## How changes are tested

The game is deterministic when run headless:

```
ROR_NO_MC=1 build/recomp/ror --no-window --no-sync --frames N --shots M --script "frame:buttons,..."
```

This writes `build/recomp/shots/frame_NNNNNN.png` every `M` frames. `--script`
feeds pad states (active-low button mask in hex) at given frames. A script
that plays from boot into the first gameplay scene, and the frame numbers of
the landmarks along the way, are in [recomp/NOTES.md](../recomp/NOTES.md).
`--interp` runs the same game through a reference interpreter generated from
the same instruction templates, for comparison with the recompiled code.
