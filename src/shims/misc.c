/*
 * misc.c - the small USER32/GDI32/KERNEL32/... imports king.exe has that
 * win32hle does not answer. Mostly MFC 4.2 plumbing (props, DDE lParams,
 * mapping modes, print setup) that a full-screen DirectDraw game reaches only
 * in passing. Each is the documented "nothing to do here" answer, with its
 * stdcall purge right; a stub that turns out to matter moves out of here and
 * gets real behaviour.
 *
 * Args counts are from the SDK prototypes (pcrecomp tools/pe/stdcall_argc.py).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ht2_shims.h"

/* A stub that answers `val` and pops `argc` dwords. */
#define CONST_STUB(fn, val, argc) static void fn(void) { RET(val, argc); }

/* ---- KERNEL32 ---- */
CONST_STUB(k_GlobalFlags,       0, 1)
CONST_STUB(k_GetProcessVersion, 0x00040000u, 1)   /* "4.0": MFC checks >= 4 for new-shell paths */
CONST_STUB(k_SuspendThread,     0xFFFFFFFFu, 1)   /* the crash reporter only; fail it */
CONST_STUB(k_ReadProcessMemory, 0, 5)
CONST_STUB(k_GetThreadContext,  0, 2)

/* ---- USER32 ---- */
static void u_ReuseDDElParam(void) { RET(A32(4), 5); }      /* (lParam, msgIn, msgOut, uiLo, uiHi) */
static void u_UnpackDDElParam(void) {                        /* (msg, lParam, puiLo, puiHi) */
    uint32_t *lo = APTR(2), *hi = APTR(3), lp = A32(1);
    if (lo) *lo = lp & 0xFFFF;
    if (hi) *hi = lp >> 16;
    RET(1, 4);
}
CONST_STUB(u_TabbedTextOutA,  0, 8)
CONST_STUB(u_GrayStringA,     0, 9)
CONST_STUB(u_GetSysColorBrush, 0, 1)
static void u_SystemParametersInfoA(void) {                  /* (action, uiParam, pvParam, winIni) */
    uint32_t action = A32(0);
    uint32_t *pv = APTR(2);
    if (action == 0x30 && pv) {                              /* SPI_GETWORKAREA */
        pv[0] = 0; pv[1] = 0; pv[2] = 640; pv[3] = 480;
    } else if (pv && action < 0x1000 && (action & 1) == 0) {
        /* SPI_GET*: zero is "off" / "none" for every one MFC asks about */
        pv[0] = 0;
    }
    RET(1, 4);
}

/* Window properties: a small table keyed by (hwnd, atom-or-name). */
#define MAX_PROPS 256
static struct { uint32_t hwnd; char name[64]; uint32_t val; } g_props[MAX_PROPS];
static void prop_key(uint32_t s, char *out) {
    if (s < 0x10000) snprintf(out, 64, "#%u", s);            /* an atom */
    else snprintf(out, 64, "%s", (const char *)hle_ptr(s));
}
static int prop_find(uint32_t hwnd, const char *k) {
    for (int i = 0; i < MAX_PROPS; i++)
        if (g_props[i].hwnd == hwnd && !strcmp(g_props[i].name, k)) return i;
    return -1;
}
static void u_SetPropA(void) {                               /* (hwnd, name, data) */
    char k[64]; prop_key(A32(1), k);
    int i = prop_find(A32(0), k);
    for (int j = 0; i < 0 && j < MAX_PROPS; j++) if (!g_props[j].hwnd) i = j;
    if (i < 0) RET(0, 3);
    g_props[i].hwnd = A32(0); snprintf(g_props[i].name, 64, "%s", k); g_props[i].val = A32(2);
    RET(1, 3);
}
static void u_GetPropA(void) {
    char k[64]; prop_key(A32(1), k);
    int i = prop_find(A32(0), k);
    RET(i < 0 ? 0 : g_props[i].val, 2);
}
static void u_RemovePropA(void) {
    char k[64]; prop_key(A32(1), k);
    int i = prop_find(A32(0), k);
    uint32_t v = 0;
    if (i >= 0) { v = g_props[i].val; g_props[i].hwnd = 0; }
    RET(v, 2);
}
CONST_STUB(u_GetMenuCheckMarkDimensions, (13u << 16) | 13u, 0)
CONST_STUB(u_SetMenuItemBitmaps, 1, 5)
static void u_GetLastActivePopup(void) { RET(A32(0), 1); }
CONST_STUB(u_SetClassLongA, 0, 3)
CONST_STUB(u_GetClassLongA, 0, 2)
static void u_GetMessageTime(void) { RET(hle_ticks_ms(), 0); }
CONST_STUB(u_GetMessagePos, 0, 0)
CONST_STUB(u_IsIconic, 0, 1)
static void u_GetWindowPlacement(void) {                     /* (hwnd, WINDOWPLACEMENT*) */
    uint32_t *wp = APTR(1);
    if (wp) {
        memset(wp + 1, 0, 40);
        wp[0] = 44; wp[2] = 1;                               /* length, showCmd = SW_SHOWNORMAL */
        wp[9] = 640; wp[10] = 480;                           /* rcNormalPosition right/bottom */
    }
    RET(1, 2);
}
CONST_STUB(u_ShowOwnedPopups, 1, 2)
CONST_STUB(u_UnregisterClassA, 1, 2)

/* ---- GDI32 ---- */
static void g_SetDIBitsToDevice(void) { RET(A32(8), 12); }  /* "drew" cLines */
CONST_STUB(g_Escape,       0, 5)
CONST_STUB(g_ExtTextOutA,  1, 8)
CONST_STUB(g_RectVisible,  1, 2)
CONST_STUB(g_PtVisible,    1, 3)
CONST_STUB(g_GetMapMode,   1, 1)                             /* MM_TEXT */
CONST_STUB(g_GetDIBits,    0, 7)
CONST_STUB(g_GetObjectA,   0, 3)
static void g_GetClipBox(void) {                             /* (hdc, RECT*) */
    int32_t *r = APTR(1);
    if (r) { r[0] = 0; r[1] = 0; r[2] = 640; r[3] = 480; }
    RET(2, 2);                                               /* SIMPLEREGION */
}
CONST_STUB(g_OffsetViewportOrgEx, 1, 4)
CONST_STUB(g_SetMapMode,          1, 2)
CONST_STUB(g_ScaleViewportExtEx,  1, 6)
CONST_STUB(g_SetViewportExtEx,    1, 4)
CONST_STUB(g_SetWindowExtEx,      1, 4)
CONST_STUB(g_ScaleWindowExtEx,    1, 6)

/* ---- WINSPOOL.DRV: no printers ---- */
CONST_STUB(w_OpenPrinterA, 0, 3)
CONST_STUB(w_ClosePrinter, 1, 1)
CONST_STUB(w_DocumentPropertiesA, 0xFFFFFFFFu, 6)

/* ---- SHELL32: nothing is ever dropped on the window ---- */
CONST_STUB(s_DragQueryFileA, 0, 4)
static void s_DragFinish(void) { RETV(1); }

/* ---- ole32 / OLEAUT32 ---- */
/* What reaches it: the names in an IEnumSTATSTG's STATSTGs (win32hle's heap) */
static void o_CoTaskMemFree(void) { if (APTR(0)) hle_free(APTR(0)); RETV(1); }

static void a_SysAllocStringLen(void) {                      /* #4 (const OLECHAR*, UINT len) */
    uint32_t n = A32(1);
    const uint16_t *src = APTR(0);
    uint8_t *b = (uint8_t *)hle_alloc(4 + 2 * n + 2);
    *(uint32_t *)b = 2 * n;
    if (src) memcpy(b + 4, src, 2 * n);
    RETP(b + 4, 2);
}

const win32hle_shim ht2_shims_misc[] = {
    { "GlobalFlags", k_GlobalFlags }, { "GetProcessVersion", k_GetProcessVersion },
    { "SuspendThread", k_SuspendThread }, { "ReadProcessMemory", k_ReadProcessMemory },
    { "GetThreadContext", k_GetThreadContext },

    { "ReuseDDElParam", u_ReuseDDElParam }, { "UnpackDDElParam", u_UnpackDDElParam },
    { "TabbedTextOutA", u_TabbedTextOutA }, { "GrayStringA", u_GrayStringA },
    { "GetSysColorBrush", u_GetSysColorBrush }, { "SystemParametersInfoA", u_SystemParametersInfoA },
    { "SetPropA", u_SetPropA }, { "GetPropA", u_GetPropA }, { "RemovePropA", u_RemovePropA },
    { "GetMenuCheckMarkDimensions", u_GetMenuCheckMarkDimensions },
    { "SetMenuItemBitmaps", u_SetMenuItemBitmaps }, { "GetLastActivePopup", u_GetLastActivePopup },
    { "SetClassLongA", u_SetClassLongA }, { "GetClassLongA", u_GetClassLongA },
    { "GetMessageTime", u_GetMessageTime }, { "GetMessagePos", u_GetMessagePos },
    { "IsIconic", u_IsIconic }, { "GetWindowPlacement", u_GetWindowPlacement },
    { "ShowOwnedPopups", u_ShowOwnedPopups }, { "UnregisterClassA", u_UnregisterClassA },

    { "SetDIBitsToDevice", g_SetDIBitsToDevice }, { "Escape", g_Escape },
    { "ExtTextOutA", g_ExtTextOutA }, { "RectVisible", g_RectVisible }, { "PtVisible", g_PtVisible },
    { "GetMapMode", g_GetMapMode }, { "GetDIBits", g_GetDIBits }, { "GetObjectA", g_GetObjectA },
    { "GetClipBox", g_GetClipBox }, { "OffsetViewportOrgEx", g_OffsetViewportOrgEx },
    { "SetMapMode", g_SetMapMode }, { "ScaleViewportExtEx", g_ScaleViewportExtEx },
    { "SetViewportExtEx", g_SetViewportExtEx }, { "SetWindowExtEx", g_SetWindowExtEx },
    { "ScaleWindowExtEx", g_ScaleWindowExtEx },

    { "OpenPrinterA", w_OpenPrinterA }, { "ClosePrinter", w_ClosePrinter },
    { "DocumentPropertiesA", w_DocumentPropertiesA },
    { "DragQueryFileA", s_DragQueryFileA }, { "DragFinish", s_DragFinish },
    { "CoTaskMemFree", o_CoTaskMemFree },
    { "oleaut32.dll#4", a_SysAllocStringLen }, { "SysAllocStringLen", a_SysAllocStringLen },
    { 0, 0 }
};

void ht2_shims_register(void) {
    win32hle_register(ht2_shims_misc);
    ht2_ddraw7_register();      /* ddraw7.c: DirectDrawCreateEx + the DirectDraw 7 interfaces */
    ht2_d3d7_register();        /* d3d7.c: the Direct3D 7 interfaces */
    ht2_dinput_register();      /* dinput.c: DirectInputCreateA, an IDirectInput7A with no devices */
    ht2_dplay_register();       /* dplay.c: CLSID_DirectPlay / DirectPlayLobby that find nothing */
    ht2_quartz_register();      /* quartz.c: CLSID_FilterGraph, the MP3 music player (mpg123) */
}

