# Status

As of 2026-10-24: pcrecomp `944e296` with `patches/pcrecomp/0001-0005`, and
LongRoad2's renderer at `bc5fd1d`.

## Milestones

| | Milestone | State |
|---|---|---|
| M0 | Recon: PE, imports, RTTI, vtables, Ghidra reference | **done** |
| M1 | Catalog: disasm32 scored against Ghidra | **done** |
| M2 | Lift: whole exe to C | **done**: 0 failures |
| M3 | Compile + link (64-bit based, win32hle) | **done**: ~3 min, 16 threads |
| M4 | CRT startup → MFC → WinMain | **done** |
| M5 | DirectDraw 7 → menus | **done**: main menu, Single Player screen, mouse input |
| M6 | Direct3D 7 → in-game 3D | **done**: a new game loads and renders the cabin view at 1024x768 |
| M6b | GLES 2.0 | **done**: menus and the race render the same with `HT2_GLES=2` (bare-2.0 fallbacks) |
| M7a | Keyboard in the race | **done**: window activation (patch 0005) |
| M7b | Sound effects (DirectSound) | **done**: win32hle's mixer, unchanged |
| M7c | Music (DirectShow `CLSID_FilterGraph`) | **done**: `src/shims/quartz.c`, MP3 through libmpg123 |
| M7d | AVI intros, gamepad | next |
| M8 | KMSDRM on an ARM handheld, aarch64 native build | |

## What it took to get from WinMain to the race

Each item below stopped the run:

1. `DirectDrawCreateEx` and Direct3D 7: LongRoad2's renderer behind the COM
   bridge (`docs/HOST.md`).
2. `read_defFile_kmjIn(): #1`: win32hle's .ini reader did not know
   `key = value` with spaces, or the NULL section/key lists (patch 0002).
3. `DirectInputCreateA`: an IDirectInput7A with no devices.
4. A silent hang in InitInstance. A failed `CoCreateInstance(CLSID_DirectPlay)`
   logs a warning, and the game's warning routine parks any thread it does not
   recognise, the main thread included, in a `jmp $` (FUN_005dcb90). Fixed by a
   DirectPlay that finds nothing (`dplay.c`).
5. `Enum Elements failed`: `IStorage::EnumElements` (patch 0003).
6. `No nodes were defined` (the AI road graph): `GetFullPathNameA` handed the
   guest a host path, so `ENV\*.way` was never found (patch 0004).
7. `ITAIL: unresolved VA 0x0061CC44`: the CRT memmove's backward jump table
   is indexed negatively. Its targets are in `config/seeds.txt`.

And after it:

8. No keyboard in the race. The game destroys its menu frame window and plays
   in a second one it only `SetWindowPos`es. win32hle left no active window,
   so key messages were dropped (patch 0005).
9. No music: `CLSID_FilterGraph`. `quartz.c` gives the game an IGraphBuilder,
   IMediaControl, IMediaEventEx and IBasicAudio. RenderFile decodes
   `MUSIC\trackNNl.mp3` with mpg123 into win32hle's host audio stream, and
   EC_COMPLETE comes once the track has played out, so the playlist moves on.

## Numbers

**king.exe**: 2,908,160 bytes, sha256 `4412a5f6…edd86`, MSVC 6.0, image base
0x400000, entry 0x61F57A, `.text` 2.4 MB. No packer, no DRM. 313 imports
from 15 DLLs.

**Catalog** (`run_lift.py catalog`, ~2 min): 16,830 dispatchable entries,
seeded with the entry point, 190 RTTI methods (37 classes), 1,611 vtable
methods, Ghidra's 9,899 starts and the 7 entries in `config/seeds.txt`.

Scored against Ghidra 12.1.2 (a second opinion, not ground truth: no symbols
shipped): recall 100% (by construction), precision 85.19% (144 split, 1,577
outside every Ghidra function), ends 96.38% exact.

**Lift** (`run_lift.py lift`, ~35 s): 16,830 functions, 43 files, ~3.77M lines,
0 lift failures, 0 `RECOMP_UNIMPL`.

**Imports**: 313. 260 are answered by win32hle and the rest by `src/shims/`,
except the 4 MSVFW32 `IC*` calls (`scripts/import_coverage.py --missing`).

## Known gaps

- No videos: the intro/demo AVIs ("avi subsystem error" in warn.log), MSVFW32 `IC*` unimplemented.
- The game's music volume (IBasicAudio, hundredths of a dB) is answered but
  not applied: at `midivol=9` it asks for -38 dB, which silenced the music.
  Music plays at full gain.


- No gamepad (DirectInput enumerates nothing). Keyboard and mouse go through messages.
- The game writes `warn.log` / `aimsg.log` into the install, as on Windows.
- Here, SDL's X11 backend hangs creating a window (an XWayland issue, outside
  this code); run with `SDL_VIDEODRIVER=wayland` on a Wayland desktop.
- OPEN: black every other frame in the race. With HT2_PRESENT_RESET=1 (GL state cache reset per present) one run showed 9/2016 black frames; making that the default showed 276/637 black and the run aborted (work/blink5.log). Not understood yet. HT2_PRESENT_TRACE=1 logs each present's brightness; HT2_CMDBUF_SYNC=1 (synchronous D3D7 replay) did not help.
