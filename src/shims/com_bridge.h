/*
 * com_bridge.h - the guest side of the DirectDraw 7 / Direct3D 7 COM objects.
 *
 * The objects themselves are LongRoad2's (src/gfx/: ddraw.c, d3d7.c), plain C
 * structs in host memory. The guest never sees them. It holds a PROXY: a few
 * dwords in guest memory whose first dword is a guest vtable, and every slot
 * of that vtable is a registered win32hle shim ("IDirectDrawSurface7::Blt").
 * So the game's `call [ecx+N]` lands in a shim (ddraw7.c, d3d7.c), which
 * unwraps `this` and every interface argument to its host object, turns
 * every other pointer argument into a host pointer, calls the LongRoad2
 * method with its real prototype, and wraps any interface it hands back.
 *
 * One proxy per (host object, interface kind), so the guest sees a stable
 * pointer for an object, as COM promises. The native Release that frees an
 * object calls gfx_native_freed() (gfx_mem.h), which drops its proxies.
 */
#ifndef HT2_COM_BRIDGE_H
#define HT2_COM_BRIDGE_H
#include <stdint.h>
#include "win32hle.h"

typedef enum {
    CK_NONE = 0,
    CK_DDRAW7,      /* IDirectDraw7          -> ddraw7_t (a tear-off on a ddraw_core_t) */
    CK_SURFACE7,    /* IDirectDrawSurface7   -> surface7_t */
    CK_PALETTE,     /* IDirectDrawPalette    -> palette7_t */
    CK_CLIPPER,     /* IDirectDrawClipper    -> clipper7_t */
    CK_GAMMA,       /* IDirectDrawGammaControl -> gamma_control_t */
    CK_D3D7,        /* IDirect3D7            -> d3d7_t */
    CK_DEVICE7,     /* IDirect3DDevice7      -> d3d_device7_t */
    CK_VB7,         /* IDirect3DVertexBuffer7 -> vb7_t */
    CK_DINPUT,      /* IDirectInput7A        -> dinput_t (dinput.c) */
    CK_DPLAY4,      /* IDirectPlay4A         -> dp_obj_t (dplay.c) */
    CK_DPLOBBY3,    /* IDirectPlayLobby3A    -> dp_obj_t (dplay.c) */
    CK_QGRAPH,      /* IGraphBuilder         -> graph_t (quartz.c) */
    CK_QCONTROL,    /* IMediaControl         -> graph_t, the same object */
    CK_QEVENT,      /* IMediaEventEx         -> graph_t, the same object */
    CK_QAUDIO,      /* IBasicAudio           -> graph_t, the same object */

    CK_COUNT
} com_kind_t;

/* Define an interface: register one shim per slot, named "iface::slot", in
 * vtable order. The guest vtable itself is built on first use (guest memory
 * is not up yet when shims are registered). */
void com_define(com_kind_t kind, const char *iface, const win32hle_shim *slots, int nslots);

uint32_t com_wrap(const void *native, com_kind_t kind);    /* the proxy VA, made on first use; NULL -> 0 */
void    *com_unwrap(uint32_t proxy, com_kind_t kind);      /* the host object, or NULL (logged) */
void     com_out(uint32_t out_va, const void *native, com_kind_t kind);   /* *out = proxy (or 0) */
int      com_guid_is(uint32_t riid_va, const void *guid16);

/* Slot shims that need no host object: the "unimplemented" (DDERR_GENERIC,
 * logged with the slot's name) and "harmless" (DD_OK) answers, one per
 * argument count since a stdcall shim must pop exactly what was pushed. */
void com_fail0(void); void com_fail1(void); void com_fail2(void); void com_fail3(void);
void com_fail4(void); void com_fail5(void); void com_fail6(void); void com_fail7(void);
void com_ok0(void);   void com_ok1(void);   void com_ok2(void);   void com_ok3(void);
void com_ok4(void);   void com_ok5(void);   void com_ok6(void);   void com_ok7(void);


/* HRESULTs the bridge itself answers with. */
#define COM_E_NOINTERFACE 0x80004002u
#define COM_E_POINTER     0x80004003u
#define COM_DDERR_GENERIC 0x80004005u

/* `this` of a slot shim with `argc` arguments, as host type T; returns
 * E_POINTER from the shim if the guest passed something that is not ours. */
#define COM_SELF(T, kind, argc) \
    T *self = (T *)com_unwrap(A32(0), (kind)); \
    if (!self) RET(COM_E_POINTER, (argc) + 1)
#define COM_P(i)        hle_ptr(A32(i))                   /* a plain pointer argument */
#define COM_IN(i, kind) com_unwrap(A32(i), (kind))        /* an interface argument (0 -> NULL) */

void ht2_ddraw7_register(void);   /* ddraw7.c */
void ht2_d3d7_register(void);     /* d3d7.c */
void ht2_dinput_register(void);   /* dinput.c */
void ht2_dplay_register(void);    /* dplay.c */
void ht2_quartz_register(void);   /* quartz.c */




#endif
