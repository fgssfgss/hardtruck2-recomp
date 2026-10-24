# AGENTS.md — Hard Truck 2 static recompilation

Read this first. `CLAUDE.md` is a symlink to it.

## What this is

A static recompilation of **Hard Truck 2: King of the Road** (`king.exe`,
GOG build, MSVC 6 + MFC 4.2, 2002) into C with
[pcrecomp](../pcrecomp) (`lift32`, global-register model). It runs on Linux
on pcrecomp's **win32hle** runtime (an implemented Win32 on libc/POSIX/SDL2),
with no Wine and no emulator.

The end target is the same as the sibling **LongRoad2** project: ARM Linux
handhelds with **KMSDRM + OpenGL ES 3.x**, no X11 or Wayland. LongRoad2 gets
there by running the original x86 code in-process under box86. This project
gets there by recompiling: the lifted C builds natively for any 64-bit host,
including aarch64, so no box86 is needed.

## Hard rules

- **Never commit game material.** Not `king.exe`, not anything under `game/`,
  `work/` or `gen/` (the lifted C is a derivative of the binary), not the
  Ghidra project. `.gitignore` covers them. Before pushing anywhere public, run
  `python ../pcrecomp/tools/audit_repo.py .`.
- **Never edit `gen/`.** It is regenerated. Fix the lifter (a pcrecomp patch),
  the catalog (`config/seeds.txt`, `config/exclude.txt`), or replace a function
  (`config/overrides.txt` + a manual implementation).
- **One exact binary.** Every address in `config/` and every hook is tied to
  king.exe sha256 `4412a5f6…edd86` (`config/hardtruck2.toml`).
  `scripts/run_lift.py check` refuses any other build.
- **Don't touch the user's originals**: `~/Documents/longroad_ghidra.*` or
  `../LongRoad2` (copy code out of it, don't edit it there). Ghidra runs on its
  own project under `work/ghidra/`. The install at
  `~/.wine/drive_c/HardTruck2` is the game's working directory: the game
  writes its own `warn.log`, `aimsg.log` and settings there, as on Windows.
  Nothing else in it is ours to change.
- **pcrecomp changes are patches.** Change `../pcrecomp` only on a branch, and
  export each change to `patches/pcrecomp/NNNN-*.patch`
  (`git format-patch`). See `patches/pcrecomp/README.md`.
- Plain **C11** for everything hand-written. Build with **CMake + Ninja**.

## Layout

```
AGENTS.md            this file (CLAUDE.md -> AGENTS.md)
README.md            short human overview
CMakeLists.txt       host build: gen/ + win32hle + src/
config/
  hardtruck2.toml    the single source of truth: exe hash, paths, lift options
  seeds.txt          extra function entries      "0xVA  # why"
  overrides.txt      VA -> hand-written replacement (recomp_lookup_manual)
  exclude.txt        VA -> not code (lifted as an abort)
scripts/
  recon.sh           phase 0: pe_analyze, imports, RTTI, vtables, Ghidra reference
  ghidra_ref.py      Ghidra export -> work/ghidra_ref.json, ghidra_seeds.json, names.json
  run_lift.py        check | catalog | lift [--only VA,..] | all
  import_coverage.py which imports have a shim (src/shims, win32hle) and which don't
  pcrecomp_patches.sh  ../pcrecomp onto branch ht2 = base_commit + patches/pcrecomp
  gfx_diff.sh        src/gfx against ../LongRoad2 (what we changed in the renderer)
  ppm2png.py         frame dumps (LONGROAD2_FRAME_DUMP_DIR) to PNG
src/host/main.c      the executable: maps C: to the install's parent, boots, runs
src/shims/           Win32 imports win32hle does not answer (registered FIRST, so they win)
  ht2_shims.h        the registry for ours
  misc.c             small MFC-flavoured USER32/GDI32/KERNEL32/... gaps
  com_bridge.c/.h    guest-side COM: proxies, guest vtables, gfx_mem.h on win32hle's heap
  ddraw7.c           DirectDrawCreateEx + IDirectDraw7/Surface7/Palette/Clipper/GammaControl
  d3d7.c             IDirect3D7, IDirect3DDevice7, IDirect3DVertexBuffer7
  dinput.c           DirectInputCreateA: an IDirectInput7A with no devices
  dplay.c            CLSID_DirectPlay / DirectPlayLobby that find nothing (not optional!)
  quartz.c           CLSID_FilterGraph: the MP3 music player (libmpg123 -> win32hle host stream)
  gfx_host.c/.h      the SDL/GLES window, SDL input -> win32hle, HT2_INPUT scripts
src/gfx/             LongRoad2's ddraw/d3d7/render/GL-thread, vendored + ported (its README.md)
docs/
  STATUS.md          milestones, numbers, what's next
  HOST.md            the runtime design: the COM bridge, window, input
patches/pcrecomp/    our pcrecomp patches (0001-0004, see its README.md)
tests/               (empty) shim tests go here
game -> ~/.wine/drive_c/HardTruck2     symlink, gitignored
work/                recon + catalog + Ghidra project, gitignored
gen/                 lifted C, gitignored
build/               64-bit build; build32/ for -DHT2_FLAT32=ON
.venv/               python venv: capstone, pefile, unicorn
```

## Pipeline (from a clean checkout)

```sh
python3 -m venv .venv && .venv/bin/pip install capstone pefile unicorn
ln -s ~/.wine/drive_c/HardTruck2 game
scripts/pcrecomp_patches.sh               # ../pcrecomp on branch ht2 (our win32hle fixes)
scripts/recon.sh --ghidra                 # ~10 min, mostly Ghidra; without --ghidra: seconds
.venv/bin/python scripts/run_lift.py all  # catalog ~2 min, lift ~35 s
cmake -B build -G Ninja && cmake --build build   # ~3 min on 16 threads
SDL_VIDEODRIVER=wayland ./build/ht2       # --game DIR or $HT2_GAME, default ./game
```

Needs SDL2, SDL2_ttf, GLESv2 and libmpg123 (64-bit). On this machine SDL's X11 backend
hangs in `SDL_CreateWindow` (XWayland never maps the window), so use
`SDL_VIDEODRIVER=wayland`; on the handheld it is `kmsdrm`.

## Running and watching it

| env | what |
|---|---|
| `HT2_INPUT="12000:click:660,145;16000:click:400,143"` | scripted input, ms after the window opens, display-mode pixels: `click`, `move`, `key:VK`. Those two clicks are Single Player, then New Game: into a race in ~20 s |
| `LONGROAD2_FRAME_DUMP_DIR=dir LONGROAD2_FRAME_DUMP_EVERY=50` | every Nth presented frame as `gl_frame_N.ppm` (`scripts/ppm2png.py`); this is how to see the screen unattended |
| `HT2_GLES=2` | a GLES 2.0 context, treated as a bare 2.0 driver (no extensions): exercises every fallback |
| `HT2_TRACE=1` | one line per import and COM method (`[hle] IDirectDrawSurface7::Blt`): slow, and huge |
| `HLE_FILETRACE=1`, `HLE_MSGTRACE=1`, `HLE_REGTRACE=1` | win32hle: file opens and lookups, messages, registry |
| `LONGROAD2_DDRAW_TRACE=1`, `LONGROAD2_PERF_TRACE=1`, `LONGROAD2_NO_VSYNC`, `LONGROAD2_FPS_LIMIT` | LongRoad2's own switches, still in src/gfx |
| `SDL_AUDIODRIVER=dummy` / `disk` (`SDL_DISKAUDIOFILE`) | no sound card / the mix to a file. **Silent on the speakers**: say so before using it on the user's desktop |
| `SDL_VIDEODRIVER=offscreen` | a real GLES context with **no window**: renders, frame dumps and traces work, nothing appears on the user's desktop. Use it for rendering investigations (with `SDL_AUDIODRIVER=dummy` it is silent too) |
| `SDL_VIDEODRIVER=dummy` | no window and no GL (the renderer fails to open; the game never reaches its menu) |
| `HT2_PRESENT_TRACE=1` | per present: path, brightness, bound texture and its storage; `[gl]` texture/FBO lifecycle |
| `HT2_PRESENT_RESET=1` | forget the GL state cache before each present (diagnostic) |

| `HT2_NO_MUSIC=1` | quartz loads nothing |


The game's own diagnostics are the best first look: `game/warn.log` (its
warnings and FATAL errors, with source file and line) and `game/aimsg.log`.

A hang with no output: this machine forbids ptrace attach, so start the game
under gdb and interrupt it from a Python timer:

```python
# g.py: gdb -q -batch -x g.py --args ./build/ht2
import gdb, threading
threading.Timer(30.0, lambda: gdb.post_event(lambda: gdb.execute("interrupt"))).start()
gdb.execute("run"); gdb.execute("thread apply 1 bt 30")
# guest memory (64-bit build): *(unsigned*)(0x100000000000 + va); g_eax etc. are globals
```

The lifted frames are `sub_<VA>` and match `~/king.exe.c`'s `FUN_<va>`.

Useful while iterating:

- `run_lift.py lift --only 0x0062FA96` lifts one function to `gen/recomp_only.c`
  so you can read what the lifter made of it. Then run a full `lift` again before
  building, because the build globs `gen/recomp_[0-9]*.c`.
- `scripts/import_coverage.py --missing` shows the imports with no shim.
- Ghidra pseudo-C for the whole exe: `~/king.exe.c` (11.8 MB, made by the user,
  same build). Ghidra's FID names (MFC/CRT) are in `work/names.json` and appear
  as comments above lifted functions.
- win32hle env switches: `HLE_FILETRACE=1` (file opens), and the
  `win32hle_trace` global (every import, by name).
- Headless runs: `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`.

## How the pieces fit

- **Lifted code** (`gen/`): every function is `void sub_XXXXXXXX(void)` working
  on global registers (`g_eax`, `g_esp`, …) and guest memory (`MEM32(va)` etc.,
  `runtime/recomp32/recomp_types.h`). Indirect calls go through
  `RECOMP_ICALL(va)`: first manual overrides, then the dispatch table
  (`gen/recomp_dispatch.c`), then imports.
- **Memory model.** By default this is 64-bit **based**: the guest's 4 GB is
  one mapping at `HT2_MEM_BASE` (`0x100000000000`), and `ADDR(va)` adds that
  constant. `-DHT2_FLAT32=ON` builds `gcc -m32` **flat** instead, where guest
  VA == host VA. Shims must work in both: use
  `hle_ptr(va)`/`hle_va(p)`/`APTR`/`RETP`, never cast a guest dword to a
  pointer, and allocate anything the guest gets an address of in guest memory
  (`hle_alloc`, `hle_scratch`).
- **Imports.** win32hle's PE loader binds each IAT slot by name (ordinals as
  `"dll#N"`, lower-case dll) to a synthetic VA in `0x7F000000+`. A shim is
  `void fn(void)`. On entry `g_esp` points at `[ret][arg0]…`, and it returns
  with `RET(eax, argc)`, which pops for stdcall. A wrong argc corrupts the
  *caller's* stack and crashes somewhere else entirely, so take argc from the
  SDK prototype. The boot is **permissive**: an import with no shim stops the
  run, naming itself and its caller. That is the bring-up loop.
- **COM** (DirectDraw/D3D/DSound): the guest calls `call [ecx+N]`, so a COM
  object's vtable must be **in guest memory** and hold shim VAs. Ours go
  through `src/shims/com_bridge.h`: one proxy per (host object, interface), a
  registered shim per slot (`com_define`). `docs/HOST.md` explains it, and
  `ddraw7.c` / `d3d7.c` / `dplay.c` are the examples to copy.


## The bring-up loop

1. Run. It stops at an unimplemented import, a `RECOMP_UNIMPL`, an unresolved
   `ICALL`, or a fault (with a crash report).
2. Unimplemented import: implement it in `src/shims/` (or upstream it into
   win32hle as a pcrecomp patch if it's generic Win32). Check
   `../LongRoad2/docs/api-surface/` for how the game really uses it. Those are
   traces of this same exe under Wine and LongRoad2, and the call counts tell
   you whether a stub is enough.
3. `ICALL: unresolved VA`: the catalog missed a function (often reached only
   through a pointer in data). Add it to `config/seeds.txt`, then
   `run_lift.py all`.
4. Wrong behaviour with no crash: suspect the lifter (flags, x87). Use pcrecomp
   `tools/lift/difftest.py` on the instruction pattern, fix it in pcrecomp, and
   export the patch.
5. Update `docs/STATUS.md` when a milestone moves.

## LongRoad2: what to reuse, what not

`../LongRoad2` runs this same king.exe in-process (its own PE loader + shims,
32-bit, box86 on ARM). Its renderer is **vendored in `src/gfx/`** and reached
through the COM bridge (`docs/HOST.md`). `src/gfx/README.md` lists every change
made to it; `scripts/gfx_diff.sh` shows them. When LongRoad2 improves the
renderer, merge from there and keep those changes.

It is also the source of truth for **behaviour** elsewhere: `docs/api-surface/*.md`
(the trace-derived API surface of king.exe), its winmm/dsound, msvfw32 and
quartz work, and its notes on what king.exe needs. A LongRoad2 comment saying a
stub exists to dodge some FUN_xxxx path is worth believing (see `dplay.c`).

Its **ABI is not reusable as is**: those are native stdcall functions in native
vtables, taking host pointers. Port the logic and re-wrap it in the shim ABI.
Constraints carried over: **SDL2 + GLES (3.x preferred, 2.0 supported), no
desktop GL, nothing that needs X11/Wayland**, and shaders stay `#version 100`.
Anything new in src/gfx must compile against `<GLES2/gl2.h>` alone.


## Conventions

- Match the surrounding code: pcrecomp's comment style explains *why*, in
  full sentences, and keeps file headers that say what the file is for.
- One shim module per Windows DLL (`ddraw7.c`, `d3d7.c`, `dinput.c`,
  `msvfw32.c`, …), each exporting a `win32hle_shim` table listed in
  `ht2_shims.h`.
- Only implement what the game is shown to call (a run stopping on it, or
  LongRoad2's traces). Don't build general Windows compatibility.
- Generated or derived numbers go in `docs/STATUS.md` with the date and the
  pcrecomp commit they came from.
