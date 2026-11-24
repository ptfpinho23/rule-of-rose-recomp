# Rule of Rose — static recompilation (new pipeline, started 2026-11-22)

This directory replaces an earlier attempt (no longer in the tree, which decoded the
game as MIPS32 and was not a valid translation).

## Pipeline

`recomp/build.sh` → `build/recomp/ror`

1. `r5900.py` — R5900 (PS2 EE) decoder: MIPS III core, MMI, COP0, COP1,
   COP2 (VU0 macro). All 413,499 code words of `SLES_542.18` decode.
2. `sem.py` — one C template per instruction (single source of truth).
3. `gen.py` — finds function entries (jal targets, linkonce section starts,
   code pointers in data, lui/addiu constants), emits one C function per
   entry into `build/recomp/gen/fn_*.c`. Transfers to another entry are
   `musttail` calls; `jr $ra` returns; `jalr`/`jr reg` go through
   `rt_lookup`. After every call the caller checks `c->pc` against its
   return address and unwinds if it differs (longjmp-style control flow).
   Also writes `insn.bin` + `interp_*.inc` for the reference interpreter.
4. `rt/` — runtime:
   - `cpu.h`, `vu_ops.h`: CPU state (`Ctx`) and semantics helpers.
   - `mem.c`: 4 GiB guest space at `MEMBASE`, RAM mirrors, scratchpad.
   - `kernel.c`: EE kernel HLE (threads as ucontext coroutines, semaphores,
     INTC/DMAC handlers, alarms, virtual clock, SetSyscall table).
   - `hw.c`: hardware registers (timers done; INTC; GS CSR stub).
   - `interp.c`: reference interpreter (`--interp`), same semantics and
     tick accounting as the recompiled code.
   - `support.c`: lookup, COP0, ERET, diagnostics, call tracing.

`recomp/disasm.py ADDR [N]` disassembles game code.

## Verification

- `ROR_TRACE=N build/recomp/ror` vs `... --interp`: both hash every call
  (target + all GPRs); identical hashes mean the generated control flow
  matches the interpreter. `ROR_TRACE_FROM/TO` print a window of calls.
- Unhandled instructions fail the build (`UNHANDLED` in gen.py output).
- Unknown syscalls / hardware registers / code addresses stop with a
  register dump instead of being ignored.

## Facts learned about the game

- EE ELF only (no EE overlays); `.DVP.overlay.*` and `.vutext` are VU micro
  programs. PAL (50 Hz).
- SDK start-up installs kernel patches via SetSyscall (0x5A kCopy, 0x5B
  GetEntryAddress, 0x83 FindAddress; alarm patch -> syscalls 0xFC-0xFF;
  TLB patch -> 0x55-0x59). Handled natively in `kernel.c`.
- Kernel-mode register writes use an ERL/ERET trick (`0x269c90`).
- Debug text goes through Deci2Call(0x10) (kputs).
- IOP modules on disc: sio2man, sio2d, dbcman, pad2/ds1o_d, padman, mcman,
  mcserv, libsd (all Sony) and RXSNDDRV.IRX (game-specific sound driver).
- FMVs are `.PSS` in `/MOVIE` (first: CP_LOGO.PSS) -> needs IPU.

## Status (2026-11-22)

Working, verified by screenshots (`--no-window --no-sync --shots N --frames N
--script "frame:buttons,..."`, PNGs in build/recomp/shots):
boot -> language select (512x448, correct) -> "No memory card" message ->
"Begin anyway" -> CP_LOGO.PSS -> MV00_AD.PSS intro movie (clean frames).
Keyboard input works in the X11 window (arrows, Z=cross, X=circle, A=square,
S=triangle, Enter=start, Backspace=select, Q/W/E/R=L1/R1/L2/R2, Esc quits).

Runtime modules now: gs.c (GS: real memory layout, registers, transfers,
software rasterizer), gif.c, vif.c, vu.c (VU0/VU1 micro interpreter with
flag/Q/P pipelines; NOT YET EXERCISED by 3D scenes), dmac.c, ipu.c (MPEG-2
macroblock decoder as a coroutine pulling input from DMA ch4), video.c (X11
window, PNG shots, scripted input, 50 Hz pacing), sif.c, iop.c (iopheap,
loadfile, cdvd, fileio libkernl-3.0, mcserv: "no card" only), iop_dbc.c
(libdbc/libpad2, protocol in file header), iop_rxsnd.c (sound
driver protocol, silent; spec in docs/rxsnddrv.md), iso.c.

Kernel facts learned the hard way:
- main thread id is 1; no "0 = self" thread ids.
- I_STAT stays latched for the game's polling (sceGsSyncV) while handler
  dispatch uses a separate pending set.
- Interrupts are off when Status.IE=0 or EXL/ERL set, not only after DI;
  pending interrupts are taken as soon as they are re-enabled.
- Alarms fire on the global H-sync grid (the game's own time-slicing
  scheduler at 0x24c5a0 depends on it not being phase-locked to callers).
- D_ENABLEW bit 16 suspends DMA; channels armed meanwhile start on release.
- RPC 0x8000059C is non-blocking sceCdDiskReady.

Progress later the same day: title screen, New Game, opening movie with
subtitles, the 3D storybook (D-pad left/right turn pages, Circle exits) and
the first gameplay scene (Jennifer at the bus stop) all render correctly.
Fixes that got there: TEST_1 is register 0x47 (odd) so context = reg - 0x47;
VU MAX/MINI must not clear MAC (tools/vudis.py,
tools/vurun.sh, ROR_VUDUMP/ROR_VUTRACE); the VI "old value for the next
branch" rule applies to integer ALU/loads only, not flag tests; IPU motion
code table entries 8-10 are 9 bits. gs.c now shades large primitives on
worker threads (ROR_GS_THREADS) with fast paths for CT32. Memory card:
iop_mc.c, host dir saves/mc0 (ROR_MC_DIR), ROR_NO_MC=1 removes the card
(the scripted test timelines assume no card). Debug aids: ROR_GSDUMP=frame
(draw calls, textures to shots/tex_*.png), ROR_PROF=file +
tools/prof.py, ROR_SNDDBG.

Test script to reach gameplay (no card):
  S="800:bfff,810:ffff,1300:ffdf,1310:ffff,1350:bfff,1360:ffff,4600:fff7,
  4610:ffff,5200:fff7,5210:ffff,6000:bfff,6010:ffff,7000:fff7,7010:ffff,
  7600:fff7,7610:ffff,8200:fff7,8210:ffff,8800:fff7,8810:ffff" then Right
  every 250 frames 9700..13400, Circle at 13600, Cross every 400 after;
  gameplay is on screen by frame ~21000.

Performance (unthrottled, Ryzen 9 7900X, measured with ROR_FPSLOG=1): menus
and movies 70-400 fps, storybook ~55 fps, bus-stop gameplay ~14 fps (82 ms
CPU per frame single-threaded: GS pixel path ~70%, VU1 interpreter ~14%).
Done since (2026-11-22): deferred band-parallel GS renderer with
capture/replay benchmark, VU interpreter speed
(vu.c, vu_ops.h, vif.c), SPU2 mixer + host audio (iop_rxsnd.c, audio_out.c).
Launcher: PLAY handler written in launcher/mainwindow.cpp but the launcher
could not be rebuilt (its toolchain headers under /tmp/glvnd, /tmp/libc6dev,
/tmp/gccdev are gone and must be re-fetched).

Still missing: audio output (SPU2 mixer on top of iop_rxsnd.c's state),
memory card files (host directory), GS local->host transfer, VIF1 read-back,
mipmapping/dithering in gs.c, MPEG-1 streams in ipu.c. 3D rendering through
VU1 is untested.

## Roadmap (in order)

1. [done] decoder, generator, interpreter, kernel core, timers, kputs.
2. [done] SIF command/RPC layer; loadfile, cdvd, fileio on the ISO.
3. [done] pad, sound protocol (silent), DMAC/GIF/VIF/GS, X11 window, IPU.
4. Title menu -> new game: exercise and fix VU1/3D rendering.
5. SPU2 audio mixer + host audio output.
6. Memory card on a host directory; launcher PLAY button; performance.

## Windows build (2026-11-22)

`recomp/build-win.sh` -> `build/recomp/win/ror.exe` (+ `ror.pdb`), native
Windows x64, built on Linux.

- Toolchain: llvm-mingw 20240619 (clang/lld 18.1.8, the same LLVM as the
  Linux compiler), UCRT, unpacked in `tools/llvm-mingw/` from
  github.com/mstorsjo/llvm-mingw releases
  (`llvm-mingw-20240619-ucrt-ubuntu-20.04-x86_64.tar.xz`, sha256 27d33157...b90e).
  Target `x86_64-w64-mingw32`. `musttail` compiles for all generated files.
- Flags are build.sh's (`-O2 -fwrapv -fno-strict-aliasing -fno-math-errno
  -ffp-contract=off`) plus `-gcodeview` (PDB), `-include
  recomp/rt/win_compat.h`. Linked `-static` (winpthreads inside); imports
  only KERNEL32, USER32, GDI32, SHELL32, WINMM and the UCRT
  `api-ms-win-crt-*` DLLs. Console subsystem (the log goes to stderr); when
  started by double-click the console is dropped as the window opens
  (`ROR_KEEP_CONSOLE=1` keeps it).
- Interface: `OBJ`, `OUT`, `EXTRA` as in build.sh. It does NOT regenerate
  the C unless `build/recomp/gen` is missing or `GEN=1` is given.
- Generated code and gen.py/sem.py needed no change (fixed-width types
  throughout; no 64-bit-`long` assumptions were found in the runtime either).

Platform layer (`recomp/rt/plat.h` is the whole OS interface):

| what | Linux | Windows |
|---|---|---|
| process, env, exe dir, faults, guest address space, coroutines, time, files/dirs | `plat_posix.c` | `plat_win32.c` |
| window + keyboard (`vwin_*`) | `video_x11.c` | `video_win32.c` |
| sound device (`adev_*`) | `audio_pulse.c` | `audio_waveout.c` |

`video.c` (script, PNG, pacing, fps log) and `audio_out.c` (WAV, mixer pull)
are common. Each platform file is wrapped in `#ifdef _WIN32` / `#ifndef
_WIN32`, so both build scripts compile every `recomp/rt/*.c`.
`win_compat.h` only supplies `sysconf(_SC_NPROCESSORS_ONLN)` for gs.c.

What differs on Windows:
- Guest memory: nothing in the runtime depends on faults for function (a
  stray access is only reported). The 4 GiB at MEMBASE are probed with one
  reservation, released, then rebuilt from PAGE_NOACCESS reservations (gaps),
  `MapViewOfFileEx` views of two pagefile-backed objects (RAM x5, ROM x3) and
  a committed scratchpad. Unmapped guest addresses still fault and are
  reported with the guest address (exit 71).
- Coroutines are Fibers (8 MiB reserved per guest thread, 1 MiB for the IPU,
  committed on demand; FIBER_FLAG_FLOAT_SWITCH, MXCSR copied from the creator).
- Faults: vectored exception handler prints the exception, `image+0xOFFSET`
  (resolve with the PDB), guest address and register dump, exits 71; stack
  overflow prints one line. Ctrl-C dumps guest state, exits 72. WER dialogs
  are disabled. `ROR_CRASHTEST=1` (both platforms) provokes a fault to test this.
- No sampling profiler (`ROR_PROF` prints a notice).
- Text is UTF-8 internally; argv, the ISO, the memory-card directory,
  `ROR_WAV`, `ROR_ISO`, `ROR_MC_DIR` go through the wide APIs (no manifest).
  Debug dumps in gs.c/vu.c/iop_rxsnd.c still use plain `fopen` (ASCII paths).
- Pacing: high-resolution waitable timer (Windows 10 1803+), else Sleep at
  1 ms granularity. Audio: waveOut, 16 x 20 ms blocks polled for completion,
  frame dropped when 10 are queued, 60 ms of silence after a dry queue.
  `ROR_AUDIODBG=1` prints sent/dropped/underruns.
- Dragging the window blocks the game while the mouse button is held (the
  window is pumped from the game thread).

Testing the Windows build: copy `ror.exe` to a folder on a Windows drive and
run it there; the ISO can be passed as a path or a network path.
Verified 2026-11-22: the NOTES test script (no card, `--no-window --no-sync
--frames 16000 --shots 1000`) gives the same md5 for all 16 screenshots and
for the `ROR_WAV` file as the Linux build of the same sources; no libm
substitution was needed for that run (cosf in ipu.c, pow in iop_rxsnd.c and
the VU sinf/atanf/expf paths still come from each platform's libm). Headless
speed on Windows, frames 14000-16000: 134-177 fps (Linux: 157-159).

## Picture options / post-processing (2026-11-22)

`recomp/rt/post.c` (+ `post.h`) turns the raw GS frame into the output-size
picture; `video.c` calls it once per shown frame and the platform window
files only copy the result 1:1 (X11 `XPutImage`, Windows `SetDIBitsToDevice`;
the Windows process is now DPI-aware so sizes are real pixels).

Command line (all integers; a bad value prints the usage and exits 2):
`--scale 1..4`, `--res WxH`, `--fullscreen` (borderless, monitor size;
Alt+Enter / F11 toggle at run time on both platforms), `--aspect
4:3|stretch`, `--filter nearest|bilinear|fsr` (default fsr), `--sharpness
0..100` (60), `--brightness -100..100`, `--contrast -100..100`, `--gamma
50..200` (100 = 1.00), `--saturation 0..200`, `--bloom 0..100`, and
`--post-shots`.

- `--res WxH` (320..7680 x 240..4320): client area in real pixels instead of
  640N x 480N; overrides `--scale`; with `--fullscreen` it is the size the
  window returns to. A size that does not fit the screen (Windows: work area
  minus the frame; X11: screen size) is reduced keeping its aspect, with one
  log line. Headless it is the `--post-shots` output size. `[video] output
  WxH` is logged whenever the output size changes.
- Order: scale -> brightness -> contrast -> gamma (one 256-entry table) ->
  saturation -> bloom. Each is skipped at its default.
- `fsr` is a CPU port of AMD FidelityFX Super Resolution 1.0: EASU then RCAS
  (no denoise), same taps/constants/clamps as `ffx_fsr1.h`, float on 0..1
  gamma-encoded colour, RCAS on the unquantised EASU output. `--sharpness S`
  = `(100 - S) / 50` stops (100 -> 0 stops, sharpest; 0 = RCAS off). SSE2;
  pixels that share a source cell are batched. **Licence: MIT, AMD's text is
  in `recomp/rt/ffx_fsr1_license.txt` and must ship with the binaries.**
  `recomp/tools/posttool check` compares the SSE2 path with a scalar
  transliteration of the AMD header: 0 differing pixels on the sizes tried.
- Bloom: 4x4-averaged frame, threshold, 3 box blurs at quarter resolution,
  screen-blended. Tuned for this dark game: the glow starts at luma 40 and
  is at full weight from 150 (`BLOOM_THRESHOLD` / `BLOOM_FULL` in post.c);
  thin bright detail (text) does not glow because of the 4x4 average.
- No libm in post.c (own exp/log for the curves), result independent of the
  thread count: `post_*.png` are byte-identical between Linux and Windows
  (checked at 1280x960, 1920x1080, 2560x1440 and 3840x2160).
- Threads: own pool, default half the logical CPUs (`ROR_POST_THREADS`).
- `--shots` still writes the raw GS frame (`frame_NNNNNN.png`, md5 tests
  unchanged). `--post-shots` additionally writes the processed picture as
  `post_NNNNNN.png`; headless the output size is the `--res` / `--scale`
  window or `ROR_POST_SIZE=WxH`. `ROR_POSTBENCH=frame` post-processes every
  frame from that frame on in a headless run (timing; `ROR_FPSLOG=1` prints
  ms/frame).
- `recomp/tools/posttool.sh` builds `build/posttool/posttool`:
  `run` (process a screenshot), `bench`, `check`, `montage` (labelled crops).

Measured 2026-11-22 (Ryzen 9 7900X, 12 post threads, 512x448 source):
- Isolated (`posttool bench`), ms per frame, nearest / bilinear / fsr / fsr
  with all colour effects: 1280x960 0.3 / 0.5 / 3.8 / 5.7; 2560x1920 0.6 /
  1.3 / 9.0 / 15.2; 3840x2160 0.8 / 1.6 / 10.5 / 16.5 (the non-fsr and
  all-effects figures predate the last EASU restructure).
- In the game, gameplay scene frames 14000-16000, headless, fsr defaults:
  1920x1080 post 4.9 ms, 85-87 fps (Linux) / 4.15 ms, 100-101 fps (Windows);
  2560x1440 post 6.6 ms, 74-75 fps (Linux) / 5.5 ms, 88-90 fps (Windows).
  fsr + all effects at 3840x2160: post 14.0 ms, 49 fps (Linux); 13.3 ms,
  52 fps (Windows) — right at the 50 fps line, not comfortably above it.
- Window update at 2560x1440 fullscreen: X11 5.3 ms, Windows 0.8 ms.

Not verified: the Alt+Enter / F11 toggle (no key input in the test runs),
`--aspect stretch` in a real window, a real 4K monitor.

Not done (possible follow-ups): higher internal resolution in the GS
renderer, 16:9 rendering, changing the game's own lighting.

## GPU renderer (2026-11-22; experimental, opt-in with `--renderer gpu`)

Vulkan 1.3 backend for the GS, `recomp/rt/hw/` plus `#ifdef GS_HW` glue in
`gs.c` ("hardware renderer" section). Both build scripts compile
`recomp/rt/hw/*.c` with `-DGS_HW`. The software renderer is the default, the
reference and the fallback; nothing of Vulkan is loaded unless `--renderer
gpu` is given (see "In the game" below).

- `hw/gs_hw.c, gs_hw.h`: Vulkan layer (dlopen/LoadLibrary, function table,
  dynamic rendering, dual-source blending, one command buffer per flush).
  `hw/gs.vert, gs.frag, blit.*` -> `hw/gs_hw_spv.h` with
  `recomp/tools/gshw-shaders.sh` (needs `tools/vulkan/glslang`, 16.6.0 linux
  release, sha256 2c34071f...91ce; headers `tools/vulkan/Vulkan-Headers-1.3.290`,
  sha256 f38a653b...bcf5). gs_hw.c includes the headers by relative path and
  needs `-I tools/vulkan/Vulkan-Headers-1.3.290/include` (vk_video).
- Tools: `HW=1 recomp/tools/gsreplay.sh CAP -hw N [-o out.raw]` (lavapipe in
  Linux), `recomp/tools/gsreplay-win.sh` -> `build/gsreplay/gsreplay-hw.exe`
  (run from a Windows folder for the real GPU), `recomp/tools/gscmp.sh REF OUT
  [-scale N] [-png DIR FRAME...]`, `recomp/tools/vkprobe.c`.
- Env: `ROR_GS_HW=N` (set by `-hw N`), `ROR_VK_DEVICE=i`, `ROR_HW_SHBLEND=1`
  (all blending in the shader), `ROR_HW_NOREAD=1` (timing: no image
  read-back), `ROR_HW_INTERP=n[,first[,count]]` + `ROR_HW_INTERP_DIR`,
  `ROR_HW_INTERP_SNAP=px`, `ROR_HW_INTERP_DEBUG=1`, `GSBATCH=file`
  (+`GSBATCH_XY=1`) draw-list dump.

Conventions (details in the shader and gs.c comments):
- device = native * N + 0.5; sprites take their texture coordinates from the
  GS sprite formula at the fragment's native position; other primitives shift
  UVs by (0.5 - 0.5 N) * (dFdx + dFdy).
- Depth: floor(z) * 2^-bits into D32F, written from the fragment shader.
- Blending: triangles by blend state where the equation maps (factor As/128
  clamped at 1, driver rounding); sprites and everything unmappable, DATE and
  partial FBMSK in the shader against a per-primitive copy of the target.
  AFAIL modes: primitive drawn twice.
- Targets are GPU images (FBW*64 x 512 native); host uploads into a target
  with its own layout are replayed as N x N blocks; a texture with a target's
  address/width is the target; other CPU-side uses download it. A CSM1 32-bit
  CLUT load from a target is a queued 16x16 copy collected after the frame's
  submit.

Measured:
- Scale 1 vs software reference, pixels differing (any / >1 / >8 / max):
  880 0.10/0.10/0.10% 15; 3000 0.18/0.14/0.11% 17; 5700 0.20/0.15/0.11% 14;
  10700 1.86/0.58/0.03% 230; 14000 33.0/15.6/0.11% 63; 14650 32.1/14.9/0.11% 35.
  Classes: row 0 (wrapped texture row that is depth memory, GPU-only);
  fixed-function blend rounding on 3D (+-1..2); alpha-test flips on cut-out
  contours from float32 UV interpolation (read from the difference images).
- RTX 3090 (Windows exe through interop), capture 14000, ms per vblank
  without read-back: 1x 4.2, 4x 4.8, 8x 8.4 (p95 8.5 / 10.0 / 20.8); about
  3,400 draw calls per vblank, CPU-bound, nothing batched yet. 3090 and
  lavapipe agree except blend rounding (74 px of 45.9 M differ by more than 8).
- Picture rate: 25 distinct frames/s everywhere (menu 880: every 6th vblank).

Frame interpolation prototype (in the glue, replay only): every primitive of a
logic frame is retained (state + vertices, before any culling); at a display
flip the list is paired with the previous frame's in order, per primitive,
and batches whose pairs jump are re-paired by texture coordinates; in-between
pictures are drawn from the start-of-frame copies of the targets with blended
vertices. Vertices interpolated: 14650 100.0% (min 99.8%), 14000 97.7% (4 of
98 frames below 90%, 3 refused), 10700 99.9% (min 97.8%, page turn needs the
UV re-pairing). Replaying a list at t = 1 reproduces the real frame exactly
in 236 of 241 frames (worst 130 px, storybook).

In the game (video.c / main.c):
- `--renderer software|gpu` (default software), `--internal 1..8` (4),
  `--vsync 0|1` (1), `--fps 0|display|25..1000` (0). Device set-up failing ->
  one log line, software renderer. Swapchain failing -> one log line, the GPU
  picture is read back and shown by the CPU path.
- Presentation: swapchain on the game window (`vwin_native`: Xlib or Win32
  surface). Without `--fps`: one present per emulated vblank from the game
  thread, MAILBOX when vsync is on (never blocks), IMMEDIATE when off; the
  game is paced by the same 50 Hz sleep as before. Final blit shader
  (`hw/present.frag`): nearest or bilinear, and post.c's brightness /
  contrast / gamma table (`post_lut`) and integer saturation; fsr = bilinear,
  sharpness and bloom ignored. `--post-shots` in GPU mode renders that pass
  off screen. `--no-window` + `--shots`: read-back of the GPU picture.
- `--fps`: a presenter thread (own command buffer and buffers, own queue when
  the device has two) owns the swapchain. At every display flip the game
  thread publishes a package (draw list with current and previous vertices,
  start-of-frame and final copies of the targets in 4 rotating slots); the
  presenter shows package k over [ideal vblank time + lead, + frame
  duration), blending vertices by clock time, and the final copy at t = 1 or
  when the frame may not be interpolated (under 70% of vertices paired,
  upload into a target, texture dropped). Lead is 20 ms, growing to at most
  60 ms when the game thread finishes its frames late. Sprites that change
  size and primitives whose texture address changes are not paired (they
  jump); the blend constant FIX is interpolated. The game thread never waits
  for the presenter (a frame gets no package instead). Rate: `display` = the
  OS refresh rate of the window's monitor (Windows; 60 assumed on X11), by
  the clock.
- plat_win32.c: `plat_sleep_until` used one process-wide waitable timer; with
  a second thread sleeping it made the game thread oversleep by ~80 ms every
  few frames. The timer is now per thread.
- Alpha-fail modes are drawn batch-wise (passing fragments, then failing
  ones); `ROR_HW_EXACT_AFAIL=1` draws primitive by primitive (exact, slow).
  Scale-1 difference numbers did not change measurably.
- Testing aids: `ROR_FAST_UNTIL=frame` (no window, no pacing before that
  frame), `ROR_STALLTRACE=frame`, `ROR_HW_PRESENT_TRACE=frame`,
  `ROR_HW_STRIP=frame,count` + `ROR_HW_STRIP_DIR` (in-between pictures of two
  packages as PNGs), `ROR_HW_STATELOG=1`. `build/recomp/gscap/script_play.txt`
  is the standard script plus Cross every 400 frames and Up held from 21400
  (player control from about 21300).

Measured in the game (private builds of 2026-11-22):
- Software path: standard script to 16000, `--shots 1000`, `ROR_WAV`: 16 PNG +
  WAV md5s equal the previous shipping binaries on Linux and Windows.
- Headless GPU, lavapipe `--internal 1`, against software at landmarks: 900
  0.003%; 3050 0.07%; 5750 0.32% (0.22% >8, row 0); 6500/7500 0.09/0.07%;
  8500/10800 2.6/1.8% (0.25% >8, max 228); 14200..16000 32.8% (15.3% >1,
  0.16% >8, max 36).
- RTX 3090, Windows, headless `--internal 4`, unthrottled: 129 fps in the
  walking scene (frames 21500-22000); software there: 116 fps.
- `--fps display --internal 4` on a 240 Hz monitor (the window opened there;
  the 143 Hz monitor was not measured): storybook 240 presents/s, interval
  mean 4.17 ms, p95 5.2, p99 6.0, max 8.3; walking, windowed 1440x1080:
  223-235/s, mean 4.3-4.5, p95 7.2, p99 7.9, max 8.2 after the first second;
  fullscreen: 207-209/s, mean 4.8, p95 8.3, p99 8.8-12.2; game at 50 fps in
  all; 96-99% of vertices paired. Audio with interpolation on (30 s from
  boot): 1500 frames sent, 0 dropped, 1 underrun.

Not done / known gaps: row 0 of the feedback pass (wrapped texture row that is
depth memory); depth initial contents and depth as texture; non-32-bit frame
formats; lines are drawn as quads and made no measurable difference in the
captures (not checked visually); in-between pictures are judged by eye only;
the few frames where a t = 1 replay differed and the crease cracks in the
storybook were not investigated; `--fullscreen`/`--aspect stretch` on the GPU
path were run but not looked at; motion vectors, bloom and FSR on the GPU
path, widescreen: not started. Widescreen search so far: no 4:3 or 2048.0
literal in the ELF; `lui` of 256.0/224.0/448.0 at 0x193b9c-0x193c7c,
0x194040-0x194060, 0x196be0-0x196c0c (0x193b70 onwards converts screen
pixels to view-plane units for 2D placement); the 3D projection set-up was
not located.


## Widescreen (`--wide`) and GPU present pass, wrap-up (2026-11-22)

### Widescreen

`--wide off|16:9|21:9|32:9|W:H|ratio` (or `ROR_WIDE=`; the command line
wins). Ratio 1.0..3.6; anything up to 4:3 (and `off`) means off. `rt/wide.c`,
both renderers, both platforms. Hor+: same vertical view, more to the sides.

What was found in the game (found by tracing camera set-up and the VIF stream):
- Every camera is built by `0x228b80(a0 = renderer, a1 = frustum, a2 =
  world-to-view)`; 9 call sites. Frustum floats: [0] near, [1] far, [2] left,
  [3] right, [4] bottom, [5] top (on the near plane), [6..7] depth range,
  [8..11] viewport 0, 511, 0, 447, [12..13] XYOFFSET 1792, 1824. The 4:3 is
  `lui at,0x3f40` at 0x13bf70.
- The frame buffer stays 512x448; scissor and XYOFFSET are unchanged. The
  picture is rendered horizontally compressed and shown stretched.

How it works:
- `cpu.h` defines `TRACE_FN` (gen.py already emits it at the top of every
  generated function) as a test for entry 0x228b80 that calls `wide_hook`;
  the test folds away in every other function. **All `fn_*.c` are recompiled
  when cpu.h changes; the generated C itself is unchanged.**
- `wide_hook` runs the real function on a copy of the frustum (on the guest
  stack, marked with a magic word so the nested call is let through) whose
  left/right are scaled about their centre by aspect / (4/3). Only cameras
  with a ~4:3 frustum (1.30..1.37) are widened; the square 256x256 shadow
  camera is not.
- During the nested call it divides the guard-band half-width (floats at
  0x32a5e4 / 0x32a5e8) and the VU1 clip factor (renderer + 0x12128) by the
  widening and restores them afterwards. Without that a ground polygon
  reaching behind the camera is drawn with wrapped Z across the picture
  (seen at 16:9 and 21:9). This is also why the limit is 3.6.
- 2D: `gs.c submit()` (before the software/GPU split, so both renderers get
  it) squeezes PATH2/PATH3 primitives that go into a display buffer about the
  screen centre by (4/3) / aspect, except primitives spanning the full width
  (clears, fades, the game's letterbox bars, feedback and grain passes).
  `gif.c` exports `g_gif_path` for this.
- A frame counts as 3D when its cameras were widened and PATH1 drew into a
  display buffer (the game builds cameras during movies too). `wide_flip`
  (called from `gs_write_priv` when the displayed buffer changes) latches
  this; `wide_present()` gives post.c's `post_rect` the aspect of the frame
  on display: the wide one for 3D frames, 4:3 otherwise. post.c references
  it weakly (posttool has no wide.c); gsreplay.c has stubs.
- Window: without `--res` the window is 480N high and aspect * 480N wide
  (16:9, N = 2: 1707x960; `video_fit` shrinks what does not fit the screen).
  With `--res` / `--fullscreen` the picture is fitted per frame kind: 3D at
  the wide aspect, movies/menus 4:3 (pillarboxed). On a screen of another
  shape than `--wide` (e.g. 21:9 on a 2560x1440 monitor) the 3D picture is
  letterboxed (2560x1097) and a movie is full height (1920x1440), so the
  picture changes size at the cut.
- `--fps`: the picture rectangle now travels with the presenter's package
  (`Pkg.pdx..pwh`), because a package is shown some vblanks after the
  rectangle was computed; without it the first/last frames around a
  3D <-> 4:3 cut would be shown in the other shape.
- Log: `[video] widescreen: ...` at start, `[wide] vblank N: picture wide
  (3D)` / `4:3` at every change of shape (first 400).

Verified (final sources, Linux and Windows builds):
- `--wide` off: standard script to 16000, 16 PNG + WAV md5s equal the
  17:38/17:39 binaries on both platforms.
- Software `--wide 16:9` and `21:9`, play script to 22000, `--shots 200`:
  110 of 110 raw frames byte-identical to the prototype build
  on Linux and on Windows.
- Shape changes in the play script to 22000: wide at 7642 (storybook), 4:3
  at 8002, wide at 8026, then wide to the end (chapter card, gameplay). The
  8002-8026 gap is black (frames 8008 and 8021 are the all-black frame; 7995
  is the end of a fade), so nothing is seen to jump there. The storybook's 2D
  backdrop is squeezed into the middle 4:3 with the 3D book over it.
- GPU, lavapipe `--internal 1`, against software `--wide` (pixels differing
  any / >1 / >8, max): 21:9 at 6400 0.08/0.02/0% 2; 10800 0.9/0.26/0% 3;
  14400 30.7/14.7/0% 8; 21800 40.8/20.5/0.001% 14; 22000 41.1/20.7/0.004% 23
  - the same classes as without `--wide`.
- RTX 3090 (Windows), `--renderer gpu --internal 4 --wide 21:9 --res
  2520x1080`: headless with `--post-shots` exit 0, pictures looked at (3D
  fills 2520x1080, the caption and its ornaments are centred and 4:3
  proportioned, the movie is 1440x1080 in the middle); two runs of different
  builds gave 220 of 220 identical files. Windowed `--fps display` (144 Hz
  monitor), exit 0: caption scene 14000-14700 133.7 presents/s, 99.2% of
  vertices paired, 187 of 192 frames interpolated; walking 21350-22100 see
  the limitation below. The squeeze does not trip the pairing safeguards
  (95.7-96.4% paired while walking, the same as without `--wide`: 95.8%).

Known limitation (GPU renderer with `--wide`, found in the walking test):
for about one second of game time (vblanks 21482-21532 of the play script,
as the walk starts) the game re-decodes about 1.5 million texels per vblank
(`texture cache: ... texels decoded`: 73 k per frame averaged over 500
frames against 12 k at 4:3; the software renderer shows the same counts).
The GPU path decodes textures on the game thread, so those vblanks take
35-41 ms without `--fps` and 40-90 ms with `--fps display`; the 500-frame
average drops to 47 fps and the present rate to 105-115/s (p99 interval
~90 ms) for that stretch, then returns to 144/s. Software renderer: no
vblank over 35 ms there. Not the cause (tried and reverted): reusing
texture images instead of creating them; 16 cache ways instead of 4; the
texture-coordinate re-pairing search (now bounded to 16 candidates anyway).
The likely fix is decoding on the worker pool as the software path does, or
palette lookup in the shader. What the game changes in those textures was not
identified.

Not checked: 32:9 on the GPU renderer and on Windows; anything past the
first seconds of walking; `--aspect stretch` with `--wide`; Alt+Enter with
`--wide`; a real 21:9 or 32:9 monitor.

### What the GPU present pass does now (work of an interrupted session, kept after checking)

This supersedes "fsr = bilinear, sharpness and bloom ignored" and the row-0
gap in the GPU renderer section above.
- `hw/present.frag` is a multi-pass shader (pc.opt.w selects): EASU when the
  internal picture is narrower than the rectangle, an area filter when it is
  wider, RCAS (`--sharpness`), the colour table and saturation, bloom (glow
  from 4x4 cells, three box blurs, screen blend), through half-float /
  RGBA8 stage images of the presenting thread. `gs_hw_present_opts` hands
  over the RCAS constant (`post_rcas_con`) and the bloom strength.
  Checked: lavapipe `--internal 1 --res 1920x1440 --bloom 60 --post-shots`
  against the software post-processing: movie frames 3-8% of pixels differ,
  under 0.1% by more than 1, max 8; 3D frames differ as the raw frames do.
- Row 0: rows of a target that continue into 16-bit depth memory are built
  from the depth image (pass 7) or from the known clear value. Scale-1
  differences against software, by more than 8: 5750 0.22% -> 0; 8500 0.25%
  -> 0.014%; 14200 0.16% -> 0.001%; capture 5700 replay 0.11% -> 0.
- `ROR_HW_GRAB=frame,count[,stride]` + `ROR_HW_GRAB_DIR`: PNGs of the
  swapchain images exactly as presented (needs TRANSFER_SRC on the
  swapchain); the file name carries the vblank and t * 100.
- Interpolation: depth is saved/restored with the start-of-frame targets
  (`hw_ds_save/load`); sprites showing another texture cell are not paired;
  textured primitives left alone are re-paired by texture coordinates
  across states (storybook page turn). Replay self-check on capture 14650:
  t = 1 replay differs in 0-11 pixels per frame, 99.9-100% interpolated.
  The storybook crease itself was not looked at again.
- X11: refresh rate through XRandR, loaded with dlopen (falls back to 60).
  On the test system it reports 60, which cannot be told apart from the fallback.
- Descriptor pool access is under a mutex (two threads create images).

## Gamepad (2026-11-22)

### What the game expects
- The pad stack is DBCMAN + PAD2/DS1O_D (see the header of `rt/iop_dbc.c`).
  The driver configures a DualShock 2 as a DualShock 1: analog mode, ID 0x73,
  no pressure bytes (it never sends 0x4F). The emulated pad already answered
  that way before this work (ID 0x73, six data bytes: two button bytes, RX,
  RY, LX, LY); only the stick bytes were always 0x80. Nothing in `iop_dbc.c`
  had to change.
- RPCs seen in a `-v` run to frame 22400 (play script): version, SetWorkAddr,
  CreateSocket x2 (port 0 linked, port 1 never), GetDepNumber, 14 x
  ReceiveData 0x0101800c (connection state), and set-actuators 0x0103400b
  once on each SendData2 server at start-up. No vibration is requested
  in that stretch; the game has three call sites of its actuator function
  (0x101b50), so it does use vibration later.
- Per frame (0x101ccc) the game takes the button word and the four stick
  bytes. Stick axis conversion (0x101e64/0x101eec right X/Y, 0x101f74/0x101ffc
  left X/Y): byte < 96 -> (byte-96)/96, byte > 160 -> (byte-160)/95, else 0
  (a 25 % dead zone of its own). 0x102188 builds the movement vector: a held
  D-pad direction gives exactly -1/+1, otherwise the left stick value is
  used, and a stick axis beyond +-0.7 also sets the D-pad bit (menus).
  **The left stick is movement and is analog** (partial tilt walks slower);
  the D-pad equals full tilt. The right stick is read at 0x1b2620 and
  0x209268 (not investigated what for).

### Implementation
- `rt/pad.c` (common): HostPad (Xbox terms, `plat.h`) -> PS2 button word and
  stick bytes, OR-ed with the keyboard word, `pad_set_state`; vibration from
  `pad_get_actuators` to `hpad_rumble` when it changes (and 0,0 at exit).
  Stick mapping: radial dead zone (`--pad-deadzone`, default 24 %), the rest
  rescaled to 0..1, then each axis is placed directly outside the game's own
  dead band (96-n / 160+n, the inverse of the game's conversion), so only
  one dead zone is felt and full tilt is exactly +-1 (byte 0 / 255).
  Triggers count as L2/R2 from 64/255.
- `rt/pad_xinput.c` (Windows): `LoadLibraryEx(..., LOAD_LIBRARY_SEARCH_SYSTEM32)`
  of xinput1_4.dll, then xinput1_3.dll, then xinput9_1_0.dll; no import in the
  .exe. All four slots are read and merged (since 2026-11-24; before: the
  first connected one); empty slots are re-asked once per second.
  `XInputSetState`: big motor -> left (low frequency), small -> right.
- `rt/pad_evdev.c` (Linux): `/dev/input/event*` with `<linux/input.h>` (from
  linux-libc-dev in `tools/debs/qtdev`, added to build.sh's include path
  after the others). Gamepad = reports BTN_SOUTH; checked in
  `/sys/class/input/eventN/device/capabilities/key` before opening, so
  keyboards/mice are not opened and a gamepad that cannot be opened gets one
  log line. (Device choice was replaced on 2026-11-24, see "Linux gamepads:
  classification, all pads merged" below.) Rescan once per second while there is
  none; state by EVIOCGKEY/EVIOCGABS per frame; unplug = read/ioctl error.
  Codes 0x133/0x134 are X/Y for xpad-style devices and top/left for Sony
  (054c) and Nintendo (057e). Right stick on ABS_RX/RY (triggers ABS_Z/RZ) or,
  without RX/RY, on ABS_Z/RZ; ABS_BRAKE/GAS and BTN_TL2/TR2 are triggers too.
  Vibration: FF_RUMBLE effect (needs the device opened read-write).
- `video.c`: a real gamepad is read only in `pad_update(frame, kbd, live)`
  after `vwin_pump`, i.e. only with a window open, and `live` is 0 when
  `--script` is given. `--no-window` never reaches that call. The scripted
  path (`pad_set_state` at script frames) is unchanged.
- Switches: `--pad auto|off` (default auto), `--pad-deadzone 0..90` (default
  24); a bad value prints usage and exits 2.
- Log lines (stderr, prefix `[pad]`): at the first window frame one of
  `gamepad: NAME (interface...; stick dead zone N %)`,
  `no gamepad found (...); keyboard only until one is connected`,
  `gamepad input is off (--pad off)`, `scripted run: no gamepad is read`;
  later `gamepad connected: NAME (...)` / `gamepad disconnected: NAME`.

### Test hook
`ROR_PAD_FAKE="frame:buttons[:lx:ly:rx:ry[:lt:rt]],..."` feeds a synthetic
HostPad through the same mapping code (`pad_update`), with or without a
window: buttons = XInput mask in hex (0001 up, 0002 down, 0004 left, 0008
right, 0010 start, 0020 back, 0040/0080 stick clicks, 0100 LB, 0200 RB, 1000
A, 2000 B, 4000 X, 8000 Y), sticks -32768..32767 with +y up, triggers 0..255;
an entry holds until the next. While it is set no real device is read.
`recomp/tools/padfake.py SCRIPT [stick|half FROMFRAME]` converts a
`--script` string (optionally turning D-pad directions into left-stick tilt);
The result is passed in `ROR_PAD_FAKE` in place of `--script`.

### Verification (no physical controller was available: none on the Linux test system, where
/dev/input does not exist and /dev/uinput is root-only, and none on the
Windows side)
- Standard script to 16000, `--shots 1000`, `ROR_WAV`: 16 PNG + WAV md5s
  equal for the previous binaries and the new
  ones, Linux == Windows (frame_016000 ed535164..., audio.wav 670b42cf...).
  Play script to 22400, `--shots 100` (224 PNG + WAV): new == previous (Linux).
- The same standard script given only as `ROR_PAD_FAKE` (A/Start/D-pad/B, no
  `--script`): identical md5s, Linux and Windows.
- Play script as fake pad with the D-pad walking (Up 21400, Right 21900)
  replaced by full left-stick tilt: all 224 PNG + WAV identical to the
  keyboard-script run, Linux and Windows (full tilt = D-pad, as the game
  code says). With 60 % tilt the frames differ from 21500 on: the character
  walks more slowly along the same path (looked at 21800, 22200).
- Window: Linux `--frames 300` logs `no gamepad found (evdev:
  /dev/input does not exist)`; with `ROR_PAD_FAKE` in a window the frames
  100..1400 equal the headless ones. Windows `ror.exe --frames 300` in a real
  window: exit 0, `no gamepad found (XInput, xinput1_4.dll)`. `--pad off`
  and `--script` in a window log their lines; bad values exit 2.
- The sysfs capability-bitmap parser was checked against sample strings.
- NOT tested: any real device on either platform (detection, hot-plug,
  disconnect, axis ranges, the 0x133/0x134 layouts, permission message),
  vibration (XInputSetState / EV_FF), the right stick in game.
- Not done: button remapping, a vibration on/off switch, ignoring the pad
  while the window is unfocused (XInput and evdev deliver input regardless
  of focus), more than one pad (port 2 stays empty), DirectInput-only pads
  on Windows, evdev devices without BTN_GAMEPAD.

## PlayStation pads (2026-11-22, on top of "Gamepad")

### Files
- `rt/pad_ps.c`, `pad_ps.h` (both platforms): DualShock 4 / DualSense input
  report decoding to HostPad, vibration output reports, CRC-32, self-test.
  The report layouts are written out at the top of `pad_ps.c`.
- `rt/pad_hid.c` (Windows): finds 054c:05c4 / 09cc (DualShock 4), 0ba0 (USB
  wireless adapter; "no pad linked" = byte 31 bit 2 of its report), 0ce6
  (DualSense), 0df2 (DualSense Edge) with SetupAPI + hid.dll, both loaded at
  run time from System32 (no import; imports are still KERNEL32, USER32,
  GDI32, SHELL32, WINMM, UCRT). One background thread does everything that
  can wait: enumeration (once per second while there is no device; only
  interface paths containing "054c" are opened, first with no access rights
  for HidD_GetAttributes), opening (read-write, else read-only = no
  vibration), feature report 0x05 (and 0x02 for DualShock 4) on Bluetooth to
  switch on the extended reports, overlapped ReadFile, vibration WriteFile
  (padded to the device's output report length, 200 ms limit). The game
  thread copies the last state under a critical section. Bluetooth is
  recognised by the HID service UUID in the path or input reports longer
  than 64 bytes. The short Bluetooth report 0x01 is decoded too, so buttons
  and sticks work even if the extended mode never starts.
- `rt/pad_win32.c` (Windows): source selection. `pad_xinput.c` now exports
  `xi_*`. Rule in `auto`: XInput first — while XInput reports a pad it is
  the only source and the HID reader is idle with its device closed; a Sony
  pad is read directly only while XInput has none; the source in use is
  kept until it disconnects. One source at a time, so a pad wrapped by Steam
  Input / DS4Windows cannot give double input (the wrapper wins).
- `rt/pad_evdev.c` (Linux): Sony layout re-checked against hid-playstation /
  hid-sony (BTN_SOUTH Cross, EAST Circle, NORTH Triangle, WEST Square;
  ABS_Z/RZ triggers; HAT0 D-pad; ABS_X/Y, RX/RY sticks) — it was already
  handled (vendor 054c = positional). Changes: devices with the
  ACCELEROMETER, POINTER or BUTTONPAD property are refused (the motion-sensor
  and touchpad devices of the same controller; they have no BTN_SOUTH
  anyway), and BTN_TL2/TR2 are ignored when the pad has analog triggers, so
  L2/R2 switch at a quarter of the travel like everywhere else.
- Mapping: Cross/Circle/Square/Triangle, L1/R1, L3/R3, D-pad and both
  sticks 1:1; L2/R2 from the analog bytes at >= 64/255; Share/Create =
  Select; Options = Start; PS button and touchpad click unmapped. Light bar
  and LEDs are left alone (the output reports set only the motor flags).
- Vibration reports: DualShock 4 USB 0x05 / Bluetooth 0x11 + CRC; DualSense
  USB 0x02 / Bluetooth 0x31 + CRC (layout as SDL's HIDAPI driver: byte 1 =
  0x02, flags at 2; the Linux driver frames 0x31 differently, with a
  sequence byte and tag 0x10).
- `--pad auto|off|xinput|playstation`. On Linux `xinput` means "not a Sony
  device" and `playstation` "Sony devices only".
- Log: `[pad] gamepad: NAME (INTERFACE; kind xbox|playstation; stick dead
  zone N %)`, e.g. `[pad] gamepad: DualSense (HID, USB; kind playstation;
  stick dead zone 24 %)`; `kind` tells a front end which button legends to
  show. The "no gamepad found" start-up line now comes at the 25th window
  frame (the HID search runs in the background) and names both interfaces.
- `ROR_PAD_SELFTEST=1 ror` prints the decoded sample reports and exits 0/1.

### Verification (no controller of any kind was available)
- `ROR_PAD_SELFTEST=1`, identical output from the Linux binary and ror.exe:
  idle DualShock 4 / DualSense USB reports decode to all-zero; one
  constructed state (LX 0, LY 255, RX 255, RY 64, hat up-right, Cross,
  Triangle, L1, Options, R3, PS, L2 30, R2 200) decodes to buttons 9199, lt
  30, rt 200, lx -32768, ly -32767, rx 32767, ry 16384, extra 1 in all six
  formats (DS4 USB 0x01, DS4 BT short 0x01, DS4 BT 0x11, DualSense USB 0x01,
  DualSense BT short 0x01, DualSense BT 0x31); wrong IDs / short reports are
  refused; crc32("123456789") = cbf43926; the CRC of both Bluetooth output
  reports matches a second computation. The samples were built from the
  same layout description as the decoder: this checks the code against the
  description, not the description against a controller.
- Standard script to 16000 (16 PNG + WAV) and the fake-stick play run to
  22400 (224 PNG + WAV): identical to the earlier baselines on Linux and
  with the final ror.exe. Headless runs print no `[pad]` line and start no
  HID thread (the selector is only reached from the window path).
- Windows, real window, `--frames 300`, three runs each, previous exe vs
  new: 6.43 / 6.10 / 6.13 s vs 6.10 / 6.13 / 6.10 s (start-up not slowed).
  Log: `[pad] no gamepad found (XInput, xinput1_4.dll: no pad; HID: 26
  devices, no DualShock 4 / DualSense among them); keyboard only until one
  is connected`; `--pad playstation` and `--pad xinput` name only their own
  interface; a bad value exits 2.
- NEVER RUN against hardware: everything in pad_hid.c past the enumeration
  (opening a Sony device, USB/Bluetooth detection, the feature-report
  switch, the read loop, disconnect, the wireless adapter, vibration
  writes, the exit-time motor stop), the XInput-first hand-over, the real
  report contents of any pad, and on Linux any Sony device.
- Not done: DualShock 3 on Windows (needs a special driver there; on Linux
  it is an ordinary evdev pad through hid-sony), input CRC check on
  Bluetooth reports, vibration keep-alive, light bar colour, touchpad/PS
  button functions, more than one pad.

## PlayStation pads: review fixes (2026-11-22)

An independent review compared `pad_ps.c` / `pad_hid.c` / `pad_evdev.c` /
`pad_xinput.c` with the Linux kernel's hid-playstation.c and current SDL
(HIDAPI PS4/PS5 drivers). Confirmed as matching those sources: all input
report layouts, DualShock 4 vibration (USB and Bluetooth), DualSense USB
vibration, the CRC, the feature-report switch to extended Bluetooth reports,
the evdev mapping and XInput. Changed after it:
- DualSense Bluetooth vibration report now uses the kernel's framing
  (`struct dualsense_output_report_bt`; SDL moved to it in 2026): [0] 0x31,
  [1] sequence << 4 (4 bits, one up per report), [2] 0x10, [3] flags 0x03,
  [5] weak, [6] strong, CRC at [74]. The earlier form (0x31, 0x02, flags...)
  described in the section above is gone.
- DualShock 4 Bluetooth output byte [1] is 0xC4 (HID + CRC + 4 ms report
  interval, as SDL) instead of 0xC0.
- A failed vibration write is logged once per distinct error:
  `[pad] HID: vibration write failed (error N, len M)`.
- Enumeration no longer stops at the first Sony device: a pad that cannot be
  opened, or a USB wireless adapter with no pad linked (probed with one read,
  200 ms limit, on the background thread), is passed over and the search
  goes on; an adapter whose pad goes away later is closed so that another
  pad can be found. Status in that case: `HID: N devices, a DualShock 4 USB
  wireless adapter with no pad linked to it`.
- Left as it was: DualSense vibration flags 0x03 (legacy mode, documented in
  the kernel driver as working on all firmware; COMPATIBLE_VIBRATION2 is not
  used); no Windows 7 fallback for CancelIoEx / LOAD_LIBRARY_SEARCH_SYSTEM32;
  sysfs capability words are assumed to be 64-bit.
Verified again: self-test all ok and identical on both platforms (rumble
heads `11 c4 00 01 00 00 ff 80`, `31 00 10 03 00 ff 80`, sequence byte 10
then 00 after sixteen more); standard script to 16000, 16 PNG + WAV equal to
the baseline on Linux and Windows; Windows window run of 300 frames in 6.09 s
with the same "no gamepad found ... HID: 26 devices" line.
Still unverified on hardware: everything listed under "NEVER RUN" above —
the layouts now agree with two independent implementations, but no
controller has been connected to this code on either platform.

## FSR 3.1 (`--filter fsr3`), hairline fix, `--film-grain` (2026-11-23)

### FSR 3.1
Developed as a separate patch (applied without
offsets). Design is described at "FSR 3.1" in `rt/gs.c` and `rt/hw/gs_hw.c`;
in short: Windows + `--renderer gpu` + AMD's `amd_fidelityfx_loader_vk.dll`
and `amd_fidelityfx_upscaler_vk.dll` beside the exe (FidelityFX SDK 2.4.0,
`Kits/FidelityFX/signedbin`; not in this tree, shipped by the package step
with AMD's licence). The scene is drawn a second time from the retained draw
list with jitter, VU1 triangles once more into a motion-vector/depth/reactive
target, the 2D layer after the last VU1 primitive is composited after the
upscale; frames without VU1 geometry go the FSR 1.0 way. `rt/hw/ffx_min.h`
is a transcription of the few API declarations used (MIT, AMD's notice kept).
`wide_hook` now reads the camera frustum on every 0x228b80 call (for the
upscaler's near/far/fov), widescreen or not.

Fallbacks, each with one log line, then exactly `--filter fsr`:
software renderer (`[video] --filter fsr3 (FSR 3.1) needs --renderer gpu`),
Linux (`FSR 3.1 needs AMD's FidelityFX DLL, which exists for Windows only`),
DLL missing (`amd_fidelityfx_loader_vk.dll could not be loaded ... (error
126)`), context creation or dispatch failing.

Verified:
- Software path with the patch: standard script to 16000, 16 PNG + WAV md5s
  equal the pre-patch binaries, Linux and Windows. Pad self-test passes on both.
- GPU path without fsr3, patch only (before the hairline fix below): 3090,
  `--internal 2 --filter fsr`, play script 22000/200 with `--post-shots`,
  4:3 1920x1440 and `--wide 21:9` 2520x1080: 220 of 220 files identical to
  the pre-patch exe in both. With the final build the same holds at
  `--internal 1` (220 of 220, wide); at 2 and above the hairline fix changes
  the picture on purpose.
- fsr3 on the 3090: log `temporal upscaler: 3.1.6, 512x448 -> 1920x1440, 112
  jitter phases` (1x) / `1024x896 -> 1920x1440, 28 jitter phases` (2x), exit 0.
  Against the patch author's frames (6400, 10800, 14400, 21800, 22000):
  `--internal 2` byte-identical; `--internal 1` identical at 6400 (movie),
  0.3-2.4% of pixels differ at the 3D frames (under 0.011% by more than 1).
  That is run-to-run variation, not the build: two runs of the same exe
  agree in 82 of 110 files, and the author's own exe re-run also differs from
  the author's stored frames. Cause not looked for.
- Window, `--fps display --internal 1 --filter fsr3 --res 1440x1080`, 240 Hz
  monitor, frames 21350-21950: 205.8 and 221.0 presents/s, 99.7% of vertices
  paired, exit 0 (one 286 ms interval in the second block, while two
  swapchain grabs were being written).
- Linux lavapipe and Windows without the DLLs: `--filter fsr3` output files
  are byte-identical to `--filter fsr`.

Upscaler context at 3D <-> 4:3 cuts under `--wide`: the first 3D frame after
a cut is still presented in the 4:3 rectangle (the widescreen latch is one
frame late), so the context is created twice in two consecutive vblanks
(1024x896 -> 1440x1080, then -> 2520x1080), at 7641/7642 and at 8025/8026 in
the play script. Paced window run with `ROR_STALLTRACE`: the vblank at 7641
came after 40.0 ms (41.0 ms with `--filter fsr`: the scene start itself), and
no vblank around 8025/8026 exceeded 35 ms. So two creations fit in the
15 ms a vblank may run late before it shows in that trace; left as it is.
Not done: the upscaler context is not destroyed at exit. Nothing of Vulkan is
(the process exits with the device alive), and with `--fps` the context
belongs to the presenter thread, which is not stopped at exit.

### Hairlines at `--internal` 2 and above (fixed)
Seen with the GPU renderer, FSR 1 and a wide picture: thin dark lines across the scene.
- Not the game's film effect: the only LINE primitives of a frame are four
  that clear the border of the 256x256 shadow texture.
- Not FSR: `--filter bilinear` shows them the same.
- Replaying a capture of frames 21996-22004 at 4x with one texture's
  primitives left out at a time (`ROR_HW_SKIPTEX`): they disappear with TBP
  2ea0 in the frame where that address holds a 256x128 grass texture (785
  alpha-tested triangles, REPEAT, bilinear).
- Cause: `gs.frag` shifted the texture coordinates of triangles by
  (0.5 - 0.5 N) device pixels so that an N x N block was centred on the native
  sample. At the top/left edges that is up to 0.375 native pixels outside the
  triangle; the coordinate wraps and the opaque far edge of the texture is
  drawn as a line one or two device pixels wide along the polygon edge.
- Fix: no shift (`pc.rt[3] = 0`); a fragment takes the coordinates
  interpolated at its own centre, which is always inside the triangle.
  `ROR_HW_UVSHIFT=1` brings the old behaviour back for comparison. No change
  at `--internal 1` (verified byte-identical). Sprites still use the centred
  formula.

### Grain
The game draws its own film grain: full-screen PATH2/3 sprites (4-5 per
frame) that tile a 64x64 32-bit noise texture with random offset and scale
(1.4 to 3.4 native pixels per texel in the frame looked at), blended
`(Cs - Cd) * FIX + Cd` and `(Cd - Cs) * FIX + Cd` (FIX 15 at the bus stop,
32 where it is first seen). It is in the software picture too. It is more
visible on the GPU path at high `--internal` because each speck stays sharp,
and because of the next point.
`--film-grain 0..100` (default 100): `gs.c submit()` recognises those sprites
(sprite, textured 64x64 CT32, blend by FIX, full width, texture repeated
across it) and scales FIX; 0 drops them. Both renderers. One log line
`[gs] film grain: N% (noise texture at ..., blend constant ...)` when it
first acts. At 100 the code path is not entered (md5s unchanged).
Only checked in the scripted stretch (storybook to first walk).

### Found, not changed
- The frame's last pass copies the frame buffer onto itself through a
  bilinear sprite offset by half a pixel, colour x 1.19: on the PS2 that is a
  2x2 box blur of the whole picture (grain included). At `--internal` N > 1
  the GPU path samples the scaled target at device texel centres, so the pass
  is only the brightening: the picture is sharper than the game makes it and
  the grain is not softened. Reproducing the blur at N x would need taps N
  device texels apart.
- Lines are drawn one native pixel wide (N device pixels), as on the GS.
- Under `--wide` the full-width grain sprites are not squeezed, so the grain
  is stretched horizontally by the widening.
- The "streaky foliage" seen in a later area was not
  reproduced: the scripted run does not reach that area.

Debug aids added: `ROR_HW_SKIPTEX=hex[,hex]` (`fffff` = untextured),
`ROR_HW_SKIPLOG=n`, `ROR_HW_UVSHIFT=k`.
Final build (with the hairline fix and `--film-grain`): software md5s, pad
self-test and the `--internal 1` GPU identity re-checked; fsr3 headless
re-run at 1x and 2x, exit 0, same upscaler log lines; the 2x fsr3 picture
differs from the patch author's now (the hairline fix applies to its scene
pass too; looked at, hairline gone). The fsr3 window run and the fallbacks
were not repeated after those two changes.

## Building without game-derived files in the tree (2026-11-23)

The repository holds nothing made from the game. Not tracked any more and not
needed: `recomp/rt/vu1_fast.inc`, `recomp/tools/vu1_micro_*.bin`,
`recomp/tools/vu1_entries_*.txt`. What the build needs from the user's disc
is produced under `build/` by `recomp/tools/gamefiles.sh`, which both build
scripts call:
- `gamefiles.sh elf`: if `build/native/SLES_542.18` is missing it is copied
  out of the disc image (`ROR_ISO`, or the usual names in the repository
  root) by `tools/isoextract.py` (root directory of the ISO 9660 image).
- `gamefiles.sh vu1` (after gen.py): `tools/vu1extract.py` reads the VU1
  micro program out of the executable, `tools/vubench.c gen` (built for the
  host as `build/vubench/vugen`, with `-DVU_NO_FAST`) translates it, and the
  result is `build/recomp/gen/vu1_fast_gen.inc`, which `rt/vu.c` includes
  when it exists (`__has_include`; the directory is on the include path).
  Regenerated when the executable, the extractor, vubench.c, vu.c, vu_ops.h
  or vif.c are newer than `build/recomp/vu1/.stamp`; the file keeps its time
  when the content did not change. build.sh recompiles vu.c when it is newer.

Where the program is: section `.vutext` of the ELF is a ready-made VIF
stream of MPG blocks (44 of them: 30 for VU1 addresses up to 0x3de8, then the
VU0 program from address 0 again; the `.DVP.overlay..ADDR...` section names
list the same blocks). The game does not send all of it: the function at
0x221000 builds one REF tag from two address constants, start 0x2932b0 and
(0x2967ff - start) >> 4 = 0x354 quadwords, which ends before the 30th block
(address 0x3c00, 61 instructions; never loaded). vu1extract.py reads those
two constants from the code (and stops with a message if the instructions
are not the expected lui/addiu pairs) and applies the 29 MPG blocks in that
range to an empty 16 KiB image: 1668 instructions, byte-identical to the
image that had been dumped from a running game.

The seven computed-jump targets the generator cannot find by itself (the
command loop's return address 0x28 and the clipper's six plane handlers
0x2a50..0x2c68) used to be an untracked entries file. They are now a small
table in vubench.c keyed by the program's hash (208dc456c8f9e587); another
program gets no hints (still correct: an unlabelled target leaves the fast
path for the interpreter). An entries file given on the command line still
overrides. Two attempts to derive them from the program (constants loaded
into registers that JR uses; code nothing falls into) gave different sets
and were dropped.

Checked: generated file == the old `recomp/rt/vu1_fast.inc` byte for byte; a
copy of the tree made from `git ls-files -co --exclude-standard` (so without
the three files), with only the toolchain directories and the ISO linked in,
builds both binaries with `recomp/build.sh` / `build-win.sh`, creates nothing
outside `build/` except `__pycache__`, and its Linux binary is, stripped,
byte-identical to the installed one; software md5 regression (16 PNG + WAV)
identical on Linux and Windows; same speed (fast path in use).

Other game-derived material looked for in the tree (numeric tables of 24+
literals, disassembly listings, binary files): none. The only tables are the
GS memory-layout tables (gs.c, tools/gs_ref.c) and the MPEG-2 scan and
quantiser tables (ipu.c). Game addresses and single instructions quoted in
comments and in these notes are short references.

## Uphill path: what the streaks are; grain under `--wide` (2026-11-23)

Reaching the area: `recomp/tools/script-uphill.txt` (`--script "$(cat ...)"`,
no memory card): the play script up to the bus stop, then Left from 21400 and
Up+Left from 22200. The path with the broken fences is on screen from about
22500; a caption cutscene runs around 24250-24500; 24750 and 25000 show the
gate and fences from above. The walk is the same on Linux, Windows, both
renderers and with `--wide`.

Streaks and smeared areas in the foliage at `--internal 4`, examined at
vblank 24750 (capture replayed at 4x, primitives of one texture address left
out at a time, software against GPU):
- The streaked translucent bands are tree trunks and branches: opaque
  alpha-blended meshes with a 256x512 bark texture (vertical grain), seen
  through the leaf layers drawn over them. Leaving out the bark texture
  removes the band. The software renderer and the GPU renderer at 1x show the
  same thing; at 4x the bark lines are resolved instead of averaged.
- The smeared background is the distant hillside, a low-resolution texture
  magnified with bilinear filtering.
- Minification: every textured primitive of the two captures uses MMAG =
  linear and MMIN = linear without mip levels (MXL is 1 on some, but MMIN
  never selects a level). The game does not mip-map, so nothing is missing in
  the port; minified textures alias on the console too.
- Wrapped-Z polygons (the widescreen clip problem): none in the frames
  rendered at 21:9 along this walk.
So this class is the game's content at a higher resolution, not a defect.

Correction to the previous section ("Found, not changed"): the frame's last
pass is NOT a 2x2 blur. Its sprite maps pixel x to u = x + 0.5, which is the
texel centre for the GS's bilinear filter, so horizontally it is an exact
copy; v is 1/16 texel off (a 15:1 blend of each row with the one above).
The pass is the x1.19 brightening on the console as well, and the GPU path
reproduces it at any scale. There is therefore no console blur to offer as
a switch; none was added.

Grain under `--wide` (defect of the widescreen layer, fixed): the full-width
grain sprites are not squeezed, so the stretched presentation widened every
speck by aspect / (4/3). `gs.c submit()` now widens their U range by the same
factor while 3D is widened (the 64x64 noise repeats more often), which keeps
the specks' shape. Both renderers; no change without `--wide`.

## Releases: the game program is built on the player's machine (2026-11-23)

A release archive contains no game code. It holds the launcher, the runtime
as compiled objects, the recompiler, a C compiler and a Python, and
`ror-build`, which makes `ror-game` from the player's own disc image.

Files: `release/ror_build.py` (the tool), `release/ror-build.sh` and
`release/ror-build-win.c` (its starters), `release/make-kit.sh` (builds the
kit), `release/package.sh` (archives), `release/fetch.sh` (pinned downloads),
`.github/workflows/release.yml` and `build.yml`.

Contract of `ror-build` (used by the launcher):
- `ror-build --iso PATH --out DIR [--jobs N]`: stdout, UTF-8, one line per
  step: `PROGRESS <done> <total> <text>` (total is 5 until the file list is
  known, then 5 + number of files, 83 today), then `DONE <path>` (exit 0) or
  `ERROR <one line>` (exit 1). Log: `DIR/build.log`. Work files:
  `DIR/.build`, removed on success, on error and on SIGTERM; the executable
  and `DIR/ror-game.stamp` are put in place by renames at the end.
- `ror-build --check --iso PATH --out DIR`: `up to date` (0) or
  `needs build: <reason>` (1). The stamp is JSON: image size, sha256 of the
  game executable, release version, kit hash (`buildkit/KITHASH`, a hash of
  the recompiler, the runtime objects and sources, the VU1 translator and
  the tool).
- The image must hold `SLES_542.18` with the known sha256 (European disc).
- Killing it: children are tied to it (Linux: own process groups, killed on
  SIGTERM, and PR_SET_PDEATHSIG; Windows: a kill-on-close job object in the
  starter and in the tool).

What runs where:
- Compiler: Zig 0.13.0 (`zig cc`, clang 18.1.6, the LLVM release the project
  already used), targets `x86_64-linux-gnu.2.31` and `x86_64-windows-gnu`.
  The same sources built entirely with it give the same 16 PNG + WAV md5s as
  the clang 18.1.8 builds, at the same speed (103 against 102 fps headless in
  the first scene).
- On the player's machine: the generated C (`fn_*.c`, `fn_table.c`) and the
  three runtime sources that include generated headers (`support.c` for
  funcs.h, `vu.c` for the VU1 include, `interp.c` for the interpreter
  tables), 78 files, then the link with the prebuilt objects.
- Prebuilt (`buildkit/rt/lib`): every other runtime source. Linux: compiled
  with zig for glibc 2.31. Windows: compiled with llvm-mingw as before,
  because the runtime uses pthreads and Zig's Windows target has none;
  llvm-mingw's `libpthread.a` (winpthreads) is shipped and linked by zig.
- `vugen`: recomp/tools/vubench.c built with `-DVU_NO_FAST`.
- Python: python-build-standalone 3.12.16 runs the tool and gen.py; the
  recompiler is shipped as source. Chosen over a frozen executable because it
  needs no build step per platform (both archives are made on one Linux
  runner) and the same files run in development.
- The kit drops what is not needed from Zig (other targets' C headers, libc++,
  musl, WASI, macOS) and from Python (tests, Tk, IDLE, pip).

Changes made for this in the runtime:
- `video_x11.c` loads libX11.so.6 with dlopen; nothing of X11 is linked.
  Without the library a windowed start logs one line and runs without a window.
- `vu.c` takes the time from `plat_time_ns` instead of `clock_gettime`
  (winpthreads); vubench.c / vurun.c supply it with `timespec_get`.
- `gs_hw.c` looks for the AMD FSR 3.1 DLLs beside the program and then in
  `%ROR_APP_DIR%` (the launcher's folder), since ror-game no longer sits
  beside the launcher.

Build scripts take their tool locations from the environment (defaults are
the private set-up): `CLANG`, `ROR_TOOLCHAIN=system` + `CC`, `VULKAN_INC`,
`LLVM_MINGW` (recomp/build.sh, build-win.sh); `CMAKE`, `NINJA`, `QT_LINUX`,
`QT_WIN`, `ROR_CLANG`, `ROR_TOOLCHAIN=system` (tools/build.sh, build-win.sh,
the two CMake toolchain files, recomp/package.sh). `GAME=none
recomp/package.sh` lays out a package without ror-game; `release/package.sh`
uses that and adds the kit.

Measured (Ryzen 9 7900X, 24 threads, from the unpacked archives, paths with a
space and an accent, read-only install folder, PATH without any compiler or
Python): Linux 35-37 s, Windows 42 s; 220 CPU-seconds on Linux; largest single
compiler process 437 MB. Archives: Linux 122 MB, Windows 125 MB. The game
built that way gives the reference md5s (16 PNG + WAV) on both systems.

Pinned downloads and their SHA-256 are in `release/fetch.sh`.

## ror-build: progress, timing and cancelling (2026-11-23, follow-up)

- Progress lines are `PROGRESS <n> 100 <text>` from the first line on, with
  player-facing text only: "Checking your disc", "Reading the game",
  "Building the game (N of 78)", "Finishing". File names and commands are in
  build.log, each with its duration and the time since the start.
- What took the last 25 s of a build was not the link but `rt/vu.c`: with the
  translated VU1 program it is one very large function and takes 24 s alone
  (28-29 s with the other compilers running), and it was started last. Files
  are now started longest first, so it runs beside everything else: 31 s in
  all on both systems on the 24-thread development machine (before: 35-38 s
  Linux, 42 s Windows), 45 s with `--jobs 4`.
- On Windows the link took 16 s because the first link for a target makes Zig
  build its C library and support code. A link of an empty program with the
  same options (they are part of Zig's cache key; `-s` matters) now runs
  beside the compiling; the real link then takes 0.2 s.
- The percentage follows time: the compile phase is expected to last as long
  as the longest file or as all files shared among the workers, whichever is
  more, from nominal times (measured here) scaled by how fast the first fifth
  of the work went on this machine. A ticker prints twice a second.
- Cancelling. Linux: ror-build makes itself a process group; a small guard
  process outside it watches a pipe and, when ror-build ends without telling
  it (SIGKILL, crash), kills the group and removes the work folder. SIGTERM:
  the group is signalled, the folder removed, `ERROR cancelled`. Windows: the
  job objects end the compilers; the work folder of a killed build is removed
  by the next build or `--check`. `DIR/.build/pid` (touched twice a second)
  tells a stale folder from a running build. `--check` never looks at the
  work folder to decide: only ror-game plus a matching stamp count.
- With `ROR_MC_DIR` set, ror-game writes nothing into its own folder or the
  current folder in a normal run (checked from a read-only folder). Without
  it, it creates `saves/mc0` beside itself. `--shots` and the debug dumps
  write under the current folder.

## Linux gamepads: classification, all pads merged, diagnostics (2026-11-24)

Reported: an Xbox controller does not work on Linux (no log available). No
input devices exist on the development machine, so the cause was looked for
in the sources and in a simulation (`recomp/tools/padsim.sh`).

### What was wrong in `rt/pad_evdev.c`
- It took the lowest-numbered event node that reports BTN_SOUTH and stopped.
  Virtual devices made by key remappers and similar tools declare the whole
  key range (BTN_SOUTH included) and exist before a pad is plugged in, so
  they have lower numbers: the game then held on to a device that never
  reports a button and never looked at the pad. The simulation reproduces
  this with the old file (scenario 1: the device in use is "keyd virtual
  keyboard", no input arrives) — this is the most likely cause of the report,
  but without a log from an affected machine it is not proven.
- Only one device was ever used, so any wrong pick was final.
- A node that could not be opened was reported only for EACCES, once, in a
  line that did not name the remedy; other nodes left no trace in the log.

### Checked against the sources
- udev `input_id` (systemd `src/udev/udev-builtin-input_id.c`): a joystick
  has buttons in BTN_JOYSTICK..BTN_DIGI (or BTN_DPAD_*, BTN_TRIGGER_HAPPY*) or
  axes ABS_RX..; "Joystick un-detection. Some keyboards have random joystick
  buttons set": with 4 or more of ten well-known keyboard keys it is a
  keyboard. With absolute axes and a mouse button it is an absolute mouse.
- SDL `SDL_EVDEV_GuessDeviceClass`: joystick = ABS_X and ABS_Y plus
  BTN_TRIGGER / BTN_A / BTN_1 or a further axis; accelerometer and button-pad
  properties decide first.
- Access: systemd `rules.d/50-udev-default.rules.in` has `SUBSYSTEM=="input",
  GROUP="input"` (and mode 0664 only for `js[0-9]*`); `70-uaccess.rules.in`
  has `SUBSYSTEM=="input", ENV{ID_INPUT_JOYSTICK}=="?*", TAG+="uaccess"`. So
  the user of the active local logind session gets an ACL on joystick event
  nodes; without such a session (no logind, a container, ssh) the node is
  root:input 0660 and the open fails unless the user is in the input group.
- EVIOCGRAB (kernel `drivers/input/evdev.c`): events go only to the grabbing
  client (`client = rcu_dereference(evdev->grab); if (client)
  evdev_pass_values(client, ...)`), but EVIOCGKEY / EVIOCGABS are answered
  from the device's own state (`evdev_handle_get_val(client, dev, EV_KEY,
  dev->key, ...)`, `bitmap_copy(mem, bits, maxbit)`). This file reads state,
  not events, so a grabbed pad still reports; if the physical pad and a
  virtual copy both report, the merged state is the same as one.
- Xbox drivers: xpad registers BTN_A/B/X/Y, BTN_START/SELECT, BTN_THUMBL/R,
  BTN_TL/TR, BTN_MODE, ABS_X/Y/RX/RY (-32768..32767), triggers ABS_Z/RZ
  (0..255, 0..1023 for Xbox One) or BTN_TL2/TR2, D-pad ABS_HAT0X/Y or
  BTN_DPAD_*; xone and xpadneo the same with 0..1023 triggers. BTN_X is code
  0x133 and BTN_Y 0x134 in all three, which the existing mapping assumed.
  On the generic HID mapping (hid-input: buttons from BTN_GAMEPAD upwards in
  usage order) a Bluetooth pad has its right stick on ABS_Z/RZ and triggers
  on ABS_BRAKE/GAS; that was handled already.
- The sysfs bitmap parser was correct for the kernel's format
  (`input_print_bitmap`: unsigned long words, highest first, leading zero
  words skipped); the simulation now produces that format from the device
  tables, e.g. an xpad key bitmap `7cdb000000000000 0 0 0 0`.

### What changed
- Classification (`why_not_gamepad`): BTN_SOUTH, and stick or hat axes or the
  four BTN_DPAD_* buttons, and none of: 4 of udev's well-known keyboard keys
  (or KEY_A + KEY_SPACE + KEY_ENTER), REL_X + REL_Y, mouse / touch / pen
  buttons, the accelerometer / pointer / button-pad properties. From sysfs
  first (capabilities/{ev,key,abs,rel}, properties, name, id/*), and again
  from the opened device; without sysfs, from the device alone.
- Every gamepad is opened (up to 8) and the states are merged: buttons OR,
  triggers maximum, each stick from the device with the largest deflection.
  /dev/input is looked through once per second also while pads are open; a
  node is classified again only when its inode or change time is new (udev
  applying the ACL changes the latter). Devices are dropped one by one on
  read errors. Vibration goes to every pad that has FF_RUMBLE. `--pad
  xinput|playstation` filter per device. `hpad_kind` is that of the
  lowest-numbered pad.
- Sticks are scaled around the middle of the declared range (0..255 rests at
  128 and now gives exactly 0; before it gave 128 of 32767).
- Ten buttons numbered in sequence without BTN_SELECT / BTN_START (codes
  0x130..0x139 on the generic HID mapping) are taken as A B X Y LB RB Back
  Start LS RS. BTN_TRIGGER_HAPPY1..4 are the D-pad when there is neither a
  hat nor BTN_DPAD_*. Both are from the mapping rules, not from a device.
- Windows, the analogous case: `pad_xinput.c` used the first connected XInput
  slot; it now reads all four and merges them the same way. `pad_hid.c`
  already goes on past devices it cannot use. Unchanged: in `auto` a Sony pad
  is read directly only while XInput reports no pad at all.

### Log lines (support: ask for the `[pad]` lines of game.log)
```
[pad] evdev: /dev/input/event17 "Microsoft X-Box 360 pad" 045e:028e: used as gamepad (xbox layout, vibration)
[pad] evdev: /dev/input/event2 "keyd virtual keyboard" 0fac:0ade: skipped, has keyboard keys too (a keyboard or a virtual device)
[pad] evdev: /dev/input/event5 "Microsoft X-Box 360 pad" 045e:028e: is a gamepad but cannot be opened: permission denied for /dev/input/event5. Fix: log out and in again, or 'sudo usermod -aG input $USER' and log in again, or a udev rule with TAG+="uaccess" for this device
[pad] evdev: /dev/input/event17 "Microsoft X-Box 360 pad" 045e:028e: disconnected
[pad] gamepad: Microsoft X-Box 360 pad (evdev, 1 device, /dev/input/event17 045e:028e; kind xbox; stick dead zone 24 %)
[pad] no gamepad found (evdev: 3 input devices in /dev/input, no gamepad among them); keyboard only until one is connected
```
One `evdev:` line per node that has joystick-range buttons, when its verdict
changes. Other skip reasons: `has mouse axes too`, `has mouse or touch
buttons too`, `gamepad buttons but no stick or D-pad axes`, `joystick buttons
but no gamepad layout (no BTN_SOUTH)`, `motion sensor of a controller`,
`touchpad or pointer`, `skipped by --pad playstation (not a Sony device)`,
`skipped, 8 gamepads are in use already`; `is a gamepad but cannot be opened:
<error text>`. With several pads the `gamepad:` line names the first "and N
more". Other statuses in the `no gamepad found` line: `evdev: /dev/input does
not exist`, `evdev: N gamepads present but not accessible, see the lines
above`. `ROR_PAD_DEBUG=1` adds a line for every node, for each opened pad
`bus BBBB, keys <codes>; axes <code>[min..max] ...`, and on every change
`[pad] evdev: eventN state: buttons XXXX lt .. rt .. lx .. ly .. rx .. ry ..;
keys down: <codes>; axes: <code>=<value> ...`.

### Simulation: `recomp/tools/padsim.sh [pad_evdev.c [N]]`
Builds `pad_evdev.c` and `pad.c` unchanged with `padsim.c`; the linker's
`--wrap` redirects opendir / readdir / closedir / stat / fopen / open / close
/ read / write / ioctl for `/dev/input/event*` and `/sys/class/input/event*`
to a table of devices whose capabilities, IDs, properties and ranges are
those the drivers register. 13 scenarios, 42 checks: key-remapper keyboard
before an xpad (with the state carried through `pad.c` to the PS2 word and a
vibration request back), Xbox One ranges, Bluetooth generic layout,
sequential buttons, DualSense with its motion and touchpad nodes, a node
without access that becomes accessible, hot-plug / unplug / replug of three
pads, a silent pad plus Steam's virtual pad, the two `--pad` filters, no
sysfs and read-only open, no gamepad / no /dev/input, the debug output.
Result: all pass with the new file; with the file of the previous commit
(`git show 654409c:recomp/rt/pad_evdev.c > old.c; padsim.sh old.c`) 10 of 13
fail, among them scenario 1 with `the device in use is the Xbox pad (name
"keyd virtual keyboard")`.

### Verification of the build
Standard script to 16000 (16 PNG + WAV): identical for the previous binary,
a build of the same sources with the old pad files, and the new build, on
Linux and Windows (frame_016000 ed535164..., audio.wav 670b42cf...); the same
script given as `ROR_PAD_FAKE` (recomp/tools/padfake.py) gives the same md5s
on both; `ROR_PAD_SELFTEST=1` all ok and identical on both; Windows window
run of 300 frames in 6.07 s with the usual `no gamepad found` line.

### Not verifiable here
No controller and no /dev/input on the development machine: the new code has
run only against the simulated devices. Whether the reported failure was the
device choice, missing access rights or something else is not known until a
`[pad]` log from an affected system is seen. The simulated capability sets
come from the driver sources, not from captures of real devices; the
sequential-button and BTN_TRIGGER_HAPPY rules and the merged XInput slots
have never met hardware. Not done: reading `/dev/input/js*` (world-readable
by default) as a fallback when the event node is not accessible.
