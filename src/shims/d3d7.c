/*
 * d3d7.c - Direct3D 7 for king.exe: LongRoad2's fixed-function-over-GLES
 * renderer (src/gfx/d3d7*.c, d3d7.c included below for its static methods),
 * reached from the guest through com_bridge.h.
 *
 * Objects: IDirect3D7 (from IDirectDraw7::QueryInterface), IDirect3DDevice7,
 *          IDirect3DVertexBuffer7.
 *
 * Pointer arguments arrive as guest VAs and leave as host pointers; the
 * strided-vertex description, the one struct that carries pointers INTO
 * guest memory, is rebuilt per call (strided_from_guest). Slot order and
 * argument counts are LongRoad2's, checked against its vtable structs below.
 */
#include "../gfx/d3d7.c"

#include "com_bridge.h"

static const guid_t IID_IUnknown_ =
    { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const guid_t IID_IDirect3D7_ =
    { 0xf5049e77, 0x4861, 0x11d2, { 0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8 } };
static const guid_t IID_IDirect3DDevice7_ =
    { 0xf5049e79, 0x4861, 0x11d2, { 0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8 } };
static const guid_t IID_IDirect3DVertexBuffer7_ =
    { 0xf5049e7d, 0x4861, 0x11d2, { 0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8 } };

static void qi_fail(const char *who) {
    const uint8_t *g = hle_ptr(A32(1));
    if (g) fprintf(stderr, "[d3d7] %s: QueryInterface for an interface we do not offer: "
                           "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}\n", who,
                   *(const uint32_t *)g, *(const uint16_t *)(g + 4), *(const uint16_t *)(g + 6),
                   g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    com_out(A32(2), NULL, CK_NONE);
}
#define QI(T, kind, iid, addref, who) do { \
        COM_SELF(T, kind, 2); \
        if (com_guid_is(A32(1), (iid)) || com_guid_is(A32(1), &IID_IUnknown_)) { \
            (addref)(self); com_out(A32(2), self, (kind)); RET(DD_OK, 3); } \
        qi_fail(who); RET(COM_E_NOINTERFACE, 3); \
    } while (0)

/* ---------------------------------------------------------------- callbacks */
typedef struct { uint32_t cb, ctx; } tramp_t;

static uint32_t WINAPI tr_pixfmt(ddpixelformat_t *fmt, void *c) {   /* EnumTextureFormats, EnumZBufferFormats */
    tramp_t *t = c;
    uint32_t mark = hle_scratch_mark();
    uint32_t args[2] = { hle_scratch_copy(fmt, sizeof *fmt), t->ctx };
    uint32_t r = hle_call_guest(t->cb, 2, args);
    hle_scratch_release(mark);
    return r;
}
static uint32_t WINAPI tr_device(char *desc, char *name, uint8_t *devdesc, void *c) {   /* EnumDevices */
    tramp_t *t = c;
    uint32_t mark = hle_scratch_mark();
    uint32_t args[4] = { hle_scratch_copy(desc, (uint32_t)strlen(desc) + 1),
                         hle_scratch_copy(name, (uint32_t)strlen(name) + 1),
                         hle_scratch_copy(devdesc, 256),   /* the size d3d7_EnumDevices fills */
                         t->ctx };
    uint32_t r = hle_call_guest(t->cb, 4, args);
    hle_scratch_release(mark);
    return r;
}

/* D3DDRAWPRIMITIVESTRIDEDDATA as the guest lays it out: 12 streams of
 * { LPVOID lpvData; DWORD dwStride } (position, normal, diffuse, specular,
 * texcoord[8]); LongRoad2's d3ddrawprimitivestrideddata_t holds host pointers. */
static const d3ddrawprimitivestrideddata_t *strided_from_guest(uint32_t va, d3ddrawprimitivestrideddata_t *h) {
    const uint32_t *w = hle_ptr(va);
    if (!w) return NULL;
    d3ddp_ptrstride_t *s = &h->position;   /* the 12 streams are consecutive, in this order */
    _Static_assert(sizeof(*h) == 12 * sizeof(d3ddp_ptrstride_t), "strided streams are contiguous");
    for (int i = 0; i < 12; i++) {
        s[i].data = hle_ptr(w[2 * i]);
        s[i].stride = w[2 * i + 1];
    }
    return h;
}

/* ---------------------------------------------------------------- IDirect3D7 */
static void d_QueryInterface(void) { QI(d3d7_t, CK_D3D7, &IID_IDirect3D7_, d3d7_AddRef, "IDirect3D7"); }
static void d_AddRef(void)  { COM_SELF(d3d7_t, CK_D3D7, 0); RET(d3d7_AddRef(self), 1); }
static void d_Release(void) { COM_SELF(d3d7_t, CK_D3D7, 0); RET(d3d7_Release(self), 1); }
static void d_EnumDevices(void) {                  /* (callback, ctx) */
    COM_SELF(d3d7_t, CK_D3D7, 2);
    tramp_t t = { A32(1), A32(2) };
    RET(t.cb ? d3d7_EnumDevices(self, (void *)tr_device, &t) : DD_OK, 3);
}
static void d_CreateDevice(void) {                 /* (REFCLSID, render target, LPDIRECT3DDEVICE7*) */
    COM_SELF(d3d7_t, CK_D3D7, 3);
    void *dev = NULL;
    uint32_t hr = d3d7_CreateDevice(self, COM_P(1), COM_IN(2, CK_SURFACE7), &dev);
    com_out(A32(3), hr == DD_OK ? dev : NULL, CK_DEVICE7);
    RET(hr, 4);
}
static void d_CreateVertexBuffer(void) {           /* (D3DVERTEXBUFFERDESC*, LPDIRECT3DVERTEXBUFFER7*, flags) */
    COM_SELF(d3d7_t, CK_D3D7, 3);
    void *vb = NULL;
    uint32_t hr = d3d7_CreateVertexBuffer(self, COM_P(1), &vb, A32(3));
    com_out(A32(2), hr == DD_OK ? vb : NULL, CK_VB7);
    RET(hr, 4);
}
static void d_EnumZBufferFormats(void) {           /* (REFCLSID, callback, ctx) */
    COM_SELF(d3d7_t, CK_D3D7, 3);
    tramp_t t = { A32(2), A32(3) };
    RET(t.cb ? d3d7_EnumZBufferFormats(self, COM_P(1), (void *)tr_pixfmt, &t) : DD_OK, 4);
}
static void d_EvictManagedTextures(void) { COM_SELF(d3d7_t, CK_D3D7, 0); RET(d3d7_EvictManagedTextures(self), 1); }

static const win32hle_shim k_d3d7[] = {
    { "QueryInterface", d_QueryInterface }, { "AddRef", d_AddRef }, { "Release", d_Release },
    { "EnumDevices", d_EnumDevices }, { "CreateDevice", d_CreateDevice },
    { "CreateVertexBuffer", d_CreateVertexBuffer }, { "EnumZBufferFormats", d_EnumZBufferFormats },
    { "EvictManagedTextures", d_EvictManagedTextures },
};

/* ---------------------------------------------------------------- IDirect3DDevice7 */
#define DEV(argc) COM_SELF(d3d_device7_t, CK_DEVICE7, argc)
static void v_QueryInterface(void) {
    QI(d3d_device7_t, CK_DEVICE7, &IID_IDirect3DDevice7_, d3d_device7_AddRef, "IDirect3DDevice7");
}
static void v_AddRef(void)     { DEV(0); RET(d3d_device7_AddRef(self), 1); }
static void v_Release(void)    { DEV(0); RET(d3d_device7_Release(self), 1); }
static void v_GetCaps(void)    { DEV(1); RET(d3d_device7_GetCaps(self, COM_P(1)), 2); }
static void v_EnumTextureFormats(void) {           /* (callback, ctx) */
    DEV(2);
    tramp_t t = { A32(1), A32(2) };
    RET(t.cb ? d3d_device7_EnumTextureFormats(self, (void *)tr_pixfmt, &t) : DD_OK, 3);
}
static void v_BeginScene(void) { DEV(0); RET(d3d_device7_BeginScene(self), 1); }
static void v_EndScene(void)   { DEV(0); RET(d3d_device7_EndScene(self), 1); }
static void v_GetDirect3D(void) {
    DEV(1);
    void *d3d = NULL;
    uint32_t hr = d3d_device7_GetDirect3D(self, &d3d);
    com_out(A32(1), hr == DD_OK ? d3d : NULL, CK_D3D7);
    RET(hr, 2);
}
static void v_SetRenderTarget(void) { DEV(2); RET(d3d_device7_SetRenderTarget(self, COM_IN(1, CK_SURFACE7), A32(2)), 3); }
static void v_GetRenderTarget(void) {
    DEV(1);
    void *s = NULL;
    uint32_t hr = d3d_device7_GetRenderTarget(self, &s);
    com_out(A32(1), hr == DD_OK ? s : NULL, CK_SURFACE7);
    RET(hr, 2);
}
static void v_Clear(void) {                        /* (count, D3DRECT*, flags, color, float z, stencil) */
    DEV(6);
    RET(d3d_device7_Clear(self, A32(1), (uintptr_t)COM_P(2), A32(3), A32(4), A32(5), A32(6)), 7);
}
static void v_SetTransform(void)      { DEV(2); RET(d3d_device7_SetTransform(self, A32(1), COM_P(2)), 3); }
static void v_GetTransform(void)      { DEV(2); RET(d3d_device7_GetTransform(self, A32(1), COM_P(2)), 3); }
static void v_SetViewport(void)       { DEV(1); RET(d3d_device7_SetViewport(self, COM_P(1)), 2); }
static void v_MultiplyTransform(void) { DEV(2); RET(d3d_device7_MultiplyTransform(self, A32(1), COM_P(2)), 3); }
static void v_GetViewport(void)       { DEV(1); RET(d3d_device7_GetViewport(self, COM_P(1)), 2); }
static void v_SetMaterial(void)       { DEV(1); RET(d3d_device7_SetMaterial(self, COM_P(1)), 2); }
static void v_GetMaterial(void)       { DEV(1); RET(d3d_device7_GetMaterial(self, COM_P(1)), 2); }
static void v_SetLight(void)          { DEV(2); RET(d3d_device7_SetLight(self, A32(1), COM_P(2)), 3); }
static void v_GetLight(void)          { DEV(2); RET(d3d_device7_GetLight(self, A32(1), COM_P(2)), 3); }
static void v_SetRenderState(void)    { DEV(2); RET(d3d_device7_SetRenderState(self, A32(1), A32(2)), 3); }
static void v_GetRenderState(void)    { DEV(2); RET(d3d_device7_GetRenderState(self, A32(1), COM_P(2)), 3); }
static void v_BeginStateBlock(void)   { DEV(0); RET(d3d_device7_BeginStateBlock(self), 1); }
static void v_EndStateBlock(void)     { DEV(1); RET(d3d_device7_EndStateBlock(self, COM_P(1)), 2); }
static void v_PreLoad(void)           { DEV(1); RET(d3d_device7_PreLoad(self, A32(1)), 2); }
static void v_DrawPrimitive(void) {                /* (type, fvf, vertices, count, flags) */
    DEV(5);
    RET(d3d_device7_DrawPrimitive(self, A32(1), A32(2), (uintptr_t)COM_P(3), A32(4), A32(5)), 6);
}
static void v_DrawIndexedPrimitive(void) {         /* (type, fvf, vertices, vcount, indices, icount, flags) */
    DEV(7);
    RET(d3d_device7_DrawIndexedPrimitive(self, A32(1), A32(2), (uintptr_t)COM_P(3), A32(4),
                                         (uintptr_t)COM_P(5), A32(6), A32(7)), 8);
}
static void v_SetClipStatus(void) { DEV(1); RET(d3d_device7_SetClipStatus(self, COM_P(1)), 2); }
static void v_GetClipStatus(void) { DEV(1); RET(d3d_device7_GetClipStatus(self, COM_P(1)), 2); }
static void v_DrawPrimitiveStrided(void) {         /* (type, fvf, D3DDRAWPRIMITIVESTRIDEDDATA*, count, flags) */
    DEV(5);
    d3ddrawprimitivestrideddata_t sd;
    RET(d3d_device7_DrawPrimitiveStrided(self, A32(1), A32(2), (uintptr_t)strided_from_guest(A32(3), &sd),
                                         A32(4), A32(5)), 6);
}
static void v_DrawIndexedPrimitiveStrided(void) {  /* (type, fvf, strided, vcount, indices, icount, flags) */
    DEV(7);
    d3ddrawprimitivestrideddata_t sd;
    RET(d3d_device7_DrawIndexedPrimitiveStrided(self, A32(1), A32(2), (uintptr_t)strided_from_guest(A32(3), &sd),
                                                A32(4), (uintptr_t)COM_P(5), A32(6), A32(7)), 8);
}
static void v_DrawPrimitiveVB(void) {              /* (type, vb, start, count, flags) */
    DEV(5);
    RET(d3d_device7_DrawPrimitiveVB(self, A32(1), (uintptr_t)COM_IN(2, CK_VB7), A32(3), A32(4), A32(5)), 6);
}
static void v_DrawIndexedPrimitiveVB(void) {       /* (type, vb, start, vcount, indices, icount, flags) */
    DEV(7);
    RET(d3d_device7_DrawIndexedPrimitiveVB(self, A32(1), (uintptr_t)COM_IN(2, CK_VB7), A32(3), A32(4),
                                           (uintptr_t)COM_P(5), A32(6), A32(7)), 8);
}
static void v_ComputeSphereVisibility(void) {      /* (centers, radii, n, flags, results) */
    DEV(5);
    RET(d3d_device7_ComputeSphereVisibility(self, (uintptr_t)COM_P(1), (uintptr_t)COM_P(2), A32(3), A32(4),
                                            (uintptr_t)COM_P(5)), 6);
}
static void v_GetTexture(void) {
    DEV(2);
    void *s = NULL;
    uint32_t hr = d3d_device7_GetTexture(self, A32(1), &s);
    com_out(A32(2), hr == DD_OK ? s : NULL, CK_SURFACE7);
    RET(hr, 3);
}
static void v_SetTexture(void) { DEV(2); RET(d3d_device7_SetTexture(self, A32(1), COM_IN(2, CK_SURFACE7)), 3); }
static void v_GetTextureStageState(void) {
    DEV(3);
    RET(d3d_device7_GetTextureStageState(self, A32(1), A32(2), COM_P(3)), 4);
}
static void v_SetTextureStageState(void) {
    DEV(3);
    RET(d3d_device7_SetTextureStageState(self, A32(1), A32(2), A32(3)), 4);
}
static void v_ValidateDevice(void)    { DEV(1); RET(d3d_device7_ValidateDevice(self, COM_P(1)), 2); }
static void v_ApplyStateBlock(void)   { DEV(1); RET(d3d_device7_ApplyStateBlock(self, A32(1)), 2); }
static void v_CaptureStateBlock(void) { DEV(1); RET(d3d_device7_CaptureStateBlock(self, A32(1)), 2); }
static void v_DeleteStateBlock(void)  { DEV(1); RET(d3d_device7_DeleteStateBlock(self, A32(1)), 2); }
static void v_CreateStateBlock(void)  { DEV(2); RET(d3d_device7_CreateStateBlock(self, A32(1), COM_P(2)), 3); }
static void v_Load(void)              { DEV(5); RET(d3d_device7_Load(self, A32(1), A32(2), A32(3), A32(4), A32(5)), 6); }
static void v_LightEnable(void)       { DEV(2); RET(d3d_device7_LightEnable(self, A32(1), A32(2)), 3); }
static void v_GetLightEnable(void)    { DEV(2); RET(d3d_device7_GetLightEnable(self, A32(1), COM_P(2)), 3); }
static void v_SetClipPlane(void)      { DEV(2); RET(d3d_device7_SetClipPlane(self, A32(1), COM_P(2)), 3); }
static void v_GetClipPlane(void)      { DEV(2); RET(d3d_device7_GetClipPlane(self, A32(1), COM_P(2)), 3); }

static const win32hle_shim k_device7[] = {
    { "QueryInterface", v_QueryInterface }, { "AddRef", v_AddRef }, { "Release", v_Release },
    { "GetCaps", v_GetCaps }, { "EnumTextureFormats", v_EnumTextureFormats },
    { "BeginScene", v_BeginScene }, { "EndScene", v_EndScene }, { "GetDirect3D", v_GetDirect3D },
    { "SetRenderTarget", v_SetRenderTarget }, { "GetRenderTarget", v_GetRenderTarget }, { "Clear", v_Clear },
    { "SetTransform", v_SetTransform }, { "GetTransform", v_GetTransform }, { "SetViewport", v_SetViewport },
    { "MultiplyTransform", v_MultiplyTransform }, { "GetViewport", v_GetViewport },
    { "SetMaterial", v_SetMaterial }, { "GetMaterial", v_GetMaterial },
    { "SetLight", v_SetLight }, { "GetLight", v_GetLight },
    { "SetRenderState", v_SetRenderState }, { "GetRenderState", v_GetRenderState },
    { "BeginStateBlock", v_BeginStateBlock }, { "EndStateBlock", v_EndStateBlock }, { "PreLoad", v_PreLoad },
    { "DrawPrimitive", v_DrawPrimitive }, { "DrawIndexedPrimitive", v_DrawIndexedPrimitive },
    { "SetClipStatus", v_SetClipStatus }, { "GetClipStatus", v_GetClipStatus },
    { "DrawPrimitiveStrided", v_DrawPrimitiveStrided },
    { "DrawIndexedPrimitiveStrided", v_DrawIndexedPrimitiveStrided },
    { "DrawPrimitiveVB", v_DrawPrimitiveVB }, { "DrawIndexedPrimitiveVB", v_DrawIndexedPrimitiveVB },
    { "ComputeSphereVisibility", v_ComputeSphereVisibility },
    { "GetTexture", v_GetTexture }, { "SetTexture", v_SetTexture },
    { "GetTextureStageState", v_GetTextureStageState }, { "SetTextureStageState", v_SetTextureStageState },
    { "ValidateDevice", v_ValidateDevice }, { "ApplyStateBlock", v_ApplyStateBlock },
    { "CaptureStateBlock", v_CaptureStateBlock }, { "DeleteStateBlock", v_DeleteStateBlock },
    { "CreateStateBlock", v_CreateStateBlock }, { "Load", v_Load },
    { "LightEnable", v_LightEnable }, { "GetLightEnable", v_GetLightEnable },
    { "SetClipPlane", v_SetClipPlane }, { "GetClipPlane", v_GetClipPlane }, { "GetInfo", com_fail3 },
};

/* ---------------------------------------------------------------- IDirect3DVertexBuffer7 */
static void b_QueryInterface(void) {
    QI(vb7_t, CK_VB7, &IID_IDirect3DVertexBuffer7_, vb7_AddRef, "IDirect3DVertexBuffer7");
}
static void b_AddRef(void)  { COM_SELF(vb7_t, CK_VB7, 0); RET(vb7_AddRef(self), 1); }
static void b_Release(void) { COM_SELF(vb7_t, CK_VB7, 0); RET(vb7_Release(self), 1); }
static void b_Lock(void) {                         /* (flags, LPVOID* data, DWORD* size): data is guest memory */
    COM_SELF(vb7_t, CK_VB7, 3);
    void *data = NULL;
    uint32_t hr = vb7_Lock(self, A32(1), &data, COM_P(3));
    uint32_t *out = COM_P(2);
    if (out) *out = hr == DD_OK ? gfx_guest_va(data) : 0;
    RET(hr, 4);
}
static void b_Unlock(void) { COM_SELF(vb7_t, CK_VB7, 0); RET(vb7_Unlock(self), 1); }
static void b_ProcessVertices(void) {
    COM_SELF(vb7_t, CK_VB7, 7);
    RET(vb7_ProcessVertices(self, A32(1), A32(2), A32(3), A32(4), A32(5), A32(6), A32(7)), 8);
}
static void b_GetVertexBufferDesc(void) { COM_SELF(vb7_t, CK_VB7, 1); RET(vb7_GetVertexBufferDesc(self, COM_P(1)), 2); }
static void b_Optimize(void) { COM_SELF(vb7_t, CK_VB7, 2); RET(vb7_Optimize(self, A32(1), A32(2)), 3); }
static void b_ProcessVerticesStrided(void) {
    COM_SELF(vb7_t, CK_VB7, 7);
    RET(vb7_ProcessVerticesStrided(self, A32(1), A32(2), A32(3), A32(4), A32(5), A32(6), A32(7)), 8);
}
static const win32hle_shim k_vb7[] = {
    { "QueryInterface", b_QueryInterface }, { "AddRef", b_AddRef }, { "Release", b_Release },
    { "Lock", b_Lock }, { "Unlock", b_Unlock }, { "ProcessVertices", b_ProcessVertices },
    { "GetVertexBufferDesc", b_GetVertexBufferDesc }, { "Optimize", b_Optimize },
    { "ProcessVerticesStrided", b_ProcessVerticesStrided },
};

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
void ht2_d3d7_register(void) {
    com_define(CK_D3D7,    "IDirect3D7",             k_d3d7,    N(k_d3d7));
    com_define(CK_DEVICE7, "IDirect3DDevice7",       k_device7, N(k_device7));
    com_define(CK_VB7,     "IDirect3DVertexBuffer7", k_vb7,     N(k_vb7));
}

_Static_assert(N(k_d3d7)    == sizeof(d3d7_vtbl_t) / sizeof(void *),        "IDirect3D7 slots");
_Static_assert(N(k_device7) == sizeof(d3d_device7_vtbl_t) / sizeof(void *), "IDirect3DDevice7 slots");
_Static_assert(N(k_vb7)     == sizeof(vb7_vtbl_t) / sizeof(void *),         "IDirect3DVertexBuffer7 slots");
