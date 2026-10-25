## Notes

Hard Truck 2: King of the Road (2002), recompiled from the PC executable
(`king.exe`) into native aarch64 code with
[pcrecomp](https://github.com/fgssfgss/pcrecomp) ([hardtruck2-recomp](https://github.com/fgssfgss/hardtruck2-recomp)), running on its Win32 runtime (win32hle) with
the game's DirectDraw 7 / Direct3D 7 drawn through OpenGL ES 3 or 2. No Wine,
no box64.

The game itself is not included. Copy your own install, the GOG release, into
`ports/hardtruck2/gamedata/` so that `king.exe` is in that folder.

Not there yet: the intro videos, and native gamepad support (the pad is
mapped to the keyboard and mouse below).

## Controls

| Button | Action |
|--|--|
| Left stick up / down | Accelerate / brake |
| Left stick left / right | Steer |
| Right stick | Mouse (menus) |
| A | Left click |
| B | Parking brake |
| X | Start engine |
| Y | Inside / outside view |
| L1 / R1 | Gear down / up |
| L2 | Map |
| R2 | Horn |
| L3 / R3 | Headlights / wipers |
| D-pad left / right | Look left / right |
| D-pad up / down | Zoom in / out |
| Start | Enter |
| Select | Esc (menu) |

The other keyboard keys (from the game's help): S first gear, X reverse, P pager,
T hook / drop trailer, Num + / Num - load / unload cargo, F1 key mapping,
F2 rating, F3 player info, F4 company. Edit `hardtruck2.gptk` to put them on
buttons.
