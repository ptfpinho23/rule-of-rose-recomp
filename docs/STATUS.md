# Status in detail

Verified by scripted runs and screenshots: language select, memory-card check,
logo and intro movies, title screen, the New Game opening movie with
subtitles, the 3D storybook, the chapter card and the first gameplay scene.
The Windows and Linux builds produce byte-identical frames and audio over that
run. Both hold the game's 50 Hz in a real window with sound, with a large
margin (the first gameplay scene runs at roughly 150 fps unthrottled on the
development machine).

Not yet verified or known to be missing:

- Nothing past the first gameplay scene has been played, so later areas,
  combat, the remaining movies and **saving/loading at a save point** are
  untested.
- Sound is mixed without reverb; loudness and panning have not been compared
  with real hardware.
- Gamepad support (XInput on Windows, evdev on Linux) has been checked only
  with synthetic gamepad input fed through the mapping code (buttons and
  analog walking reproduce the keyboard runs exactly); **no physical
  controller has been tried on either platform**, so device detection,
  hot-plug, the Linux button layouts and vibration are untested on hardware.
  The same holds for the direct PlayStation support on Windows: its report
  decoding is checked against constructed sample reports only, and reading a
  real DualShock 4 / DualSense (USB or Bluetooth) has never been tried.
  DualShock 3 is not supported on Windows.
  Pads that Linux does not report as a gamepad (old DirectInput-style
  joysticks) are not picked up; there is no button remapping.
- By default the picture is rendered on the CPU at PS2 resolution and scaled
  up (FSR 1.0). An experimental GPU renderer (`--renderer gpu`) renders at up
  to 8x the PS2 resolution and can interpolate frames for high refresh rates
  (`--fps display`; the game draws 25 pictures per second). It has been run
  through the scripted intro and a few seconds of walking only; see
  `recomp/NOTES.md`, "GPU renderer", for what is and is not verified.
- Widescreen (`--wide`) has been run through the same scripted stretch only
  (storybook, chapter card, first gameplay scene, a few seconds of walking) at
  16:9 and 21:9 on both renderers; 32:9 only on the software renderer. The game
  culls and clips for the wider view there without visible errors, but later
  areas are unchecked: objects popping in at the sides or HUD parts that are
  not centred may turn up. The picture changes shape between 3D scenes and
  movies/menus (in the scripted run this happens while the screen is black).
  With the GPU renderer and `--wide`, the start of the walk in the first scene
  runs at a quarter to half speed for about a second of game time, worst with
  `--fps` (many textures change at once there); the software renderer does not.
- `--filter fsr3` (FSR 3.1) has been checked on one GPU (RTX 3090) over the
  same scripted stretch. Its output varies slightly from run to run at
  `--internal 1`.
- With `--renderer gpu` at `--internal 2` or more, detail that a PS2 picture
  hides becomes visible: the bark and leaf textures (the game uses no
  mip-mapping), its translucent foliage layers and its film grain, whose
  specks stay crisp (`--film-grain` turns the grain down). This is the game's
  content at a higher resolution, not a rendering error.
- `--bloom` is accepted but its effect is too weak to see in this game yet.
- Fullscreen start is verified on both platforms; the Alt+Enter / F11 toggle
  and `--aspect stretch` in a real window have not been tried by hand.
- Renderer gaps that nothing has needed so far: mip-mapping, dithering,
  reading the frame buffer back to the CPU.
- MPEG-1 video (the game's movies are MPEG-2).

## Testing

The game is deterministic when run headless, which is how changes are checked:

```
ROR_NO_MC=1 build/recomp/ror --no-window --no-sync --frames N --shots M --script "frame:buttons,..."
```

writes `build/recomp/shots/frame_NNNNNN.png` every `M` frames; `--script`
feeds pad states (active-low button mask in hex) at given frames. A script
that plays from boot to the first gameplay scene, and the frame numbers of the
landmarks along the way, are in `recomp/NOTES.md`. `--interp` runs the same
game through a reference interpreter generated from the same instruction
templates, for comparing against the recompiled code.
