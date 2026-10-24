# src/gfx — LongRoad2's DirectDraw 7 / Direct3D 7 over GLES

Vendored from `../LongRoad2` at commit **bc5fd1d** (2026-08-20). LongRoad2 runs
this same king.exe in-process; its renderer is what this project draws with.

| here | LongRoad2 |
|---|---|
| `ddraw.c` `ddraw.h` `ddraw_types.h` | `src/shim/ddraw/` |
| `d3d7*.c` `d3d7*.h` | `src/shim/d3d7/` |
| `render.c` `render.h` `gl_thread.c` `gl_thread.h` | `src/render/` |
| `gl_state_cache.c` `gl_state_cache.h` | `src/glstate/` |
| `activity_sampler.c` `activity_sampler.h` | `src/platform/` |

New here: `shim_common.h`, `platform.h`, `kernel32.h` stand in for LongRoad2's
loader/platform/kernel32 headers (implemented in `src/shims/gfx_host.c`), and
`gfx_mem.h` (guest memory, implemented in `src/shims/com_bridge.c`).

`scripts/gfx_diff.sh` prints the diff against a LongRoad2 checkout. When
LongRoad2 moves, merge from there and keep the changes below.

## What changed, and why

LongRoad2's functions are entered straight from 32-bit game code, with guest
addresses equal to host addresses. Here the guest calls the COM bridge in
`src/shims/` (`ddraw7.c`, `d3d7.c`), which calls these as plain C on a 64-bit
host. Its guest memory is one 4 GB block, so a guest address is not a host
pointer and a host pointer does not fit in a DWORD.

**64-bit, guest memory** (`ddraw.c`, `ddraw_types.h`, `d3d7.c`, `d3d7_internal.h`, `d3d7_d3d3_bridge.c`)

- `WINAPI` is empty (`shim_common.h`). Nothing here is entered from guest code.
- `DDSURFACEDESC2.lpSurface` is a `uint32_t` guest VA. The struct is read and
  written in place in guest memory, at its real 32-bit layout.
- Surface pixel buffers and vertex buffer storage come from `gfx_guest_alloc`
  (win32hle's heap), because their addresses go to the guest through `Lock`.
- Draw methods take their pointer arguments as `uintptr_t`, not `uint32_t`
  (`DrawPrimitive*`, `*VB`, `Clear` rects, `ComputeSphereVisibility`). The bridge
  passes host pointers.
- State-block handles are slots in a table, not heap pointers cast to a DWORD.
- Every Release that frees an object calls `gfx_native_freed()`, so the
  bridge drops the guest proxies of that object.

**Fixes found on the way** (`ddraw.c`)

- `Lock` with a rect returns a pointer to that rect's first pixel. LongRoad2
  always returned the surface base.

**GLES 2.0** (`render.c`, `gl_state_cache.c/.h`, all `#include <GLES3/gl3.h>` → `GLES2/gl2.h`)

The whole module compiles against GLES 2 headers alone. Every shader was
already GLSL ES 1.00. The context ladder is 3.2 → 3.1 → 3.0 → 2.0, and
`gles_detect_caps()` picks a path for each GLES3-only feature:

| GLES 3 | GLES 2 path |
|---|---|
| `GL_UNPACK_ROW_LENGTH` | `EXT_unpack_subimage`, or rows repacked (`upload_strided`) |
| `GL_R8`/`GL_RED` palette indices | `GL_LUMINANCE` everywhere (`.r` reads the same byte) |
| `GL_RGBA8`, `GL_RGB565` storage | unsized `GL_RGBA`, `GL_RGB` everywhere |
| `GL_DEPTH24_STENCIL8` | `OES_packed_depth_stencil`, or a 16-bit depth-only renderbuffer |
| `GL_REPEAT` on NPOT textures | `OES_texture_npot`, or clamped (`gl_state_note_tex_size`) |

`HT2_GLES=2` asks for 2.0 and behaves as a bare 2.0 driver (no extensions)
even when the driver gives back 3.x, so every fallback above is exercised on
a desktop.

**Include paths**: `../ddraw/ddraw_types.h` → `ddraw_types.h`.

## Not used by king.exe

The DX1–6 / Direct3D 3 paths (`d3d7_d3d3_bridge.c`, the legacy vtables in
`ddraw.c`, for SWE1R and NFS5 in LongRoad2) still compile, but the bridge only
exposes the DX7 interfaces. A legacy interface the game asks for is logged by
QueryInterface and refused.
