/*
 * ddraw7.c - DirectDraw 7 for king.exe: LongRoad2's implementation
 * (src/gfx/ddraw.c, included below so its static methods are callable with
 * their real prototypes), reached from the guest through com_bridge.h.
 *
 * Imports:   DirectDrawCreateEx (DirectDrawEnumerateA stays win32hle's)
 * Objects:   IDirectDraw7, IDirectDrawSurface7, IDirectDrawPalette,
 *            IDirectDrawClipper, IDirectDrawGammaControl
 *
 * Each slot below is in the interface's real vtable order (the same order
 * as LongRoad2's *_vtbl_t structs, whose stdcall argument counts this game
 * already proved). Slots LongRoad2 answers with its generic failure stay
 * failures here (com_fail[n], logged by name); a method the game turns out
 * to need gets a real body in src/gfx/ddraw.c and a slot shim here.
 */
#include "../gfx/ddraw.c"

#include "com_bridge.h"
#include "gfx_host.h"

/* ---------------------------------------------------------------- helpers */
static const guid_t IID_IUnknown_ =
    { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

static void log_iid(const char *who, uint32_t riid) {
    const uint8_t *g = hle_ptr(riid);
    if (!g) { fprintf(stderr, "[ddraw7] %s: QueryInterface(NULL)\n", who); return; }
    fprintf(stderr, "[ddraw7] %s: QueryInterface for an interface we do not offer: "
                    "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}\n", who,
            *(const uint32_t *)g, *(const uint16_t *)(g + 4), *(const uint16_t *)(g + 6),
            g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}
#define QI_FAIL(who) do { log_iid(who, A32(1)); com_out(A32(2), NULL, CK_NONE); RET(COM_E_NOINTERFACE, 3); } while (0)

/* The same object again, one more reference: QueryInterface for the
 * interface a proxy already is (or IUnknown). */
#define QI_SELF(addref, kind) do { (addref)(self); com_out(A32(2), self, (kind)); RET(DD_OK, 3); } while (0)

/* Native -> guest callbacks: the native method is handed a host trampoline
 * and a host context naming the guest's callback; the trampoline copies its
 * arguments into guest scratch memory and calls the guest. */
typedef struct { uint32_t cb, ctx; } tramp_t;

static uint32_t WINAPI tr_mode(ddsurfacedesc2_t *d, void *c) {
    tramp_t *t = c;
    uint32_t mark = hle_scratch_mark();
    uint32_t args[2] = { hle_scratch_copy(d, sizeof *d), t->ctx };
    uint32_t r = hle_call_guest(t->cb, 2, args);
    hle_scratch_release(mark);
    return r;
}
static uint32_t WINAPI tr_surface(surface7_t *s, ddsurfacedesc2_t *d, void *c) {
    tramp_t *t = c;
    uint32_t mark = hle_scratch_mark();
    uint32_t args[3] = { com_wrap(s, CK_SURFACE7), hle_scratch_copy(d, sizeof *d), t->ctx };
    uint32_t r = hle_call_guest(t->cb, 3, args);
    hle_scratch_release(mark);
    return r;
}

/* ---------------------------------------------------------------- import */
static void x_DirectDrawCreateEx(void) {          /* (GUID*, LPVOID* out, REFIID, IUnknown* outer) */
    void *dd = NULL;
    uint32_t hr = stub_DirectDrawCreateEx(COM_P(0), &dd, COM_P(2), COM_P(3));
    com_out(A32(1), hr == DD_OK ? dd : NULL, CK_DDRAW7);
    RET(hr, 4);
}

/* ---------------------------------------------------------------- IDirectDraw7 */
static void dd_QueryInterface(void) {
    COM_SELF(ddraw7_t, CK_DDRAW7, 2);
    void *out = NULL;
    if (com_guid_is(A32(1), &IID_IDirectDraw7) || com_guid_is(A32(1), &IID_IUnknown_)) {
        uint32_t hr = ddraw7_QueryInterface(self, (guid_t *)&IID_IDirectDraw7, &out);
        com_out(A32(2), out, CK_DDRAW7);
        RET(hr, 3);
    }
    if (com_guid_is(A32(1), &IID_IDirect3D7)) {
        uint32_t hr = ddraw7_QueryInterface(self, COM_P(1), &out);
        com_out(A32(2), out, CK_D3D7);
        RET(hr, 3);
    }
    QI_FAIL("IDirectDraw7");
}
static void dd_AddRef(void)  { COM_SELF(ddraw7_t, CK_DDRAW7, 0); RET(ddraw7_AddRef(self), 1); }
static void dd_Release(void) { COM_SELF(ddraw7_t, CK_DDRAW7, 0); RET(ddraw7_Release(self), 1); }
static void dd_CreateClipper(void) {              /* (flags, LPDIRECTDRAWCLIPPER*, outer) */
    COM_SELF(ddraw7_t, CK_DDRAW7, 3);
    clipper7_t *c = NULL;
    uint32_t hr = ddraw7_CreateClipper(self, A32(1), &c, COM_P(3));
    com_out(A32(2), hr == DD_OK ? c : NULL, CK_CLIPPER);
    RET(hr, 4);
}
static void dd_CreatePalette(void) {              /* (flags, PALETTEENTRY*, LPDIRECTDRAWPALETTE*, outer) */
    COM_SELF(ddraw7_t, CK_DDRAW7, 4);
    palette7_t *p = NULL;
    uint32_t hr = ddraw7_CreatePalette(self, A32(1), COM_P(2), &p, COM_P(4));
    com_out(A32(3), hr == DD_OK ? p : NULL, CK_PALETTE);
    RET(hr, 5);
}
static void dd_CreateSurface(void) {              /* (DDSURFACEDESC2*, LPDIRECTDRAWSURFACE7*, outer) */
    COM_SELF(ddraw7_t, CK_DDRAW7, 3);
    ddsurfacedesc2_t *desc = COM_P(1);
    if (desc && (desc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE))
        gfx_host_open(self->core->mode_w, self->core->mode_h);
    surface7_t *s = NULL;
    uint32_t hr = ddraw7_CreateSurface(self, desc, &s, COM_P(3));
    com_out(A32(2), hr == DD_OK ? s : NULL, CK_SURFACE7);
    RET(hr, 4);
}
static void dd_EnumDisplayModes(void) {           /* (flags, DDSURFACEDESC2* filter, ctx, callback) */
    COM_SELF(ddraw7_t, CK_DDRAW7, 4);
    tramp_t t = { A32(4), A32(3) };
    RET(t.cb ? ddraw7_EnumDisplayModes(self, A32(1), COM_P(2), &t, (void *)tr_mode) : DD_OK, 5);
}
static void dd_EnumSurfaces(void) {               /* (flags, DDSURFACEDESC2*, ctx, callback) */
    COM_SELF(ddraw7_t, CK_DDRAW7, 4);
    tramp_t t = { A32(4), A32(3) };
    RET(t.cb ? ddraw7_EnumSurfaces(self, A32(1), COM_P(2), &t, (void *)tr_surface) : DD_OK, 5);
}
static void dd_FlipToGDISurface(void) { COM_SELF(ddraw7_t, CK_DDRAW7, 0); RET(log_FlipToGDISurface(self), 1); }
static void dd_GetCaps(void) { COM_SELF(ddraw7_t, CK_DDRAW7, 2); RET(ddraw7_GetCaps(self, COM_P(1), COM_P(2)), 3); }
static void dd_GetDisplayMode(void) { COM_SELF(ddraw7_t, CK_DDRAW7, 1); RET(ddraw7_GetDisplayMode(self, COM_P(1)), 2); }
static void dd_GetFourCCCodes(void) { COM_SELF(ddraw7_t, CK_DDRAW7, 2); RET(ddraw7_GetFourCCCodes(self, COM_P(1), COM_P(2)), 3); }
static void dd_Initialize(void) { COM_SELF(ddraw7_t, CK_DDRAW7, 1); RET(ddraw7_Initialize(self, COM_P(1)), 2); }
static void dd_RestoreDisplayMode(void) { COM_SELF(ddraw7_t, CK_DDRAW7, 0); RET(ddraw7_RestoreDisplayMode(self), 1); }
static void dd_SetCooperativeLevel(void) {        /* (HWND, flags) */
    COM_SELF(ddraw7_t, CK_DDRAW7, 2);
    RET(ddraw7_SetCooperativeLevel(self, A32(1), A32(2)), 3);
}
static void dd_SetDisplayMode(void) {             /* (w, h, bpp, refresh, flags) */
    COM_SELF(ddraw7_t, CK_DDRAW7, 5);
    if (A32(1) && A32(2)) gfx_host_open((int)A32(1), (int)A32(2));
    RET(ddraw7_SetDisplayMode(self, A32(1), A32(2), A32(3), A32(4), A32(5)), 6);
}
static void dd_GetAvailableVidMem(void) {
    COM_SELF(ddraw7_t, CK_DDRAW7, 3);
    RET(ddraw7_GetAvailableVidMem(self, COM_P(1), COM_P(2), COM_P(3)), 4);
}
static void dd_GetDeviceIdentifier(void) {
    COM_SELF(ddraw7_t, CK_DDRAW7, 2);
    RET(ddraw7_GetDeviceIdentifier(self, COM_P(1), A32(2)), 3);
}

static const win32hle_shim k_ddraw7[] = {
    { "QueryInterface", dd_QueryInterface }, { "AddRef", dd_AddRef }, { "Release", dd_Release },
    { "Compact", com_fail0 }, { "CreateClipper", dd_CreateClipper }, { "CreatePalette", dd_CreatePalette },
    { "CreateSurface", dd_CreateSurface }, { "DuplicateSurface", com_fail2 },
    { "EnumDisplayModes", dd_EnumDisplayModes }, { "EnumSurfaces", dd_EnumSurfaces },
    { "FlipToGDISurface", dd_FlipToGDISurface }, { "GetCaps", dd_GetCaps },
    { "GetDisplayMode", dd_GetDisplayMode }, { "GetFourCCCodes", dd_GetFourCCCodes },
    { "GetGDISurface", com_fail1 }, { "GetMonitorFrequency", com_fail1 },
    { "GetScanLine", com_fail1 }, { "GetVerticalBlankStatus", com_fail1 },
    { "Initialize", dd_Initialize }, { "RestoreDisplayMode", dd_RestoreDisplayMode },
    { "SetCooperativeLevel", dd_SetCooperativeLevel }, { "SetDisplayMode", dd_SetDisplayMode },
    { "WaitForVerticalBlank", com_fail2 }, { "GetAvailableVidMem", dd_GetAvailableVidMem },
    { "GetSurfaceFromDC", com_fail2 }, { "RestoreAllSurfaces", com_fail0 },
    { "TestCooperativeLevel", com_fail0 }, { "GetDeviceIdentifier", dd_GetDeviceIdentifier },
    { "StartModeTest", com_fail3 }, { "EvaluateMode", com_fail2 },
};

/* ---------------------------------------------------------------- IDirectDrawSurface7 */
static void s_QueryInterface(void) {
    COM_SELF(surface7_t, CK_SURFACE7, 2);
    if (com_guid_is(A32(1), &IID_IDirectDrawSurface7) || com_guid_is(A32(1), &IID_IUnknown_))
        QI_SELF(surface7_AddRef, CK_SURFACE7);
    if (com_guid_is(A32(1), &IID_IDirectDrawGammaControl)) {
        void *out = NULL;
        uint32_t hr = surface7_QueryInterface(self, COM_P(1), &out);
        com_out(A32(2), hr == DD_OK ? out : NULL, CK_GAMMA);
        RET(hr, 3);
    }
    QI_FAIL("IDirectDrawSurface7");
}
static void s_AddRef(void)  { COM_SELF(surface7_t, CK_SURFACE7, 0); RET(surface7_AddRef(self), 1); }
static void s_Release(void) { COM_SELF(surface7_t, CK_SURFACE7, 0); RET(surface7_Release(self), 1); }
static void s_AddAttachedSurface(void) {
    COM_SELF(surface7_t, CK_SURFACE7, 1);
    RET(surface7_AddAttachedSurface(self, COM_IN(1, CK_SURFACE7)), 2);
}
static void s_Blt(void) {                         /* (RECT* dst, src, RECT* src, flags, DDBLTFX*) */
    COM_SELF(surface7_t, CK_SURFACE7, 5);
    RET(surface7_Blt(self, COM_P(1), COM_IN(2, CK_SURFACE7), COM_P(3), A32(4), COM_P(5)), 6);
}
static void s_BltFast(void) {                     /* (x, y, src, RECT* src, trans) */
    COM_SELF(surface7_t, CK_SURFACE7, 5);
    RET(surface7_BltFast(self, A32(1), A32(2), COM_IN(3, CK_SURFACE7), COM_P(4), A32(5)), 6);
}
static void s_DeleteAttachedSurface(void) {
    COM_SELF(surface7_t, CK_SURFACE7, 2);
    RET(surface7_DeleteAttachedSurface(self, A32(1), COM_IN(2, CK_SURFACE7)), 3);
}
static void s_EnumAttachedSurfaces(void) {        /* (ctx, callback) */
    COM_SELF(surface7_t, CK_SURFACE7, 2);
    tramp_t t = { A32(2), A32(1) };
    RET(t.cb ? surface7_EnumAttachedSurfaces(self, &t, (void *)tr_surface) : DD_OK, 3);
}
static void s_Flip(void) {
    COM_SELF(surface7_t, CK_SURFACE7, 2);
    RET(surface7_Flip(self, COM_IN(1, CK_SURFACE7), A32(2)), 3);
}
static void s_GetAttachedSurface(void) {          /* (DDSCAPS2*, LPDIRECTDRAWSURFACE7*) */
    COM_SELF(surface7_t, CK_SURFACE7, 2);
    surface7_t *a = NULL;
    uint32_t hr = surface7_GetAttachedSurface(self, COM_P(1), &a);
    com_out(A32(2), hr == DD_OK ? a : NULL, CK_SURFACE7);
    RET(hr, 3);
}
static void s_GetBltStatus(void)  { COM_SELF(surface7_t, CK_SURFACE7, 1); RET(surface7_GetBltStatus(self, A32(1)), 2); }
static void s_GetCaps(void)       { COM_SELF(surface7_t, CK_SURFACE7, 1); RET(surface7_GetCaps(self, COM_P(1)), 2); }
static void s_GetColorKey(void)   { COM_SELF(surface7_t, CK_SURFACE7, 2); RET(surface7_GetColorKey(self, A32(1), COM_P(2)), 3); }
static void s_GetDC(void) {                       /* the HDC is the surface's own proxy, as LongRoad2 used the object */
    COM_SELF(surface7_t, CK_SURFACE7, 1);
    uint32_t *out = COM_P(1);
    if (!out) RET(DDERR_INVALIDPARAMS, 2);
    *out = A32(0);
    RET(DD_OK, 2);
}
static void s_GetFlipStatus(void) { COM_SELF(surface7_t, CK_SURFACE7, 1); RET(surface7_GetFlipStatus(self, A32(1)), 2); }
static void s_GetPalette(void) {
    COM_SELF(surface7_t, CK_SURFACE7, 1);
    palette7_t *p = NULL;
    uint32_t hr = surface7_GetPalette(self, &p);
    com_out(A32(1), hr == DD_OK ? p : NULL, CK_PALETTE);
    RET(hr, 2);
}
static void s_GetPixelFormat(void) { COM_SELF(surface7_t, CK_SURFACE7, 1); RET(surface7_GetPixelFormat(self, COM_P(1)), 2); }
static void s_GetSurfaceDesc(void) { COM_SELF(surface7_t, CK_SURFACE7, 1); RET(surface7_GetSurfaceDesc(self, COM_P(1)), 2); }
static void s_IsLost(void)         { COM_SELF(surface7_t, CK_SURFACE7, 0); RET(surface7_IsLost(self), 1); }
static void s_Lock(void) {                        /* (RECT*, DDSURFACEDESC2*, flags, HANDLE) */
    COM_SELF(surface7_t, CK_SURFACE7, 4);
    RET(surface7_Lock(self, COM_P(1), COM_P(2), A32(3), COM_P(4)), 5);
}
static void s_ReleaseDC(void) { COM_SELF(surface7_t, CK_SURFACE7, 1); RET(surface7_ReleaseDC(self, COM_P(1)), 2); }
static void s_Restore(void)   { COM_SELF(surface7_t, CK_SURFACE7, 0); RET(surface7_Restore(self), 1); }
static void s_SetColorKey(void) { COM_SELF(surface7_t, CK_SURFACE7, 2); RET(surface7_SetColorKey(self, A32(1), COM_P(2)), 3); }
static void s_SetPalette(void) {
    COM_SELF(surface7_t, CK_SURFACE7, 1);
    RET(surface7_SetPalette(self, COM_IN(1, CK_PALETTE)), 2);
}
static void s_Unlock(void) { COM_SELF(surface7_t, CK_SURFACE7, 1); RET(surface7_Unlock(self, COM_P(1)), 2); }
static void s_SetSurfaceDesc(void) {
    COM_SELF(surface7_t, CK_SURFACE7, 2);
    RET(surface7_SetSurfaceDesc(self, COM_P(1), A32(2)), 3);
}

static const win32hle_shim k_surface7[] = {
    { "QueryInterface", s_QueryInterface }, { "AddRef", s_AddRef }, { "Release", s_Release },
    { "AddAttachedSurface", s_AddAttachedSurface }, { "AddOverlayDirtyRect", com_fail1 },
    { "Blt", s_Blt }, { "BltBatch", com_fail3 }, { "BltFast", s_BltFast },
    { "DeleteAttachedSurface", s_DeleteAttachedSurface }, { "EnumAttachedSurfaces", s_EnumAttachedSurfaces },
    { "EnumOverlayZOrders", com_fail3 }, { "Flip", s_Flip }, { "GetAttachedSurface", s_GetAttachedSurface },
    { "GetBltStatus", s_GetBltStatus }, { "GetCaps", s_GetCaps }, { "GetClipper", com_fail1 },
    { "GetColorKey", s_GetColorKey }, { "GetDC", s_GetDC }, { "GetFlipStatus", s_GetFlipStatus },
    { "GetOverlayPosition", com_fail2 }, { "GetPalette", s_GetPalette }, { "GetPixelFormat", s_GetPixelFormat },
    { "GetSurfaceDesc", s_GetSurfaceDesc }, { "Initialize", com_fail2 }, { "IsLost", s_IsLost },
    { "Lock", s_Lock }, { "ReleaseDC", s_ReleaseDC }, { "Restore", s_Restore }, { "SetClipper", com_fail1 },
    { "SetColorKey", s_SetColorKey }, { "SetOverlayPosition", com_fail2 }, { "SetPalette", s_SetPalette },
    { "Unlock", s_Unlock }, { "UpdateOverlay", com_fail5 }, { "UpdateOverlayDisplay", com_fail1 },
    { "UpdateOverlayZOrder", com_fail2 }, { "GetDDInterface", com_fail1 }, { "PageLock", com_fail1 },
    { "PageUnlock", com_fail1 }, { "SetSurfaceDesc", s_SetSurfaceDesc }, { "SetPrivateData", com_fail4 },
    { "GetPrivateData", com_fail3 }, { "FreePrivateData", com_fail1 }, { "GetUniquenessValue", com_fail1 },
    { "ChangeUniquenessValue", com_fail0 }, { "SetPriority", com_fail1 }, { "GetPriority", com_fail1 },
    { "SetLOD", com_fail1 }, { "GetLOD", com_fail1 },
};

/* ---------------------------------------------------------------- IDirectDrawPalette */
static const guid_t IID_IDirectDrawPalette_ =
    { 0x6C14DB84, 0xA733, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static void p_QueryInterface(void) {
    COM_SELF(palette7_t, CK_PALETTE, 2);
    if (com_guid_is(A32(1), &IID_IDirectDrawPalette_) || com_guid_is(A32(1), &IID_IUnknown_))
        QI_SELF(palette7_AddRef, CK_PALETTE);
    QI_FAIL("IDirectDrawPalette");
}
static void p_AddRef(void)  { COM_SELF(palette7_t, CK_PALETTE, 0); RET(palette7_AddRef(self), 1); }
static void p_Release(void) { COM_SELF(palette7_t, CK_PALETTE, 0); RET(palette7_Release(self), 1); }
static void p_GetCaps(void) { COM_SELF(palette7_t, CK_PALETTE, 1); RET(palette7_GetCaps(self, COM_P(1)), 2); }
static void p_GetEntries(void) {
    COM_SELF(palette7_t, CK_PALETTE, 4);
    RET(palette7_GetEntries(self, A32(1), A32(2), A32(3), COM_P(4)), 5);
}
static void p_Initialize(void) {
    COM_SELF(palette7_t, CK_PALETTE, 3);
    RET(palette7_Initialize(self, COM_IN(1, CK_DDRAW7), A32(2), COM_P(3)), 4);
}
static void p_SetEntries(void) {
    COM_SELF(palette7_t, CK_PALETTE, 4);
    RET(palette7_SetEntries(self, A32(1), A32(2), A32(3), COM_P(4)), 5);
}
static const win32hle_shim k_palette[] = {
    { "QueryInterface", p_QueryInterface }, { "AddRef", p_AddRef }, { "Release", p_Release },
    { "GetCaps", p_GetCaps }, { "GetEntries", p_GetEntries }, { "Initialize", p_Initialize },
    { "SetEntries", p_SetEntries },
};

/* ---------------------------------------------------------------- IDirectDrawClipper */
static const guid_t IID_IDirectDrawClipper_ =
    { 0x6C14DB85, 0xA733, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static void c_QueryInterface(void) {
    COM_SELF(clipper7_t, CK_CLIPPER, 2);
    if (com_guid_is(A32(1), &IID_IDirectDrawClipper_) || com_guid_is(A32(1), &IID_IUnknown_))
        QI_SELF(clipper7_AddRef, CK_CLIPPER);
    QI_FAIL("IDirectDrawClipper");
}
static void c_AddRef(void)  { COM_SELF(clipper7_t, CK_CLIPPER, 0); RET(clipper7_AddRef(self), 1); }
static void c_Release(void) { COM_SELF(clipper7_t, CK_CLIPPER, 0); RET(clipper7_Release(self), 1); }
static const win32hle_shim k_clipper[] = {
    { "QueryInterface", c_QueryInterface }, { "AddRef", c_AddRef }, { "Release", c_Release },
    { "GetClipList", com_fail3 }, { "GetHWnd", com_fail1 }, { "Initialize", com_fail2 },
    { "IsClipListChanged", com_fail1 }, { "SetClipList", com_fail2 }, { "SetHWnd", com_fail2 },
};

/* ---------------------------------------------------------------- IDirectDrawGammaControl */
static void g_QueryInterface(void) {
    COM_SELF(gamma_control_t, CK_GAMMA, 2);
    if (com_guid_is(A32(1), &IID_IDirectDrawGammaControl) || com_guid_is(A32(1), &IID_IUnknown_))
        QI_SELF(gamma_control_AddRef, CK_GAMMA);
    QI_FAIL("IDirectDrawGammaControl");
}
static void g_AddRef(void)  { COM_SELF(gamma_control_t, CK_GAMMA, 0); RET(gamma_control_AddRef(self), 1); }
static void g_Release(void) { COM_SELF(gamma_control_t, CK_GAMMA, 0); RET(gamma_control_Release(self), 1); }
static void g_GetGammaRamp(void) {
    COM_SELF(gamma_control_t, CK_GAMMA, 2);
    RET(gamma_control_GetGammaRamp(self, A32(1), COM_P(2)), 3);
}
static void g_SetGammaRamp(void) {
    COM_SELF(gamma_control_t, CK_GAMMA, 2);
    RET(gamma_control_SetGammaRamp(self, A32(1), COM_P(2)), 3);
}
static const win32hle_shim k_gamma[] = {
    { "QueryInterface", g_QueryInterface }, { "AddRef", g_AddRef }, { "Release", g_Release },
    { "GetGammaRamp", g_GetGammaRamp }, { "SetGammaRamp", g_SetGammaRamp },
};

/* ---------------------------------------------------------------- registration */
static const win32hle_shim k_imports[] = {
    { "DirectDrawCreateEx", x_DirectDrawCreateEx },
    { 0, 0 }
};

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
void ht2_ddraw7_register(void) {
    win32hle_register(k_imports);
    com_define(CK_DDRAW7,   "IDirectDraw7",            k_ddraw7,   N(k_ddraw7));
    com_define(CK_SURFACE7, "IDirectDrawSurface7",     k_surface7, N(k_surface7));
    com_define(CK_PALETTE,  "IDirectDrawPalette",      k_palette,  N(k_palette));
    com_define(CK_CLIPPER,  "IDirectDrawClipper",      k_clipper,  N(k_clipper));
    com_define(CK_GAMMA,    "IDirectDrawGammaControl", k_gamma,    N(k_gamma));
}

/* The real vtables must keep their slot counts: a slot added or dropped in
 * LongRoad2's *_vtbl_t would shift every later method in ours. */
_Static_assert(N(k_ddraw7)   == sizeof(ddraw7_vtbl_t) / sizeof(void *),        "IDirectDraw7 slots");
_Static_assert(N(k_surface7) == sizeof(surface7_vtbl_t) / sizeof(void *),      "IDirectDrawSurface7 slots");
_Static_assert(N(k_palette)  == sizeof(palette7_vtbl_t) / sizeof(void *),      "IDirectDrawPalette slots");
_Static_assert(N(k_clipper)  == sizeof(clipper7_vtbl_t) / sizeof(void *),      "IDirectDrawClipper slots");
_Static_assert(N(k_gamma)    == sizeof(gamma_control_vtbl_t) / sizeof(void *), "IDirectDrawGammaControl slots");
