# Hard Truck 2 — Recompiled

A static recompilation of *Hard Truck 2: King of the Road* (`king.exe`, 2002)
into C with [pcrecomp](https://github.com/sp00nznet/pcrecomp). It runs
natively on Linux, as a 64-bit program, on pcrecomp's win32hle runtime, with
no Wine. DirectDraw 7 and Direct3D 7 are drawn through OpenGL ES (3.x or 2.0),
using the renderer from the sibling LongRoad2 project. The menus work, and a
new game loads into the truck cabin in 3D. The goal is KMSDRM handhelds.

This repository contains **no game code or assets**. You need your own copy of
the game (GOG build). Everything generated from it stays local.

* How to build and work on it: [AGENTS.md](AGENTS.md)
* Where it is: [docs/STATUS.md](docs/STATUS.md)
* How the runtime and the DirectX bridge work: [docs/HOST.md](docs/HOST.md)
* The renderer, and what changed from LongRoad2: [src/gfx/README.md](src/gfx/README.md)

