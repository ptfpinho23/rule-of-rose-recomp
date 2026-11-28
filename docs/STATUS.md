# Status in detail

Two discs are supported: the European one (SLES-54218, 50 Hz) and the North
American one (SLUS-21448, 59.94 Hz). Most of what follows was first done on the
European disc; what was checked on the USA disc is in its own section below.

## What has been verified

Scripted runs with screenshots cover: language select, memory-card check, logo
and intro movies, title screen, the New Game opening movie with subtitles, the
3D storybook, the chapter card, the first gameplay scene and the path leading
uphill from it.

- The Windows and Linux builds produce byte-identical frames and audio over
  that run.
- Both hold the European game's 50 Hz in a real window with sound, with a large margin
  (the first gameplay scene runs at about 150 fps unthrottled on the
  development machine).
- The GPU renderer, frame interpolation (`--fps display`), widescreen at 16:9
  and 21:9 and FSR 3.1 have been run over the same stretch on one GPU, an RTX
  3090.
- Frame interpolation from the game's geometry inputs (`--interp auto`, the
  default with `--fps`) has been measured on that card over scripted walks
  through the first three outdoor areas, the storybook and the first
  cutscene: a detector that looks for pixels belonging to neither of two game
  frames finds none in the in-between pictures of the walks outside a loading
  screen, and in the first two areas the game holds full speed at 240
  pictures per second. Scenes the scripted runs do not reach have not been
  looked at; for
  anything the method does not recognise it falls back to the older pairing
  of drawn primitives.
- DLSS (`--filter dlss`, with NVIDIA's files supplied by the player) has been
  run in the first two areas and the first cutscene on the RTX 3090 only
  (about 180 pictures per second with `--fps 240` at `--internal 2`).

## The USA disc (SLUS-21448)

Verified with a scripted run of 18,000 vblanks (five minutes of play) with
screenshots: the memory-card message (this disc has no language screen), the
publisher and disclaimer screens, logo and intro movies, title screen, New
Game, the opening movie with subtitles, the chapter card, the 3D storybook,
the first gameplay scene with its captions, the path uphill and the cutscene
at its end.

- Windows and Linux produce byte-identical frames and audio over that run, and
  so do the games made by the release's own builder on both systems.
- Timing: the game runs at 59.94 vblanks a second (measured over 15,400
  vblanks without a window) and draws 30 pictures a second; the sound comes
  out at exactly 800.8 samples per vblank, so it neither drifts nor changes
  pitch. The intro movie plays through with its sound stream never running
  dry.
- On the RTX 3090: the GPU renderer at 1x, 2x, 4x and 8x; widescreen at 16:9,
  21:9 and 32:9 (3D wide, movies and menus 4:3); the upscalers (nearest,
  bilinear, FSR 1.0, FSR 3.1, DLSS); film grain, the texture filter and the
  picture settings; frame interpolation at 60, 120, 144, 240 and the display's
  rate with both methods, over the uphill walk: 686 of 690 game frames
  interpolated, 99.5 % of the geometry matched with its previous frame (the
  European walk gives the same figure), and the artifact detector found
  nothing in 248 in-between pictures. On Linux the GPU renderer ran at 1x on
  the CPU Vulkan driver.
- Gamepad input fed through the mapping code (buttons, and the stick for
  walking) reproduces the keyboard run exactly, as on the European disc.
- Memory card: with no card the game shows its message at boot; with an empty
  card it starts without it and Load Game reports that there is no save data.

Not checked on the USA disc: everything listed under "Not tested yet" (it
applies to both discs), `--frameskip` in action (it needs a machine that is too
slow), the fullscreen picture by eye (a fullscreen run started and presented;
nobody looked at the screen), frame generation, and any play beyond the
stretch above. The known problems below were found on the European disc and
should be assumed for both.

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
- With the GPU renderer the third outdoor area (the gate of the orphanage)
  runs below full speed on the development machine, with or without `--fps`.
  Not investigated yet.
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
