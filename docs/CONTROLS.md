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
a PlayStation pad is read directly only while XInput has none). Linux: any
device in `/dev/input/event*` that reports gamepad buttons (Xbox, DualShock
3/4, DualSense, Switch Pro and compatible pads with a kernel driver); your
user needs read access to the device.

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
