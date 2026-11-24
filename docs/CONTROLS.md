# Controls

## Keyboard

| Key | Pad | Key | Pad |
|---|---|---|---|
| Arrow keys | D-pad | Enter | Start |
| Z | Cross | Backspace | Select |
| X | Circle | Q / W | L1 / R1 |
| A | Square | E / R | L2 / R2 |
| S | Triangle | Esc | Quit |

## Gamepad

A gamepad is used automatically when one is connected (also when plugged in
while the game runs); the keyboard keeps working alongside it. Windows:
anything XInput sees (Xbox pads, and pads presented as one by Steam Input or
DS4Windows), and PlayStation pads directly: DualShock 4, DualSense and
DualSense Edge, by USB or Bluetooth (a pad that XInput reports is used first;
a PlayStation pad is read directly only while XInput has none). Linux: every
device in `/dev/input/event*` that is a gamepad (Xbox, DualShock 3/4,
DualSense, Switch Pro and compatible pads with a kernel driver); your user
needs read access to the device. If several gamepads are connected they all
control the game.

| Gamepad (Xbox names) | PS2 pad | Gamepad | PS2 pad |
|---|---|---|---|
| A | Cross | LB / RB | L1 / R1 |
| B | Circle | LT / RT (pressed a quarter of the way) | L2 / R2 |
| X | Square | Back / View | Select |
| Y | Triangle | Start / Menu | Start |
| D-pad | D-pad | Left / right stick click | L3 / R3 |
| Left stick | Left analog stick (movement) | Right stick | Right analog stick |

On a PlayStation pad the buttons are the PS2 ones (Cross, Circle, Square,
Triangle, L1/R1, L2/R2, L3/R3); Share/Create is Select and Options is Start.
The PS button and the touchpad are not used.

The game itself moves the character with the left stick (analog) or the
D-pad. Vibration requests of the game are passed to the gamepad.

### If a gamepad does nothing on Linux

The game writes what it found to its log (`game.log` beside the launcher, or
the terminal when the game is started by hand). Look for the lines that start
with `[pad]`:

- `[pad] evdev: /dev/input/eventN "NAME" vvvv:pppp: used as gamepad (...)` —
  this device is read.
- `... is a gamepad but cannot be opened: permission denied for
  /dev/input/eventN` — your user may not read the device. Desktop systems
  grant this to the logged-in user automatically; where they do not, add
  yourself to the `input` group (`sudo usermod -aG input $USER`, then log out
  and in again) or install a udev rule for the device.
- `... skipped, REASON` — the device was looked at and is not treated as a
  gamepad, for the reason given.
- `[pad] no gamepad found (...)` and no `evdev:` line for your controller —
  the kernel has not made an input device for it (check `ls /dev/input`, and
  whether the controller needs a driver such as `xone` or `xpadneo`).

Starting the game with the environment variable `ROR_PAD_DEBUG=1` adds every
input device, what each gamepad can do and every change of its buttons and
axes to the log. Please include those lines when reporting a problem.
