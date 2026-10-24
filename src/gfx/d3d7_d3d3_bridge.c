#include "d3d7_internal.h"

#include <stdint.h>

/*
 * IDirect3DDevice3 delegation surface -- see d3d7.h's own header comment
 * for the design ([[project_swep1rcr_port_scoping]]: a standalone, real
 * d3d_device7_t driven directly, never exposed to the game as an actual
 * IDirect3DDevice7 COM object). ddraw.c's D3D3/Viewport3/Light/Material3
 * objects translate D3D3's older wire structs (D3DLIGHT/D3DMATERIAL/
 * D3DVIEWPORT2) into d3d7.h's wire-compatible types and forward every
 * call that has a direct D3D7 equivalent through these thin wrappers --
 * each one just calls the existing, real, already-tested d3d_device7_*
 * function under a public name, no logic duplicated. The vertex/index
 * pointer params on the Draw* wrappers are typed `void *` here for a
 * cleaner public API, but the underlying d3d_device7_Draw* functions
 * take them as raw uint32_t (matching how they're actually received off
 * the real x86 stdcall stack via the generic vtable dispatch machinery),
 * so each wrapper does the same (uintptr_t) cast ddraw.c's own
 * call sites already use elsewhere for this.
 *
 * Split out of d3d7.c purely for readability -- see
 * [[project_phase2_command_buffer]]'s memory entry for the sibling
 * d3d7_cmdbuf.c split and why both needed d3d7_internal.h to exist.
 */
void *d3d7_device_create_standalone(void)
{
    return d3d_device7_create(NULL);
}
uint32_t d3d7_device_release(void *device) { return d3d_device7_Release(device); }
uint32_t d3d7_device_begin_scene(void *device) { return d3d_device7_BeginScene(device); }
uint32_t d3d7_device_end_scene(void *device) { return d3d_device7_EndScene(device); }
uint32_t d3d7_device_set_render_target(void *device, void *surface, uint32_t flags)
{ return d3d_device7_SetRenderTarget(device, surface, flags); }
uint32_t d3d7_device_get_render_target(void *device, void **out_surface)
{ return d3d_device7_GetRenderTarget(device, out_surface); }
uint32_t d3d7_device_enum_texture_formats(void *device, void *cb, void *ctx)
{ return d3d_device7_EnumTextureFormats(device, cb, ctx); }
uint32_t d3d7_device_set_render_state(void *device, uint32_t state, uint32_t value)
{ return d3d_device7_SetRenderState(device, state, value); }
uint32_t d3d7_device_get_render_state(void *device, uint32_t state, uint32_t *out_value)
{ return d3d_device7_GetRenderState(device, state, out_value); }
uint32_t d3d7_device_set_texture_stage_state(void *device, uint32_t stage, uint32_t state, uint32_t value)
{ return d3d_device7_SetTextureStageState(device, stage, state, value); }
uint32_t d3d7_device_get_texture_stage_state(void *device, uint32_t stage, uint32_t state, uint32_t *out_value)
{ return d3d_device7_GetTextureStageState(device, stage, state, out_value); }
uint32_t d3d7_device_set_texture(void *device, uint32_t stage, void *surface)
{ return d3d_device7_SetTexture(device, stage, surface); }
uint32_t d3d7_device_get_texture(void *device, uint32_t stage, void **out_surface)
{ return d3d_device7_GetTexture(device, stage, out_surface); }
uint32_t d3d7_device_set_transform(void *device, uint32_t state, d3dmatrix_t *matrix)
{ return d3d_device7_SetTransform(device, state, matrix); }
uint32_t d3d7_device_get_transform(void *device, uint32_t state, d3dmatrix_t *out_matrix)
{ return d3d_device7_GetTransform(device, state, out_matrix); }
uint32_t d3d7_device_multiply_transform(void *device, uint32_t state, d3dmatrix_t *matrix)
{ return d3d_device7_MultiplyTransform(device, state, matrix); }
uint32_t d3d7_device_set_viewport(void *device, d3dviewport7_t *vp)
{ return d3d_device7_SetViewport(device, vp); }
uint32_t d3d7_device_get_viewport(void *device, d3dviewport7_t *out_vp)
{ return d3d_device7_GetViewport(device, out_vp); }
uint32_t d3d7_device_set_material(void *device, d3dmaterial7_t *mat)
{ return d3d_device7_SetMaterial(device, mat); }
uint32_t d3d7_device_get_material(void *device, d3dmaterial7_t *out_mat)
{ return d3d_device7_GetMaterial(device, out_mat); }
uint32_t d3d7_device_set_light(void *device, uint32_t index, d3dlight7_t *light)
{ return d3d_device7_SetLight(device, index, light); }
uint32_t d3d7_device_get_light(void *device, uint32_t index, d3dlight7_t *out_light)
{ return d3d_device7_GetLight(device, index, out_light); }
uint32_t d3d7_device_light_enable(void *device, uint32_t index, uint32_t enable)
{ return d3d_device7_LightEnable(device, index, enable); }
uint32_t d3d7_device_get_light_enable(void *device, uint32_t index, uint32_t *out_enable)
{ return d3d_device7_GetLightEnable(device, index, out_enable); }
uint32_t d3d7_device_draw_primitive(void *device, uint32_t prim_type, uint32_t fvf,
                                     void *vertices, uint32_t count, uint32_t flags)
{ return d3d_device7_DrawPrimitive(device, prim_type, fvf, (uintptr_t)vertices, count, flags); }
uint32_t d3d7_device_draw_indexed_primitive(void *device, uint32_t prim_type, uint32_t fvf,
                                             void *vertices, uint32_t vertex_count,
                                             void *indices, uint32_t index_count, uint32_t flags)
{
    return d3d_device7_DrawIndexedPrimitive(device, prim_type, fvf, (uintptr_t)vertices, vertex_count,
                                             (uintptr_t)indices, index_count, flags);
}
uint32_t d3d7_device_draw_primitive_strided(void *device, uint32_t prim_type, uint32_t fvf,
                                             void *strided_data, uint32_t count, uint32_t flags)
{
    return d3d_device7_DrawPrimitiveStrided(device, prim_type, fvf, (uintptr_t)strided_data, count, flags);
}
uint32_t d3d7_device_draw_indexed_primitive_strided(void *device, uint32_t prim_type, uint32_t fvf,
                                                     void *strided_data, uint32_t vertex_count,
                                                     void *indices, uint32_t index_count, uint32_t flags)
{
    return d3d_device7_DrawIndexedPrimitiveStrided(device, prim_type, fvf, (uintptr_t)strided_data,
                                                    vertex_count, (uintptr_t)indices, index_count, flags);
}
uint32_t d3d7_device_validate_device(void *device, uint32_t *out_passes)
{ return d3d_device7_ValidateDevice(device, out_passes); }

/* IDirect3DVertexBuffer7's real storage (vb7_t), reused as-is for D3D3's
   own, differently-shaped IDirect3DVertexBuffer tear-off object in
   ddraw.c -- same "wrap the real D3D7 implementation behind D3D3's own
   real vtable" pattern as everything else in this file, see
   [[project_swep1rcr_port_scoping]]. D3DVERTEXBUFFERDESC is identical
   between D3D3 and D3D7 (confirmed against a real MinGW header), so no
   translation is needed beyond building the struct from separate fields
   -- ddraw.c reads the game's own D3DVERTEXBUFFERDESC directly rather
   than sharing this module's internal wire-struct type across the
   ddraw.c/d3d7.c link-cycle boundary. */
void *d3d7_vb_create(uint32_t caps, uint32_t fvf, uint32_t num_vertices)
{
    d3dvertexbufferdesc_t desc = { sizeof(desc), caps, fvf, num_vertices };
    return vb7_create(&desc);
}
uint32_t d3d7_vb_addref(void *vb) { return vb7_AddRef(vb); }
uint32_t d3d7_vb_release(void *vb) { return vb7_Release(vb); }
uint32_t d3d7_vb_lock(void *vb, uint32_t flags, void **out_data, uint32_t *out_size)
{ return vb7_Lock(vb, flags, out_data, out_size); }
uint32_t d3d7_vb_unlock(void *vb) { return vb7_Unlock(vb); }
uint32_t d3d7_vb_get_desc(void *vb, uint32_t *out_caps, uint32_t *out_fvf, uint32_t *out_num_vertices)
{
    d3dvertexbufferdesc_t desc;
    desc.size = sizeof(desc);
    uint32_t hr = vb7_GetVertexBufferDesc(vb, &desc);
    if (hr == 0 /* DD_OK */) {
        if (out_caps != NULL) *out_caps = desc.caps;
        if (out_fvf != NULL) *out_fvf = desc.fvf;
        if (out_num_vertices != NULL) *out_num_vertices = desc.num_vertices;
    }
    return hr;
}
uint32_t d3d7_device_draw_primitive_vb(void *device, uint32_t prim_type, void *vb,
                                        uint32_t start_vertex, uint32_t count, uint32_t flags)
{ return d3d_device7_DrawPrimitiveVB(device, prim_type, (uintptr_t)vb, start_vertex, count, flags); }
uint32_t d3d7_device_draw_indexed_primitive_vb(void *device, uint32_t prim_type, void *vb,
                                                void *indices, uint32_t index_count, uint32_t flags)
{
    return d3d_device7_DrawIndexedPrimitiveVB3(device, prim_type, (uintptr_t)vb,
                                                (uintptr_t)indices, index_count, flags);
}
