# What's new

## 0.1.5

- The North American disc (SLUS-21448) now works too, next to the European
  one. Choose either disc image in the launcher; it shows which one it is and
  prepares the right game. Both can stay prepared at the same time.
- Each disc has its own memory card in the `saves` folder, so saves from the
  two versions never mix. European saves stay where they were.
- After this update the game has to be prepared once more.

## 0.1.4

- Laptops with integrated graphics: the launcher now picks the light
  upscaler at large screen sizes, where FSR 1.0 made the game run slowly
  with broken sound, and warns when a slow combination is chosen.
- With high dynamic range (HDR) switched on in Windows, the game now asks
  for a standard picture format first.
- Better problem reports: `game.log` (next to the launcher) now starts with
  the version, your system, graphics card and settings, and says whether
  pictures are reaching the window. The log of the run before is kept as
  `game-previous.log`. If something goes wrong, send those two files.

- Windows: the launcher and the game builder now carry proper version and
  publisher information, which some antivirus programs expect to see.

## 0.1.3

**The picture**

- Stray shapes and coloured bands that flashed across the screen are gone.
- Smooth motion (the Frame rate option) no longer throws polygons around when
  you run or turn the camera, and it stutters far less. It now follows how the
  game itself moves the camera and the characters instead of guessing from the
  finished picture.
- NVIDIA DLSS is supported as an upscaler on Windows with a GeForce RTX card.
  NVIDIA does not allow its files to be included here, so you add them
  yourself; docs/OPTIONS.md says which four files and where they go. Once
  they are in the launcher's folder, DLSS appears under Upscaling.
- Distant ground and walls shimmer less at high resolution.
- The film grain is calmer by default at high resolution, and you can turn it
  down or off under Image.

**Speed**

- The game is much faster on laptops and older PCs, roughly twice as fast
  without a graphics card.
- If your PC still cannot keep up, the game now skips a few pictures instead
  of running in slow motion with broken sound.
- Smoother play on fast PCs too.

**Controllers**

- Fixed controllers not being found on Linux.

**The launcher**

- It now looks at your graphics hardware and picks sensible settings the
  first time you start it.
- Frame rate can be chosen straight from the Display tab.
- The Display tab shows which renderer is in use.
- New "Smooth distant textures" switch.

You will be asked to prepare the game once more after updating. It takes
about a minute.

## 0.1.2

- First public release.
