#ifndef LONGROAD2_DDRAW_TYPES_H
#define LONGROAD2_DDRAW_TYPES_H

#include <string.h>

#include "shim_common.h"

/*
 * DirectDraw7/Direct3D7 are COM interfaces, not flat DLL exports: only
 * DirectDrawCreateEx/DirectDrawEnumerateA are real PE imports (see
 * docs/api-surface/ddraw.md) -- everything else is a vtable method
 * obtained via QueryInterface at runtime. Every struct/constant here
 * mirrors the real 32-bit DirectX7 SDK layout exactly (cross-checked
 * against /usr/include/wine-staging-11.13/wine/windows/ddraw.h and
 * d3d.h) since king.exe fills these in/reads them back itself.
 */

typedef struct { uint32_t Data1; uint16_t Data2; uint16_t Data3; uint8_t Data4[8]; } guid_t;

static inline int guid_equal(const guid_t *a, const guid_t *b)
{
    return a != NULL && b != NULL &&
           a->Data1 == b->Data1 && a->Data2 == b->Data2 && a->Data3 == b->Data3 &&
           memcmp(a->Data4, b->Data4, 8) == 0;
}

/* HRESULT: only success/generic-failure are ever distinguished by the
   game here (SUCCEEDED()/FAILED() just test bit 31). */
#define DD_OK             0x00000000u
#define DDERR_GENERIC     0x80004005u
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_OUTOFMEMORY 0x8007000Eu
#define DDERR_NOINTERFACE 0x80004002u
#define DDERR_UNSUPPORTED 0x80004001u

#define DDSCL_FULLSCREEN         0x00000001u
#define DDSCL_ALLOWREBOOT        0x00000002u
#define DDSCL_NOWINDOWCHANGES    0x00000004u
#define DDSCL_NORMAL             0x00000008u
#define DDSCL_EXCLUSIVE          0x00000010u

#define DDSD_CAPS         0x00000001u
#define DDSD_HEIGHT       0x00000002u
#define DDSD_WIDTH        0x00000004u
#define DDSD_PITCH        0x00000008u
#define DDSD_BACKBUFFERCOUNT 0x00000020u
#define DDSD_LPSURFACE    0x00000800u
#define DDSD_PIXELFORMAT  0x00001000u
#define DDSD_CKDESTBLT    0x00004000u
#define DDSD_CKSRCBLT     0x00010000u
#define DDSD_MIPMAPCOUNT  0x00020000u
#define DDSD_REFRESHRATE	0x00040000

#define DDSCAPS_BACKBUFFER     0x00000004u
#define DDSCAPS_COMPLEX        0x00000008u
#define DDSCAPS_FLIP           0x00000010u
#define DDSCAPS_FRONTBUFFER    0x00000020u
#define DDSCAPS_OFFSCREENPLAIN 0x00000040u
#define DDSCAPS_PALETTE        0x00000100u
#define DDSCAPS_PRIMARYSURFACE 0x00000200u
#define DDSCAPS_SYSTEMMEMORY   0x00000800u
#define DDSCAPS_TEXTURE        0x00001000u
#define DDSCAPS_VIDEOMEMORY    0x00004000u
#define DDSCAPS_ZBUFFER        0x00020000u
#define DDSCAPS_MIPMAP         0x00400000u

#define DDPF_ALPHAPIXELS 0x00000001u
#define DDPF_RGB         0x00000040u
#define DDPF_PALETTEINDEXED8 0x00000020u
#define DDPF_ZBUFFER     0x00000400u

#define DDPCAPS_8BIT 0x00000004u

/* DDCAPS.dwCaps (driver capability flags, distinct from DDSCAPS_*
   surface-capability flags above) -- BLT is the one that actually
   matters here: king.exe's own mode-negotiation code
   (FUN_005d72d0/FUN_005e31e0 in docs/game_code/king.exe.c) treats "no
   DDCAPS_BLT" as "this driver can't do what I need," falls back to a
   hardcoded 640x480 mode, and that fallback path dereferences a
   not-yet-set-up window pointer -- a real crash traced back to GetCaps
   claiming zero capabilities. Since every real DirectDraw driver
   (hardware or software/HEL) reports blit support, so do we. */
#define DDCAPS_3D           0x00000001u
#define DDCAPS_BLT          0x00000040u
#define DDCAPS_BLTQUEUE     0x00000080u
#define DDCAPS_GDI          0x00000400u
#define DDCAPS_PALETTE      0x00008000u
#define DDCAPS_BLTDEPTHFILL 0x10000000u

#define DDCAPS2_CANRENDERWINDOWED 0x00080000u

/* Blt/BltFast flags actually acted on by the real pixel-copying blitter in
   ddraw.c -- everything else in the real DDBLT_ and DDBLTFAST_ flag space
   (ROP, alpha, z-buffer, mirroring/rotation FX...) is accepted but
   ignored, same as a HEL software blitter falling back to a plain copy
   when it doesn't support a requested effect. */
#define DDBLT_COLORFILL 0x00000400u
#define DDBLT_KEYSRC    0x00008000u
#define DDBLT_ROP       0x00020000u
#define DDBLTFAST_SRCCOLORKEY 0x00000001u

/* Lock() flags -- not acted on by anything yet (surface7_Lock currently
   discards its own flags parameter and conservatively assumes any
   Lock/Unlock pair could have both read and written), but real values,
   needed to decode a live LONGROAD2_DDRAW_TRACE capture and find out
   whether this game's own Lock calls ever actually pass READONLY/
   WRITEONLY -- see [[project_ddraw_gpu_blt_glreadpixels_elimination]]'s
   own open follow-up. Only the two this investigation cares about are
   defined; the rest of the real DDLOCK_* space (WAIT, EVENT, NOSYSLOCK,
   NOOVERWRITE, DISCARDCONTENTS/OKTOSWAP, DONOTWAIT,
   HASVOLUMETEXTUREBOXRECT, NODIRTYUPDATE) isn't relevant to this
   specific question and isn't defined here. */
#define DDLOCK_READONLY  0x00000010u
#define DDLOCK_WRITEONLY 0x00000020u

#pragma pack(push, 1)

typedef struct { int32_t left, top, right, bottom; } ddraw_rect_t;
typedef struct { int32_t x, y; } ddraw_point_t;

typedef struct {
    uint32_t dwCaps, dwCaps2, dwCaps3, dwCaps4;
} ddscaps2_t;

typedef struct {
    uint32_t dwColorSpaceLowValue;
    uint32_t dwColorSpaceHighValue;
} ddcolorkey_t;

typedef struct {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwFourCC;
    uint32_t dwRGBBitCount;
    uint32_t dwRBitMask;
    uint32_t dwGBitMask;
    uint32_t dwBBitMask;
    uint32_t dwRGBAlphaBitMask;
} ddpixelformat_t;

typedef struct {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwHeight;
    uint32_t dwWidth;
    uint32_t lPitchOrLinearSize;
    uint32_t dwBackBufferCountOrDepth;
    uint32_t dwMipMapCountOrRefreshRateOrSrcVBHandle;
    uint32_t dwAlphaBitDepth;
    uint32_t dwReserved;
    /* hardtruck2-recomp: a guest VA, not a host pointer -- this struct is
       read and written in place in guest memory, at its real 32-bit layout
       (gfx_mem.h). */
    uint32_t lpSurface;

    ddcolorkey_t ddckCKDestOverlayOrEmptyFaceColor;
    ddcolorkey_t ddckCKDestBlt;
    ddcolorkey_t ddckCKSrcOverlay;
    ddcolorkey_t ddckCKSrcBlt;
    ddpixelformat_t ddpfPixelFormat; /* union w/ dwFVF in the real struct -- unused here */
    ddscaps2_t ddsCaps;
    uint32_t dwTextureStage;
} ddsurfacedesc2_t;

typedef struct {
    char     szDriver[512];
    char     szDescription[512];
    int64_t  liDriverVersion;
    uint32_t dwVendorId, dwDeviceId, dwSubSysId, dwRevision;
    guid_t   guidDeviceIdentifier;
    uint32_t dwWHQLLevel;
} dddeviceidentifier2_t;

/* DDCAPS_DX7 prefix, up through dwVidMemTotal/dwVidMemFree at their real
   offsets (60/64) -- the rest of the real (much larger) struct is a
   ROP-capability table and reserved fields nothing here reads, so
   GetCaps() below just memsets the caller's own buffer (sized by the
   caller, per real DDCAPS_DX7) to 0 using the caller's own dwSize rather
   than transcribing every trailing field. */
typedef struct {
    uint32_t dwSize;
    uint32_t dwCaps, dwCaps2, dwCKeyCaps, dwFXCaps, dwFXAlphaCaps, dwPalCaps, dwSVCaps;
    uint32_t dwAlphaBltConstBitDepths, dwAlphaBltPixelBitDepths, dwAlphaBltSurfaceBitDepths;
    uint32_t dwAlphaOverlayConstBitDepths, dwAlphaOverlayPixelBitDepths, dwAlphaOverlaySurfaceBitDepths;
    uint32_t dwZBufferBitDepths;
    uint32_t dwVidMemTotal;
    uint32_t dwVidMemFree;
} ddcaps_dx7_t;

/* DDBLTFX prefix, up through dwFillColor at its real offset (0x50 / DWORD
   20) -- the trailing fields (src/dest color-key overrides, pattern
   surface, edge-blend...) aren't read by anything here, same rationale as
   ddcaps_dx7_t above. */
typedef struct {
    uint32_t dwSize;
    uint32_t dwDDFX;
    uint32_t dwROP;
    uint32_t dwDDROP;
    uint32_t dwRotationAngle;
    uint32_t dwZBufferOpCode;
    uint32_t dwZBufferLow;
    uint32_t dwZBufferHigh;
    uint32_t dwZBufferBaseDest;
    uint32_t dwZDestConstBitDepth;
    uint32_t dwZDestConst; /* union w/ lpDDSZBufferDest in the real struct */
    uint32_t dwZSrcConstBitDepth;
    uint32_t dwZSrcConst;
    uint32_t dwAlphaEdgeBlendBitDepth;
    uint32_t dwAlphaEdgeBlend;
    uint32_t dwReserved;
    uint32_t dwAlphaDestConstBitDepth;
    uint32_t dwAlphaDestConst;
    uint32_t dwAlphaSrcConstBitDepth;
    uint32_t dwAlphaSrcConst;
    uint32_t dwFillColor; /* union w/ dwFillDepth/dwFillPixel */
} ddbltfx_t;

#pragma pack(pop)

#endif /* LONGROAD2_DDRAW_TYPES_H */
