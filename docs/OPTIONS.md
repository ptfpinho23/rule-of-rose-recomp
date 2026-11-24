# Command-line options

The launcher sets all of this for you. These are the switches of the game executable itself (`ror-game` / `ror-game.exe`).

```
ror-game --iso PATH [--scale 1..4 | --res WxH] [--wide 16:9] [picture options]
```

| Switch | Effect |
|---|---|
| `--scale N` | window size 640N x 480N, N = 1..4 (default 2); with `--wide` the window is 480N high and as wide as the wide picture (16:9, N = 2: 1707x960) |
| `--res WxH` | window client area of exactly WxH pixels, e.g. `--res 1920x1080` (320x240 to 7680x4320); overrides `--scale`; the picture is fitted inside per `--aspect`; a size larger than the screen is reduced |
| `--fullscreen` | borderless fullscreen on the monitor the window is on; Alt+Enter or F11 toggles while running, Esc quits |
| `--aspect 4:3\|stretch` | keep the picture's shape with black bars (default; the shape is 4:3, or the `--wide` one while a 3D scene is shown) or fill the window |
| `--wide off\|16:9\|21:9\|32:9\|W:H\|R` | widescreen (default `off`): 3D scenes are rendered with a wider field of view (same height, more to the sides) and shown at that shape; captions, text and other 2D drawn over 3D keep their proportions in the middle; movies, menus and everything else that is not 3D stay 4:3 with bars at the sides. `W:H` or a ratio `R` from 1.0 to 3.6 (e.g. `2.37`); `4:3` means off. Works with both renderers. Also `ROR_WIDE=value` |
| `--film-grain 0..100` | strength of the game's own film grain (a noise pattern it draws over every 3D picture): `100` (default) as the game draws it, `0` off. Both renderers. The grain is much more noticeable with `--renderer gpu` at a high `--internal`, where each grain speck stays sharp |
| `--texture-filter auto\|original\|smooth` | GPU renderer only. `original` samples textures as the PS2 does (four texels, however far away the surface is), which makes distant ground and foliage crawl when the camera moves. `smooth` filters distant textures from smaller copies of them (mip levels, up to four samples along the stretched direction); textures seen up close, sprites, text and menus are drawn exactly as before. `auto` (default) is `smooth` from `--internal 2` on a discrete graphics card and `original` otherwise (it costs up to a fifth of the frame time on integrated graphics) |
| `--filter nearest\|bilinear\|fsr\|fsr3` | how the PS2 picture is scaled; `fsr` (default) is AMD FidelityFX Super Resolution 1.0 (EASU + RCAS) on the CPU; `fsr3` is AMD FSR 3.1 temporal upscaling: Windows with `--renderer gpu` only, and only when `amd_fidelityfx_loader_vk.dll` and `amd_fidelityfx_upscaler_vk.dll` (AMD FidelityFX SDK 2.4.0, signed builds) are beside the game executable. It reconstructs detail over several frames from a low internal resolution and is meant for `--internal 1` or `2`; at 4 and above plain `fsr` is the better choice. Movies, menus and other 2D screens are scaled with FSR 1.0 as before. Anywhere it cannot run (software renderer, Linux, DLLs missing) it is exactly `fsr`, with one log line |
| `--sharpness 0..100` | FSR sharpening strength (default 60; 0 turns the sharpening pass off) |
| `--brightness -100..100` | default 0 |
| `--contrast -100..100` | default 0 |
| `--gamma 50..200` | 100 = unchanged; above 100 brightens the midtones (useful: the game is very dark) |
| `--saturation 0..200` | 100 = unchanged |
| `--bloom 0..100` | soft glow around bright areas (default 0) |
| `--renderer software\|gpu` | `software` is the CPU renderer (the default when started without the launcher). `gpu` draws with Vulkan 1.3 at a multiple of the PS2 resolution; if no suitable Vulkan device is found the software renderer is used |
| `--internal 1..8` | GPU renderer only: internal resolution as a multiple of 512x448 (default 4 = 2048x1792) |
| `--vsync 0\|1` | GPU renderer only: tear-free presentation (default 1) |
| `--pad auto\|off\|xinput\|playstation` | gamepad: `auto` (default) uses the gamepads it finds, `off` ignores gamepads; `xinput` / `playstation` restrict it to XInput pads or to PlayStation pads (Linux: to non-Sony / Sony devices) |
| `--pad-deadzone 0..90` | part of the stick travel around the centre that is ignored, in percent (default 24); the rest of the travel is the full analog range |
| `--fps 0\|display\|25..1000` | GPU renderer only (experimental): frame interpolation. `0` (default) shows the game's own 25 pictures per second; `display` presents in-between pictures at the monitor's refresh rate; a number presents at most that many per second. Adds one game frame (40 ms) plus 20-60 ms of picture delay. Geometry is moved between two frames only where both frames agree on what it is; everything else is shown as the game drew it, and after a camera cut the real frames are shown without in-betweens |

With `--renderer gpu`, `--filter nearest` shows every PS2 pixel as an even block: output pixels that fall between two blocks mix them, so that a window size that is not a whole multiple of the picture does not give blocks of unequal width (at whole multiples it is plain nearest). The software renderer's `nearest` is unchanged.

With `--renderer gpu` the same picture options run as shaders: `--filter fsr`
is FSR 1.0 (EASU) when the internal picture is smaller than the window and an
area (box) filter when it is larger, followed by the `--sharpness` pass (RCAS);
brightness, contrast, gamma, saturation and `--bloom` work as on the software
path (close to it, not bit-identical).

Without `--iso` the game looks for `Rule of Rose (Europe) (En,Fr,De,Es,It).iso`
or `RuleOfRose.iso` in the current folder, beside the executable and up to
four folders above it.

| Environment variable | Effect |
|---|---|
| `ROR_ISO=path` | disc image location |
| `ROR_MC_DIR=dir` | memory-card folder (default `saves/mc0` beside the executable) |
| `ROR_NO_MC=1` | start with no memory card inserted |
| `ROR_NOAUDIO=1` | no sound |
| `ROR_PAD_DEBUG=1` | Linux: log every input device, the capabilities of each gamepad and every change of its state (see [CONTROLS.md](CONTROLS.md)) |
| `ROR_WIDE=16:9` | same as `--wide` (the command line wins) |
