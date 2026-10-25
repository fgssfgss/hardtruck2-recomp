# The host: win32hle + DirectDraw 7 / Direct3D 7 on GLES

```
 king.exe, lifted (gen/)                       src/host/main.c
     │  call [IAT]  /  call [ecx+N]
     ▼
 win32hle shims (pcrecomp)  ◄── name lookup, first match ──  src/shims/ (registered first)
   kernel32 user32 gdi32 winmm dsound ole32 ...                misc.c        small Win32 gaps
                                                               ddraw7.c      DirectDrawCreateEx + DDraw 7 COM
                                                               d3d7.c        Direct3D 7 COM
                                                               dinput.c      IDirectInput7A, no devices
                                                               dplay.c       DirectPlay that finds nothing
                                                               quartz.c      FilterGraph: MP3 music (mpg123)
                                                               com_bridge.c  proxies, guest vtables, gfx_mem.h
                                                               gfx_host.c    SDL window + input -> win32hle
                                                                    │
                                                                    ▼
                                                    src/gfx/ (LongRoad2 @ bc5fd1d, src/gfx/README.md)
                                                      ddraw.c  d3d7*.c  render.c  gl_thread.c
                                                                    │
                                                              SDL2 + GLES 3.x / 2.0
```

## Memory modes

| | 64-bit based (default) | 32-bit flat (`-DHT2_FLAT32=ON`) |
|---|---|---|
| guest VA -> host | `0x100000000000 + va` (aarch64: `0x4000000000 + va`, for 39-bit kernels) | `va` |
| runs on | x86-64, aarch64 natively | x86 (box86 on ARM) |
| shim pointer handling | translated (`hle_ptr`/`hle_va`) | the translation is a no-op |

All shim code is written for both. The 32-bit build also needs 32-bit SDL2_ttf.

## The COM bridge (src/shims/com_bridge.h)

The game holds DirectX objects as COM pointers and calls them through
`call [ecx+N]`. LongRoad2's objects are host structs with host vtables. So
for each (object, interface) the guest gets a **proxy**: 16 bytes of guest
memory, `{ vtbl, 'HT2C', kind }`. Its vtable is an array of shim VAs, one
registered shim per slot (`"IDirectDrawSurface7::Blt"`, so every log names
the method). A slot shim:

1. unwraps `this` (`COM_SELF`): proxy VA → host object, or `E_POINTER`;
2. turns each argument into what LongRoad2's method takes: a plain pointer
   becomes a host pointer (`COM_P`), an interface becomes its host object
   (`COM_IN`), a DWORD passes as-is;
3. calls the LongRoad2 method by name, with its real prototype. The bridge
   files `#include` ddraw.c / d3d7.c to reach the static methods, and never
   call through LongRoad2's vtables, whose `uint32_t`-only function types
   would truncate pointers on 64-bit;
4. wraps any interface it hands back (`com_out`), and returns with
   `RET(hr, argc + 1)`. The argument counts are LongRoad2's, which this game
   already validated. `_Static_assert`s keep the slot count equal to
   LongRoad2's vtable structs.

Two hash maps (proxy VA → object, (object, kind) → proxy) give COM's stable
identity. A Release that frees an object calls `gfx_native_freed()`, which
drops its proxies.

Things that cross the boundary by value:

- **Callbacks** (`EnumDisplayModes`, `EnumAttachedSurfaces`, `EnumSurfaces`,
  `EnumDevices`, `EnumZBufferFormats`, `EnumTextureFormats`): the LongRoad2
  method gets a host trampoline. The trampoline copies the arguments into guest
  scratch memory and `hle_call_guest`s the game's callback.
- **Pointers inside structs**: `DDSURFACEDESC2.lpSurface` is a guest VA, and
  surface buffers live in guest memory. `D3DDRAWPRIMITIVESTRIDEDDATA` (king's
  main 3D path, ~330k draws a session) is rebuilt per call into LongRoad2's
  host-pointer struct (`strided_from_guest`).
- **GetDC** hands back the surface's own proxy as the HDC, as LongRoad2 did.

## Window and input (src/shims/gfx_host.c)

win32hle owns the Win32 windows, which have no display of their own. The
real SDL window is LongRoad2's `render_init`, opened on the first
`SetDisplayMode` or primary surface. win32hle's `screen.c` (its DirectDraw
1–3 display) is never opened. SDL events reach the game through win32hle's
pump hook: keys go to `hle_input_key`, using win32hle's own scancode table
(patch 0001), and the mouse goes to `hle_input_mouse` in display-mode pixels.

## Music (src/shims/quartz.c)

`CLSID_FilterGraph` is one host object behind four proxies (IGraphBuilder,
IMediaControl, IMediaEventEx, IBasicAudio) sharing one refcount. Only the
calls the game makes do anything. `RenderFile` opens the MP3 with libmpg123,
at the file's own rate. A decoder thread keeps about half a second queued in a
win32hle host stream (`hle_audio_stream_*`, the API Bink uses), so the
DirectSound mixer is not touched. `Run`/`Stop` set the stream's gain to the
full gain or to 0 (the IBasicAudio volume is kept, not applied). `GetEvent` returns EC_COMPLETE once, when the
track is decoded and fully played. `HT2_NO_MUSIC=1` turns it off.

## What is not there yet


| | |
|---|---|
| AVI movies | the intro/demo videos: "avi subsystem error" in warn.log, harmless. MSVFW32 `IC*` are still unimplemented, and a run stops if the game calls them |

| `{568DF44F-DEAC-11D3-BBEE-0000B4908D28}` | an unknown class asked for on the way into the menu, harmless so far |
| DirectInput devices | a gamepad would be one, fed from SDL |
| `GetDC` on a surface + GDI | text drawn by GDI onto a surface goes nowhere |
