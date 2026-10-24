#ifndef LONGROAD2_SHIM_D3D7_H
#define LONGROAD2_SHIM_D3D7_H

/*
 * d3d7 shim: real GLES3.2 fixed-function rendering backend, not a stub.
 * GOAL.md/AGENT.md's original "Finding 1" (D3D7 never actually driven --
 * a WINEDEBUG trace that never reached real gameplay) was retracted once
 * live tracing showed 312,615 DrawPrimitive + 286,232 DrawIndexedPrimitive
 * calls in one gameplay session. D3D7 is this game's real 3D-world
 * renderer; DirectDraw Blt (src/shim/ddraw/ddraw.c) is the HUD/2D
 * compositor.
 *
 * BeginScene/Clear/DrawPrimitive/DrawIndexedPrimitive genuinely
 * transform (world/view/projection, including pre-transformed XYZRHW
 * vertices), light (per-vertex D3DLIGHT7 against D3DMATERIAL7), texture
 * (SetTexture's bound surfaces sampled through a real D3DTOP_* / D3DTA_*
 * texture-stage-state combiner), and rasterize (all primitive
 * topologies, real Z-buffer, CULLMODE culling) into the bound render
 * target's GL framebuffer -- see d3d7.c's own file header and
 * d3d7_surface_interop_t below for how that stays interoperable with
 * plain DirectDraw Blt/Lock on the same surfaces.
 *
 * Confirmed call list: docs/api-surface/d3d7.md
 *
 * NOTE: D3D7 has zero PE imports of its own -- IDirect3D7 is obtained via
 * IDirectDraw7::QueryInterface at runtime, not a "D3D7.dll" import. So
 * shim_d3d7_lookup() is never reached through the DLL-name dispatch in
 * src/loader/shim_registry.c; d3d7_create() below is what ddraw.c's own
 * QueryInterface calls directly to hand out a real (COM-correct, and now
 * actually rendering) IDirect3D7 object.
 */

#include "shim_common.h"
#include <stdint.h>

shim_fn_t shim_d3d7_lookup(const char *name, uint32_t ordinal);

/* Returns a new IDirect3D7 object (refcount already at 1, as
   QueryInterface's contract requires) with a real vtable but no actual
   rendering behind any method. */
void *d3d7_create(void);

/* Phase 3 (rendering-architecture plan): invalidates the draw-time
   uniform-upload dedup cache in d3d7.c's d3d_draw_prepare -- must be
   called anywhere gl_state_reset() (glstate/gl_state_cache.h) already
   is at a test's device-group boundary, for the same test-isolation
   reason. Deliberately NOT called from render_init (src/render/render.c)
   -- render doesn't link shim_d3d7 (see render_smoke_test's own
   link line, which only pulls in render+m), and doesn't need to: the
   cache's own `valid` flag is a plain static global, correctly
   zero-initialized to "invalid" at process start with no explicit
   reset required for a real single-session deployment. */
void d3d7_draw_uniform_cache_reset(void);

/* Real, confirmed-live bug fix: d3d_device7_BeginScene's own implicit
   Clear(D3DCLEAR_ZBUFFER) (added for a genuine, separate gap -- this
   game never calls Clear() itself, so the depth buffer needs SOME
   periodic reset or it accumulates stale values across the whole
   session, see BeginScene's own comment) was firing on EVERY BeginScene
   call, but a real trace confirmed most of this game's own frames issue
   TWO BeginScene/EndScene passes before one Flip (~71% of frames, e.g.
   world+vehicle geometry in pass 1, then a driver/character model in
   pass 2) -- the second pass's own implicit clear wiped out the first
   pass's real depth values before the second pass's geometry could ever
   test against them, so second-pass content always won the depth test
   regardless of true depth (live-reported: a driver model rendering in
   front of its own pod's cockpit geometry it should be partly occluded
   by). Real hardware only ever gave a fresh depth buffer once per
   PRESENTED frame, not once per scene -- this must match that
   granularity, called once per real Flip() (ddraw.c), not from
   anywhere in d3d7.c itself (mirrors d3d7_draw_uniform_cache_reset's
   own cross-module-boundary shape immediately above). */
void d3d7_reset_frame_depth_clear(void);

/* Phase 4 (state-sorted merging) test hooks: g_d3d7_perf.gl_draws (real
   glDrawArrays/glDrawElements calls actually issued) only normally gets
   read/reset by d3d7_perf_report_for_frame below, called once per real
   Flip() -- useless for a test that wants "exactly how many real GL
   draws did THIS one EndScene issue". These bypass that entirely: reset
   right before a scene, read the delta right after. */
void d3d7_perf_reset_for_test(void);
long d3d7_perf_get_gl_draws_for_test(void);

/* Per-vtable-method call-count test hook for the 9 state-setter methods
   summed into d3d7_perf_frame_report_t.state_call_count/_ms above -- see
   d3d7.c's own g_d3d7_perf comment. Values match array indices d3d7.c
   uses internally; kept here (not a d3d7_call_id_t enum) so tests don't
   need to reach into a private header for 9 stable small integers. */
enum {
    D3D7_TEST_CALL_SETRENDERSTATE = 0,
    D3D7_TEST_CALL_SETTEXTURESTAGESTATE,
    D3D7_TEST_CALL_SETTRANSFORM,
    D3D7_TEST_CALL_SETVIEWPORT,
    D3D7_TEST_CALL_SETMATERIAL,
    D3D7_TEST_CALL_SETLIGHT,
    D3D7_TEST_CALL_LIGHTENABLE,
    D3D7_TEST_CALL_SETTEXTURE,
    D3D7_TEST_CALL_MULTIPLYTRANSFORM,
    /* Draw-call record/dispatch funnel points (all 6 Draw*-family vtable
       methods route through exactly one of these two) -- a real
       BOX86_DYNAREC_LOG=2 capture (king.exe, ~1.3M lines) showed these two
       specific functions account for ~100% of ALL dynarec blocks executed
       across the entire session (87% + 13%), dwarfing every other
       instrumented bucket including the 9 state-setters above (which
       measured cheap: ~4ms/frame for ~2400 calls). Kept in a separate
       report field (draw_dispatch_ms/_count, not folded into
       state_call_ms/_count) specifically so that comparison stays visible
       in one capture rather than being averaged away. */
    D3D7_TEST_CALL_DRAWCOMMON,
    D3D7_TEST_CALL_DRAWCOMMON_STRIDED,
};
long d3d7_perf_get_call_count_for_test(int call_id);
long d3d7_perf_get_cmdbuf_flush_calls_for_test(void);

/* Shader-variant lookup cache (d3d7_shader.c) test hooks: same
   call/skip-counting idiom as gl_state_cache.c's vertex-attrib counters
   above, exposed through this public header since ensure_shader_variant
   itself lives in d3d7_shader.h, a header private to this module (see
   its own top-of-file comment) that tests never include directly. */
long d3d7_shader_get_variant_lookup_calls_for_test(void);
long d3d7_shader_get_variant_lookup_skips_for_test(void);
void d3d7_shader_reset_variant_lookup_counters_for_test(void);

/* emit_stage_glsl's duplicate color/alpha-formula merge (d3d7_shader.c,
   [[project_shader_variant_grooming_followon]]) test hook -- same
   reasoning/pattern as the counters above. */
long d3d7_shader_get_stage_merge_count_for_test(void);
void d3d7_shader_reset_stage_merge_count_for_test(void);

/* Native quicksort test hook (d3d7_cmdbuf.c, [[project_cmdbuf_flush_perf_investigation]]):
   ptr_sort itself is generic (a plain void** array, no d3d_device7_t
   dependency), so it's tested directly here rather than only indirectly
   through a full device/scene -- order_sort's own correctness (same
   algorithm, but keyed through render_cmd_t state) is covered by the
   existing pixel-identical d3d7_blt_interleave_test.c/present_flip_check
   regression tests, which exercise the real reordering path end to end. */
void d3d7_cmdbuf_ptr_sort_for_test(void **a, uint32_t n);

/* LONGROAD2_PERF_TRACE per-frame report, pulled by ddraw.c's Flip() so
   3D-scene stats and ddraw's own Blt/present/sync stats print as ONE
   correlated line per real displayed frame, instead of two independently-
   cadenced trace streams. d3d_device7_EndScene used to glFinish()+print+
   reset g_d3d7_perf on ITS OWN, every single EndScene call -- fine for a
   game that calls EndScene once per frame, but this project's own trace
   captures found some scenes call it several times per one real Flip(),
   which meant several full GPU-pipeline-drain stalls (glFinish blocks
   ALL pending GPU work, not just this scene's) every displayed frame
   whenever this flag was on, and several disjoint, hard-to-correlate
   report lines to read. EndScene now only accumulates; this function is
   the sole place that still calls glFinish() and resets the counters,
   letting ddraw.c fold the result into its own per-frame line. No-ops
   (leaves *out zeroed) when LONGROAD2_PERF_TRACE isn't set, so calling
   it unconditionally from Flip() costs nothing when tracing is off. */
typedef struct {
    long draw_calls, vertices, indices, uniform_uploads, uniform_skips, gl_draws;
    double gpu_finish_ms;
    /* Fixed-function state-setter call volume + in-function wall time --
       see d3d7.c's own g_d3d7_perf comment for why these 9 specifically
       (every other frequently-called vtable method already flows into
       the counters above). Aggregated as one total rather than 9 separate
       named pairs: the per-frame trace line is already long, and the
       question these answer ("is call VOLUME or in-function TIME the
       cost") only needs the sum -- a breakdown by individual method is
       available via d3d7_perf_get_call_count_for_test in a real capture
       if this total ever looks large enough to be worth splitting out. */
    long state_call_count;
    double state_call_ms;
    /* d3d_draw_common/d3d_draw_common_strided (the record-vs-execute-now
       funnel every Draw*-family vtable method routes through) -- see
       d3d7.h's D3D7_TEST_CALL_DRAWCOMMON* comment for why this is a
       separate field from state_call_ms/_count above. */
    long draw_dispatch_count;
    double draw_dispatch_ms;
    /* cmdbuf_flush's own full wall time (build_order's qsort + the
       merge/replay loop) -- see d3d7_internal.h's own comment on
       d3d7_perf_record_cmdbuf_flush for why this is separate from
       draw_dispatch_ms/state_call_ms above. */
    long cmdbuf_flush_count;
    double cmdbuf_flush_ms;
} d3d7_perf_frame_report_t;
void d3d7_perf_report_for_frame(d3d7_perf_frame_report_t *out);

/* Real D3D7 wire-compatible POD types (byte-exact vs. the real
   D3DVECTOR/D3DCOLORVALUE/D3DMATRIX/D3DLIGHT7/D3DVIEWPORT7 structs --
   confirmed via compiled sizeof/offsetof probes against real Wine/MinGW
   headers, not eyeballed). Shared with ddraw.c's DX6-era IDirect3DDevice3/
   IDirect3DViewport3/IDirect3DLight/IDirect3DMaterial3 delegation layer
   below -- D3D3 and D3D7 are genuinely the same fixed-function rendering
   pipeline underneath, reached through an older, more indirect COM
   surface in D3D3's case (see [[project_swep1rcr_port_scoping]]). Packed
   to guarantee this layout regardless of platform default alignment,
   matching d3d7.c's own convention for wire-compatible structs. */
#pragma pack(push, 1)
typedef struct { float x, y, z; } d3dvector_t;
typedef struct { float r, g, b, a; } d3dcolorvalue_t;
typedef struct { float m[16]; } d3dmatrix_t; /* _11.._14, _21.._24, ... row-major */

typedef struct {
    d3dcolorvalue_t diffuse, ambient, specular, emissive;
    float power;
} d3dmaterial7_t;

typedef struct {
    uint32_t type; /* D3DLIGHTTYPE */
    d3dcolorvalue_t diffuse, specular, ambient;
    d3dvector_t position, direction;
    float range, falloff;
    float attenuation0, attenuation1, attenuation2;
    float theta, phi;
} d3dlight7_t;

typedef struct {
    uint32_t x, y, width, height;
    float min_z, max_z;
} d3dviewport7_t;
#pragma pack(pop)

/*
 * IDirect3DDevice3 delegation surface -- a standalone, real d3d_device7_t
 * driven directly (never exposed to the game as an actual IDirect3DDevice7
 * COM object; d3d7_owner is NULL and only ever read back verbatim by
 * GetDirect3D, which nothing here calls). ddraw.c's D3D3/Viewport3/Light/
 * Material3 objects translate D3D3's older wire structs (D3DLIGHT/
 * D3DMATERIAL/D3DVIEWPORT2) into the types above and forward every call
 * that has a direct D3D7 equivalent through these thin wrappers -- each
 * one just calls the existing, real, already-tested d3d_device7_* static
 * function under a public name, no logic duplicated. See
 * [[project_swep1rcr_port_scoping]].
 */
void *d3d7_device_create_standalone(void);
uint32_t d3d7_device_release(void *device);
uint32_t d3d7_device_begin_scene(void *device);
uint32_t d3d7_device_end_scene(void *device);
uint32_t d3d7_device_set_render_target(void *device, void *surface, uint32_t flags);
uint32_t d3d7_device_get_render_target(void *device, void **out_surface);
uint32_t d3d7_device_enum_texture_formats(void *device, void *cb, void *ctx);

/* Real IDirect3DVertexBuffer7 storage, reused for D3D3's own
   IDirect3DVertexBuffer object (see [[project_swep1rcr_port_scoping]]).
   Every function here takes/returns opaque void* handles (the real vb7_t
   type stays private to the d3d7 module, same as d3d_device7_t). */
void *d3d7_vb_create(uint32_t caps, uint32_t fvf, uint32_t num_vertices);
uint32_t d3d7_vb_addref(void *vb);
uint32_t d3d7_vb_release(void *vb);
uint32_t d3d7_vb_lock(void *vb, uint32_t flags, void **out_data, uint32_t *out_size);
uint32_t d3d7_vb_unlock(void *vb);
uint32_t d3d7_vb_get_desc(void *vb, uint32_t *out_caps, uint32_t *out_fvf, uint32_t *out_num_vertices);
uint32_t d3d7_device_draw_primitive_vb(void *device, uint32_t prim_type, void *vb,
                                        uint32_t start_vertex, uint32_t count, uint32_t flags);
uint32_t d3d7_device_draw_indexed_primitive_vb(void *device, uint32_t prim_type, void *vb,
                                                void *indices, uint32_t index_count, uint32_t flags);
uint32_t d3d7_device_set_render_state(void *device, uint32_t state, uint32_t value);
uint32_t d3d7_device_get_render_state(void *device, uint32_t state, uint32_t *out_value);
uint32_t d3d7_device_set_texture_stage_state(void *device, uint32_t stage, uint32_t state, uint32_t value);
uint32_t d3d7_device_get_texture_stage_state(void *device, uint32_t stage, uint32_t state, uint32_t *out_value);
uint32_t d3d7_device_set_texture(void *device, uint32_t stage, void *surface);
uint32_t d3d7_device_get_texture(void *device, uint32_t stage, void **out_surface);
uint32_t d3d7_device_set_transform(void *device, uint32_t state, d3dmatrix_t *matrix);
uint32_t d3d7_device_get_transform(void *device, uint32_t state, d3dmatrix_t *out_matrix);
uint32_t d3d7_device_multiply_transform(void *device, uint32_t state, d3dmatrix_t *matrix);
uint32_t d3d7_device_set_viewport(void *device, d3dviewport7_t *vp);
uint32_t d3d7_device_get_viewport(void *device, d3dviewport7_t *out_vp);
uint32_t d3d7_device_set_material(void *device, d3dmaterial7_t *mat);
uint32_t d3d7_device_get_material(void *device, d3dmaterial7_t *out_mat);
uint32_t d3d7_device_set_light(void *device, uint32_t index, d3dlight7_t *light);
uint32_t d3d7_device_get_light(void *device, uint32_t index, d3dlight7_t *out_light);
uint32_t d3d7_device_light_enable(void *device, uint32_t index, uint32_t enable);
uint32_t d3d7_device_get_light_enable(void *device, uint32_t index, uint32_t *out_enable);
uint32_t d3d7_device_draw_primitive(void *device, uint32_t prim_type, uint32_t fvf,
                                     void *vertices, uint32_t count, uint32_t flags);
uint32_t d3d7_device_draw_indexed_primitive(void *device, uint32_t prim_type, uint32_t fvf,
                                             void *vertices, uint32_t vertex_count,
                                             void *indices, uint32_t index_count, uint32_t flags);
uint32_t d3d7_device_draw_primitive_strided(void *device, uint32_t prim_type, uint32_t fvf,
                                             void *strided_data, uint32_t count, uint32_t flags);
uint32_t d3d7_device_draw_indexed_primitive_strided(void *device, uint32_t prim_type, uint32_t fvf,
                                                     void *strided_data, uint32_t vertex_count,
                                                     void *indices, uint32_t index_count, uint32_t flags);
uint32_t d3d7_device_validate_device(void *device, uint32_t *out_passes);

/*
 * GL rendering backend interop with src/shim/ddraw/ddraw.c's surfaces.
 *
 * shim_ddraw already links shim_d3d7 (ddraw.c creates D3D7 objects via
 * d3d7_create() above), so d3d7.c calling directly into ddraw.c would
 * create a link cycle. Dependency injection instead: ddraw.c owns the
 * real surface7_t layout and its GL resources (gl_target_t, gpu/cpu
 * dirty flags) privately, and hands d3d7.c a small function-pointer
 * table it can call through generically -- same "opaque COM neighbor"
 * boundary already used for AddRef/Release (com_addref/com_release in
 * d3d7.c), just for a project-private, non-COM interface.
 */
typedef struct {
    /* Ensures the surface (an IDirectDrawSurface7*, opaque here) has a
       GL framebuffer sized to match it, binds it as the current render
       target, and reports its pixel dimensions. Returns nonzero on
       success (0 if surface is NULL/unrecognized or GL isn't ready). */
    unsigned (*bind_render_target)(void *surface, int *out_w, int *out_h);

    /* Marks the surface's GL-rendered contents as newer than its CPU
       buffer -- ddraw.c lazily reads them back (glReadPixels) the next
       time anything needs the CPU buffer (Lock/Blt/Flip/present). */
    void (*mark_gpu_dirty)(void *surface);

    /* Ensures the surface has a GL texture matching its current CPU
       pixel content (uploading it if the surface was written to since
       the last upload -- see ddraw.c's surface_sync_t/sync_state) and
       reports its dimensions. Used by SetTexture-bound surfaces as
       texture-stage sampler sources. Returns 0 (texture id 0 is never
       valid) if the surface is NULL/unrecognized or GL isn't ready. */
    unsigned (*get_texture)(void *surface, int *out_w, int *out_h);

    /* Reports whether the surface has a real SetColorKey(DDCKEY_SRCBLT)
       value set (ddraw.c already honors this for plain 2D Blt/BltFast --
       this is what lets D3DRENDERSTATE_COLORKEYENABLE honor the SAME key
       for 3D texturing). When set, out_rgba_low/high are filled with
       that key's low/high raw pixel values converted to RGBA bytes via
       the surface's own pixel format (the same conversion its GL texture
       upload already applies), so they're directly comparable against a
       texel sampled from that same GL texture. Returns 0 (no key set, or
       surface NULL/unrecognized) or 1 (out params filled). */
    int (*get_color_key)(void *surface, uint8_t out_rgba_low[4], uint8_t out_rgba_high[4]);
} d3d7_surface_interop_t;

/* Called once by ddraw.c (from DirectDrawCreateEx) to install the table
   above. NULL until then -- every d3d7.c rendering entry point degrades
   to its prior no-op behavior if no interop is registered, so tests that
   drive d3d7.c's vtables without a real ddraw/GL environment (see
   tests/d3d7_test.c) keep working unchanged. */
void d3d7_set_surface_interop(const d3d7_surface_interop_t *interop);

#endif /* LONGROAD2_SHIM_D3D7_H */
