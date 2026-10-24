#include "d3d7.h"
#include "gfx_mem.h"

#include "d3d7_internal.h"
#include "d3d7_shader.h"
#include "activity_sampler.h"

#include "gl_state_cache.h"
#include "gl_thread.h"
#include <GLES2/gl2.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ddraw_types.h"

/*
 * Real IDirect3D7/IDirect3DDevice7/IDirect3DVertexBuffer7 vtable layouts
 * (cross-checked against /usr/include/wine-staging-11.13/wine/windows/
 * {d3d.h,d3dtypes.h,d3dcaps.h}).
 *
 * STATUS: GOAL.md/AGENT.md's original "Finding 1" (a WINEDEBUG trace
 * showing zero DrawPrimitive/BeginScene/etc. calls, concluding D3D7 is
 * inert) was captured from a session that never reached real driving
 * gameplay and has been retracted -- see GOAL.md's Finding 1 section for
 * the full story. Live tracing of this exact file, once this loader
 * could actually reach gameplay, showed 312,615 DrawPrimitive + 286,232
 * DrawIndexedPrimitive calls and ~2,930 BeginScene/EndScene pairs in one
 * session, overwhelmingly real triangle geometry (fan/strip/list) with
 * FVF vertex formats carrying position+normal+texcoord+diffuse. D3D7 is
 * the real 3D-world renderer; DirectDraw Blt (src/shim/ddraw/ddraw.c) is
 * the HUD/2D compositor, not the driving view.
 *
 * Every COM method below is a real, stateful implementation (Set* / Get*
 * round-trip correctly, ref-counting is real, vertex buffers are real
 * heap allocations, state blocks really capture/restore, EnumTextureFormats
 * reports a broad realistic format set) -- this correctly satisfies the
 * game's own state-machine/capability-negotiation expectations and gets
 * it past setup without crashing.
 *
 * REAL GLES3.2 RENDERING BACKEND (see "vertex/rasterizer stage" below):
 * Clear/BeginScene/DrawPrimitive/DrawIndexedPrimitive genuinely
 * transform, light, and rasterize into the bound render target's GL
 * framebuffer, synced back into the owning DirectDraw surface's CPU
 * buffer via d3d7_surface_interop_t (see d3d7.h) -- surfaces stay
 * interoperable with plain DirectDraw Blt/Lock either way. One "uber
 * shader" GLSL ES 100 program (not per-state-combination shader
 * permutations -- see d7vk's own D3D9 fixed-function shaders, which take
 * the same approach) implements:
 *   - transform: world/view/projection matrices, including the
 *     pre-transformed XYZRHW vertex case (Milestone B)
 *   - rasterize: all D3DPRIMITIVETYPE topologies, real Z-buffer
 *     depth-test/write/func, CULLMODE-driven face culling (Milestone B)
 *   - lighting: per-vertex (Gouraud) D3DLIGHT7 point/spot/directional
 *     lights against D3DMATERIAL7, computed in view space (Milestone C)
 *   - texturing: SetTexture's bound surfaces are sampled through a real
 *     D3DTOP_* / D3DTA_* texture-stage-state combiner, one UV set
 *     (Milestone D) -- see "texture-stage-state combiner" below for the
 *     exact D3DTEXTUREOP coverage and its documented scope cuts
 *     (D3DTA_SPECULAR approximation, no bump-mapped texcoord offset).
 * DrawPrimitiveVB/DrawIndexedPrimitiveVB draw from a real vb7_t vertex
 * buffer through this same rasterizer. DrawPrimitiveStrided/DrawIndexed-
 * PrimitiveStrided draw from a real D3DDRAWPRIMITIVESTRIDEDDATA (see
 * d3d_draw_common_strided) -- confirmed via real gameplay trace to be
 * THIS game's actual path for its static meshes (truck cabin, 3rd-
 * person model, skybox, buildings: 129,483 + 202,724 calls in one
 * session), not the *VB path above (0 CreateVertexBuffer calls in that
 * same session -- an earlier "the cabin needs DrawPrimitiveVB" theory,
 * reasonable at the time, that real data then ruled out; *VB is still
 * correctly implemented, just apparently unused by this particular
 * game). Neither *VB nor *Strided are safe to alias to the plain draw
 * calls above -- see each one's own definition for why (a COM vertex-
 * buffer pointer vs. a small per-attribute {pointer,stride} struct,
 * neither a flat FVF vertex array).
 *
 * Code below is organized in stages mirroring a real GPU pipeline's
 * concerns (transform / lighting / texture-stage / rasterizer-state /
 * output-merger).
 */

/* LONGROAD2_DDRAW_TRACE (same var as ddraw.c's own copy): opt-in trace
   of render-target/device lifecycle and per-draw state, used throughout
   this project's real-gameplay investigations. */
/* Unset OR set to "0" both mean off, any other value means on --
   getenv() != NULL alone treats VAR=0 the same as VAR=1, since both
   merely SET the variable, which does not match a user reasonably
   expecting VAR=0 to mean "disabled" (a more common shell convention
   than "unset it to disable"). Same fix as ddraw.c's own copy of this
   pattern -- duplicated here (not shared) since d3d7.c and ddraw.c are
   separate translation units. */
int env_flag_enabled(const char *name)
{
    const char *v = getenv(name);
    return v != NULL && strcmp(v, "0") != 0;
}

static int ddraw_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = env_flag_enabled("LONGROAD2_DDRAW_TRACE");
    return enabled;
}

/* TEMP DIAGNOSTIC, reuses the same LONGROAD2_PERF_TRACE flag ddraw.c's own
   per-frame timing summary uses (see that file's own header comment) --
   investigating a live RK3326/Mali-G31/box86 report of ~50ms/frame that
   turned out NOT to be vsync (LONGROAD2_NO_VSYNC=1 made no difference) and
   only marginally the present/Blt draw calls themselves (converting them
   off client-side vertex arrays barely moved the number). The user's own
   sharp catch: game_loader was only using ~60% of one CPU core the whole
   time -- if box86 were burning CPU emulating a slow SDL2/EGL call, that
   should show up as much closer to 100%. 60% is far more consistent with
   the CPU being genuinely BLOCKED (idle, not computing) for a real chunk
   of every frame -- and the most ordinary reason a present/swap call
   blocks is that the driver has to wait for previously-issued GPU work
   (this frame's whole D3D7 3D scene, dispatched asynchronously across
   many draw calls earlier in the frame) to actually finish rendering
   before it can flip buffers. ddraw.c's own perf trace has never
   separated "how long did it take to ISSUE this frame's draw calls" from
   "how long did the GPU actually take to render them" -- the two look
   identical from the CPU's point of view until something forces a real
   wait, which normally only happens at present/swap time, attributing
   the GPU's ENTIRE backlog to whatever call happened to be the one that
   finally had to wait for it. glFinish() right here, at EndScene (the
   real end of this frame's issued 3D work, before any ddraw HUD Blits or
   the present/swap itself), isolates that GPU completion cost as its own
   number -- if this is where the ~50ms actually is, it directly confirms
   the D3D7->GLES3 draw/shader workload itself (not the present/swap
   mechanism, not box86's SDL2 wrapping) is the real bottleneck. */
int d3d7_perf_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = env_flag_enabled("LONGROAD2_PERF_TRACE");
    return enabled;
}

double d3d7_perf_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Per-scene draw-call/vertex/index volume, reset alongside the
   EndScene GPU-finish timer above -- printed on the SAME line so a
   LONGROAD2_PERF_TRACE capture directly correlates "how much did this
   scene ask the GPU to do" with "how long did the GPU actually take,"
   rather than needing two separate captures to line those up by eye.
   `vertices` counts non-indexed draws (DrawPrimitive/Strided/VB's own
   vertex-count param); `indices` counts indexed draws' index-count param
   instead, since that's the real per-vertex-shader-invocation work an
   indexed draw asks for, not the (usually much smaller) vertex pool
   size it indexes into. */
/* Per-vtable-method call count + in-function wall time, for the fixed-
   function state-setter methods a real D3D7 game calls very frequently
   (often many times per draw, e.g. re-specifying every texture stage
   state before each different material) but which none of the existing
   counters above ever measured -- Clear/DrawPrimitive-family/BeginScene/
   EndScene already flow into draw_calls/gl_draws/gpu_finish_ms, these
   9 never had ANY instrumentation before. Added specifically to test the
   user's box86-call-boundary-overhead hypothesis after [[cs_enter/wfso]]
   (see docs/../kernel32 perf work) ruled out mutex/wait contention as the
   ~120ms/frame unaccounted cost: if call_ms stays tiny while call_counts
   are in the thousands/frame, that's real evidence the cost is box86's
   own per-call x86<->native marshaling overhead (paid on every vtable
   crossing, invisible to a timer that only wraps the native function
   body) rather than genuine work happening inside these functions. */
/* Indices match d3d7.h's D3D7_TEST_CALL_* enum exactly (single source of
   truth for tests); D3D7_CALL_COUNT itself stays private since tests only
   ever need the named constants, never the count. */
#define D3D7_CALL_COUNT (D3D7_TEST_CALL_DRAWCOMMON_STRIDED + 1)

typedef struct {
    long draw_calls;
    long vertices;
    long indices;
    long uniform_uploads; /* Phase 3: draws that had to re-upload lighting/alpha/fog/clip/transform uniforms */
    long uniform_skips;   /* Phase 3: draws that reused the previous draw's already-uploaded uniforms */
    long gl_draws;         /* Phase 3b: real glDrawArrays/glDrawElements calls actually issued -- vs. draw_calls
                               above (what the game issued at the D3D API level); a merged run collapses several
                               draw_calls into one gl_draws */
    long call_counts[D3D7_CALL_COUNT];
    double call_ms[D3D7_CALL_COUNT];
    long cmdbuf_flush_calls;   /* see d3d7_perf_record_cmdbuf_flush's own comment (d3d7_internal.h) */
    double cmdbuf_flush_ms;
} d3d7_perf_accum_t;
static d3d7_perf_accum_t g_d3d7_perf;

void d3d7_perf_record_cmdbuf_flush(double ms)
{
    if (!d3d7_perf_trace_enabled()) return;
    g_d3d7_perf.cmdbuf_flush_calls++;
    g_d3d7_perf.cmdbuf_flush_ms += ms;
}

/* TEMP DIAGNOSTIC (same informal, print-only shape as d3d7_cmdbuf.c's own
   [cmdbuf-sort-diag] line, not a permanent counter/test) -- splits
   draw_flat_execute_now/draw_strided_execute_now's own replay-time cost
   (called directly from cmdbuf_flush's merge/replay loop, NOT through
   d3d_draw_common's own draw_dispatch wrapper, which only covers the
   cheap record-time path) into 4 phases, to find out which part of the
   ~122us/draw real capture showed is the actual cost, now that a real
   LONGROAD2_CMDBUF_NO_REORDER=1 capture proved the sort itself is
   negligible (~0.01ms for ~500 elements) -- see
   [[project_cmdbuf_flush_perf_investigation]]. */
typedef struct {
    double prepare_ms;   /* d3d_draw_prepare: render-state/viewport apply, texture-stage bind, FBO setup */
    double upload_ms;    /* upload_stream_vertices/_indices (+ strided's own repack into scratch first) */
    double attrib_ms;    /* the up to 5 gl_state_set_vertex_attrib_array/_disabled calls */
    double dispatch_ms;  /* upload_stream_indices (when present) + d3d_draw_dispatch_and_cleanup:
                             the real glDraw*() call + cleanup -- index upload folds in here (it
                             happens immediately before dispatch in both draw_flat_execute_now/
                             draw_strided_execute_now) rather than a 5th bucket */
    long draw_count;
} replay_diag_accum_t;
static replay_diag_accum_t g_replay_diag;

void d3d7_replay_diag_report_and_reset(double *prepare_ms, double *upload_ms, double *attrib_ms,
                                        double *dispatch_ms, long *draw_count)
{
    *prepare_ms = g_replay_diag.prepare_ms;
    *upload_ms = g_replay_diag.upload_ms;
    *attrib_ms = g_replay_diag.attrib_ms;
    *dispatch_ms = g_replay_diag.dispatch_ms;
    *draw_count = g_replay_diag.draw_count;
    memset(&g_replay_diag, 0, sizeof(g_replay_diag));
}

/* Cheap even when trace is on (one extra clock_gettime + array write per
   call): matches every other per-call-site perf helper in this codebase
   (kernel32's stub_EnterCriticalSection, e.g.) rather than adding a new
   dispatch mechanism. Zero cost when trace is off, same as the rest of
   g_d3d7_perf. */
static double d3d7_call_perf_begin(int id)
{
    /* Both DRAWCOMMON ids map to ACTIVITY_DRAW_DISPATCH, every other id
       (the 9 state setters) maps to ACTIVITY_STATE_CALLS -- see
       activity_sampler.h's own enum comment. */
    activity_sampler_enter(id == D3D7_TEST_CALL_DRAWCOMMON || id == D3D7_TEST_CALL_DRAWCOMMON_STRIDED
                            ? ACTIVITY_DRAW_DISPATCH : ACTIVITY_STATE_CALLS);
    return d3d7_perf_trace_enabled() ? d3d7_perf_now_ms() : 0.0;
}
static void d3d7_call_perf_end(int id, double t0)
{
    activity_sampler_leave();
    if (!d3d7_perf_trace_enabled()) return;
    g_d3d7_perf.call_counts[id]++;
    g_d3d7_perf.call_ms[id] += d3d7_perf_now_ms() - t0;
}

/* Phase 4 test hooks -- see the declaration comment in d3d7.h. g_d3d7_perf
   is now exclusively touched by the dedicated GL thread (every draw-
   dispatch/perf-report call site that increments it is wrapped, see
   [[project_gl_issuer_thread]]); with EndScene's own real GL replay now
   ASYNC (gl_thread_post, not gl_thread_call -- see cmdbuf_flush in
   d3d7_cmdbuf.c), a test calling one of these getters right after
   EndScene with no OTHER intervening real GL-thread dispatch (e.g. no
   Lock()/Flip() of its own, which would already provide this for free via
   gl_thread_call's own FIFO ordering) could otherwise read these before
   the replay it's checking for has actually run. gl_thread_wait_idle()
   is a real drain barrier, not a poll -- see its own comment. */
void d3d7_perf_reset_for_test(void)
{
    gl_thread_wait_idle();
    memset(&g_d3d7_perf, 0, sizeof(g_d3d7_perf));
}
long d3d7_perf_get_gl_draws_for_test(void)
{
    gl_thread_wait_idle();
    return g_d3d7_perf.gl_draws;
}
long d3d7_perf_get_call_count_for_test(int call_id)
{
    gl_thread_wait_idle();
    return (call_id >= 0 && call_id < D3D7_CALL_COUNT) ? g_d3d7_perf.call_counts[call_id] : 0;
}
long d3d7_perf_get_cmdbuf_flush_calls_for_test(void)
{
    gl_thread_wait_idle();
    return g_d3d7_perf.cmdbuf_flush_calls;
}

static uint32_t WINAPI fail3(void *t, uint32_t a, uint32_t b, uint32_t c)
{ (void)t; (void)a; (void)b; (void)c; fprintf(stderr, "[d3d7] unimplemented 3-arg method called\n"); return DDERR_GENERIC; }

/* Any COM object in this codebase (ddraw.c's surfaces/palettes, d3d7.c's
   own objects) starts with `const XXX_vtbl_t *lpVtbl`, and every vtbl's
   first 3 slots are always QueryInterface/AddRef/Release -- the whole
   point of IUnknown. That means AddRef/Release can be called on a
   surface pointer handed to us (SetTexture/SetRenderTarget/CreateDevice)
   generically, without d3d7.c needing to know surface7_t's concrete
   layout (kept private to ddraw.c) -- preserves the existing module
   boundary while still doing real (proper) COM refcounting. */
void com_addref(void *obj)
{
    if (obj == NULL) return;
    void **vtbl = *(void ***)obj;
    ((uint32_t (WINAPI *)(void *))vtbl[1])(obj);
}
void com_release(void *obj)
{
    if (obj == NULL) return;
    void **vtbl = *(void ***)obj;
    ((uint32_t (WINAPI *)(void *))vtbl[2])(obj);
}

/* ===================== GL rendering backend =====================
   See d3d7.h's d3d7_surface_interop_t for why this is a registered
   function-pointer table rather than a direct call into ddraw.c. */
static const d3d7_surface_interop_t *g_surface_interop = NULL;

void d3d7_set_surface_interop(const d3d7_surface_interop_t *interop)
{
    g_surface_interop = interop;
}

static float u32_bits_to_float(uint32_t u)
{
    union { uint32_t u; float f; } conv;
    conv.u = u;
    return conv.f;
}

/* ===================== D3D7 POD types =====================
   Layouts cross-checked field-by-field against d3dtypes.h/d3dcaps.h.
   Packed to pin the exact real layout regardless of host compiler
   defaults, same convention as ddraw_types.h.

   d3dvector_t/d3dcolorvalue_t/d3dmatrix_t/d3dmaterial7_t/d3dlight7_t/
   d3dviewport7_t moved to d3d7.h (see there) -- shared with ddraw.c's
   DX6-era D3D3 delegation layer, see [[project_swep1rcr_port_scoping]]. */
#pragma pack(push, 1)


typedef struct {
    uint32_t size, misc_caps, raster_caps, zcmp_caps, src_blend_caps, dest_blend_caps,
             alpha_cmp_caps, shade_caps, texture_caps, texture_filter_caps,
             texture_blend_caps, texture_address_caps, stipple_width, stipple_height;
} d3dprimcaps_t;

typedef struct {
    uint32_t dev_caps;
    d3dprimcaps_t line_caps, tri_caps;
    uint32_t device_render_bit_depth, device_zbuffer_bit_depth;
    uint32_t min_texture_width, min_texture_height, max_texture_width, max_texture_height;
    uint32_t max_texture_repeat, max_texture_aspect_ratio, max_anisotropy;
    float guard_band_left, guard_band_top, guard_band_right, guard_band_bottom;
    float extents_adjust;
    uint32_t stencil_caps, fvf_caps, texture_op_caps;
    uint16_t max_texture_blend_stages, max_simultaneous_textures;
    uint32_t max_active_lights;
    float max_vertex_w;
    guid_t device_guid;
    uint16_t max_user_clip_planes, max_vertex_blend_matrices;
    uint32_t vertex_processing_caps;
    uint32_t reserved1, reserved2, reserved3, reserved4;
} d3ddevicedesc7_t;

#pragma pack(pop)

#define D3DDEVCAPS_FLOATTLVERTEX         0x00000001u
#define D3DDEVCAPS_EXECUTESYSTEMMEMORY   0x00000010u
#define D3DDEVCAPS_TLVERTEXSYSTEMMEMORY  0x00000040u
#define D3DDEVCAPS_DRAWPRIMTLVERTEX      0x00000400u
#define D3DDEVCAPS_CANRENDERAFTERFLIP    0x00000800u
#define D3DDEVCAPS_TEXTURENONLOCALVIDMEM 0x00001000u
#define D3DDEVCAPS_HWTRANSFORMANDLIGHT   0x00010000u

#define D3DPRASTERCAPS_DITHER   0x00000001u
#define D3DPRASTERCAPS_ZTEST    0x00000010u
#define D3DPCMPCAPS_LESSEQUAL   0x00000008u
#define D3DPSHADECAPS_COLORGOURAUDRGB 0x00000008u
#define D3DPTEXTURECAPS_PERSPECTIVE 0x00000001u
#define D3DPTEXTURECAPS_ALPHA      0x00000004u

/* D3DRENDERSTATETYPE / D3DCMPFUNC / D3DCULL / D3DSHADEMODE values used by
   the rendering backend below -- cross-checked against wine-staging's
   d3dtypes.h. Only the subset Milestone B (transform + rasterize +
   Z-buffer) actually reads. */
#define D3DRENDERSTATE_ZENABLE          7u
#define D3DRENDERSTATE_SRCBLEND         19u
#define D3DRENDERSTATE_DESTBLEND        20u
#define D3DRENDERSTATE_SHADEMODE        9u
#define D3DRENDERSTATE_ZWRITEENABLE     14u
#define D3DRENDERSTATE_ALPHABLENDENABLE 27u
/* D3DRENDERSTATE_SPECULARENABLE/LIGHTING now live in d3d7_internal.h
   (shared with d3d7_shader.c). */
#define D3DRENDERSTATE_CULLMODE         22u
#define D3DRENDERSTATE_ZFUNC            23u
#define D3DRENDERSTATE_AMBIENT          139u
#define D3DRENDERSTATE_LOCALVIEWER      142u
#define D3DRENDERSTATE_NORMALIZENORMALS 143u
/* Cross-checked against wine-staging's d3dtypes.h, same as everything
   else in this block. D3DRENDERSTATE_ALPHATESTENABLE/FOGENABLE/
   ALPHAFUNC/FOGTABLEMODE/FOGVERTEXMODE and D3DFOG_* now live in
   d3d7_internal.h (shared with d3d7_shader.c). */
#define D3DRENDERSTATE_ALPHAREF         24u
#define D3DRENDERSTATE_FOGCOLOR         34u
#define D3DRENDERSTATE_FOGSTART         36u /* D3DVALUE (float bits), see u32_bits_to_float */
#define D3DRENDERSTATE_FOGEND           37u /* D3DVALUE (float bits) */
#define D3DRENDERSTATE_FOGDENSITY       38u /* D3DVALUE (float bits) */

#define D3DRENDERSTATE_DITHERENABLE     26u
#define D3DRENDERSTATE_COLORKEYENABLE   41u
#define D3DRENDERSTATE_STENCILENABLE    52u
#define D3DRENDERSTATE_STENCILFAIL      53u
#define D3DRENDERSTATE_STENCILZFAIL     54u
#define D3DRENDERSTATE_STENCILPASS      55u
#define D3DRENDERSTATE_STENCILFUNC      56u
#define D3DRENDERSTATE_STENCILREF       57u
#define D3DRENDERSTATE_STENCILMASK      58u
#define D3DRENDERSTATE_STENCILWRITEMASK 59u
#define D3DSTENCILOP_KEEP     1u
#define D3DSTENCILOP_ZERO     2u
#define D3DSTENCILOP_REPLACE  3u
#define D3DSTENCILOP_INCRSAT  4u
#define D3DSTENCILOP_DECRSAT  5u
#define D3DSTENCILOP_INVERT   6u
#define D3DSTENCILOP_INCR     7u
#define D3DSTENCILOP_DECR     8u

/* D3DRENDERSTATE_CLIPPING/CLIPPLANEENABLE and D3DCLIPPLANE0..5 now live
   in d3d7_internal.h (shared with d3d7_shader.c). */

/* D3DBLEND -- real values, cross-checked against wine-staging's d3dtypes.h. */
#define D3DBLEND_ZERO            1u
#define D3DBLEND_ONE             2u
#define D3DBLEND_SRCCOLOR        3u
#define D3DBLEND_INVSRCCOLOR     4u
#define D3DBLEND_SRCALPHA        5u
#define D3DBLEND_INVSRCALPHA     6u
#define D3DBLEND_DESTALPHA       7u
#define D3DBLEND_INVDESTALPHA    8u
#define D3DBLEND_DESTCOLOR       9u
#define D3DBLEND_INVDESTCOLOR    10u
#define D3DBLEND_SRCALPHASAT     11u

#define D3DLIGHT_POINT       1u
#define D3DLIGHT_SPOT        2u
#define D3DLIGHT_DIRECTIONAL 3u

#define D3DRENDERSTATE_TEXTUREFACTOR 60u

/* Legacy D3D3-era render states -- real numeric values (cross-checked
   against wine-staging's d3dtypes.h), but NO pipeline effect in this
   shim: they either predate the D3DTSS_* stage-state combiner
   (TEXTUREHANDLE/TEXTUREMAG/TEXTUREMIN/TEXTUREMAPBLEND/WRAPU/WRAPV/
   TEXTUREADDRESS/TEXTUREADDRESSU/V, all superseded by SetTextureStageState
   in this codebase's actual rendering path) or have no GLES equivalent
   wired up yet (ZBIAS/anisotropy/mipmap LOD bias/border color/sphere-map
   WRAP0-7/stippled alpha/edge antialiasing/colorkey-blend-as-opposed-to-
   hard-cutout). d3d_device7_SetRenderState's generic array store already
   takes any of these silently -- given #define'd names here purely so
   rejection_state_label (below) can print them in a trace instead of
   them being invisible, to find out whether SWEP1RCR actually uses any
   of this legacy path. See [[project_swep1rcr_port_scoping]]. */
#define D3DRENDERSTATE_TEXTUREHANDLE      1u
#define D3DRENDERSTATE_TEXTUREADDRESS     3u
#define D3DRENDERSTATE_TEXTUREPERSPECTIVE 4u
#define D3DRENDERSTATE_WRAPU              5u
#define D3DRENDERSTATE_WRAPV              6u
#define D3DRENDERSTATE_TEXTUREMAG         17u
#define D3DRENDERSTATE_TEXTUREMIN         18u
#define D3DRENDERSTATE_TEXTUREMAPBLEND    21u
/* D3DTEXTUREBLEND -- real values (cross-checked against wine-staging's
   d3dtypes.h). UPDATE (2026-07-31): confirmed via a real trace that
   SWEP1RCR actually uses this legacy state (toggles MODULATE<->
   MODULATEALPHA ~26 times through a play session) -- see
   apply_legacy_texturemapblend below, which is no longer a diagnostic-
   only stub for this one. */
#define D3DTBLEND_DECAL         1u
#define D3DTBLEND_MODULATE      2u
#define D3DTBLEND_DECALALPHA    3u
#define D3DTBLEND_MODULATEALPHA 4u
#define D3DTBLEND_DECALMASK     5u
#define D3DTBLEND_MODULATEMASK  6u
#define D3DTBLEND_COPY          7u
#define D3DTBLEND_ADD           8u
#define D3DRENDERSTATE_STIPPLEDALPHA      33u
#define D3DRENDERSTATE_EDGEANTIALIAS      40u
#define D3DRENDERSTATE_BORDERCOLOR        43u
#define D3DRENDERSTATE_TEXTUREADDRESSU    44u
#define D3DRENDERSTATE_TEXTUREADDRESSV    45u
#define D3DRENDERSTATE_MIPMAPLODBIAS      46u
#define D3DRENDERSTATE_ZBIAS              47u
#define D3DRENDERSTATE_ANISOTROPY         49u
#define D3DRENDERSTATE_WRAP0              128u
#define D3DRENDERSTATE_WRAP1              129u
#define D3DRENDERSTATE_WRAP2              130u
#define D3DRENDERSTATE_WRAP3              131u
#define D3DRENDERSTATE_WRAP4              132u
#define D3DRENDERSTATE_WRAP5              133u
#define D3DRENDERSTATE_WRAP6              134u
#define D3DRENDERSTATE_WRAP7              135u
#define D3DRENDERSTATE_COLORKEYBLENDENABLE 144u

/* D3DTSS_/D3DTOP_/D3DTA_ constants now live in d3d7_internal.h (shared with
   d3d7_shader.c). */

#define D3DTADDRESS_WRAP   1u
#define D3DTADDRESS_MIRROR 2u
#define D3DTADDRESS_CLAMP  3u
#define D3DTADDRESS_BORDER 4u

#define D3DTFG_POINT 1u /* D3DTEXTUREMAGFILTER */
#define D3DTFN_POINT 1u /* D3DTEXTUREMINFILTER */

/* D3DCMP_* now lives in d3d7_internal.h (shared with d3d7_shader.c). */

#define D3DCULL_NONE 1u
#define D3DCULL_CW   2u
#define D3DCULL_CCW  3u

#define D3DSHADE_GOURAUD 2u

static const d3dmatrix_t k_identity = { {
    1,0,0,0,
    0,1,0,0,
    0,0,1,0,
    0,0,0,1
} };

static d3dmatrix_t mat4_mul(const d3dmatrix_t *a, const d3dmatrix_t *b)
{
    d3dmatrix_t r;
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) sum += a->m[row * 4 + k] * b->m[k * 4 + col];
            r.m[row * 4 + col] = sum;
        }
    }
    return r;
}

/* Transforms a point/direction by a D3DMATRIX using D3D's own convention
   (row vector times row-major matrix: p' = p * M) -- used to move
   D3DLIGHT7 positions/directions (always given in world space by the
   app) into view space on the CPU before uploading to the fixed-
   function lighting shader, exactly mirroring what the shader itself
   does for vertex positions (see d3d_draw_common's comment on why
   uploading D3D's row-major matrix bytes untransposed and doing M*v_col
   in GLSL already implements this same p*M transform). w=1 for a point
   (translation applies), w=0 for a direction (translation doesn't). */
static d3dvector_t mat4_transform(const d3dmatrix_t *m, d3dvector_t p, int is_point)
{
    d3dvector_t r;
    float w = is_point ? 1.0f : 0.0f;
    r.x = p.x * m->m[0] + p.y * m->m[4] + p.z * m->m[8]  + w * m->m[12];
    r.y = p.x * m->m[1] + p.y * m->m[5] + p.z * m->m[9]  + w * m->m[13];
    r.z = p.x * m->m[2] + p.y * m->m[6] + p.z * m->m[10] + w * m->m[14];
    return r;
}

/* ===================== IDirect3DVertexBuffer7 =====================
   Real heap-backed vertex storage -- CreateVertexBuffer/Lock/Unlock are
   genuinely functional (a game that writes vertices via Lock and expects
   them to still be there on the next Lock gets real behavior). Device7's
   DrawPrimitiveVB/DrawIndexedPrimitiveVB (below, near DrawPrimitive)
   feed this storage straight into the same real rasterizer plain
   DrawPrimitive uses. ProcessVertices/ProcessVerticesStrided remain
   no-ops -- real semantics is "transform+light into another vertex
   buffer for later reuse," meaningless as a separate step in a pipeline
   that already transforms/lights per-draw in the vertex shader. */

/* Decodes a D3DFVF_* flag word into the real per-vertex byte layout --
   both the total stride (needed to size Lock()'d vertex-buffer storage)
   and each attribute's byte offset within a vertex (needed by the
   rendering backend below to point glVertexAttribPointer at the right
   spot in a raw DrawPrimitive vertex array). Attribute order matches
   real D3D FVF layout: position [+blend weights], normal, diffuse,
   specular, texcoords. */
fvf_layout_t decode_fvf(uint32_t fvf)
{
    fvf_layout_t l;
    memset(&l, 0, sizeof(l));

    int off = 0;
    uint32_t pos = fvf & 0x00Eu;
    l.is_rhw = (pos == 0x004u);
    switch (pos) {
        case 0x002u: off += 12; break; /* XYZ */
        case 0x004u: off += 16; break; /* XYZRHW */
        case 0x006u: off += 16; break; /* XYZB1 */
        case 0x008u: off += 20; break; /* XYZB2 */
        case 0x00au: off += 24; break; /* XYZB3 */
        case 0x00cu: off += 28; break; /* XYZB4 */
        case 0x00eu: off += 32; break; /* XYZB5 */
        default: break;
    }
    if (fvf & 0x010u) { l.has_normal = 1; l.normal_offset = off; off += 12; }
    /* D3DFVF_PSIZE (point size, one float) -- this game genuinely emits it
       (fvf=0x1e2, confirmed via real gameplay trace: 46,337 DrawPrimitive
       calls in one session, a substantial fraction of world geometry, not
       an edge case). Nothing here samples per-vertex point size, but its 4
       bytes still sit in the real vertex between normal and diffuse and
       MUST be skipped -- without this, diffuse/specular/texcoord offsets
       and the total stride all come out 4 bytes short, so every vertex
       past the first in a batch gets read starting further and further
       into the wrong place (vertex i misaligned by 4*i bytes). That
       cumulative misalignment is what turned strips/fans of this format
       into oversized, self-intersecting exploded triangles. */
    if (fvf & 0x020u) { off += 4; }
    if (fvf & 0x040u) { l.has_diffuse = 1; l.diffuse_offset = off; off += 4; }
    if (fvf & 0x080u) { l.has_specular = 1; l.specular_offset = off; off += 4; }

    uint32_t tex_count = (fvf & 0xf00u) >> 8;
    if (tex_count > 8) tex_count = 8;
    l.tex_count = (int)tex_count;
    for (uint32_t i = 0; i < tex_count; i++) {
        l.tex_offset[i] = off;
        uint32_t fmt = (fvf >> (16 + i * 2)) & 0x3u;
        int floats;
        switch (fmt) {
            case 3:  floats = 1; break; /* TEXTUREFORMAT1 */
            case 0:  floats = 2; break; /* TEXTUREFORMAT2 */
            case 1:  floats = 3; break; /* TEXTUREFORMAT3 */
            case 2:  floats = 4; break; /* TEXTUREFORMAT4 */
            default: floats = 2; break;
        }
        l.tex_floats[i] = floats;
        off += floats * 4;
    }

    l.stride = off;
    return l;
}

uint32_t fvf_stride(uint32_t fvf)
{
    return (uint32_t)decode_fvf(fvf).stride;
}

static uint32_t WINAPI vb7_QueryInterface(void *self, guid_t *riid, void **out)
{
    (void)riid;
    vb7_t *vb = (vb7_t *)self;
    vb->refcount++;
    if (out != NULL) *out = self;
    return DD_OK;
}
uint32_t WINAPI vb7_AddRef(void *self) { return ++((vb7_t *)self)->refcount; }
uint32_t WINAPI vb7_Release(void *self)
{
    vb7_t *vb = (vb7_t *)self;
    uint32_t rc = --vb->refcount;
    if (rc == 0) { gfx_native_freed(vb); gfx_guest_free(vb->data); free(vb); }
    return rc;
}
uint32_t WINAPI vb7_Lock(void *self, uint32_t flags, void **out_data, uint32_t *out_size)
{
    (void)flags;
    vb7_t *vb = (vb7_t *)self;
    vb->locked = 1;
    if (out_data != NULL) *out_data = vb->data;
    if (out_size != NULL) *out_size = vb->data_size;
    return DD_OK;
}
/* Pure CPU-side sanity check, no GL calls at all -- deliberately kept
   this way (see vb7_Unlock below): a prior attempt to actually RENDER
   every Unlock()'d vertex buffer into an on-screen debug gallery
   crashed the real game live and was reverted, so this investigation
   round sticks to read-only logging of the same underlying question
   ("is this buffer's position data even sane") instead of drawing
   anything. Logs min/max/center/diagonal so a garbled FVF decode (all
   zeros, NaN, or an implausible scale) is visible directly in the trace
   without needing to render it. */
static void vb_log_bbox_if_traced(const vb7_t *vb)
{
    if (!ddraw_trace_enabled()) return;
    fvf_layout_t layout = decode_fvf(vb->fvf);
    if (layout.stride == 0 || vb->num_vertices == 0) {
        fprintf(stderr, "[d3d7-trace] Unlock: vb=%p fvf=0x%08x -- stride=0 or 0 vertices, nothing to bbox\n",
                (const void *)vb, vb->fvf);
        return;
    }
    float bmin[3] = { 1e30f, 1e30f, 1e30f };
    float bmax[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < vb->num_vertices; i++) {
        const float *p = (const float *)(vb->data + (size_t)i * layout.stride);
        for (int k = 0; k < 3; k++) {
            if (p[k] < bmin[k]) bmin[k] = p[k];
            if (p[k] > bmax[k]) bmax[k] = p[k];
        }
    }
    float dx = bmax[0] - bmin[0], dy = bmax[1] - bmin[1], dz = bmax[2] - bmin[2];
    fprintf(stderr, "[d3d7-trace] Unlock: vb=%p fvf=0x%08x nverts=%u is_rhw=%d "
                    "bbox_min=(%.3f,%.3f,%.3f) bbox_max=(%.3f,%.3f,%.3f) size=(%.3f,%.3f,%.3f)\n",
            (const void *)vb, vb->fvf, vb->num_vertices, layout.is_rhw,
            bmin[0], bmin[1], bmin[2], bmax[0], bmax[1], bmax[2], dx, dy, dz);
}
uint32_t WINAPI vb7_Unlock(void *self)
{
    vb7_t *vb = (vb7_t *)self;
    vb->locked = 0;
    vb_log_bbox_if_traced(vb);
    return DD_OK;
}
/* Real semantics: transform+light vertices from this buffer into
   dst_buffer starting at dst_idx. Meaningless without a transform/light
   pipeline actually feeding a rasterizer -- no-op, same rationale as
   DrawPrimitive*. */
static uint32_t WINAPI vb7_ProcessVertices(void *self, uint32_t vertex_op, uint32_t dst_idx, uint32_t count,
                                            uint32_t src_buffer, uint32_t src_idx, uint32_t device, uint32_t flags)
{
    (void)self; (void)vertex_op; (void)dst_idx; (void)count; (void)src_buffer; (void)src_idx; (void)device; (void)flags;
    return DD_OK;
}
uint32_t WINAPI vb7_GetVertexBufferDesc(void *self, d3dvertexbufferdesc_t *out_desc)
{
    vb7_t *vb = (vb7_t *)self;
    if (out_desc == NULL) return DDERR_INVALIDPARAMS;
    out_desc->size = sizeof(*out_desc);
    out_desc->caps = 0;
    out_desc->fvf = vb->fvf;
    out_desc->num_vertices = vb->num_vertices;
    return DD_OK;
}
static uint32_t WINAPI vb7_Optimize(void *self, uint32_t device, uint32_t flags)
{
    (void)self; (void)device; (void)flags;
    return DD_OK;
}
static uint32_t WINAPI vb7_ProcessVerticesStrided(void *self, uint32_t vertex_op, uint32_t dst_idx, uint32_t count,
                                                   uint32_t data, uint32_t fvf, uint32_t device, uint32_t flags)
{
    (void)self; (void)vertex_op; (void)dst_idx; (void)count; (void)data; (void)fvf; (void)device; (void)flags;
    return DD_OK;
}

static const vb7_vtbl_t g_vb7_vtbl = {
    .QueryInterface = (comfn2_t)vb7_QueryInterface,
    .AddRef = vb7_AddRef,
    .Release = vb7_Release,
    .Lock = (comfn3_t)vb7_Lock,
    .Unlock = vb7_Unlock,
    .ProcessVertices = (comfn7_t)vb7_ProcessVertices,
    .GetVertexBufferDesc = (comfn1_t)vb7_GetVertexBufferDesc,
    .Optimize = (comfn2_t)vb7_Optimize,
    .ProcessVerticesStrided = (comfn7_t)vb7_ProcessVerticesStrided,
};

vb7_t *vb7_create(const d3dvertexbufferdesc_t *desc)
{
    vb7_t *vb = calloc(1, sizeof(*vb));
    vb->lpVtbl = &g_vb7_vtbl;
    vb->refcount = 1;
    vb->fvf = desc->fvf;
    vb->num_vertices = desc->num_vertices;
    vb->stride = fvf_stride(desc->fvf);
    vb->data_size = vb->stride * (desc->num_vertices != 0 ? desc->num_vertices : 1);
    /* guest memory: Lock hands the guest this address (gfx_mem.h) */
    vb->data = gfx_guest_alloc(vb->data_size != 0 ? vb->data_size : 1);
    return vb;
}

/* --- IDirect3DDevice7 (46 methods after IUnknown) --- */

/* d3d_state_t/d3d_device7_vtbl_t/d3d_device7_t/d3ddp_ptrstride_t/
   d3ddrawprimitivestrideddata_t now live in d3d7_internal.h (shared with
   d3d7_cmdbuf.c and d3d7_d3d3_bridge.c); render_cmd_t's full definition
   lives in d3d7_cmdbuf.c (d3d_device7_t only ever holds a pointer to it). */

static void d3d_state_init(d3d_state_t *s)
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < MAX_TRANSFORMS; i++) s->transforms[i] = k_identity;
    s->viewport.max_z = 1.0f;

    /* Real D3D7 default render state -- matters now that Milestone B
       actually reads these to drive GL depth-test/cull state instead of
       just storing them: a game that never explicitly calls
       SetRenderState(ZENABLE/...) still expects these real defaults, not
       "everything off" (all-zero, which memset above already gave every
       other render state -- correct for the ones with a real 0/FALSE
       default). */
    s->render_state[D3DRENDERSTATE_ZENABLE] = 1;      /* D3DZB_TRUE */
    s->render_state[D3DRENDERSTATE_ZWRITEENABLE] = 1; /* TRUE */
    s->render_state[D3DRENDERSTATE_ZFUNC] = D3DCMP_LESSEQUAL;
    s->render_state[D3DRENDERSTATE_CULLMODE] = D3DCULL_CCW;
    s->render_state[D3DRENDERSTATE_SHADEMODE] = D3DSHADE_GOURAUD;
    /* ALPHABLENDENABLE defaults to FALSE (0, already correct via memset
       above), but SRCBLEND/DESTBLEND's real D3D7 defaults are ONE/ZERO,
       not 0/0 -- 0 isn't even a valid D3DBLEND_* value (they start at 1),
       so a game that enables blending without also explicitly setting
       both factors needs to see these, not garbage. */
    s->render_state[D3DRENDERSTATE_SRCBLEND] = D3DBLEND_ONE;
    s->render_state[D3DRENDERSTATE_DESTBLEND] = D3DBLEND_ZERO;

    /* Real D3D7 defaults for stencil ops/func/masks -- STENCILENABLE
       itself correctly defaults FALSE via memset, so none of this has
       any effect until a game turns it on, but a game that enables
       stencil testing without also setting every one of these
       explicitly needs to see real no-op defaults (KEEP/ALWAYS/all-bits
       masks), not 0 (0 isn't even a valid D3DSTENCILOP or D3DCMP
       value, and a 0 mask would silently stencil-test/write nothing). */
    s->render_state[D3DRENDERSTATE_STENCILFAIL] = D3DSTENCILOP_KEEP;
    s->render_state[D3DRENDERSTATE_STENCILZFAIL] = D3DSTENCILOP_KEEP;
    s->render_state[D3DRENDERSTATE_STENCILPASS] = D3DSTENCILOP_KEEP;
    s->render_state[D3DRENDERSTATE_STENCILFUNC] = D3DCMP_ALWAYS;
    s->render_state[D3DRENDERSTATE_STENCILMASK] = 0xFFFFFFFFu;
    s->render_state[D3DRENDERSTATE_STENCILWRITEMASK] = 0xFFFFFFFFu;

    /* Real D3D7 default is CLIPPING=TRUE -- practically inert either way
       until CLIPPLANEENABLE's mask (correctly 0/none via memset) turns
       on an actual plane, but kept faithful for any game that queries
       GetRenderState(CLIPPING) and branches on it. */
    s->render_state[D3DRENDERSTATE_CLIPPING] = 1;

    /* Real per-stage texture-stage-state defaults (D3D7): stage 0
       modulates the bound texture with the interpolated vertex color;
       every other stage starts disabled (a game must explicitly enable
       additional stages for multitexturing). Same "read now for real"
       rationale as the render-state defaults above -- Milestone D
       actually samples/combines textures instead of just tracking
       Set/GetTextureStageState round-trips. */
    for (int stage = 0; stage < MAX_TEX_STAGES; stage++) {
        s->tss_state[stage][D3DTSS_COLOROP] = (stage == 0) ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        s->tss_state[stage][D3DTSS_ALPHAOP] = (stage == 0) ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        s->tss_state[stage][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        s->tss_state[stage][D3DTSS_COLORARG2] = D3DTA_CURRENT;
        s->tss_state[stage][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        s->tss_state[stage][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        s->tss_state[stage][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
        s->tss_state[stage][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
        s->tss_state[stage][D3DTSS_MAGFILTER] = D3DTFG_POINT;
        s->tss_state[stage][D3DTSS_MINFILTER] = D3DTFN_POINT;
    }
}

/* D3DVIEWPORT7 is measured from the top-left of the render target; GL's
   viewport origin is bottom-left -- flip Y here so both agree on which
   pixels are "inside" the viewport. Falls back to the full render target
   when no viewport has been set yet (SetViewport not yet called, or a
   0-sized one), matching what a fresh D3D7 device would show. */
static void apply_device_viewport(const d3d_device7_t *dev, int rt_w, int rt_h)
{
    const d3dviewport7_t *vp = &dev->state.viewport;
    int x = (int)vp->x, y = (int)vp->y, w = (int)vp->width, h = (int)vp->height;
    if (w <= 0 || h <= 0) { x = 0; y = 0; w = rt_w; h = rt_h; }
    int gl_y = y; /* unflipped, paired with the clip-space flip and the unflipped gl_target_read/upload_rgba below */
    gl_state_set_viewport(x, gl_y, w, h);
    /* Real D3D clips ALL rasterization to the viewport rectangle, not
       just Clear() -- but GL's glViewport is only a coordinate-transform
       parameter; it does not reliably guarantee that a primitive
       extending past it gets clipped there rather than rasterizing over
       the rest of the framebuffer (only glScissor is spec-guaranteed to
       do that). This was already known to matter for Clear() (fixed
       separately, scoped to this same rectangle) -- applying it here too
       means it covers every DrawPrimitive/DrawIndexedPrimitive call as
       well, so any triangle whose vertex data or transform sends it
       outside the current viewport (this game's own splitting of the
       screen into a D3D 3D area and a DirectDraw-owned HUD strip
       deliberately excluded from that viewport) gets hard-clipped there
       instead of potentially bleeding across the whole render target. */
    gl_state_set_scissor_enabled(1);
    gl_state_set_scissor(x, gl_y, w, h);

    /* D3DVIEWPORT7's dvMinZ/dvMaxZ is D3D's own equivalent of
       glDepthRangef -- a per-viewport remapping of the post-projection
       [0,1] depth range, e.g. to reserve the front of the depth buffer
       for HUD-in-3D-space elements. Previously never applied at all, so
       every device silently used GL's own default depth range (0,1)
       regardless of what the game actually requested here.

       Real, live-confirmed root cause of a black-screen report: XYZRHW
       (screen-space, pre-transformed) content this game draws with dvZ
       exactly 1.0 (the real, common "far plane"/2D-UI value) was being
       silently discarded by the depth test every time, because a depth
       range of EXACTLY (0,1) here gives a rasterized z=1.0 fragment and a
       Clear()-cleared depth buffer value of exactly 1.0 zero floating-
       point margin against each other -- confirmed empirically (disabling
       the depth test for RHW draws made real content appear exactly where
       expected) and confirmed via a real WINEDEBUG capture of this same
       game under real Wine: its own actual GPU depth range is never
       (0,1) exactly, it's consistently (0, 0.999984741) == (0,
       65535/65536) -- real Windows (or dx7z.dll itself) always leaves this
       same tiny protective margin against exactly this class of far-plane
       z-fighting. Clamping OUR max_z the same way, only when the game
       requests the full (no-margin) range, matches that real, observed
       behavior -- a deliberately narrower range some other real caller
       sets (e.g. reserving the front of the depth buffer for HUD-in-3D
       elements) is left completely untouched. */
    float max_z = vp->max_z >= 1.0f ? (65535.0f / 65536.0f) : vp->max_z;
    gl_state_set_depth_range(vp->min_z, max_z);
}

/* Binds the device's current render target as the active GL framebuffer
   and applies the D3D viewport on top of it, reporting the target's
   pixel dimensions (needed by DrawPrimitive/DrawIndexedPrimitive below
   to map XYZRHW's already-in-pixels vertices into NDC). Returns 0 (and
   does nothing) if no interop is registered or no render target is set
   -- every caller below is written to degrade to its prior no-op
   behavior in that case, so tests/d3d7_test.c's pure state-tracking
   checks keep passing unchanged. */
static int ensure_rendering_target_impl(d3d_device7_t *dev, int *out_w, int *out_h)
{
    int w = 0, h = 0;
    if (g_surface_interop == NULL || dev->render_target == NULL) return 0;
    if (!g_surface_interop->bind_render_target(dev->render_target, &w, &h)) return 0;
    apply_device_viewport(dev, w, h);
    if (out_w != NULL) *out_w = w;
    if (out_h != NULL) *out_h = h;
    return 1;
}

/* Real GL-touching leaf (glViewport/glScissor/glDepthRangef via
   apply_device_viewport, plus the interop's own gl_target_ensure/bind
   calls) -- wrapped the same way as clear_execute_now/draw_flat_
   execute_now/draw_strided_execute_now above. NOT just reachable through
   those three: d3d_device7_BeginScene also calls this directly,
   unconditionally, every scene -- a real gap the first version of this
   fix missed. Missing it meant BeginScene's call ran on whatever thread
   called BeginScene (never the GL thread), so its glViewport/glScissor
   calls silently no-op'd (no context current there) while gl_state_
   cache's shadow copy still recorded them as if they'd succeeded --
   confirmed live via a real corrupted render (render_backend_test's own
   "Phase 4: permutation-integrity stress test": the cache believed the
   scissor rect had been widened to the new render target's full size,
   but the real GL scissor silently stayed at a smaller stale rect from
   an earlier, unrelated render target, clipping away most of the
   frame). Wrapping this function itself, rather than BeginScene,
   automatically covers all 4 real call sites (BeginScene directly;
   clear_execute_now_impl, d3d_draw_prepare, and d3d_draw_dispatch_and_
   cleanup transitively, all already running on the GL thread by the
   time they reach here, so they hit the self-thread bypass for free). */
typedef struct { d3d_device7_t *dev; int *out_w, *out_h; int result; } ensure_rendering_target_args_t;
static void ensure_rendering_target_trampoline(void *arg)
{
    ensure_rendering_target_args_t *a = (ensure_rendering_target_args_t *)arg;
    a->result = ensure_rendering_target_impl(a->dev, a->out_w, a->out_h);
}
static int ensure_rendering_target(d3d_device7_t *dev, int *out_w, int *out_h)
{
    ensure_rendering_target_args_t a = { dev, out_w, out_h, 0 };
    gl_thread_call(ensure_rendering_target_trampoline, &a);
    return a.result;
}

/* Phase 2 rendering architecture (command buffer) now lives in
   d3d7_cmdbuf.c -- see [[project_phase2_command_buffer]]. Declared in
   d3d7_internal.h: cmdbuf_flush, cmdbuf_discard, cmdbuf_record_clear,
   cmdbuf_record_draw_flat, cmdbuf_record_draw_strided. */

static uint32_t WINAPI d3d_device7_QueryInterface(void *self, guid_t *riid, void **out)
{
    (void)riid;
    d3d_device7_t *dev = (d3d_device7_t *)self;
    dev->refcount++;
    if (out != NULL) *out = self;
    return DD_OK;
}
static uint32_t WINAPI d3d_device7_AddRef(void *self) { return ++((d3d_device7_t *)self)->refcount; }
static void release_drain_trampoline(void *arg) { (void)arg; }

uint32_t WINAPI d3d_device7_Release(void *self)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    uint32_t rc = --dev->refcount;
    if (rc == 0) {
        cmdbuf_discard(dev); /* defensive -- normally empty, EndScene already flushed; discards whatever's still in the ACTIVE recording slot only */
        /* A prior EndScene may have posted an async flush job (Phase 2,
           gl_thread_post) that's still running on the GL thread RIGHT NOW,
           reading dev->cmdbuf_slots[the OTHER slot] and dev->replay_shadow
           -- freeing those out from under it would be a real use-after-
           free. gl_thread_call's own single-slot FIFO already guarantees
           any earlier-posted job is fully done before THIS request can
           even be submitted (see gl_thread.c's own comment) -- a
           synchronous no-op dispatch here is a real, sufficient drain
           barrier, not a formality. */
        gl_thread_call(release_drain_trampoline, NULL);
        for (int i = 0; i < 2; i++) {
            free(dev->cmdbuf_slots[i].cmd_buf);
            free(dev->cmdbuf_slots[i].order);
            free(dev->cmdbuf_slots[i].state_snapshots); /* cmdbuf_discard/cmdbuf_flush_impl already freed its CONTENTS -- this frees the array-of-pointers itself */
        }
        free(dev->replay_shadow);
        com_release(dev->render_target);
        for (int i = 0; i < MAX_TEX_STAGES; i++) com_release(dev->textures[i]);
        /* g_draw_uniform_cache's device-identity guard (state_generation
           counter, see d3d_draw_prepare) only helps while this device stays
           alive -- a later calloc for a NEW device could reuse this exact
           freed address, spoofing cached_device==dev for state that was
           never actually this new device's. Invalidate here so that can
           never happen, rather than relying solely on test-suite discipline
           (the only place this reset is otherwise called from). Also
           invalidate for dev->replay_shadow's own address, for the exact
           same reason -- it's a real d3d_device7_t* that a fresh replay
           could plausibly reuse via the free() just above. */
        d3d7_draw_uniform_cache_reset();
        gfx_native_freed(dev);
        free(dev);
    }
    return rc;
}

/* ----- capability enumeration -----
   See this file's header comment: this is the one D3D7 device method
   confirmed (via king.exe.c:315197/292360) to feed a real king.exe
   decision (whether/how a texture DirectDraw surface gets created), not
   just setup formality. Reports a broad, standard set of formats a real
   HAL/HEL D3D7 device from this era would -- previously only RGB565 and
   ARGB4444 were offered, which satisfied the *other*, already-known
   "can not find 565 or 4444" fatal check but may well have been too
   narrow for FUN_005d7b10's capability-bit accumulation. */
uint32_t WINAPI d3d_device7_EnumTextureFormats(void *self, void *cb_, void *ctx)
{
    (void)self;
    typedef uint32_t (WINAPI *cb_t)(ddpixelformat_t *, void *);
    if (cb_ == NULL) return DD_OK;

    ddpixelformat_t fmt;

    struct { uint32_t flags, bits, r, g, b, a; } formats[] = {
        /* 16-bit */
        { DDPF_RGB, 16, 0xF800, 0x07E0, 0x001F, 0 },                         /* RGB565 */
        { DDPF_RGB, 16, 0x7C00, 0x03E0, 0x001F, 0 },                         /* XRGB1555 */
        { DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x7C00, 0x03E0, 0x001F, 0x8000 }, /* ARGB1555 */
        { DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x0F00, 0x00F0, 0x000F, 0xF000 }, /* ARGB4444 */
        { DDPF_RGB, 16, 0x0F00, 0x00F0, 0x000F, 0 },                        /* XRGB4444 */
        /* 24/32-bit */
        { DDPF_RGB, 24, 0x00FF0000, 0x0000FF00, 0x000000FF, 0 },                        /* RGB888 */
        { DDPF_RGB, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0 },                        /* XRGB8888 */
        { DDPF_RGB | DDPF_ALPHAPIXELS, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000u }, /* ARGB8888 */
        /* paletted -- real D3D7 HAL devices commonly still listed this */
        { DDPF_PALETTEINDEXED8, 8, 0, 0, 0, 0 },
    };

    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        memset(&fmt, 0, sizeof(fmt));
        fmt.dwSize = sizeof(fmt);
        fmt.dwFlags = formats[i].flags;
        fmt.dwRGBBitCount = formats[i].bits;
        fmt.dwRBitMask = formats[i].r;
        fmt.dwGBitMask = formats[i].g;
        fmt.dwBBitMask = formats[i].b;
        fmt.dwRGBAlphaBitMask = formats[i].a;
        if (((cb_t)cb_)(&fmt, ctx) == 0) return DD_OK; /* DDENUMRET_CANCEL */
    }
    return DD_OK;
}

static uint32_t WINAPI d3d_device7_GetDirect3D(void *self, void **out_d3d7)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (out_d3d7 != NULL) *out_d3d7 = dev->d3d7_owner;
    return DD_OK;
}

/* ----- render target ----- */
uint32_t WINAPI d3d_device7_SetRenderTarget(void *self, void *surface, uint32_t flags)
{
    (void)flags;
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[d3d7-trace] SetRenderTarget: device=%p surface=%p (was %p)\n",
                self, surface, dev->render_target);
    }
    if (surface != NULL) com_addref(surface);
    if (dev->render_target != NULL) com_release(dev->render_target);
    dev->render_target = surface;
    return DD_OK;
}
uint32_t WINAPI d3d_device7_GetRenderTarget(void *self, void **out_surface)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (out_surface == NULL) return DDERR_INVALIDPARAMS;
    if (dev->render_target != NULL) com_addref(dev->render_target);
    *out_surface = dev->render_target;
    return DD_OK;
}

/* ----- scene bracketing / clear / draw calls -----
   Real rendering backend (see this file's header comment and the GLES
   pipeline further below): Clear/DrawPrimitive/DrawIndexedPrimitive
   genuinely rasterize into the bound render target's GL framebuffer,
   synced back into the DirectDraw surface's CPU buffer via
   d3d7_surface_interop_t. in_scene is tracked for potential future
   diagnostics but never used to reject a call -- being lenient about
   call ordering here can only help compatibility, never hurt it, since
   nothing downstream depends on strict Begin/EndScene pairing (real
   D3D7 Clear() calls outside BeginScene/EndScene are common and already
   supported here for exactly that reason). */
#define D3DCLEAR_TARGET  0x00000001u
#define D3DCLEAR_ZBUFFER 0x00000002u
#define D3DCLEAR_STENCIL 0x00000004u
uint32_t clear_execute_now(d3d_device7_t *dev, uint32_t count, const ddraw_rect_t *rp,
                            uint32_t flags, uint32_t color, uint32_t z, uint32_t stencil);
/* Set once the per-real-frame implicit depth clear (below) has fired,
   cleared by d3d7_reset_frame_depth_clear (called once per real Flip(),
   ddraw.c) -- see that function's own declaration in d3d7.h for the full
   story of the real, live-confirmed bug this fixes: BeginScene used to
   clear depth unconditionally on EVERY call, but this game issues TWO
   BeginScene/EndScene passes for most real frames (confirmed via trace:
   ~71% of Flips), and the second pass's clear was wiping out the first
   pass's real depth values before the second pass's own geometry (a
   driver/character model, live-reported rendering in front of its own
   pod's cockpit geometry) ever got to test against them. A plain global,
   not a per-device field, matching every other rendering-pipeline-wide
   piece of state already tracked this way in this file (g_prog,
   g_stream_vbo, g_draw_uniform_cache, ...) -- this shim only ever runs
   one GL context/thread regardless of how many D3D7 device objects the
   game happens to create. */
static int g_depth_cleared_this_frame = 0;

void d3d7_reset_frame_depth_clear(void)
{
    g_depth_cleared_this_frame = 0;
}

uint32_t WINAPI d3d_device7_BeginScene(void *self)
{
    if (ddraw_trace_enabled()) fprintf(stderr, "[d3d7-trace] BeginScene: device=%p\n", self);
    d3d_device7_t *dev = (d3d_device7_t *)self;
    /* Defensive only -- a fresh scene should never inherit stale commands
       from an improperly-terminated previous one (EndScene always
       flushes); should be a no-op in the normal case. */
    cmdbuf_discard(dev);
    dev->in_scene = 1;
    /* Real drivers validate/bind the target at scene start too, not just
       on the first draw call -- harmless no-op if there's no render
       target or GL interop yet. */
    ensure_rendering_target(dev, NULL, NULL);
    /* Real, confirmed-live gap (SWEP1RCR.EXE, see
       [[project_swep1rcr_port_scoping]]'s "no 3D in gameplay" investigation):
       a live gameplay trace (667k lines, 1727 real BeginScene/EndScene pairs,
       119523 real DrawIndexedPrimitive calls of genuine 3D content -- not
       just HUD) showed ZERO Clear() calls anywhere in the whole session,
       and D3DRENDERSTATE_ZENABLE never explicitly toggled away from its
       real default (TRUE) -- meaning depth testing stays active the whole
       time, but this game NEVER resets the depth buffer via D3D's own
       Clear(). It manages the COLOR buffer itself (a DirectDraw Blt
       colorfill every frame, already a real, separately-working path) but
       has no equivalent mechanism for depth at all. Without a periodic
       reset, the depth buffer accumulates stale values across the entire
       session (1727+ frames), increasingly rejecting new geometry against
       leftover depth from unrelated earlier frames -- a real, well-known
       compatibility issue: many D3D games from this era relied on the
       *driver* implicitly giving them a fresh depth buffer every frame
       (never part of the formal D3D spec, but common real-world IHV
       behavior at the time), a behavior this shim didn't provide.

       Fires at most ONCE per real presented frame (see
       g_depth_cleared_this_frame above), not once per BeginScene --
       real hardware only ever gave a fresh buffer once per frame, and a
       game issuing multiple scenes before one Flip (this game routinely
       does exactly that) is relying on its own later passes seeing the
       depth values its own earlier passes in the SAME frame just wrote,
       same as real D3D7 always guaranteed within a frame. Clearing depth
       (not color -- that's already handled correctly elsewhere) once per
       real frame is harmless for a well-behaved single-scene-per-frame
       game that already calls Clear(D3DCLEAR_ZBUFFER) itself (a
       redundant, immediately-overwritten extra clear, not a behavior
       change) while fixing exactly this game's real, evidenced gap. */
    if (!g_depth_cleared_this_frame) {
        clear_execute_now(dev, 0, NULL, D3DCLEAR_ZBUFFER, 0, 0x3F800000u /* 1.0f */, 0);
        g_depth_cleared_this_frame = 1;
    }
    return DD_OK;
}
uint32_t WINAPI d3d_device7_EndScene(void *self)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    /* Phase 2 command buffer: replay everything recorded since BeginScene
       BEFORE the perf-trace glFinish() block below, so that measurement
       still captures real GPU time for the (now-batched) draws and
       g_d3d7_perf.draw_calls stays accurate. See cmdbuf_flush's own
       comment for why replaying here, in order, is behavior-preserving. */
    cmdbuf_flush(dev);

    if (ddraw_trace_enabled()) fprintf(stderr, "[d3d7-trace] EndScene: device=%p\n", self);
    /* g_d3d7_perf itself is accumulated unconditionally elsewhere (the
       various draw/dispatch call sites) regardless of this flag -- see
       d3d7_perf_report_for_frame's own comment for why the glFinish()/
       print/reset that used to live here moved to that function, called
       once per real Flip() (ddraw.c) instead of once per EndScene. */
    dev->in_scene = 0;
    return DD_OK;
}

/* Real GL-touching leaf (glFinish()) -- called once per real displayed
   frame from ddraw.c's Flip(), independently of the cmdbuf_flush/draw-
   dispatch call tree, so it needs its own gl_thread_call wrap (see
   render.c's "GL-thread dispatch wrappers" comment for the pattern).
   The d3d7_perf_trace_enabled() bail-out stays in the PUBLIC wrapper
   below, not here, so a real cross-thread hop is only ever paid when
   LONGROAD2_PERF_TRACE is actually on -- the common case (trace off)
   costs nothing extra. */
static void d3d7_perf_report_for_frame_impl(d3d7_perf_frame_report_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!d3d7_perf_trace_enabled()) return;
    /* See d3d7_perf_trace_enabled's own comment -- glFinish() here blocks
       until every draw call issued since the last report has actually
       finished on the GPU, isolating real 3D-scene GPU render time from
       whatever CPU-side call (present/swap, timed separately by
       ddraw.c's own g_perf) would otherwise be blamed for having to
       wait on it. Called once per real displayed frame now (from
       ddraw.c's Flip(), before its own present work), not once per
       EndScene -- a scene backlog of several EndScenes since the last
       Flip all gets drained and accounted for by this ONE call. */
    activity_sampler_enter(ACTIVITY_GPU_FINISH);
    double t0 = d3d7_perf_now_ms();
    glFinish();
    out->gpu_finish_ms = d3d7_perf_now_ms() - t0;
    activity_sampler_leave();
    out->draw_calls = g_d3d7_perf.draw_calls;
    out->vertices = g_d3d7_perf.vertices;
    out->indices = g_d3d7_perf.indices;
    out->uniform_uploads = g_d3d7_perf.uniform_uploads;
    out->uniform_skips = g_d3d7_perf.uniform_skips;
    out->gl_draws = g_d3d7_perf.gl_draws;
    for (int i = 0; i < D3D7_CALL_COUNT; i++) {
        if (i == D3D7_TEST_CALL_DRAWCOMMON || i == D3D7_TEST_CALL_DRAWCOMMON_STRIDED) {
            out->draw_dispatch_count += g_d3d7_perf.call_counts[i];
            out->draw_dispatch_ms += g_d3d7_perf.call_ms[i];
        } else {
            out->state_call_count += g_d3d7_perf.call_counts[i];
            out->state_call_ms += g_d3d7_perf.call_ms[i];
        }
    }
    out->cmdbuf_flush_count = g_d3d7_perf.cmdbuf_flush_calls;
    out->cmdbuf_flush_ms = g_d3d7_perf.cmdbuf_flush_ms;
    memset(&g_d3d7_perf, 0, sizeof(g_d3d7_perf));
}

static void d3d7_perf_report_for_frame_trampoline(void *arg)
{
    d3d7_perf_report_for_frame_impl((d3d7_perf_frame_report_t *)arg);
}
void d3d7_perf_report_for_frame(d3d7_perf_frame_report_t *out)
{
    /* Mirrors d3d7_perf_report_for_frame_impl's own unconditional memset --
       out is always zeroed regardless of whether trace is enabled, exactly
       preserving the original behavior even though the real hop is now
       skipped in the common (trace-off) case. */
    if (!d3d7_perf_trace_enabled()) { memset(out, 0, sizeof(*out)); return; }
    gl_thread_call(d3d7_perf_report_for_frame_trampoline, out);
}

/* Real semantics, full-target only: dwCount/lpRects (partial-rect clear)
   is accepted but ignored, same simplification level as this shim's
   other geometry-adjacent stubs (e.g. Blt's DDBLTFX effects) -- no
   caller in this game's traced usage relies on sub-rect clears. */
/* Real GL-issuing logic for Clear() -- extracted verbatim from what used
   to be d3d_device7_Clear's own full body (Phase 2 command buffer, see
   render_cmd_t's own comment) so it can be called either immediately or
   later, from cmdbuf_flush, with zero logic duplicated/changed. */
/* Real GL-touching leaf -- see render.c's own "GL-thread dispatch
   wrappers" comment for the overall pattern this mirrors. Renamed to
   _impl and wrapped below with gl_thread_call() so this always runs on
   the dedicated GL thread regardless of which real OS thread (this
   game's own D3D thread included) calls Clear()/BeginScene/cmdbuf_flush
   replay. */
uint32_t clear_execute_now_impl(d3d_device7_t *dev, uint32_t count, const ddraw_rect_t *rp,
                                   uint32_t flags, uint32_t color, uint32_t z, uint32_t stencil)
{
    int rt_w = 0, rt_h = 0;
    if (!ensure_rendering_target(dev, &rt_w, &rt_h)) return DD_OK;

    GLbitfield mask = 0;
    if (flags & D3DCLEAR_TARGET) {
        glClearColor(((color >> 16) & 0xFFu) / 255.0f, ((color >> 8) & 0xFFu) / 255.0f,
                     (color & 0xFFu) / 255.0f, ((color >> 24) & 0xFFu) / 255.0f);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & D3DCLEAR_ZBUFFER) {
        /* Real D3D7 Clear(D3DCLEAR_ZBUFFER) is unconditional -- it always
           writes the given Z value regardless of ZWRITEENABLE, which only
           governs actual DRAW-time depth writes. glClear(GL_DEPTH_BUFFER_
           BIT) is NOT similarly unconditional in real GL/GLES: it silently
           respects whatever glDepthMask is CURRENTLY set to. apply_render_
           state only ever touches the depth mask as part of a draw that
           has ZENABLE on (see its own comment, and
           [[project_minimap_texture_blinking_investigation]] for the
           original half of this bug: glDepthMask used to be called
           unconditionally, keyed only on ZWRITEENABLE, so whichever draw
           ran LAST decided the mask left in effect for the NEXT Clear).
           That fix only prevents a stale FALSE mask from a same-ZENABLE
           run of draws -- it does nothing to guarantee the mask is TRUE
           by the time THIS Clear call runs, since a draw with ZENABLE=0
           (test off) never touches the mask at all and simply leaves
           whatever was cached from before. Force it here, unconditionally,
           every time -- this has no effect on subsequent draws (each one
           re-derives its own mask from ITS OWN ZENABLE/ZWRITEENABLE via
           apply_render_state regardless of what Clear left behind). */
        gl_state_set_depth_mask(GL_TRUE);
        float zf = u32_bits_to_float(z);
        if (zf < 0.0f) zf = 0.0f;
        if (zf > 1.0f) zf = 1.0f;
        glClearDepthf(zf);
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    if (flags & D3DCLEAR_STENCIL) {
        /* Same GL gotcha, same fix, for glStencilMask -- see the
           D3DCLEAR_ZBUFFER comment above. apply_render_state only ever
           calls glStencilMask as part of a draw with STENCILENABLE on
           (d3d7.c, next to STENCILWRITEMASK), so a Clear(D3DCLEAR_STENCIL)
           following a draw that left a narrow write mask (or one with
           stencil testing off, never touching the mask at all) would
           otherwise silently drop bits of this clear too. */
        gl_state_set_stencil_write_mask(0xFFFFFFFFu);
        glClearStencil((GLint)stencil);
        mask |= GL_STENCIL_BUFFER_BIT;
    }
    /* Real D3D7 Clear() only affects pixels inside the CURRENT viewport,
       not the whole render target -- ensure_rendering_target above
       already set up GL_SCISSOR_TEST/glScissor to match it (see
       apply_device_viewport's comment: this used to be Clear-specific
       logic here, now shared with every draw call too). On top of that,
       a non-zero rect list further narrows this to JUST those rects --
       e.g. a HUD gauge/progress-bar fill via one small D3DRECT, a common
       real D3D7 technique. Previously `count`/`rects` were silently
       discarded entirely, so ANY rect-scoped Clear() call cleared the
       WHOLE current viewport instead of the one small rect the game
       actually asked for -- confirmed live as the cause of a truck-stats
       HUD panel's progress-bar fill covering the entire screen. */
    if (mask != 0) {
        if (rp != NULL) {
            for (uint32_t i = 0; i < count; i++) {
                int32_t l = rp[i].left, t = rp[i].top, r = rp[i].right, b = rp[i].bottom;
                if (l < 0) l = 0;
                if (t < 0) t = 0;
                if (r > rt_w) r = rt_w;
                if (b > rt_h) b = rt_h;
                if (r <= l || b <= t) continue;
                gl_state_set_scissor(l, t, r - l, b - t);
                glClear(mask);
            }
        } else {
            glClear(mask);
        }
    }

    if ((flags & D3DCLEAR_TARGET) && g_surface_interop != NULL) {
        g_surface_interop->mark_gpu_dirty(dev->render_target);
    }
    return DD_OK;
}

typedef struct {
    d3d_device7_t *dev; uint32_t count; const ddraw_rect_t *rp;
    uint32_t flags, color, z, stencil, result;
} clear_execute_now_args_t;
static void clear_execute_now_trampoline(void *arg)
{
    clear_execute_now_args_t *a = (clear_execute_now_args_t *)arg;
    a->result = clear_execute_now_impl(a->dev, a->count, a->rp, a->flags, a->color, a->z, a->stencil);
}
uint32_t clear_execute_now(d3d_device7_t *dev, uint32_t count, const ddraw_rect_t *rp,
                            uint32_t flags, uint32_t color, uint32_t z, uint32_t stencil)
{
    clear_execute_now_args_t a = { dev, count, rp, flags, color, z, stencil, 0 };
    gl_thread_call(clear_execute_now_trampoline, &a);
    return a.result;
}

static uint32_t WINAPI d3d_device7_Clear(void *self, uint32_t count, uintptr_t rects, uint32_t flags,
                                          uint32_t color, uint32_t z, uint32_t stencil)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    /* rects is the game's real lpRects pointer, passed through as a raw
       32-bit value like every other vtable arg in this file (see the
       comfn*_t typedefs) -- reinterpret it as the D3DRECT array it really
       is. D3DRECT has the same {left,top,right,bottom} LONG layout as
       ddraw_rect_t (cross-checked against real d3dtypes.h), so no new
       struct is needed. */
    const ddraw_rect_t *rp = (count > 0 && rects != 0) ? (const ddraw_rect_t *)(uintptr_t)rects : NULL;
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[d3d7-trace] Clear: device=%p count=%u rects=%p flags=0x%08x color=0x%08x z=0x%08x\n",
                self, count, (const void *)rects, flags, color, z);
        for (uint32_t i = 0; rp != NULL && i < count; i++) {
            fprintf(stderr, "  rect[%u]=(l=%d,t=%d,r=%d,b=%d)\n",
                    i, rp[i].left, rp[i].top, rp[i].right, rp[i].bottom);
        }
    }

    /* Phase 2 command buffer: only buffer inside a real scene -- Clear()
       calls outside BeginScene/EndScene are common (loading-screen/mode-
       transition sequences, confirmed via real trace, see
       [[project_minimap_texture_blinking_investigation]]) and have no
       larger batch to join, so they execute immediately exactly like
       before. */
    if (dev->in_scene) {
        cmdbuf_record_clear(dev, count, rp, flags, color, z, stencil);
        return DD_OK;
    }
    return clear_execute_now(dev, count, rp, flags, color, z, stencil);
}

/* ===================== vertex/rasterizer stage =====================
   Vertex/fragment shader source, the shader-VARIANT cache (a compiled
   GL program per distinct render-state combination), and the shared
   attrib/uniform location bookkeeping (g_locs/g_prog, plus the g_attr_X/
   g_unif_X macros every upload_* function and bind_texture_stages below
   still reference by their original names) now live in d3d7_shader.c/.h
   -- see [[project_shader_variant_system]]. This file only drives that
   subsystem (ensure_shader_program, called once per draw from
   d3d_draw_prepare) and uploads the actual per-draw uniform VALUES the
   active variant's compiled code reads. */

/* D3DBLEND -> GL blend factor. GLES has a direct equivalent for every
   D3D7 blend factor except D3DBLEND_BOTHSRCALPHA/BOTHINVSRCALPHA (D3D7
   legacy two-sided-alpha modes with no GL equivalent, and not worth
   emulating via dual-source blending for this project's scope) -- those
   fall back to the plain SRCALPHA/INVSRCALPHA equivalents, same
   "reasonable approximation over a hard failure" spirit as
   gl_depth_func_from_d3dcmp's default case below. */
static GLenum gl_blend_factor_from_d3dblend(uint32_t b)
{
    switch (b) {
        case D3DBLEND_ZERO:         return GL_ZERO;
        case D3DBLEND_ONE:          return GL_ONE;
        case D3DBLEND_SRCCOLOR:     return GL_SRC_COLOR;
        case D3DBLEND_INVSRCCOLOR:  return GL_ONE_MINUS_SRC_COLOR;
        case D3DBLEND_SRCALPHA:     return GL_SRC_ALPHA;
        case D3DBLEND_INVSRCALPHA:  return GL_ONE_MINUS_SRC_ALPHA;
        case D3DBLEND_DESTALPHA:    return GL_DST_ALPHA;
        case D3DBLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
        case D3DBLEND_DESTCOLOR:    return GL_DST_COLOR;
        case D3DBLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
        case D3DBLEND_SRCALPHASAT:  return GL_SRC_ALPHA_SATURATE;
        default:                    return GL_ONE;
    }
}

static GLenum gl_depth_func_from_d3dcmp(uint32_t cmp)
{
    switch (cmp) {
        case D3DCMP_NEVER:        return GL_NEVER;
        case D3DCMP_LESS:         return GL_LESS;
        case D3DCMP_EQUAL:        return GL_EQUAL;
        case D3DCMP_LESSEQUAL:    return GL_LEQUAL;
        case D3DCMP_GREATER:      return GL_GREATER;
        case D3DCMP_NOTEQUAL:     return GL_NOTEQUAL;
        case D3DCMP_GREATEREQUAL: return GL_GEQUAL;
        case D3DCMP_ALWAYS:       return GL_ALWAYS;
        default:                  return GL_LEQUAL;
    }
}

/* D3DSTENCILOP_INCRSAT/DECRSAT (4/5) are the classic clamp-at-boundary
   ops, matching GL_INCR/GL_DECR's own traditional (non-wrap) semantics.
   D3DSTENCILOP_INCR/DECR (7/8) are the wrap-around variants, matching
   GL_INCR_WRAP/GL_DECR_WRAP -- named confusingly close to their _SAT
   counterparts in the real D3D7 enum, easy to swap by mistake. */
static GLenum gl_stencil_op_from_d3d(uint32_t op)
{
    switch (op) {
        case D3DSTENCILOP_ZERO:    return GL_ZERO;
        case D3DSTENCILOP_REPLACE: return GL_REPLACE;
        case D3DSTENCILOP_INCRSAT: return GL_INCR;
        case D3DSTENCILOP_DECRSAT: return GL_DECR;
        case D3DSTENCILOP_INVERT:  return GL_INVERT;
        case D3DSTENCILOP_INCR:    return GL_INCR_WRAP;
        case D3DSTENCILOP_DECR:    return GL_DECR_WRAP;
        case D3DSTENCILOP_KEEP:
        default:                   return GL_KEEP;
    }
}

/* D3D and GL disagree about which winding is "front-facing" for the
   same clip-space vertex order (a well-known, long-documented D3D<->GL
   porting difference, unrelated to this project's own CPU-buffer
   row-flip in gl_target_read_rgba -- that's a separate, already-handled
   memory-layout concern). Concretely: a triangle D3D calls
   clockwise-wound is counterclockwise-wound in GL's own window space
   for the identical vertices, so D3DCULL_CW/CCW map to the OPPOSITE GL
   winding, always culling GL_BACK. */
static void apply_render_state(const d3d_device7_t *dev, int is_rhw)
{
    const uint32_t *rs = dev->state.render_state;

    /* Used to unconditionally disable depth-test for ALL is_rhw content
       (see [[project_porsche_menu_input_wparam_fix]] for the investigation
       that reverted this) -- root-caused at the time to a real, narrow
       far-plane precision edge case: XYZRHW UI/HUD content drawn at dvZ
       exactly 1.0 empirically failed a LESSEQUAL test against a depth
       buffer cleared to 1.0, for reasons never fully isolated. That fix
       was too broad: Porsche.exe (NFS5) live-traced doing its ENTIRE
       pipeline, including real 3D car geometry (hundreds of vertices per
       draw, not just 2-triangle HUD quads), through XYZRHW -- it
       transforms world/view/proj on its own CPU side and submits
       pre-transformed screen-space vertices for everything, not just
       overlay content. Unconditionally disabling depth-test for is_rhw
       meant a car's near body panels never occluded its own far-side
       geometry (confirmed live: "wheels can be seen from any point, car
       looks transparent for nonseen objects") -- real D3D7 does NOT
       disable depth-testing for RHW vertices; the Z each vertex carries
       is meaningful and meant to be compared against the depth buffer
       exactly like any other content. Respecting ZENABLE here now matches
       real D3D7 semantics. If the original far-plane precision case
       resurfaces (Porsche.exe's own menu/HUD, or another game sharing
       this file, going black again specifically for z=1.0 content), that
       needs its own real fix now that a live debugger is available in
       this sandbox (it wasn't when this workaround was first written) --
       not a blanket re-disable. */
    if (rs[D3DRENDERSTATE_ZENABLE] != 0) {
        gl_state_set_depth_test_enabled(1);
        gl_state_set_depth_func(gl_depth_func_from_d3dcmp(rs[D3DRENDERSTATE_ZFUNC]));
        gl_state_set_depth_mask(rs[D3DRENDERSTATE_ZWRITEENABLE] != 0 ? GL_TRUE : GL_FALSE);
    } else {
        gl_state_set_depth_test_enabled(0);
    }

    /* GL_DITHER: real GL/GLES core state, no shader/uniform involvement
       needed -- dithering only matters when actually rendering into a
       reduced-color-depth (e.g. 16-bit) framebuffer, increasingly rare
       today, but this is a one-line, zero-risk way to honor the app's
       own request either way. */
    gl_state_set_dither_enabled(rs[D3DRENDERSTATE_DITHERENABLE] != 0);

    /* Stencil test: this shim's render targets already genuinely have a
       real stencil buffer (see render.c's gl_target_ensure, which
       attaches a combined GL_DEPTH24_STENCIL8 renderbuffer) -- it was
       just never programmed via glStencilFunc/Op/Mask before this, so
       the hardware/driver-level piece was already there. */
    if (rs[D3DRENDERSTATE_STENCILENABLE] != 0) {
        gl_state_set_stencil_test_enabled(1);
        gl_state_set_stencil_func(gl_depth_func_from_d3dcmp(rs[D3DRENDERSTATE_STENCILFUNC]),
                                   (GLint)rs[D3DRENDERSTATE_STENCILREF], rs[D3DRENDERSTATE_STENCILMASK]);
        gl_state_set_stencil_op(gl_stencil_op_from_d3d(rs[D3DRENDERSTATE_STENCILFAIL]),
                                 gl_stencil_op_from_d3d(rs[D3DRENDERSTATE_STENCILZFAIL]),
                                 gl_stencil_op_from_d3d(rs[D3DRENDERSTATE_STENCILPASS]));
        gl_state_set_stencil_write_mask(rs[D3DRENDERSTATE_STENCILWRITEMASK]);
    } else {
        gl_state_set_stencil_test_enabled(0);
    }

    /* Alpha blending: never wired up at all before this -- every draw
       call was fully opaque regardless of D3DRENDERSTATE_ALPHABLENDENABLE/
       SRCBLEND/DESTBLEND, meaning any effect relying on blending (glows,
       smoke, sparks, additive particle effects -- exactly the kind of
       content this project's remaining "exploded/corrupted 3D" reports
       kept describing) rendered as solid opaque geometry instead. A
       particle system built from many overlapping alpha- or additive-
       blended quads looks like a soft glowing burst when blended
       correctly, and like a harsh, jagged, opaque white mass when it
       isn't -- which is exactly what LONGROAD2_D3D7_STAGE=1 (lighting
       and texturing both forced off, isolating pure vertex/rasterizer
       output) has been showing. */
    if (rs[D3DRENDERSTATE_ALPHABLENDENABLE] != 0) {
        gl_state_set_blend_enabled(1);
        gl_state_set_blend_func(gl_blend_factor_from_d3dblend(rs[D3DRENDERSTATE_SRCBLEND]),
                                 gl_blend_factor_from_d3dblend(rs[D3DRENDERSTATE_DESTBLEND]));
    } else {
        gl_state_set_blend_enabled(0);
    }

    /* XYZRHW vertices bypass world/view/proj entirely, and the vertex
       shader's XYZRHW branch explicitly flips Y there (D3D screen-space
       Y-down -> GL NDC Y-up) -- unlike the regular transform path, which
       has NO explicit Y-flip anywhere (the D3D-vs-GL row/column-major
       matrix-upload trick handles X/Y implicitly, and the CPU-side
       readback row-flip in gl_target_read_rgba handles the memory-layout
       side separately). That extra, RHW-only Y-flip reverses the
       winding parity between "what the game's vertex order means" and
       "what GL's rasterizer sees" relative to the regular path -- so the
       SAME D3DCULL_CW/CCW -> GL_CW/CCW mapping that's correct for
       transformed geometry is backwards for XYZRHW, and would
       backface-cull pre-transformed 2D/HUD quads the real game never
       intended to be culled at all (confirmed empirically: a screen-
       space quad wound the same way real D3D7 content commonly is
       rendered as fully invisible under the default CULLMODE=CCW until
       this was special-cased). Real D3D7 HUD/overlay content is
       essentially always meant to render regardless of winding, so
       disabling culling entirely for this case -- rather than trying to
       thread a separately-inverted winding rule through -- matches
       real-world usage and sidesteps the ambiguity outright. */
    uint32_t cull = rs[D3DRENDERSTATE_CULLMODE];
    if (cull == D3DCULL_NONE || is_rhw) {
        gl_state_set_cull_enabled(0);
    } else {
        gl_state_set_cull_enabled(1);
        gl_state_set_cull_face(GL_BACK);
        gl_state_set_front_face(cull == D3DCULL_CW ? GL_CW : GL_CCW);
    }
}

static void d3dcolor_to_vec4(uint32_t c, float out[4])
{
    out[0] = ((c >> 16) & 0xFFu) / 255.0f;
    out[1] = ((c >> 8) & 0xFFu) / 255.0f;
    out[2] = (c & 0xFFu) / 255.0f;
    out[3] = ((c >> 24) & 0xFFu) / 255.0f;
}

static GLenum gl_wrap_from_d3daddress(uint32_t addr)
{
    switch (addr) {
        case D3DTADDRESS_MIRROR: return GL_MIRRORED_REPEAT;
        case D3DTADDRESS_CLAMP:  return GL_CLAMP_TO_EDGE;
        case D3DTADDRESS_BORDER: return GL_CLAMP_TO_EDGE; /* no GLES equivalent -- closest fallback */
        default:                 return GL_REPEAT;        /* D3DTADDRESS_WRAP, and any unrecognized value */
    }
}

/* D3D7 alpha test: compares the stage combiner's final alpha against a
   reference, discarding the fragment if the comparison fails --
   alpha_test_enabled/alpha_func are now shader_key_t axes (d3d7_shader.c
   resolves the whole comparison into a straight-line GLSL expression at
   shader-build time, see alpha_func_expr), so only the continuous
   reference VALUE still needs a per-draw upload here. Suspected
   explanation for tree/foliage billboards rendering as opaque
   rectangles instead of cutout silhouettes -- see
   [[project-d3d7-shim-coverage-gaps]]. */
static void upload_alpha_test_uniforms(const d3d_device7_t *dev)
{
    const uint32_t *rs = dev->state.render_state;
    if (rs[D3DRENDERSTATE_ALPHATESTENABLE] == 0) return;
    /* Real D3DRENDERSTATE_ALPHAREF is a DWORD 0-255 byte value (like a
       D3DCOLOR channel), not a D3DVALUE float -- normalize to [0,1] to
       compare directly against the shader's own [0,1] alpha. */
    glUniform1f(g_unif_alpha_ref, (float)(rs[D3DRENDERSTATE_ALPHAREF] & 0xFFu) / 255.0f);
}

/* D3D7 fog: fog_enabled/fog_mode (including the FOGTABLEMODE-takes-
   precedence-over-FOGVERTEXMODE resolution) are now shader_key_t axes
   (d3d7_shader.c's compute_shader_key/fog_factor_expr), so only the
   continuous VALUE uniforms (color, start/end/density) still need a
   per-draw upload here. FOGSTART/FOGEND/FOGDENSITY are D3DVALUE (float
   bits in the DWORD, not an integer count) -- same convention as
   Clear()'s own z parameter, see u32_bits_to_float. */
static void upload_fog_uniforms(const d3d_device7_t *dev)
{
    const uint32_t *rs = dev->state.render_state;
    if (rs[D3DRENDERSTATE_FOGENABLE] == 0) return;

    float color[4];
    d3dcolor_to_vec4(rs[D3DRENDERSTATE_FOGCOLOR], color);
    glUniform4fv(g_unif_fog_color, 1, color);
    glUniform1f(g_unif_fog_start, u32_bits_to_float(rs[D3DRENDERSTATE_FOGSTART]));
    glUniform1f(g_unif_fog_end, u32_bits_to_float(rs[D3DRENDERSTATE_FOGEND]));
    glUniform1f(g_unif_fog_density, u32_bits_to_float(rs[D3DRENDERSTATE_FOGDENSITY]));
}

/* D3DRENDERSTATE_CLIPPING/CLIPPLANEENABLE: clipping_enabled and WHICH of
   the 6 planes are active (clip_plane_mask) are now shader_key_t axes
   (d3d7_shader.c emits a discard check only for planes actually set in
   the mask), so only the plane EQUATIONS themselves still need
   uploading -- unconditionally (cheap, rarely-changing state, same
   reasoning as always uploading WORLD/VIEW/PROJ). See k_vs_header's own
   comment on why the plane equations are assumed to be in world space. */
static void upload_clip_plane_uniforms(const d3d_device7_t *dev)
{
    const uint32_t *rs = dev->state.render_state;
    if (rs[D3DRENDERSTATE_CLIPPING] == 0) return;
    glUniform4fv(g_unif_clip_planes, 6, &dev->state.clip_planes[0][0]);
}

/* Binds each stage's texture (if any) to its own GL texture unit,
   applies that stage's D3DTSS_ADDRESSU/V + MAGFILTER/MINFILTER onto the
   bound texture object, and uploads the combiner state (colorOp/
   colorArg1/2, alphaOp/alphaArg1/2, hasTexture) for the fragment
   shader's per-stage template -- see k_fs_header's comment. */
/* Per-stage "what did we last actually upload" snapshot -- lets
   bind_texture_stages skip the GL calls entirely for a stage whose
   resolved state hasn't changed since the previous draw, instead of
   unconditionally re-uploading all ~10-12 GL calls per stage on every
   single draw regardless. Global (not per-device): there's exactly one
   shared GL program/context these uniforms live in, so what matters is
   what's ACTUALLY currently bound there, not which d3d_device7_t issued
   the call that put it there. Two things this game's own real usage
   makes this worth doing for: (1) stages 1-7 are permanently
   DISABLE'd for the entire session (confirmed via trace) -- once
   uploaded as DISABLE once, the shader's own `if (colorOp ==
   D3DTOP_DISABLE) stageActive = false` never reads that stage's other
   uniforms again either, so nothing about them ever needs re-sending;
   (2) the real game confirmed (king.exe.c) to set a texture once and
   then issue many Strided draw calls in a row against it before
   changing textures again -- exactly the pattern this cache is built
   to skip redundant work for. `tw`/`th` are part of the texture
   identity check specifically because gl_target_ensure (render.c)
   resets a texture's wrap/filter to defaults on every resize even when
   its GL object id stays the same -- comparing only the id would miss
   that and leave stale wrap/filter params applied after a resize.
   `surfacePtr` (the raw d3d7 texture-stage surface pointer, not
   dereferenced, just compared) is ALSO part of identity, not just the
   resolved GL id: surface7_Release genuinely calls glDeleteTextures
   once a surface's refcount hits zero (gl_target_destroy), so a GL
   texture id can legitimately get reused by an entirely unrelated LATER
   surface with the same pixel dimensions -- comparing the id alone
   could false-positive a cache hit against stale params for what's
   actually different content. Requiring the surface pointer to ALSO
   match makes that require two independent coincidental resource-reuse
   events at once, not just one. */
typedef struct {
    int valid;
    /* colorOp is tracked for ONE reason only: the "still DISABLE'd"
       fast-path check below (bind_texture_stages skips ALL further
       per-stage work, not just uniform uploads, when a stage stays
       disabled) -- it's real device state, unrelated to the uniform
       uploads this cache otherwise dedups. alphaOp/colorArg1/colorArg2/
       alphaArg1/alphaArg2 used to be tracked here too, back when this
       function uploaded them as uniforms directly -- now that every
       combiner op is resolved into the compiled shader itself
       (shader_key_t, see [[project_shader_variant_system]]), nothing
       reads them from here anymore, so they're gone. */
    uint32_t colorOp, texCoordIndex;
    const void *surfacePtr;
    unsigned tex;
    int tw, th;
    uint32_t addressU, addressV, magFilter, minFilter;
    int hasColorKey;
    uint8_t ckLow[3], ckHigh[3];
} tex_stage_cache_t;
static tex_stage_cache_t g_stage_cache[GL_MAX_TEX_STAGES];
static int g_texture_factor_valid = 0;
static uint32_t g_texture_factor_cache = 0;
static GLuint g_stage_cache_last_prog = 0;

static void bind_texture_stages(const d3d_device7_t *dev)
{
    /* Real bug: g_stage_cache/g_texture_factor_cache are keyed only by
       THIS stage's own tss_state/texture/colorkey fields -- they have no
       idea which GL PROGRAM is currently active. ensure_shader_variant
       (d3d7_shader.c) switches to a different linked program (different
       uniform LOCATION numbers per variant -- see shader_locs_t's own
       comment) whenever ANY shader_key_t axis changes, not just this
       stage's own combiner op. If two consecutive draws keep a stage
       byte-identical but some OTHER axis changes (e.g. FOGENABLE
       toggles), the cache below still says "same as last time, skip
       re-upload" -- but "last time" wrote into the PREVIOUS program's
       locations, and the new program's equivalent uniforms are still at
       their GL-zero-initialized default. A genuinely textured stage
       would silently render as untextured (hasTexture reads 0.0) the
       first time a new variant is used. Fix: invalidate both caches
       whenever the active program itself has changed, same shape as
       g_draw_uniform_cache's own extra invalidation keys below. */
    if (g_prog != g_stage_cache_last_prog) {
        for (int i = 0; i < GL_MAX_TEX_STAGES; i++) g_stage_cache[i].valid = 0;
        g_texture_factor_valid = 0;
        g_stage_cache_last_prog = g_prog;
    }

    uint32_t tfactor_raw = dev->state.render_state[D3DRENDERSTATE_TEXTUREFACTOR];
    if (!g_texture_factor_valid || tfactor_raw != g_texture_factor_cache) {
        float tfactor[4];
        d3dcolor_to_vec4(tfactor_raw, tfactor);
        glUniform4fv(g_unif_texture_factor, 1, tfactor);
        g_texture_factor_cache = tfactor_raw;
        g_texture_factor_valid = 1;
    }

    for (int i = 0; i < GL_MAX_TEX_STAGES; i++) {
        const uint32_t *tss = dev->state.tss_state[i];
        const tex_stage_uniform_locs_t *u = &g_unif_stages[i];
        tex_stage_cache_t *c = &g_stage_cache[i];

        /* Fast path: this stage was DISABLE'd last time too -- the
           shader never reads anything else about a DISABLE'd stage
           (see the comment on tex_stage_cache_t above), so there is
           nothing else to check or upload. */
        if (c->valid && c->colorOp == D3DTOP_DISABLE && tss[D3DTSS_COLOROP] == D3DTOP_DISABLE) {
            continue;
        }

        unsigned tex = 0;
        int tw = 0, th = 0;
        if (dev->textures[i] != NULL && g_surface_interop != NULL) {
            tex = g_surface_interop->get_texture(dev->textures[i], &tw, &th);
        }
        uint32_t addressU = tex != 0 ? tss[D3DTSS_ADDRESSU] : 0u;
        uint32_t addressV = tex != 0 ? tss[D3DTSS_ADDRESSV] : 0u;
        uint32_t magFilter = tex != 0 ? tss[D3DTSS_MAGFILTER] : 0u;
        uint32_t minFilter = tex != 0 ? tss[D3DTSS_MINFILTER] : 0u;

        /* D3DRENDERSTATE_COLORKEYENABLE: this stage's texture is honored
           through the SAME DDCKEY_SRCBLT a game already set via
           IDirectDrawSurface7::SetColorKey for plain 2D Blt/BltFast --
           D3D7 has no separate 3D-only color-key API, this IS the real
           mechanism (see ddraw_gl_get_color_key's own comment). */
        uint8_t ck_low[4] = {0}, ck_high[4] = {0};
        int has_ck = tex != 0 && dev->state.render_state[D3DRENDERSTATE_COLORKEYENABLE] != 0 &&
                     g_surface_interop != NULL && g_surface_interop->get_color_key != NULL &&
                     g_surface_interop->get_color_key(dev->textures[i], ck_low, ck_high);

        int same =
            c->valid &&
            c->texCoordIndex == (tss[D3DTSS_TEXCOORDINDEX] & 0x7u) &&
            c->surfacePtr == dev->textures[i] && c->tex == tex && c->tw == tw && c->th == th &&
            c->addressU == addressU && c->addressV == addressV &&
            c->magFilter == magFilter && c->minFilter == minFilter &&
            c->hasColorKey == has_ck &&
            (!has_ck || (memcmp(c->ckLow, ck_low, 3) == 0 && memcmp(c->ckHigh, ck_high, 3) == 0));

        /* glActiveTexture+glBindTexture go through gl_state_bind_texture_2d
           (src/glstate/) -- a cache genuinely SHARED with render.c's own
           present/GPU-Blt paths, not a cache local to this function. That
           distinction is why this is safe now when it wasn't before: this
           same pairing used to always run unconditionally here, because a
           real test failure once proved something else in this codebase
           (traced to Lock()->surface7_sync_from_gpu, and separately the
           GPU-accelerated Blt path in ddraw.c/render.c) also rebinds
           GL_TEXTURE0 between draws, which a cache blind to those call
           sites would wrongly believe never happened. gl_state_bind_
           texture_2d is updated by every one of those call sites too now,
           so "unchanged, skip it" is actually true when it claims so.

           glTexParameteri below goes through gl_state_set_tex_parameters,
           keyed by the real GL texture id (not the binding unit, since
           wrap/filter is per-TEXTURE-OBJECT state) -- same cross-module
           sharing reasoning as gl_state_bind_texture_2d just above: the
           GPU-Blt path (gl_target_blit) and the GPU-resident-direct
           present path (render_present_gl_target, render.c) both force
           their own wrap/filter on a texture this function may ALSO be
           sampling as a stage texture, so this can't be a cache local to
           this function either -- it has to observe those call sites'
           own writes too, which is why they were converted to the same
           shared cache alongside this one. A real
           LONGROAD2_ACTIVITY_SAMPLE=1 capture found this unconditional
           block costing 5.6% of ALL render-frame samples on its own --
           see [[project_texture_param_cache]]. */
        gl_state_bind_texture_2d(GL_TEXTURE0 + i, tex);
        if (tex != 0) {
            GLint wrap_s = (GLint)gl_wrap_from_d3daddress(addressU);
            GLint wrap_t = (GLint)gl_wrap_from_d3daddress(addressV);
            GLint mag = magFilter == D3DTFG_POINT ? GL_NEAREST : GL_LINEAR;
            GLint min = minFilter == D3DTFN_POINT ? GL_NEAREST : GL_LINEAR;
            gl_state_set_tex_parameters(tex, wrap_s, wrap_t, min, mag);
        }

        if (same) continue;

        /* investigating a "textures go flat white" report where
           D3DRENDERSTATE_COLORKEYENABLE was found to be 1 for literally
           every single Strided draw this session (real trace,
           1,045,533/1,045,533). That alone isn't a bug -- has_ck only
           actually triggers the shader's discard for a texture surface
           that's ALSO really had SetColorKey(SRCBLT, ...) called on it
           -- but there was no visibility into which surfaces that's
           actually true for until now. If the ground/cabin texture
           surface(s) turn out to have has_ck=1 with a key range that's
           too broad (e.g. left over from that same surface object being
           used as a colorkeyed 2D sprite elsewhere), that would explain
           fragments silently discarding instead of texturing --
           "missing", not just wrong-colored. Only fires on an actual
           change now (the cache above already filters out the
           overwhelming majority of redundant repeats), so this is lower
           volume than it used to be, not higher. */
        if (i == 0 && ddraw_trace_enabled()) {
            fprintf(stderr, "[d3d7-trace] bind_texture_stages[0]: device=%p surface=%p tex=%u (%dx%d) "
                            "colorOp=%u colorArg1=%u colorArg2=%u has_ck=%d ck_lo=(%u,%u,%u) ck_hi=(%u,%u,%u)\n",
                    (const void *)dev, dev->textures[i], tex, tw, th,
                    tss[D3DTSS_COLOROP], tss[D3DTSS_COLORARG1], tss[D3DTSS_COLORARG2], has_ck,
                    has_ck ? ck_low[0] : 0u, has_ck ? ck_low[1] : 0u, has_ck ? ck_low[2] : 0u,
                    has_ck ? ck_high[0] : 0u, has_ck ? ck_high[1] : 0u, has_ck ? ck_high[2] : 0u);
        }

        /* Low 3 bits only -- real D3DTSS_TEXCOORDINDEX also packs a
           D3DTSS_TCI_* generation mode (camera-space normal/position/
           reflection-vector, for environment mapping) into the upper
           16 bits. Only the plain "which of this vertex's own UV sets"
           case (TCI_PASSTHRU, the low bits) is implemented -- no
           evidence this game uses environment mapping, and generating
           texture coordinates from view-space vectors instead of
           reading them from the vertex is a separate, much bigger
           feature. Only 2 real UV sets exist in any FVF this game has
           been observed to use (see k_vs_src's a_texcoord1), so this
           can't meaningfully exceed 1 in practice regardless. */
        glUniform1f(u->texCoordIndex, (float)(tss[D3DTSS_TEXCOORDINDEX] & 0x7u));
        /* Real generation-mode bits set (TCI_CAMERASPACENORMAL/POSITION/
           reflection-vector -- environment mapping), silently ignored
           above -- see this stage's own comment on why. Un-gated (prints
           without needing LONGROAD2_DDRAW_TRACE) but only once per
           process, since this is a "this feature exists and is
           unimplemented" fact, not a per-frame diagnostic: if it fires at
           all, every draw using this stage is affected identically every
           time, not intermittently. Prime suspect for a surface that
           renders solid white/wrong ONLY when the player's own vehicle
           becomes visible (3rd-person view) and never otherwise -- 1st-
           person traces analyzed earlier never saw this stage at all,
           since the player's own truck isn't drawn from inside it. */
        if ((tss[D3DTSS_TEXCOORDINDEX] & 0xFFFF0000u) != 0) {
            static int warned = 0;
            if (!warned) {
                warned = 1;
                fprintf(stderr, "[d3d7] WARNING: stage %d used D3DTSS_TEXCOORDINDEX generation mode "
                                "0x%08x (environment/reflection mapping) -- NOT IMPLEMENTED, falling back "
                                "to plain vertex UV set %u instead of generated coordinates.\n",
                        i, tss[D3DTSS_TEXCOORDINDEX], tss[D3DTSS_TEXCOORDINDEX] & 0x7u);
            }
        }

        glUniform1f(u->hasTexture, tex != 0 ? 1.0f : 0.0f);

        glUniform1f(u->hasColorKey, has_ck ? 1.0f : 0.0f);
        if (has_ck) {
            float low3[3] = { ck_low[0] / 255.0f, ck_low[1] / 255.0f, ck_low[2] / 255.0f };
            float high3[3] = { ck_high[0] / 255.0f, ck_high[1] / 255.0f, ck_high[2] / 255.0f };
            glUniform3fv(u->colorKeyLow, 1, low3);
            glUniform3fv(u->colorKeyHigh, 1, high3);
        }

        c->valid = 1;
        c->colorOp = tss[D3DTSS_COLOROP];
        c->texCoordIndex = tss[D3DTSS_TEXCOORDINDEX] & 0x7u;
        c->surfacePtr = dev->textures[i]; c->tex = tex; c->tw = tw; c->th = th;
        c->addressU = addressU; c->addressV = addressV;
        c->magFilter = magFilter; c->minFilter = minFilter;
        c->hasColorKey = has_ck;
        if (has_ck) { memcpy(c->ckLow, ck_low, 3); memcpy(c->ckHigh, ck_high, 3); }
    }
}

static d3dvector_t vec3_normalize(d3dvector_t v)
{
    float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len > 0.00001f) { v.x /= len; v.y /= len; v.z /= len; }
    return v;
}

/* Uploads D3DRENDERSTATE_LIGHTING and everything the shader's
   computeLighting() reads when it's on: material properties (already
   real floats in d3d_state_t, no D3DCOLOR decode needed), global
   ambient, and a compact array (up to GL_MAX_LIGHTS) of the enabled+
   defined D3DLIGHT7s with position/direction pre-transformed into view
   space -- see mat4_transform's comment for why that mirrors exactly
   what the vertex shader itself does for vertex positions. Skips all of
   this work when lighting is off: whichever variant is active for an
   unlit draw doesn't even declare these uniforms anymore (lighting_
   enabled/specular_enabled are shader_key_t axes now, not uniforms --
   see [[project_shader_variant_system]]), so there's nothing to upload
   into regardless -- the early return here remains a real CPU-side
   perf win (skips walking the light array), not just a formality. */
static void upload_lighting_uniforms(const d3d_device7_t *dev)
{
    const uint32_t *rs = dev->state.render_state;
    int enabled = rs[D3DRENDERSTATE_LIGHTING] != 0;
    if (!enabled) return;

    glUniform1i(g_unif_local_viewer, rs[D3DRENDERSTATE_LOCALVIEWER] != 0);
    glUniform1i(g_unif_normalize_normals, rs[D3DRENDERSTATE_NORMALIZENORMALS] != 0);

    float ambient[4];
    d3dcolor_to_vec4(rs[D3DRENDERSTATE_AMBIENT], ambient);
    glUniform4fv(g_unif_global_ambient, 1, ambient);

    const d3dmaterial7_t *mat = &dev->state.material;
    glUniform4f(g_unif_mat_diffuse, mat->diffuse.r, mat->diffuse.g, mat->diffuse.b, mat->diffuse.a);
    glUniform4f(g_unif_mat_ambient, mat->ambient.r, mat->ambient.g, mat->ambient.b, mat->ambient.a);
    glUniform4f(g_unif_mat_specular, mat->specular.r, mat->specular.g, mat->specular.b, mat->specular.a);
    glUniform4f(g_unif_mat_emissive, mat->emissive.r, mat->emissive.g, mat->emissive.b, mat->emissive.a);
    glUniform1f(g_unif_mat_power, mat->power);

    const d3dmatrix_t *view = &dev->state.transforms[2]; /* D3DTRANSFORMSTATE_VIEW */
    int count = 0;
    for (int i = 0; i < MAX_LIGHTS && count < GL_MAX_LIGHTS; i++) {
        if (!dev->state.light_valid[i] || !dev->state.light_enabled[i]) continue;
        const d3dlight7_t *lt = &dev->state.lights[i];
        const light_uniform_locs_t *u = &g_unif_lights[count];

        d3dvector_t pos_vs = mat4_transform(view, lt->position, 1);
        d3dvector_t dir_vs = vec3_normalize(mat4_transform(view, lt->direction, 0));

        glUniform3f(u->position, pos_vs.x, pos_vs.y, pos_vs.z);
        glUniform3f(u->direction, dir_vs.x, dir_vs.y, dir_vs.z);
        glUniform4f(u->diffuse, lt->diffuse.r, lt->diffuse.g, lt->diffuse.b, lt->diffuse.a);
        glUniform4f(u->specular, lt->specular.r, lt->specular.g, lt->specular.b, lt->specular.a);
        glUniform4f(u->ambient, lt->ambient.r, lt->ambient.g, lt->ambient.b, lt->ambient.a);
        glUniform1f(u->range, lt->range);
        glUniform3f(u->atten, lt->attenuation0, lt->attenuation1, lt->attenuation2);
        glUniform1f(u->falloff, lt->falloff);
        /* D3DLIGHT7.theta/phi are full cone angles in radians; the
           shader's spot falloff compares against cosines of the HALF
           angles (matches d7vk's own D3D9-fixed-function convention,
           itself matching real D3D's spot-light formula). */
        glUniform2f(u->cone, cosf(lt->theta * 0.5f), cosf(lt->phi * 0.5f));
        glUniform1f(u->type, (float)lt->type);
        count++;
    }
    glUniform1i(g_unif_light_count, count);
}

/* Shared by DrawPrimitive and DrawIndexedPrimitive below -- real D3D7
   semantics: vertices is a raw, tightly-packed array in the format fvf
   describes (fvf_stride/decode_fvf), indices is NULL for the
   non-indexed call. */

/* Phase 3 (rendering-architecture plan, see [[project_phase2_command_buffer]]
   for Phase 2 and this same memory line for the Phase 3 pivot): unlike
   apply_render_state (Phase 1's glstate cache) and bind_texture_stages
   (its own per-stage g_stage_cache), the uniform uploads below were
   completely unconditional -- re-issued on literally every draw
   regardless of whether dev->state/render_target/rt size actually
   changed since the previous one. A Plan-agent review found this is
   very likely the dominant real per-draw cost given real gameplay
   traces show hundreds of thousands of draws/session, often sharing
   identical state for long runs (same texture/material/lights bound
   across many consecutive draws). Originally a whole-struct memcmp of
   dev->state (safe since d3d_state_t has no embedded pointers -- same
   property render_cmd_t's own snapshot in d3d7_cmdbuf.c already relies
   on); a later perf.data capture of real gameplay found that memcmp
   (~6KB/call) was itself a substantial cost, so this now compares
   dev->state_generation instead (see the comment at d3d_state_t's own
   definition, d3d7_internal.h, for the invariant every dev->state mutator
   must uphold). Global, not per-device -- same reasoning as g_stage_cache:
   one shared GL program regardless of which d3d_device7_t issued a call --
   which is exactly why cached_device below is needed: two DIFFERENT
   devices could otherwise coincidentally reach the same generation number
   while holding different actual state, unlike the old content-based
   memcmp which was safe regardless of device identity. */
typedef struct {
    int valid;
    d3d_device7_t *cached_device;
    uint64_t state_generation;
    /* Not redundant with state_generation: is_rhw is per-draw-call data
       derived from the vertex FVF, never part of d3d_state_t. Also now
       load-bearing for a bigger reason than it used to be -- is_rhw is
       part of shader_key_t (d3d7_shader.c), so changing it switches to
       an entirely DIFFERENT linked program with different uniform
       LOCATION numbers for world/view/proj/lighting/etc. Dropping this
       check would silently leave those uniforms at the new program's
       GL-zero-initialized defaults the first time an RHW draw and a
       non-RHW draw interleave in one scene with no intervening
       SetRenderState call (a real, common pattern -- HUD draws
       interleaved with world draws). */
    int is_rhw;
    void *render_target;
    int rt_w, rt_h; /* vp_px's viewport-fallback below reads *out_rt_w/h when the game hasn't set an explicit viewport -- must be part of the cache key, not just render_target identity */
} draw_uniform_cache_t;
static draw_uniform_cache_t g_draw_uniform_cache;

/* Invalidates the cache -- must be called anywhere gl_state_reset() is
   (render_init, and every independently-created device-group boundary
   in the test suite): unlike g_stage_cache (keyed by real surface
   pointers, naturally unlikely to collide across a fresh test part),
   d3d_state_t is a plain value struct, and a freshly-created device's
   default-initialized state could coincidentally byte-match a previous
   test part's final cached snapshot. Same class of risk Phase 1's
   gl_state_cache hit for a different reason (GL object ID reuse), see
   [[project_phase1_gl_state_cache]]. */
void d3d7_draw_uniform_cache_reset(void)
{
    g_draw_uniform_cache.valid = 0;
    /* ensure_shader_variant's own single-entry cache (d3d7_shader.c) has
       the exact same device-identity-spoofing risk this function's own
       comment describes -- fold its reset in here rather than requiring
       every one of this function's many call sites (Release, every
       test-suite device-group boundary) to separately remember a second
       reset call. */
    d3d7_shader_variant_cache_reset();
}

/* Shared by both draw paths below (flat-array and strided): everything
   up to actually binding vertex attributes, which differs between them
   (one shared buffer + fvf-derived offsets vs. one independent
   pointer/stride per attribute). Returns 0 if nothing further should
   happen -- the render target/shader program aren't ready -- in which
   case the caller must not touch vertex attribs or issue a draw. */
static int d3d_draw_prepare(d3d_device7_t *dev, int is_rhw, int *out_rt_w, int *out_rt_h)
{
    activity_sampler_enter(ACTIVITY_PREPARE_ENSURE_RT);
    if (!ensure_rendering_target(dev, out_rt_w, out_rt_h)) return 0;
    activity_sampler_enter(ACTIVITY_PREPARE_ENSURE_SHADER);
    if (!ensure_shader_program(dev, is_rhw)) return 0;
    activity_sampler_enter(ACTIVITY_PREPARE_RENDER_STATE);
    apply_render_state(dev, is_rhw);

    gl_state_use_program(g_prog);

    activity_sampler_enter(ACTIVITY_PREPARE_UNIFORM_UPLOAD);
    int need_upload = !g_draw_uniform_cache.valid ||
        g_draw_uniform_cache.cached_device != dev ||
        g_draw_uniform_cache.state_generation != dev->state_generation ||
        g_draw_uniform_cache.is_rhw != is_rhw ||
        g_draw_uniform_cache.render_target != dev->render_target ||
        g_draw_uniform_cache.rt_w != *out_rt_w || g_draw_uniform_cache.rt_h != *out_rt_h;

    if (need_upload) {
        glUniformMatrix4fv(g_unif_world, 1, GL_FALSE, dev->state.transforms[1].m); /* D3DTRANSFORMSTATE_WORLD */
        glUniformMatrix4fv(g_unif_view, 1, GL_FALSE, dev->state.transforms[2].m);  /* D3DTRANSFORMSTATE_VIEW */
        glUniformMatrix4fv(g_unif_proj, 1, GL_FALSE, dev->state.transforms[3].m);  /* D3DTRANSFORMSTATE_PROJECTION */

        const d3dviewport7_t *vp = &dev->state.viewport;
        float vx = vp->width != 0 ? (float)vp->x : 0.0f;
        float vy = vp->width != 0 ? (float)vp->y : 0.0f;
        float vw = vp->width != 0 ? (float)vp->width : (float)*out_rt_w;
        float vh = vp->height != 0 ? (float)vp->height : (float)*out_rt_h;
        glUniform4f(g_unif_vp_px, vx, vy, vw, vh);
        glUniform1f(g_unif_point_size, 1.0f);

        upload_lighting_uniforms(dev);
        upload_alpha_test_uniforms(dev);
        upload_fog_uniforms(dev);
        upload_clip_plane_uniforms(dev);

        g_draw_uniform_cache.cached_device = dev;
        g_draw_uniform_cache.state_generation = dev->state_generation;
        g_draw_uniform_cache.is_rhw = is_rhw;
        g_draw_uniform_cache.render_target = dev->render_target;
        g_draw_uniform_cache.rt_w = *out_rt_w;
        g_draw_uniform_cache.rt_h = *out_rt_h;
        g_draw_uniform_cache.valid = 1;
        g_d3d7_perf.uniform_uploads++;
    } else {
        g_d3d7_perf.uniform_skips++;
    }

    activity_sampler_enter(ACTIVITY_PREPARE_BIND_TEXTURE);
    /* Stays unconditional -- already has its own internal per-stage
       dirty cache (g_stage_cache). */
    bind_texture_stages(dev);

    return 1;
}

/* Real per-draw vertex/index data used to go straight from CPU memory
   into glVertexAttribPointer/glDrawElements as a raw client-side array
   pointer -- exactly the same slow path already found and fixed for the
   present/blit paths (see render.c's g_quad_pos_vbo comment): a real
   trace on RK3326/Mali-G31/box86 found a SINGLE such draw call costing
   ~56ms, wildly disproportionate to the actual vertex/pixel count
   involved, the signature of a driver paying a fixed per-call
   validation/upload cost for a client-side pointer instead of sourcing
   from a real GPU buffer -- a well-known slow path on mobile GPU
   drivers, made worse by box86's own call-translation overhead on top.
   That fix never got carried over to the actual game draw path; this
   is that same fix applied here. Two globals (not per-device: exactly
   one GL context, one thread, and content gets fully replaced every
   single draw regardless of which device issued it -- same reasoning
   g_draw_uniform_cache/g_stage_cache already established for other
   shared GL-side state). Re-specified (glBufferData with a fresh data
   pointer, not glBufferSubData) every draw on purpose: this is the
   standard "orphan the buffer" streaming idiom -- it lets the driver
   allocate fresh backing storage if the previous contents are still in
   flight on the GPU, instead of a synchronous stall, without this code
   needing to reason about ring-buffer wraparound itself. */
static GLuint g_stream_vbo = 0;
static GLuint g_stream_ibo = 0;

static void upload_stream_vertices(const void *data, size_t size)
{
    if (g_stream_vbo == 0) glGenBuffers(1, &g_stream_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_stream_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)size, data, GL_DYNAMIC_DRAW);
}

static void upload_stream_indices(const uint16_t *indices, uint32_t index_count)
{
    if (g_stream_ibo == 0) glGenBuffers(1, &g_stream_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_stream_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)index_count * sizeof(uint16_t)),
                 indices, GL_DYNAMIC_DRAW);
}

/* Growable scratch buffer draw_strided_execute_now repacks a strided
   draw's independent streams into before the one VBO upload above --
   same realloc-doubling "ensure" convention as ddraw.c's own
   ensure_present_scratch, reused across draws rather than malloc/freed
   every single one (this runs on every real strided DrawPrimitive
   call, potentially thousands of times per frame). */
static uint8_t *g_strided_scratch = NULL;
static size_t g_strided_scratch_cap = 0;

static uint8_t *ensure_strided_scratch(size_t needed)
{
    if (needed > g_strided_scratch_cap) {
        uint8_t *grown = realloc(g_strided_scratch, needed);
        if (grown == NULL) return NULL;
        g_strided_scratch = grown;
        g_strided_scratch_cap = needed;
    }
    return g_strided_scratch;
}

/* Shared by both draw paths below: re-establishes the render target
   binding and dispatches the actual draw. Called once vertex attributes
   are already bound by the caller -- does NOT disable them afterward
   (deliberately, as of the vertex-attribute state-cache work: the next
   draw's own setup unconditionally re-visits every one of these 5
   attributes anyway, either re-enabling it with a real pointer or
   disabling it via gl_state_set_vertex_attrib_disabled, so leaving them
   enabled here costs nothing and lets gl_state_set_vertex_attrib_array
   skip the real GL calls entirely when consecutive draws share the same
   FVF -- see [[project_state_manager_vertex_attrib_cache]]). Safe with
   render.c's own present/Blt paths interleaving between d3d7 draws only
   because those ALSO route their own attribute enable/disable through
   the same shared gl_state_cache -- a real, load-bearing precondition,
   not incidental; do not reintroduce a raw glEnableVertexAttribArray/
   glDisableVertexAttribArray call anywhere without checking that first.
   has_indices selects glDrawElements (always against g_stream_ibo's
   offset 0, already uploaded by the caller) vs glDrawArrays -- a
   separate flag rather than inferring intent from a pointer, since a
   real bound-buffer offset of 0 and "no indices at all" would otherwise
   both be a NULL-equivalent value. */
static void d3d_draw_dispatch_and_cleanup(d3d_device7_t *dev, uint32_t prim_type,
                                           int has_indices, uint32_t vertex_count, uint32_t index_count)
{
    /* Phase 3b: sole chokepoint for every real glDrawArrays/glDrawElements
       call below, regardless of caller (immediate-mode draw, or
       cmdbuf_flush replaying either an individual command or a merged
       run) -- see g_d3d7_perf.gl_draws's own comment. */
    g_d3d7_perf.gl_draws++;

    /* bind_texture_stages (in d3d_draw_prepare) may have just sampled a
       surface that was itself a D3D7 render target very recently
       (sync_state == SURFACE_GPU_AHEAD, e.g. true render-to-texture, or
       simply re-binding a texture right after some other device rendered
       into it). ddraw_gl_get_texture no longer calls surface7_sync_from_gpu
       itself (that eager sync -- and the framebuffer-rebind risk it used
       to carry -- was removed as pure waste, see
       [[project_ddraw_gpu_blt_glreadpixels_elimination]]), but
       re-establishing the render target binding here is still cheap and
       correct regardless, so it's kept as defense in depth rather than
       trusting the binding d3d_draw_prepare set up further above still
       holds. */
    ensure_rendering_target(dev, NULL, NULL);

    GLenum mode;
    switch (prim_type) {
        case 1:  mode = GL_POINTS; break;         /* D3DPT_POINTLIST */
        case 2:  mode = GL_LINES; break;           /* D3DPT_LINELIST */
        case 3:  mode = GL_LINE_STRIP; break;      /* D3DPT_LINESTRIP */
        case 5:  mode = GL_TRIANGLE_STRIP; break;  /* D3DPT_TRIANGLESTRIP */
        case 6:  mode = GL_TRIANGLE_FAN; break;    /* D3DPT_TRIANGLEFAN */
        default: mode = GL_TRIANGLES; break;       /* D3DPT_TRIANGLELIST */
    }
    if (has_indices) {
        glDrawElements(mode, (GLsizei)index_count, GL_UNSIGNED_SHORT, (const void *)0);
    } else {
        glDrawArrays(mode, 0, (GLsizei)vertex_count);
    }

    if (g_surface_interop != NULL) g_surface_interop->mark_gpu_dirty(dev->render_target);
}

/* Real GL-issuing logic for the flat-FVF-array draw path -- extracted
   verbatim from what used to be d3d_draw_common's own full body (Phase 2
   command buffer, see render_cmd_t's own comment) so it can be called
   either immediately or later, from cmdbuf_flush, with zero logic
   duplicated/changed. */
/* Real GL-touching leaf -- see clear_execute_now_impl's own comment for
   the pattern. Called both from the immediate (not-in-scene) path below
   and from cmdbuf_flush's replay loop (d3d7_cmdbuf.c) -- wrapping it
   here, once, at this shared leaf, covers both without the hot per-draw
   RECORDING path (cmdbuf_record_draw_flat, no GL touched) paying any
   cross-thread cost at all. */
void draw_flat_execute_now_impl(d3d_device7_t *dev, uint32_t prim_type, uint32_t fvf,
                                   const uint8_t *vertices, uint32_t vertex_count,
                                   const uint16_t *indices, uint32_t index_count)
{
    fvf_layout_t layout = decode_fvf(fvf);
    if (layout.stride == 0) return;

    int trace = d3d7_perf_trace_enabled();
    activity_sampler_enter(ACTIVITY_CMDBUF_PREPARE);
    double t0 = trace ? d3d7_perf_now_ms() : 0.0;
    int rt_w = 0, rt_h = 0;
    if (!d3d_draw_prepare(dev, layout.is_rhw, &rt_w, &rt_h)) { activity_sampler_leave(); return; }
    double t1 = trace ? d3d7_perf_now_ms() : 0.0;
    if (trace) g_replay_diag.prepare_ms += t1 - t0;

    activity_sampler_enter(ACTIVITY_CMDBUF_UPLOAD);
    /* Real GPU buffer instead of a raw client-side pointer -- see
       upload_stream_vertices's own comment. The whole flat array is
       already one tightly-packed, single-stride buffer (that's what
       "flat FVF array" means), so this is a single upload no matter how
       many attributes it carries. */
    upload_stream_vertices(vertices, (size_t)vertex_count * (size_t)layout.stride);
    double t2 = trace ? d3d7_perf_now_ms() : 0.0;
    if (trace) g_replay_diag.upload_ms += t2 - t1;

    activity_sampler_enter(ACTIVITY_CMDBUF_ATTRIB);

    gl_state_set_vertex_attrib_array((GLuint)g_attr_pos, layout.is_rhw ? 4 : 3, GL_FLOAT, GL_FALSE,
                                      layout.stride, g_stream_vbo, (const void *)(uintptr_t)0);

    if (layout.has_normal) {
        gl_state_set_vertex_attrib_array((GLuint)g_attr_normal, 3, GL_FLOAT, GL_FALSE,
                                          layout.stride, g_stream_vbo, (const void *)(uintptr_t)layout.normal_offset);
    } else {
        gl_state_set_vertex_attrib_disabled((GLuint)g_attr_normal);
        glVertexAttrib3f((GLuint)g_attr_normal, 0.0f, 0.0f, 1.0f);
    }

    if (layout.has_diffuse) {
        gl_state_set_vertex_attrib_array((GLuint)g_attr_diffuse, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                                          layout.stride, g_stream_vbo, (const void *)(uintptr_t)layout.diffuse_offset);
    } else {
        gl_state_set_vertex_attrib_disabled((GLuint)g_attr_diffuse);
        glVertexAttrib4f((GLuint)g_attr_diffuse, 1.0f, 1.0f, 1.0f, 1.0f); /* opaque white */
    }

    /* Texcoord sets 0 and 1 are wired to the shader -- covers this
       game's own observed max (fvf tex_count never exceeds 2 in real
       gameplay traces); D3DTSS_TEXCOORDINDEX selects between them per
       stage (see k_fs_stage_template). Sets 2-7 (real D3D7 allows up to
       8) aren't wired up -- no evidence this game uses more than 2. */
    if (layout.tex_count >= 1) {
        gl_state_set_vertex_attrib_array((GLuint)g_attr_texcoord0, layout.tex_floats[0] < 2 ? layout.tex_floats[0] : 2,
                                          GL_FLOAT, GL_FALSE, layout.stride, g_stream_vbo,
                                          (const void *)(uintptr_t)layout.tex_offset[0]);
    } else {
        gl_state_set_vertex_attrib_disabled((GLuint)g_attr_texcoord0);
        glVertexAttrib2f((GLuint)g_attr_texcoord0, 0.0f, 0.0f);
    }
    if (layout.tex_count >= 2) {
        gl_state_set_vertex_attrib_array((GLuint)g_attr_texcoord1, layout.tex_floats[1] < 2 ? layout.tex_floats[1] : 2,
                                          GL_FLOAT, GL_FALSE, layout.stride, g_stream_vbo,
                                          (const void *)(uintptr_t)layout.tex_offset[1]);
    } else {
        gl_state_set_vertex_attrib_disabled((GLuint)g_attr_texcoord1);
        glVertexAttrib2f((GLuint)g_attr_texcoord1, 0.0f, 0.0f);
    }

    double t3 = trace ? d3d7_perf_now_ms() : 0.0;
    if (trace) g_replay_diag.attrib_ms += t3 - t2;

    activity_sampler_enter(ACTIVITY_CMDBUF_DISPATCH);
    int has_indices = indices != NULL;
    if (has_indices) upload_stream_indices(indices, index_count);
    d3d_draw_dispatch_and_cleanup(dev, prim_type, has_indices, vertex_count, index_count);
    if (trace) {
        g_replay_diag.dispatch_ms += d3d7_perf_now_ms() - t3;
        g_replay_diag.draw_count++;
    }

    activity_sampler_leave();
}

typedef struct {
    d3d_device7_t *dev; uint32_t prim_type, fvf;
    const uint8_t *vertices; uint32_t vertex_count;
    const uint16_t *indices; uint32_t index_count;
} draw_flat_execute_now_args_t;
static void draw_flat_execute_now_trampoline(void *arg)
{
    draw_flat_execute_now_args_t *a = (draw_flat_execute_now_args_t *)arg;
    draw_flat_execute_now_impl(a->dev, a->prim_type, a->fvf, a->vertices, a->vertex_count, a->indices, a->index_count);
}
void draw_flat_execute_now(d3d_device7_t *dev, uint32_t prim_type, uint32_t fvf,
                            const uint8_t *vertices, uint32_t vertex_count,
                            const uint16_t *indices, uint32_t index_count)
{
    draw_flat_execute_now_args_t a = { dev, prim_type, fvf, vertices, vertex_count, indices, index_count };
    gl_thread_call(draw_flat_execute_now_trampoline, &a);
}

static void d3d_draw_common_impl(d3d_device7_t *dev, uint32_t prim_type, uint32_t fvf,
                             const uint8_t *vertices, uint32_t vertex_count,
                             const uint16_t *indices, uint32_t index_count)
{
    if (vertices == NULL || vertex_count == 0) return;
    /* Phase 2 command buffer: only buffer inside a real scene -- outside
       one, there's no larger batch to join, so execute immediately
       exactly like before (matches d3d_device7_Clear's own same rule). */
    if (dev->in_scene) {
        cmdbuf_record_draw_flat(dev, prim_type, fvf, vertices, vertex_count, indices, index_count);
        return;
    }
    draw_flat_execute_now(dev, prim_type, fvf, vertices, vertex_count, indices, index_count);
}
static void d3d_draw_common(d3d_device7_t *dev, uint32_t prim_type, uint32_t fvf,
                             const uint8_t *vertices, uint32_t vertex_count,
                             const uint16_t *indices, uint32_t index_count)
{
    double t0 = d3d7_call_perf_begin(D3D7_TEST_CALL_DRAWCOMMON);
    d3d_draw_common_impl(dev, prim_type, fvf, vertices, vertex_count, indices, index_count);
    d3d7_call_perf_end(D3D7_TEST_CALL_DRAWCOMMON, t0);
}

/* DrawPrimitiveStrided/DrawIndexedPrimitiveStrided's real vertex source:
   FVF still says WHICH attributes are present (decode_fvf), just not
   where they live relative to each other -- each one is bound straight
   from its own D3DDP_PTRSTRIDE, no repacking into one interleaved
   buffer needed (GL has no problem with different attributes coming
   from entirely separate memory). A present-per-FVF attribute whose
   own pointer is unexpectedly NULL falls back to the same disabled/
   constant-value handling as "FVF says this attribute isn't present"
   in the flat-array path above, rather than dereferencing it. */
/* Real GL-issuing logic for the strided draw path -- extracted verbatim
   from what used to be d3d_draw_common_strided's own full body (Phase 2
   command buffer, see render_cmd_t's own comment) so it can be called
   either immediately or later, from cmdbuf_flush, with zero logic
   duplicated/changed. */
/* Real GL-touching leaf -- same pattern/rationale as
   draw_flat_execute_now_impl just above. */
void draw_strided_execute_now_impl(d3d_device7_t *dev, uint32_t prim_type, uint32_t fvf,
                                      const d3ddrawprimitivestrideddata_t *sd, uint32_t vertex_count,
                                      const uint16_t *indices, uint32_t index_count)
{
    fvf_layout_t layout = decode_fvf(fvf);
    if (layout.stride == 0) return;

    int trace = d3d7_perf_trace_enabled();
    activity_sampler_enter(ACTIVITY_CMDBUF_PREPARE);
    double t0 = trace ? d3d7_perf_now_ms() : 0.0;
    int rt_w = 0, rt_h = 0;
    if (!d3d_draw_prepare(dev, layout.is_rhw, &rt_w, &rt_h)) { activity_sampler_leave(); return; }
    double t1 = trace ? d3d7_perf_now_ms() : 0.0;
    if (trace) g_replay_diag.prepare_ms += t1 - t0;

    activity_sampler_enter(ACTIVITY_CMDBUF_UPLOAD);
    /* Real GPU buffer instead of 1-5 raw client-side pointers (position
       always, plus whichever of normal/diffuse/texcoord0/texcoord1 are
       present) -- see upload_stream_vertices's own comment. Unlike the
       flat path, a strided draw's attributes are independently sourced
       (their own pointer AND stride each, D3DDP_PTRSTRIDE's whole point
       -- see this function's own header comment above), so there's no
       single existing contiguous range to upload as-is. Repacked into
       one tightly-packed (natural-stride) scratch buffer instead, via
       the exact same merge_copy_stream/strided_natural_size helpers
       Phase 3b's merge-run path already uses for the same reason (two
       commands sharing a run can have used different original strides
       for the same attribute; one glVertexAttribPointer call can only
       express one). */
    int present[5];
    uint32_t size_of[5], offset_of[5];
    present[0] = 1; /* position always present, matching the unconditional glEnableVertexAttribArray below */
    present[1] = layout.has_normal && sd->normal.data != NULL;
    present[2] = layout.has_diffuse && sd->diffuse.data != NULL;
    present[3] = layout.tex_count >= 1 && sd->texcoord[0].data != NULL;
    present[4] = layout.tex_count >= 2 && sd->texcoord[1].data != NULL;

    uint32_t total_size = 0;
    for (int w = 0; w < 5; w++) {
        if (!present[w]) continue;
        size_of[w] = strided_natural_size(w, &layout) * vertex_count;
        offset_of[w] = total_size;
        total_size += size_of[w];
    }

    uint8_t *scratch = ensure_strided_scratch(total_size);
    if (scratch == NULL) { activity_sampler_leave(); return; }
    for (int w = 0; w < 5; w++) {
        if (!present[w]) continue;
        merge_copy_stream(scratch + offset_of[w], 0, strided_stream_ptr(w, sd), vertex_count,
                           strided_natural_size(w, &layout));
    }
    upload_stream_vertices(scratch, total_size);
    double t2 = trace ? d3d7_perf_now_ms() : 0.0;
    if (trace) g_replay_diag.upload_ms += t2 - t1; /* includes the repack loop above */

    activity_sampler_enter(ACTIVITY_CMDBUF_ATTRIB);
    gl_state_set_vertex_attrib_array((GLuint)g_attr_pos, layout.is_rhw ? 4 : 3, GL_FLOAT, GL_FALSE,
                                      (GLsizei)strided_natural_size(0, &layout), g_stream_vbo,
                                      (const void *)(uintptr_t)offset_of[0]);

    if (present[1]) {
        gl_state_set_vertex_attrib_array((GLuint)g_attr_normal, 3, GL_FLOAT, GL_FALSE,
                                          (GLsizei)strided_natural_size(1, &layout), g_stream_vbo,
                                          (const void *)(uintptr_t)offset_of[1]);
    } else {
        gl_state_set_vertex_attrib_disabled((GLuint)g_attr_normal);
        glVertexAttrib3f((GLuint)g_attr_normal, 0.0f, 0.0f, 1.0f);
    }

    if (present[2]) {
        gl_state_set_vertex_attrib_array((GLuint)g_attr_diffuse, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                                          (GLsizei)strided_natural_size(2, &layout), g_stream_vbo,
                                          (const void *)(uintptr_t)offset_of[2]);
    } else {
        gl_state_set_vertex_attrib_disabled((GLuint)g_attr_diffuse);
        glVertexAttrib4f((GLuint)g_attr_diffuse, 1.0f, 1.0f, 1.0f, 1.0f);
    }

    if (present[3]) {
        gl_state_set_vertex_attrib_array((GLuint)g_attr_texcoord0, layout.tex_floats[0] < 2 ? layout.tex_floats[0] : 2,
                                          GL_FLOAT, GL_FALSE, (GLsizei)strided_natural_size(3, &layout), g_stream_vbo,
                                          (const void *)(uintptr_t)offset_of[3]);
    } else {
        gl_state_set_vertex_attrib_disabled((GLuint)g_attr_texcoord0);
        glVertexAttrib2f((GLuint)g_attr_texcoord0, 0.0f, 0.0f);
    }
    if (present[4]) {
        gl_state_set_vertex_attrib_array((GLuint)g_attr_texcoord1, layout.tex_floats[1] < 2 ? layout.tex_floats[1] : 2,
                                          GL_FLOAT, GL_FALSE, (GLsizei)strided_natural_size(4, &layout), g_stream_vbo,
                                          (const void *)(uintptr_t)offset_of[4]);
    } else {
        gl_state_set_vertex_attrib_disabled((GLuint)g_attr_texcoord1);
        glVertexAttrib2f((GLuint)g_attr_texcoord1, 0.0f, 0.0f);
    }

    double t3 = trace ? d3d7_perf_now_ms() : 0.0;
    if (trace) g_replay_diag.attrib_ms += t3 - t2;

    activity_sampler_enter(ACTIVITY_CMDBUF_DISPATCH);
    int has_indices = indices != NULL;
    if (has_indices) upload_stream_indices(indices, index_count);
    d3d_draw_dispatch_and_cleanup(dev, prim_type, has_indices, vertex_count, index_count);
    if (trace) {
        g_replay_diag.dispatch_ms += d3d7_perf_now_ms() - t3;
        g_replay_diag.draw_count++;
    }
    activity_sampler_leave();
}

typedef struct {
    d3d_device7_t *dev; uint32_t prim_type, fvf;
    const d3ddrawprimitivestrideddata_t *sd; uint32_t vertex_count;
    const uint16_t *indices; uint32_t index_count;
} draw_strided_execute_now_args_t;
static void draw_strided_execute_now_trampoline(void *arg)
{
    draw_strided_execute_now_args_t *a = (draw_strided_execute_now_args_t *)arg;
    draw_strided_execute_now_impl(a->dev, a->prim_type, a->fvf, a->sd, a->vertex_count, a->indices, a->index_count);
}
void draw_strided_execute_now(d3d_device7_t *dev, uint32_t prim_type, uint32_t fvf,
                               const d3ddrawprimitivestrideddata_t *sd, uint32_t vertex_count,
                               const uint16_t *indices, uint32_t index_count)
{
    draw_strided_execute_now_args_t a = { dev, prim_type, fvf, sd, vertex_count, indices, index_count };
    gl_thread_call(draw_strided_execute_now_trampoline, &a);
}

static void d3d_draw_common_strided_impl(d3d_device7_t *dev, uint32_t prim_type, uint32_t fvf,
                                     const d3ddrawprimitivestrideddata_t *sd, uint32_t vertex_count,
                                     const uint16_t *indices, uint32_t index_count)
{
    if (sd == NULL || sd->position.data == NULL || vertex_count == 0) return;
    /* Phase 2 command buffer: only buffer inside a real scene -- outside
       one, there's no larger batch to join, so execute immediately
       exactly like before (matches d3d_device7_Clear's own same rule). */
    if (dev->in_scene) {
        cmdbuf_record_draw_strided(dev, prim_type, fvf, sd, vertex_count, indices, index_count);
        return;
    }
    draw_strided_execute_now(dev, prim_type, fvf, sd, vertex_count, indices, index_count);
}
static void d3d_draw_common_strided(d3d_device7_t *dev, uint32_t prim_type, uint32_t fvf,
                                     const d3ddrawprimitivestrideddata_t *sd, uint32_t vertex_count,
                                     const uint16_t *indices, uint32_t index_count)
{
    double t0 = d3d7_call_perf_begin(D3D7_TEST_CALL_DRAWCOMMON_STRIDED);
    d3d_draw_common_strided_impl(dev, prim_type, fvf, sd, vertex_count, indices, index_count);
    d3d7_call_perf_end(D3D7_TEST_CALL_DRAWCOMMON_STRIDED, t0);
}

/* DrawPrimitive/DrawIndexedPrimitive: this is THE call the project's
   original trace-derived scope (GOAL.md/AGENT.md Finding 1) claimed zero
   occurrences of -- retracted once real gameplay tracing showed 312,615
   DrawPrimitive + 286,232 DrawIndexedPrimitive calls in a single
   session (see this file's header comment). g_draw_primitive_calls is
   kept as a lightweight opt-in trace counter, not load-bearing for
   rendering itself. */
static long g_draw_primitive_calls = 0;
uint32_t WINAPI d3d_device7_DrawPrimitive(void *self, uint32_t a, uint32_t b, uintptr_t c, uint32_t d, uint32_t e)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    (void)e;
    g_draw_primitive_calls++;
    g_d3d7_perf.draw_calls++;
    g_d3d7_perf.vertices += d;
    if (ddraw_trace_enabled()) {
        const d3dviewport7_t *vp = &dev->state.viewport;
        fprintf(stderr, "[d3d7-trace] DrawPrimitive: device=%p type=%u fvf=0x%08x count=%u is_rhw=%d "
                        "cull=%u rt=%p vp=(%u,%u,%ux%u) dz=(%.4f,%.4f) (total calls so far: %ld)\n",
                self, a, b, d, (b & 0x00Eu) == 0x004u, dev->state.render_state[D3DRENDERSTATE_CULLMODE],
                dev->render_target, vp->x, vp->y, vp->width, vp->height, vp->min_z, vp->max_z, g_draw_primitive_calls);
    }
    d3d_draw_common(dev, a, b, (const uint8_t *)(uintptr_t)c, d, NULL, 0);
    return DD_OK;
}
uint32_t WINAPI d3d_device7_DrawIndexedPrimitive(void *self, uint32_t a, uint32_t b, uintptr_t c, uint32_t d,
                                                          uintptr_t e, uint32_t f, uint32_t g)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    (void)g;
    g_draw_primitive_calls++;
    g_d3d7_perf.draw_calls++;
    g_d3d7_perf.indices += f;
    if (ddraw_trace_enabled()) {
        const d3dviewport7_t *vp = &dev->state.viewport;
        fprintf(stderr, "[d3d7-trace] DrawIndexedPrimitive: device=%p type=%u fvf=0x%08x vcount=%u icount=%u is_rhw=%d "
                        "cull=%u rt=%p vp=(%u,%u,%ux%u) dz=(%.4f,%.4f) (total draw calls so far: %ld)\n",
                self, a, b, d, f, (b & 0x00Eu) == 0x004u, dev->state.render_state[D3DRENDERSTATE_CULLMODE],
                dev->render_target, vp->x, vp->y, vp->width, vp->height, vp->min_z, vp->max_z, g_draw_primitive_calls);
    }
    d3d_draw_common(dev, a, b, (const uint8_t *)(uintptr_t)c, d, (const uint16_t *)(uintptr_t)e, f);
    return DD_OK;
}

/* *Strided variants: confirmed (2026-07, real gameplay trace) to be THIS
   game's actual draw path for exactly the content that was missing --
   truck cabin/3rd-person model, skybox, buildings -- 129,483
   DrawPrimitiveStrided + 202,724 DrawIndexedPrimitiveStrided calls in
   one session, vs. 0 CreateVertexBuffer calls (the *VB path fixed
   earlier turned out not to be what this game actually uses). See
   d3d_draw_common_strided/d3ddrawprimitivestrideddata_t above for the
   real per-attribute-stream layout this reads from -- genuinely
   different from the flat FVF array plain DrawPrimitive/DrawIndexed-
   Primitive use, which is why these couldn't just alias to that path. */
static long g_draw_strided_calls = 0;
uint32_t WINAPI d3d_device7_DrawPrimitiveStrided(void *self, uint32_t a, uint32_t b, uintptr_t c, uint32_t d, uint32_t e)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    (void)e;
    g_draw_strided_calls++;
    g_d3d7_perf.draw_calls++;
    g_d3d7_perf.vertices += d;
    if (ddraw_trace_enabled()) {
        const d3dviewport7_t *vp = &dev->state.viewport;
        const uint32_t *rs = dev->state.render_state;
        fprintf(stderr, "[d3d7-trace] DrawPrimitiveStrided: device=%p type=%u fvf=0x%08x count=%u "
                        "vp=(%u,%u,%ux%u) dz=(%.4f,%.4f) blend=(%u,%u,%u) atest=(%u,%u,%u) ck=%u "
                        "(total calls so far: %ld)\n",
                self, a, b, d, vp->x, vp->y, vp->width, vp->height, vp->min_z, vp->max_z,
                rs[D3DRENDERSTATE_ALPHABLENDENABLE], rs[D3DRENDERSTATE_SRCBLEND], rs[D3DRENDERSTATE_DESTBLEND],
                rs[D3DRENDERSTATE_ALPHATESTENABLE], rs[D3DRENDERSTATE_ALPHAFUNC], rs[D3DRENDERSTATE_ALPHAREF],
                rs[D3DRENDERSTATE_COLORKEYENABLE], g_draw_strided_calls);
    }
    d3d_draw_common_strided(dev, a, b, (const d3ddrawprimitivestrideddata_t *)(uintptr_t)c, d, NULL, 0);
    return DD_OK;
}
uint32_t WINAPI d3d_device7_DrawIndexedPrimitiveStrided(void *self, uint32_t a, uint32_t b, uintptr_t c, uint32_t d,
                                                                 uintptr_t e, uint32_t f, uint32_t g)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    (void)g;
    g_draw_strided_calls++;
    g_d3d7_perf.draw_calls++;
    g_d3d7_perf.indices += f;
    if (ddraw_trace_enabled()) {
        const d3dviewport7_t *vp = &dev->state.viewport;
        const uint32_t *rs = dev->state.render_state;
        fprintf(stderr, "[d3d7-trace] DrawIndexedPrimitiveStrided: device=%p type=%u fvf=0x%08x vcount=%u icount=%u "
                        "vp=(%u,%u,%ux%u) dz=(%.4f,%.4f) blend=(%u,%u,%u) atest=(%u,%u,%u) ck=%u "
                        "(total calls so far: %ld)\n",
                self, a, b, d, f, vp->x, vp->y, vp->width, vp->height, vp->min_z, vp->max_z,
                rs[D3DRENDERSTATE_ALPHABLENDENABLE], rs[D3DRENDERSTATE_SRCBLEND], rs[D3DRENDERSTATE_DESTBLEND],
                rs[D3DRENDERSTATE_ALPHATESTENABLE], rs[D3DRENDERSTATE_ALPHAFUNC], rs[D3DRENDERSTATE_ALPHAREF],
                rs[D3DRENDERSTATE_COLORKEYENABLE], g_draw_strided_calls);
    }
    d3d_draw_common_strided(dev, a, b, (const d3ddrawprimitivestrideddata_t *)(uintptr_t)c, d,
                             (const uint16_t *)(uintptr_t)e, f);
    return DD_OK;
}

/* *VB variants: draw from a previously Lock()'d/filled IDirect3DVertex-
   Buffer7 (vb7_t*, see its own comment) instead of an inline array --
   the standard way real D3D7 games store STATIC meshes (a vehicle
   model, say) that don't need re-uploading every frame, as opposed to
   the plain DrawPrimitive/DrawIndexedPrimitive calls above, which this
   game uses for more dynamic/procedural content built fresh into a CPU
   array each frame. These used to be real no-ops (see stale comment
   this replaced -- written when "no rasterizer existing anywhere in
   this shim" was still true; it no longer is), silently dropping any
   geometry drawn this way -- confirmed to matter empirically: a static
   mesh (the truck cabin) was invisible in both normal rendering and
   LONGROAD2_D3D7_SHOW_VERTS=1's point-cloud view, consistent with its
   draw calls never reaching d3d_draw_common at all rather than being a
   transform/cull/rasterizer bug. vb7_t already carries everything
   d3d_draw_common needs (fvf, stride, the real heap-backed vertex
   data) -- this is the exact same underlying draw, just sourced from
   the vertex buffer's own storage instead of a pointer the game passed
   directly, starting dwStartVertex vertices into it. Bounds-checked
   against the buffer's own vb->num_vertices/data_size (not just
   trusting the game's counts) since, unlike the inline-array variants
   above, we know the real allocated size here and an out-of-range
   dwStartVertex/dwNumVertices would otherwise read past vb->data. */
uint32_t WINAPI d3d_device7_DrawPrimitiveVB(void *self, uint32_t a, uintptr_t b, uint32_t c, uint32_t d, uint32_t e)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    vb7_t *vb = (vb7_t *)(uintptr_t)b;
    (void)e; /* flags */
    g_draw_primitive_calls++;
    g_d3d7_perf.draw_calls++;
    g_d3d7_perf.vertices += d;
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[d3d7-trace] DrawPrimitiveVB: device=%p type=%u vb=%p fvf=0x%08x start=%u count=%u "
                        "(total calls so far: %ld)\n",
                self, a, (void *)vb, vb != NULL ? vb->fvf : 0u, c, d, g_draw_primitive_calls);
    }
    if (vb == NULL || vb->data == NULL || vb->stride == 0) return DD_OK;
    if ((uint64_t)c + (uint64_t)d > vb->num_vertices) return DD_OK;
    d3d_draw_common(dev, a, vb->fvf, vb->data + (size_t)c * vb->stride, d, NULL, 0);
    return DD_OK;
}
static uint32_t WINAPI d3d_device7_DrawIndexedPrimitiveVB(void *self, uint32_t a, uintptr_t b, uint32_t c, uint32_t d,
                                                            uintptr_t e, uint32_t f, uint32_t g)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    vb7_t *vb = (vb7_t *)(uintptr_t)b;
    (void)g; /* flags */
    g_draw_primitive_calls++;
    g_d3d7_perf.draw_calls++;
    g_d3d7_perf.indices += f;
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[d3d7-trace] DrawIndexedPrimitiveVB: device=%p type=%u vb=%p fvf=0x%08x start=%u vcount=%u "
                        "icount=%u (total calls so far: %ld)\n",
                self, a, (void *)vb, vb != NULL ? vb->fvf : 0u, c, d, f, g_draw_primitive_calls);
    }
    if (vb == NULL || vb->data == NULL || vb->stride == 0) return DD_OK;
    if ((uint64_t)c + (uint64_t)d > vb->num_vertices) return DD_OK;
    /* Real D3D7 semantics: indices are relative to the dwStartVertex-th
       vertex (index 0 == that vertex), matching how vertices + c*stride
       below is already the base d3d_draw_common/glDrawElements index
       into -- no separate re-basing of the index values themselves. */
    d3d_draw_common(dev, a, vb->fvf, vb->data + (size_t)c * vb->stride, d,
                     (const uint16_t *)(uintptr_t)e, f);
    return DD_OK;
}
/* D3D3's own IDirect3DDevice3::DrawIndexedPrimitiveVB (real MinGW d3d.h,
   see [[project_swep1rcr_port_scoping]]) has a genuinely narrower real
   signature than D3D7's above -- no separate start_vertex/vertex_count,
   just (primitive_type, vb, indices, index_count, flags). Indices may
   reference anywhere in the whole buffer, matching real D3D3 semantics
   (no way for the caller to restrict the range at all through this call).
   A distinct function rather than reusing the 7-arg one with fabricated
   start/count values, since the two real APIs are genuinely different
   shapes, not just a subset -- bridged from d3d7_d3d3_bridge.c's own
   5-arg wrapper. */
uint32_t WINAPI d3d_device7_DrawIndexedPrimitiveVB3(void *self, uint32_t a, uintptr_t b, uintptr_t c, uint32_t d, uint32_t e)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    vb7_t *vb = (vb7_t *)(uintptr_t)b;
    (void)e; /* flags */
    g_draw_primitive_calls++;
    g_d3d7_perf.draw_calls++;
    g_d3d7_perf.indices += d;
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[d3d7-trace] DrawIndexedPrimitiveVB(D3D3): device=%p type=%u vb=%p fvf=0x%08x icount=%u "
                        "(total calls so far: %ld)\n",
                self, a, (void *)vb, vb != NULL ? vb->fvf : 0u, d, g_draw_primitive_calls);
    }
    if (vb == NULL || vb->data == NULL || vb->stride == 0) return DD_OK;
    d3d_draw_common(dev, a, vb->fvf, vb->data, vb->num_vertices, (const uint16_t *)(uintptr_t)c, d);
    return DD_OK;
}
static uint32_t WINAPI d3d_device7_PreLoad(void *self, uint32_t surface) { (void)self; (void)surface; return DD_OK; }
static uint32_t WINAPI d3d_device7_Load(void *self, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{ (void)self; (void)a; (void)b; (void)c; (void)d; (void)e; return DD_OK; }
uint32_t WINAPI d3d_device7_ValidateDevice(void *self, uint32_t *out_passes)
{ (void)self; if (out_passes != NULL) *out_passes = 1; return DD_OK; }
static uint32_t WINAPI d3d_device7_ComputeSphereVisibility(void *self, uintptr_t centers, uintptr_t radii,
                                                             uint32_t num_spheres, uint32_t flags, uintptr_t out_results)

{
    (void)self; (void)centers; (void)radii; (void)flags;
    uint32_t *results = (uint32_t *)(uintptr_t)out_results;
    if (results != NULL) {
        for (uint32_t i = 0; i < num_spheres; i++) results[i] = 0; /* fully visible, no clip planes crossed */
    }
    return DD_OK;
}

/* ----- transform stage ----- */

/* Dumps WORLD/VIEW/PROJECTION whenever the game sets or multiplies one
   of them, gated by the same LONGROAD2_DDRAW_TRACE already used
   throughout this file -- no separate env var, since this piggybacks on
   tracing the user is already turning on to investigate a specific
   draw call, and logging on every SetTransform/MultiplyTransform (once
   per actual change) is far lower volume than logging on every draw
   call would be. Printed one row per line, matching d3dmatrix_t's own
   row-major layout (_11.._14 / _21.._24 / _31.._34 / _41.._44), so the
   translation row (_41.._43, _44=1 for an affine matrix) and a
   perspective matrix's characteristic shape are both visually obvious
   at a glance rather than needing to be manually reshaped from 16
   numbers on one line. */
static void log_transform_if_traced(const char *label, const float *m)
{
    if (!ddraw_trace_enabled()) return;
    fprintf(stderr, "[d3d7-trace] %s:\n"
                    "[d3d7-trace]   [%9.4f %9.4f %9.4f %9.4f]\n"
                    "[d3d7-trace]   [%9.4f %9.4f %9.4f %9.4f]\n"
                    "[d3d7-trace]   [%9.4f %9.4f %9.4f %9.4f]\n"
                    "[d3d7-trace]   [%9.4f %9.4f %9.4f %9.4f]\n",
            label,
            m[0], m[1], m[2], m[3],
            m[4], m[5], m[6], m[7],
            m[8], m[9], m[10], m[11],
            m[12], m[13], m[14], m[15]);
}
static const char *transform_state_label(uint32_t state)
{
    switch (state) {
        case 1:  return "WORLD";
        case 2:  return "VIEW";
        case 3:  return "PROJECTION";
        default: return NULL; /* WORLD2/WORLD3/TEXTURE* etc. -- not this game's own 3 main transforms */
    }
}
static uint32_t WINAPI d3d_device7_SetTransform_impl(void *self, uint32_t state, d3dmatrix_t *matrix)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (state >= MAX_TRANSFORMS || matrix == NULL) return DDERR_INVALIDPARAMS;
    dev->state.transforms[state] = *matrix;
    dev->state_generation++;
    const char *label = transform_state_label(state);
    if (label != NULL) {
        char full_label[64];
        snprintf(full_label, sizeof(full_label), "SetTransform %s (device=%p)", label, self);
        log_transform_if_traced(full_label, matrix->m);
    }
    return DD_OK;
}
uint32_t WINAPI d3d_device7_SetTransform(void *self, uint32_t state, d3dmatrix_t *matrix)
{
    double t0 = d3d7_call_perf_begin(D3D7_TEST_CALL_SETTRANSFORM);
    uint32_t ret = d3d_device7_SetTransform_impl(self, state, matrix);
    d3d7_call_perf_end(D3D7_TEST_CALL_SETTRANSFORM, t0);
    return ret;
}
uint32_t WINAPI d3d_device7_GetTransform(void *self, uint32_t state, d3dmatrix_t *out_matrix)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (state >= MAX_TRANSFORMS || out_matrix == NULL) return DDERR_INVALIDPARAMS;
    *out_matrix = dev->state.transforms[state];
    return DD_OK;
}
/* Real semantics: concatenates (current * supplied), stores, and applies
   just as SetTransform would -- exact left/right multiply order has no
   observable effect here (nothing renders), a real 4x4 multiply is done
   so a later GetTransform reflects genuine concatenation rather than
   just the last-supplied matrix. */
static uint32_t WINAPI d3d_device7_MultiplyTransform_impl(void *self, uint32_t state, d3dmatrix_t *matrix)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (state >= MAX_TRANSFORMS || matrix == NULL) return DDERR_INVALIDPARAMS;
    dev->state.transforms[state] = mat4_mul(&dev->state.transforms[state], matrix);
    dev->state_generation++;
    const char *label = transform_state_label(state);
    if (label != NULL) {
        char full_label[64];
        snprintf(full_label, sizeof(full_label), "MultiplyTransform %s (device=%p)", label, self);
        log_transform_if_traced(full_label, dev->state.transforms[state].m);
    }
    return DD_OK;
}
uint32_t WINAPI d3d_device7_MultiplyTransform(void *self, uint32_t state, d3dmatrix_t *matrix)
{
    double t0 = d3d7_call_perf_begin(D3D7_TEST_CALL_MULTIPLYTRANSFORM);
    uint32_t ret = d3d_device7_MultiplyTransform_impl(self, state, matrix);
    d3d7_call_perf_end(D3D7_TEST_CALL_MULTIPLYTRANSFORM, t0);
    return ret;
}

/* ----- viewport ----- */
static uint32_t WINAPI d3d_device7_SetViewport_impl(void *self, d3dviewport7_t *vp)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (vp == NULL) return DDERR_INVALIDPARAMS;
    /* Real D3D7 validates dwWidth/dwHeight are nonzero and returns
       D3DERR_INVALIDPARAMS (== DDERR_INVALIDPARAMS, same numeric value)
       without touching the device's current viewport at all when they
       aren't. Live-traced while investigating a black-screen report:
       dx7z.dll's own internal per-draw setup routine (via
       __builtin_return_address, RVA 0x1386 inside dx7z.dll itself, not
       Porsche.exe -- a real third-party EA DLL this project doesn't
       control the source of) calls this with a genuine (0,0,0,0) viewport
       thousands of times per session, evidently relying on real D3D7
       silently rejecting it and leaving whatever viewport was already in
       effect. This shim used to accept it unconditionally, overwriting
       dev->state.viewport (and bumping state_generation, defeating the
       draw-uniform dirty-check cache) even though every downstream reader
       already falls back to the full render target size for a 0-sized
       viewport anyway -- so this alone may or may not be the actual
       visible-black-screen cause (that fallback should produce the same
       numbers either way), but it's a real, confirmed divergence from
       real D3D7 semantics regardless, worth fixing on its own merits. */
    if (vp->width == 0 || vp->height == 0) return DDERR_INVALIDPARAMS;
    dev->state.viewport = *vp;
    dev->state_generation++;
    return DD_OK;
}
uint32_t WINAPI d3d_device7_SetViewport(void *self, d3dviewport7_t *vp)
{
    double t0 = d3d7_call_perf_begin(D3D7_TEST_CALL_SETVIEWPORT);
    uint32_t ret = d3d_device7_SetViewport_impl(self, vp);
    d3d7_call_perf_end(D3D7_TEST_CALL_SETVIEWPORT, t0);
    return ret;
}
/* Real, live-debugged root cause (found comparing a real Wine capture's
   screenshot -- the actual main menu, fully visible -- against this
   port's own black screen, then re-examining the already-flagged-but-
   unresolved "SetViewport called with a genuine (0,0,0,0) viewport,
   thousands of times per session" mystery from earlier in
   [[project_porsche_intro_video_black_screen]]): `dev->state.viewport`
   is zero-initialized at device creation (`d3d_state_init` only ever
   sets `max_z`) and stays that way until the FIRST successful
   `SetViewport` call -- but real D3D7 documents a freshly created
   device's viewport as covering the ENTIRE render target by default, not
   (0,0,0,0). Every actual RENDERING path already treats a 0-sized
   viewport as "use the full render target" (`apply_device_viewport`,
   `d3d_draw_prepare`) -- but this GetViewport QUERY handed back the raw,
   still-zeroed struct verbatim. dx7z.dll's own internal UI-layout code
   (real, closed-source, not this project's) very plausibly reads back
   "the current viewport" via this exact call to compute where a menu
   button/icon quad belongs BEFORE ever calling SetViewport itself --
   receiving (0,0,0,0) here would make any relative/percentage-based
   position math against it degenerate to near-zero, matching the
   independently observed "every small UI-element draw's 4 vertices
   collapse to the identical near-origin (0.010,0.010)" finding exactly.
   Fixed by applying the SAME full-render-target fallback here that
   rendering already uses, keeping GetViewport consistent with what a
   real device would report and with what this shim's own renderer
   actually does. */
uint32_t WINAPI d3d_device7_GetViewport(void *self, d3dviewport7_t *out_vp)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (out_vp == NULL) return DDERR_INVALIDPARAMS;
    d3dviewport7_t vp = dev->state.viewport;
    if ((vp.width == 0 || vp.height == 0) && g_surface_interop != NULL && dev->render_target != NULL) {
        int rt_w = 0, rt_h = 0;
        if (g_surface_interop->get_texture(dev->render_target, &rt_w, &rt_h) != 0 && rt_w > 0 && rt_h > 0) {
            vp.x = 0;
            vp.y = 0;
            vp.width = (uint32_t)rt_w;
            vp.height = (uint32_t)rt_h;
        }
    }
    *out_vp = vp;
    return DD_OK;
}

/* ----- material ----- */
static uint32_t WINAPI d3d_device7_SetMaterial_impl(void *self, d3dmaterial7_t *mat)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (mat == NULL) return DDERR_INVALIDPARAMS;
    dev->state.material = *mat;
    dev->state_generation++;
    return DD_OK;
}
uint32_t WINAPI d3d_device7_SetMaterial(void *self, d3dmaterial7_t *mat)
{
    double t0 = d3d7_call_perf_begin(D3D7_TEST_CALL_SETMATERIAL);
    uint32_t ret = d3d_device7_SetMaterial_impl(self, mat);
    d3d7_call_perf_end(D3D7_TEST_CALL_SETMATERIAL, t0);
    return ret;
}
uint32_t WINAPI d3d_device7_GetMaterial(void *self, d3dmaterial7_t *out_mat)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (out_mat == NULL) return DDERR_INVALIDPARAMS;
    *out_mat = dev->state.material;
    return DD_OK;
}

/* ----- lighting stage ----- */
static uint32_t WINAPI d3d_device7_SetLight_impl(void *self, uint32_t index, d3dlight7_t *light)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (index >= MAX_LIGHTS || light == NULL) return DDERR_INVALIDPARAMS;
    dev->state.lights[index] = *light;
    dev->state.light_valid[index] = 1;
    dev->state_generation++;
    return DD_OK;
}
uint32_t WINAPI d3d_device7_SetLight(void *self, uint32_t index, d3dlight7_t *light)
{
    double t0 = d3d7_call_perf_begin(D3D7_TEST_CALL_SETLIGHT);
    uint32_t ret = d3d_device7_SetLight_impl(self, index, light);
    d3d7_call_perf_end(D3D7_TEST_CALL_SETLIGHT, t0);
    return ret;
}
uint32_t WINAPI d3d_device7_GetLight(void *self, uint32_t index, d3dlight7_t *out_light)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (index >= MAX_LIGHTS || out_light == NULL) return DDERR_INVALIDPARAMS;
    if (!dev->state.light_valid[index]) return DDERR_GENERIC;
    *out_light = dev->state.lights[index];
    return DD_OK;
}
/* Real D3D7 semantics: enabling an undefined light index implicitly
   defines a default light there rather than failing -- approximated by
   just tracking the enable bit regardless of light_valid (no downstream
   consumer needs the synthesized default D3DLIGHT7 data to actually
   exist, since nothing lights any geometry). */
static uint32_t WINAPI d3d_device7_LightEnable_impl(void *self, uint32_t index, uint32_t enable)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (index >= MAX_LIGHTS) return DDERR_INVALIDPARAMS;
    dev->state.light_enabled[index] = enable != 0;
    dev->state_generation++;
    return DD_OK;
}
uint32_t WINAPI d3d_device7_LightEnable(void *self, uint32_t index, uint32_t enable)
{
    double t0 = d3d7_call_perf_begin(D3D7_TEST_CALL_LIGHTENABLE);
    uint32_t ret = d3d_device7_LightEnable_impl(self, index, enable);
    d3d7_call_perf_end(D3D7_TEST_CALL_LIGHTENABLE, t0);
    return ret;
}
uint32_t WINAPI d3d_device7_GetLightEnable(void *self, uint32_t index, uint32_t *out_enable)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (index >= MAX_LIGHTS || out_enable == NULL) return DDERR_INVALIDPARAMS;
    *out_enable = dev->state.light_enabled[index];
    return DD_OK;
}

/* ----- render state ----- */
/* TEMP DIAGNOSTIC (opt-in via LONGROAD2_DDRAW_TRACE): investigating a
   depth-dependent "close geometry loses its texture" report. Every
   render state this game sets was previously invisible to tracing --
   only a real capture can show whether ALPHATESTENABLE/COLORKEYENABLE/
   CLIPPING/ZENABLE/CULLMODE (the only states that can make a fragment
   vanish outright rather than just look wrong) actually get toggled
   during gameplay, and with what values. Named allowlist, not every
   state, to keep log volume down -- and only fires when the value
   actually changes, matching log_transform_if_traced's own "once per
   real change" reasoning. */
static const char *rejection_state_label(uint32_t state)
{
    switch (state) {
        case D3DRENDERSTATE_ZENABLE:          return "ZENABLE";
        case D3DRENDERSTATE_ZWRITEENABLE:      return "ZWRITEENABLE";
        case D3DRENDERSTATE_ZFUNC:             return "ZFUNC";
        case D3DRENDERSTATE_CULLMODE:          return "CULLMODE";
        case D3DRENDERSTATE_ALPHATESTENABLE:   return "ALPHATESTENABLE";
        case D3DRENDERSTATE_ALPHAFUNC:         return "ALPHAFUNC";
        case D3DRENDERSTATE_ALPHAREF:          return "ALPHAREF";
        case D3DRENDERSTATE_ALPHABLENDENABLE:  return "ALPHABLENDENABLE";
        case D3DRENDERSTATE_COLORKEYENABLE:    return "COLORKEYENABLE";
        case D3DRENDERSTATE_CLIPPING:          return "CLIPPING";
        case D3DRENDERSTATE_CLIPPLANEENABLE:   return "CLIPPLANEENABLE";
        case D3DRENDERSTATE_FOGENABLE:         return "FOGENABLE";
        /* Widened from the original narrow allowlist (above) to literally
           every named D3DRENDERSTATE_ this file tracks -- investigating a
           live report of "the same white/wrong-render bug, but now
           unconditionally in 3rd-person view" (where the player's own
           vehicle becomes visible for the first time), user's own read:
           "definitely an issue with combining different rendering
           states." The old allowlist only covered states that can make a
           fragment vanish outright; this adds every state that changes
           what a fragment looks like once it IS drawn -- blend mode,
           shademode, specular, lighting/ambient, fog parameters, dither,
           stencil ops, texture factor -- so a fresh capture bracketing
           the 3rd-person switch shows the FULL state combination in
           effect, not just a guessed-at subset. */
        case D3DRENDERSTATE_SRCBLEND:          return "SRCBLEND";
        case D3DRENDERSTATE_DESTBLEND:         return "DESTBLEND";
        case D3DRENDERSTATE_SHADEMODE:         return "SHADEMODE";
        case D3DRENDERSTATE_SPECULARENABLE:    return "SPECULARENABLE";
        case D3DRENDERSTATE_AMBIENT:           return "AMBIENT";
        case D3DRENDERSTATE_LIGHTING:          return "LIGHTING";
        case D3DRENDERSTATE_LOCALVIEWER:       return "LOCALVIEWER";
        case D3DRENDERSTATE_NORMALIZENORMALS:  return "NORMALIZENORMALS";
        case D3DRENDERSTATE_FOGCOLOR:          return "FOGCOLOR";
        case D3DRENDERSTATE_FOGTABLEMODE:      return "FOGTABLEMODE";
        case D3DRENDERSTATE_FOGSTART:          return "FOGSTART";
        case D3DRENDERSTATE_FOGEND:            return "FOGEND";
        case D3DRENDERSTATE_FOGDENSITY:        return "FOGDENSITY";
        case D3DRENDERSTATE_FOGVERTEXMODE:     return "FOGVERTEXMODE";
        case D3DRENDERSTATE_DITHERENABLE:      return "DITHERENABLE";
        case D3DRENDERSTATE_STENCILENABLE:     return "STENCILENABLE";
        case D3DRENDERSTATE_STENCILFAIL:       return "STENCILFAIL";
        case D3DRENDERSTATE_STENCILZFAIL:      return "STENCILZFAIL";
        case D3DRENDERSTATE_STENCILPASS:       return "STENCILPASS";
        case D3DRENDERSTATE_STENCILFUNC:       return "STENCILFUNC";
        case D3DRENDERSTATE_STENCILREF:        return "STENCILREF";
        case D3DRENDERSTATE_STENCILMASK:       return "STENCILMASK";
        case D3DRENDERSTATE_STENCILWRITEMASK:  return "STENCILWRITEMASK";
        case D3DRENDERSTATE_TEXTUREFACTOR:     return "TEXTUREFACTOR";
        /* Legacy D3D3 states with no pipeline effect yet -- named purely
           so a fresh trace can show whether SWEP1RCR actually calls any
           of these (see the #define block's own comment above). If any
           of these show up with a real value, that's the concrete next
           thing to wire up. */
        case D3DRENDERSTATE_TEXTUREHANDLE:      return "TEXTUREHANDLE";
        case D3DRENDERSTATE_TEXTUREADDRESS:     return "TEXTUREADDRESS";
        case D3DRENDERSTATE_TEXTUREPERSPECTIVE: return "TEXTUREPERSPECTIVE";
        case D3DRENDERSTATE_WRAPU:              return "WRAPU";
        case D3DRENDERSTATE_WRAPV:              return "WRAPV";
        case D3DRENDERSTATE_TEXTUREMAG:         return "TEXTUREMAG";
        case D3DRENDERSTATE_TEXTUREMIN:         return "TEXTUREMIN";
        case D3DRENDERSTATE_TEXTUREMAPBLEND:    return "TEXTUREMAPBLEND";
        case D3DRENDERSTATE_STIPPLEDALPHA:      return "STIPPLEDALPHA";
        case D3DRENDERSTATE_EDGEANTIALIAS:      return "EDGEANTIALIAS";
        case D3DRENDERSTATE_BORDERCOLOR:        return "BORDERCOLOR";
        case D3DRENDERSTATE_TEXTUREADDRESSU:    return "TEXTUREADDRESSU";
        case D3DRENDERSTATE_TEXTUREADDRESSV:    return "TEXTUREADDRESSV";
        case D3DRENDERSTATE_MIPMAPLODBIAS:      return "MIPMAPLODBIAS";
        case D3DRENDERSTATE_ZBIAS:              return "ZBIAS";
        case D3DRENDERSTATE_ANISOTROPY:         return "ANISOTROPY";
        case D3DRENDERSTATE_WRAP0:              return "WRAP0";
        case D3DRENDERSTATE_WRAP1:              return "WRAP1";
        case D3DRENDERSTATE_WRAP2:              return "WRAP2";
        case D3DRENDERSTATE_WRAP3:              return "WRAP3";
        case D3DRENDERSTATE_WRAP4:              return "WRAP4";
        case D3DRENDERSTATE_WRAP5:              return "WRAP5";
        case D3DRENDERSTATE_WRAP6:              return "WRAP6";
        case D3DRENDERSTATE_WRAP7:              return "WRAP7";
        case D3DRENDERSTATE_COLORKEYBLENDENABLE: return "COLORKEYBLENDENABLE";
        default:                               return NULL;
    }
}

/* Same idea as rejection_state_label above, for SetTextureStageState --
   never traced at all before this (see d3d_device7_SetTextureStageState).
   Covers every D3DTSS_ this file defines/actually implements. */
static const char *tss_state_label(uint32_t state)
{
    switch (state) {
        case D3DTSS_COLOROP:       return "COLOROP";
        case D3DTSS_COLORARG1:     return "COLORARG1";
        case D3DTSS_COLORARG2:     return "COLORARG2";
        case D3DTSS_ALPHAOP:       return "ALPHAOP";
        case D3DTSS_ALPHAARG1:     return "ALPHAARG1";
        case D3DTSS_ALPHAARG2:     return "ALPHAARG2";
        case D3DTSS_TEXCOORDINDEX: return "TEXCOORDINDEX";
        case D3DTSS_ADDRESSU:      return "ADDRESSU";
        case D3DTSS_ADDRESSV:      return "ADDRESSV";
        case D3DTSS_MAGFILTER:     return "MAGFILTER";
        case D3DTSS_MINFILTER:     return "MINFILTER";
        /* No pipeline effect yet -- named purely so a fresh trace can
           show whether SWEP1RCR sets any of these (see the #define
           block's own comment above). */
        case D3DTSS_BUMPENVMAT00:  return "BUMPENVMAT00";
        case D3DTSS_BUMPENVMAT01:  return "BUMPENVMAT01";
        case D3DTSS_BUMPENVMAT10:  return "BUMPENVMAT10";
        case D3DTSS_BUMPENVMAT11:  return "BUMPENVMAT11";
        case D3DTSS_ADDRESS:       return "ADDRESS";
        case D3DTSS_BORDERCOLOR:  return "BORDERCOLOR";
        case D3DTSS_MIPFILTER:     return "MIPFILTER";
        case D3DTSS_MIPMAPLODBIAS: return "MIPMAPLODBIAS";
        case D3DTSS_MAXMIPLEVEL:  return "MAXMIPLEVEL";
        case D3DTSS_MAXANISOTROPY: return "MAXANISOTROPY";
        case D3DTSS_BUMPENVLSCALE: return "BUMPENVLSCALE";
        case D3DTSS_BUMPENVLOFFSET: return "BUMPENVLOFFSET";
        case D3DTSS_TEXTURETRANSFORMFLAGS: return "TEXTURETRANSFORMFLAGS";
        default:                   return NULL;
    }
}
/* Real D3D7 IM legacy compatibility: D3DRENDERSTATE_TEXTUREMAPBLEND is a
   single stage-0-only knob that predates the per-stage combiner
   (SetTextureStageState) -- real drivers implement it by setting stage
   0's COLOROP/ALPHAOP/*ARG* to a fixed equivalent, same as this
   function does. SWEP1RCR uses EXACTLY this legacy path and NEVER calls
   SetTextureStageState directly for stage 0's combiner (confirmed via
   two separate real traces, see [[project_swep1rcr_port_scoping]]) --
   so this is the game's ONLY mechanism for controlling whether a draw's
   output alpha comes from diffuse (vertex) alpha alone or is modulated
   by its bound texture's own alpha too. Before this function existed,
   the device's fixed startup default (ALPHAOP=SELECTARG1/TEXTURE, see
   this file's own device-init code) ignored this switch entirely, so
   EVERY draw always used texture alpha only regardless of what the game
   actually asked for here -- the suspected mechanism behind the
   reported "jet stream renders as solid white, no alpha" bug, if that
   effect relies on a fading vertex-alpha trail drawn with
   D3DTBLEND_MODULATE (deliberately diffuse-alpha-only, ignoring
   whatever the trail texture's own alpha channel holds). */
static void apply_legacy_texturemapblend(d3d_device7_t *dev, uint32_t value)
{
    uint32_t *tss0 = dev->state.tss_state[0];
    switch (value) {
        case D3DTBLEND_DECAL:
            tss0[D3DTSS_COLOROP] = D3DTOP_SELECTARG1; tss0[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
            tss0[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG2; tss0[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
            break;
        case D3DTBLEND_DECALALPHA:
            tss0[D3DTSS_COLOROP] = D3DTOP_BLENDTEXTUREALPHA;
            tss0[D3DTSS_COLORARG1] = D3DTA_TEXTURE; tss0[D3DTSS_COLORARG2] = D3DTA_CURRENT;
            tss0[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG2; tss0[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
            break;
        case D3DTBLEND_MODULATEALPHA:
            tss0[D3DTSS_COLOROP] = D3DTOP_MODULATE;
            tss0[D3DTSS_COLORARG1] = D3DTA_TEXTURE; tss0[D3DTSS_COLORARG2] = D3DTA_CURRENT;
            tss0[D3DTSS_ALPHAOP] = D3DTOP_MODULATE;
            tss0[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE; tss0[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
            break;
        case D3DTBLEND_COPY:
            tss0[D3DTSS_COLOROP] = D3DTOP_SELECTARG1; tss0[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
            tss0[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG1; tss0[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
            break;
        case D3DTBLEND_ADD:
            tss0[D3DTSS_COLOROP] = D3DTOP_ADD;
            tss0[D3DTSS_COLORARG1] = D3DTA_TEXTURE; tss0[D3DTSS_COLORARG2] = D3DTA_CURRENT;
            tss0[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG2; tss0[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
            break;
        /* 1-bit-alpha-test mask variants -- rare, no evidence this game
           uses them (only MODULATE/MODULATEALPHA ever observed). Falls
           back to the non-mask sibling, same scope-cut reasoning as
           D3DTOP_BUMPENVMAP's own fallback (this file's fragment
           shader header comment). */
        case D3DTBLEND_DECALMASK:
            apply_legacy_texturemapblend(dev, D3DTBLEND_DECAL);
            return;
        case D3DTBLEND_MODULATEMASK:
            apply_legacy_texturemapblend(dev, D3DTBLEND_MODULATE);
            return;
        case D3DTBLEND_MODULATE:
        default:
            tss0[D3DTSS_COLOROP] = D3DTOP_MODULATE;
            tss0[D3DTSS_COLORARG1] = D3DTA_TEXTURE; tss0[D3DTSS_COLORARG2] = D3DTA_CURRENT;
            tss0[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG2; tss0[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
            break;
    }
}
static uint32_t WINAPI d3d_device7_SetRenderState_impl(void *self, uint32_t state, uint32_t value)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (state >= MAX_RENDERSTATES) return DDERR_INVALIDPARAMS;
    if (ddraw_trace_enabled() && dev->state.render_state[state] != value) {
        const char *label = rejection_state_label(state);
        if (label != NULL) {
            fprintf(stderr, "[d3d7-trace] SetRenderState %s: device=%p %u -> %u\n",
                    label, self, dev->state.render_state[state], value);
        }
    }
    dev->state.render_state[state] = value;
    dev->state_generation++;
    if (state == D3DRENDERSTATE_TEXTUREMAPBLEND) {
        apply_legacy_texturemapblend(dev, value);
    }
    return DD_OK;
}
uint32_t WINAPI d3d_device7_SetRenderState(void *self, uint32_t state, uint32_t value)
{
    double t0 = d3d7_call_perf_begin(D3D7_TEST_CALL_SETRENDERSTATE);
    uint32_t ret = d3d_device7_SetRenderState_impl(self, state, value);
    d3d7_call_perf_end(D3D7_TEST_CALL_SETRENDERSTATE, t0);
    return ret;
}
uint32_t WINAPI d3d_device7_GetRenderState(void *self, uint32_t state, uint32_t *out_value)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (state >= MAX_RENDERSTATES || out_value == NULL) return DDERR_INVALIDPARAMS;
    *out_value = dev->state.render_state[state];
    return DD_OK;
}

/* ----- texture stage: bindings + per-stage state ----- */
static uint32_t WINAPI d3d_device7_SetTexture_impl(void *self, uint32_t stage, void *texture)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (stage >= MAX_TEX_STAGES) return DDERR_INVALIDPARAMS;
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[d3d7-trace] SetTexture: device=%p stage=%u surface=%p\n", self, stage, texture);
    }
    if (texture != NULL) com_addref(texture);
    if (dev->textures[stage] != NULL) com_release(dev->textures[stage]);
    dev->textures[stage] = texture;
    return DD_OK;
}
uint32_t WINAPI d3d_device7_SetTexture(void *self, uint32_t stage, void *texture)
{
    double t0 = d3d7_call_perf_begin(D3D7_TEST_CALL_SETTEXTURE);
    uint32_t ret = d3d_device7_SetTexture_impl(self, stage, texture);
    d3d7_call_perf_end(D3D7_TEST_CALL_SETTEXTURE, t0);
    return ret;
}
uint32_t WINAPI d3d_device7_GetTexture(void *self, uint32_t stage, void **out_texture)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (stage >= MAX_TEX_STAGES || out_texture == NULL) return DDERR_INVALIDPARAMS;
    if (dev->textures[stage] != NULL) com_addref(dev->textures[stage]);
    *out_texture = dev->textures[stage];
    return DD_OK;
}
static uint32_t WINAPI d3d_device7_SetTextureStageState_impl(void *self, uint32_t stage, uint32_t state, uint32_t value)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (stage >= MAX_TEX_STAGES || state >= MAX_TSS_STATES) return DDERR_INVALIDPARAMS;
    if (ddraw_trace_enabled() && dev->state.tss_state[stage][state] != value) {
        const char *label = tss_state_label(state);
        if (label != NULL) {
            fprintf(stderr, "[d3d7-trace] SetTextureStageState %s: device=%p stage=%u %u -> %u\n",
                    label, self, stage, dev->state.tss_state[stage][state], value);
        }
    }
    dev->state.tss_state[stage][state] = value;
    dev->state_generation++;
    return DD_OK;
}
uint32_t WINAPI d3d_device7_SetTextureStageState(void *self, uint32_t stage, uint32_t state, uint32_t value)
{
    double t0 = d3d7_call_perf_begin(D3D7_TEST_CALL_SETTEXTURESTAGESTATE);
    uint32_t ret = d3d_device7_SetTextureStageState_impl(self, stage, state, value);
    d3d7_call_perf_end(D3D7_TEST_CALL_SETTEXTURESTAGESTATE, t0);
    return ret;
}
uint32_t WINAPI d3d_device7_GetTextureStageState(void *self, uint32_t stage, uint32_t state, uint32_t *out_value)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (stage >= MAX_TEX_STAGES || state >= MAX_TSS_STATES || out_value == NULL) return DDERR_INVALIDPARAMS;
    *out_value = dev->state.tss_state[stage][state];
    return DD_OK;
}

/* ----- clip status / user clip planes ----- */
static uint32_t WINAPI d3d_device7_SetClipStatus(void *self, d3dclipstatus_t *status)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (status == NULL) return DDERR_INVALIDPARAMS;
    dev->state.clip_status = *status;
    dev->state_generation++;
    return DD_OK;
}
static uint32_t WINAPI d3d_device7_GetClipStatus(void *self, d3dclipstatus_t *out_status)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (out_status == NULL) return DDERR_INVALIDPARAMS;
    *out_status = dev->state.clip_status;
    return DD_OK;
}
static uint32_t WINAPI d3d_device7_SetClipPlane(void *self, uint32_t index, float *plane)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (index >= MAX_CLIP_PLANES || plane == NULL) return DDERR_INVALIDPARAMS;
    memcpy(dev->state.clip_planes[index], plane, sizeof(float) * 4);
    dev->state.clip_plane_enable |= (1u << index);
    dev->state_generation++;
    return DD_OK;
}
static uint32_t WINAPI d3d_device7_GetClipPlane(void *self, uint32_t index, float *out_plane)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (index >= MAX_CLIP_PLANES || out_plane == NULL) return DDERR_INVALIDPARAMS;
    memcpy(out_plane, dev->state.clip_planes[index], sizeof(float) * 4);
    return DD_OK;
}

/* ----- state blocks -----
   Real BeginStateBlock/EndStateBlock record only the deltas of Set*
   calls issued between them; here every Set* call always writes
   straight into the live dev->state unconditionally (there is no
   separate "recording buffer"), so a full snapshot taken at EndScene
   time already equals exactly those deltas layered onto whatever state
   existed at BeginStateBlock -- behaviorally equivalent for the
   standard Begin -> (Set* calls) -> End -> [later] Apply usage pattern,
   just implemented as a full copy instead of a delta list.

   hardtruck2-recomp: handles are 1-based slots in g_state_blocks, not heap
   pointers cast to a DWORD as in LongRoad2 -- a host pointer does not fit
   a DWORD on a 64-bit host. */
#define MAX_STATE_BLOCKS 1024
static d3d_state_t *g_state_blocks[MAX_STATE_BLOCKS];
static uint32_t state_block_new(const d3d_state_t *from)
{
    for (uint32_t i = 0; i < MAX_STATE_BLOCKS; i++) {
        if (g_state_blocks[i] == NULL) {
            g_state_blocks[i] = malloc(sizeof(d3d_state_t));
            *g_state_blocks[i] = *from;
            return i + 1;
        }
    }
    fprintf(stderr, "[d3d7] out of state blocks (%d)\n", MAX_STATE_BLOCKS);
    return 0;
}
static d3d_state_t *state_block_get(uint32_t handle)
{
    return handle >= 1 && handle <= MAX_STATE_BLOCKS ? g_state_blocks[handle - 1] : NULL;
}
static uint32_t WINAPI d3d_device7_BeginStateBlock(void *self)
{
    ((d3d_device7_t *)self)->in_state_block_record = 1;
    return DD_OK;
}
static uint32_t WINAPI d3d_device7_EndStateBlock(void *self, uint32_t *out_handle)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    dev->in_state_block_record = 0;
    if (out_handle == NULL) return DDERR_INVALIDPARAMS;
    *out_handle = state_block_new(&dev->state);
    return *out_handle ? DD_OK : DDERR_GENERIC;
}
static uint32_t WINAPI d3d_device7_CreateStateBlock(void *self, uint32_t type, uint32_t *out_handle)
{
    (void)type;
    d3d_device7_t *dev = (d3d_device7_t *)self;
    if (out_handle == NULL) return DDERR_INVALIDPARAMS;
    *out_handle = state_block_new(&dev->state);
    return *out_handle ? DD_OK : DDERR_GENERIC;
}
static uint32_t WINAPI d3d_device7_CaptureStateBlock(void *self, uint32_t handle)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    d3d_state_t *block = state_block_get(handle);
    if (block == NULL) return DDERR_INVALIDPARAMS;
    *block = dev->state;
    return DD_OK;
}
static uint32_t WINAPI d3d_device7_ApplyStateBlock(void *self, uint32_t handle)
{
    d3d_device7_t *dev = (d3d_device7_t *)self;
    d3d_state_t *block = state_block_get(handle);
    if (block == NULL) return DDERR_INVALIDPARAMS;
    dev->state = *block;
    dev->state_generation++;
    return DD_OK;
}
static uint32_t WINAPI d3d_device7_DeleteStateBlock(void *self, uint32_t handle)
{
    (void)self;
    d3d_state_t *block = state_block_get(handle);
    if (block == NULL) return DDERR_INVALIDPARAMS;
    free(block);
    g_state_blocks[handle - 1] = NULL;
    return DD_OK;

}

/* ----- caps ----- */
static uint32_t WINAPI d3d_device7_GetCaps(void *self, d3ddevicedesc7_t *out_desc)
{
    (void)self;
    if (out_desc == NULL) return DDERR_INVALIDPARAMS;
    memset(out_desc, 0, sizeof(*out_desc));
    out_desc->dev_caps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY |
                          D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX |
                          D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_TEXTURENONLOCALVIDMEM;

    d3dprimcaps_t prim;
    memset(&prim, 0, sizeof(prim));
    prim.size = sizeof(prim);
    prim.raster_caps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST;
    prim.zcmp_caps = D3DPCMPCAPS_LESSEQUAL;
    prim.shade_caps = D3DPSHADECAPS_COLORGOURAUDRGB;
    prim.texture_caps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA;
    out_desc->line_caps = prim;
    out_desc->tri_caps = prim;

    /* Real, live-traced root cause of the black-screen investigation:
       dwDeviceRenderBitDepth/dwDeviceZBufferBitDepth are BIT-FLAG fields
       (one bit per supported depth), NOT the literal depth number --
       real DDBD_1/2/4/8/16/24/32 are 0x4000/0x2000/0x1000/0x800/0x400/
       0x200/0x100 respectively, not 1/2/4/8/16/24/32. This shim used the
       literal numbers (`16u | 32u` = 0x30), which don't overlap the real
       bit positions AT ALL. Confirmed live via dx7z.dll's own disassembly
       (Porsche.exe's D3D7 middleware): its device-init code masks this
       exact field against 0x800/0x400/0x200/0x100 to decide which of its
       5 hardcoded display modes (all declared 16-bit) are usable, and
       zeroes out every mode table entry whose bit isn't set -- with the
       old 0x30 value, NONE of those real bits were ever set, so dx7z
       invalidated its entire mode table on every device init. That
       zeroed table is what Porsche.exe's own UI code later reads back as
       its "reference resolution", collapsing every 2D UI vertex to a
       single point (traced all the way through: dx7z's mode table ->
       Porsche.exe's DAT_00619784/88/8c reference-resolution cache ->
       the UI-transform scale_x/scale_y that must be nonzero for any menu
       element to have screen-space area). */
    out_desc->device_render_bit_depth = 0x400u | 0x100u; /* DDBD_16 | DDBD_32 */
    out_desc->device_zbuffer_bit_depth = 0x400u; /* DDBD_16 */
    out_desc->min_texture_width = 1; out_desc->min_texture_height = 1;
    out_desc->max_texture_width = 2048; out_desc->max_texture_height = 2048;
    out_desc->max_texture_repeat = 256;
    out_desc->max_texture_aspect_ratio = 2048;
    out_desc->max_anisotropy = 1;
    out_desc->max_active_lights = MAX_LIGHTS;
    out_desc->max_vertex_w = 1.0e10f;
    out_desc->max_user_clip_planes = MAX_CLIP_PLANES;
    out_desc->max_vertex_blend_matrices = 4;
    out_desc->max_texture_blend_stages = MAX_TEX_STAGES;
    out_desc->max_simultaneous_textures = MAX_TEX_STAGES;
    return DD_OK;
}

static const d3d_device7_vtbl_t g_d3d_device7_vtbl = {
    .QueryInterface = (comfn2_t)d3d_device7_QueryInterface,
    .AddRef = d3d_device7_AddRef,
    .Release = d3d_device7_Release,
    .GetCaps = (comfn1_t)d3d_device7_GetCaps,
    .EnumTextureFormats = (comfn2_t)d3d_device7_EnumTextureFormats,
    .BeginScene = d3d_device7_BeginScene,
    .EndScene = d3d_device7_EndScene,
    .GetDirect3D = (comfn1_t)d3d_device7_GetDirect3D,
    .SetRenderTarget = (comfn2_t)d3d_device7_SetRenderTarget,
    .GetRenderTarget = (comfn1_t)d3d_device7_GetRenderTarget,
    .Clear = (comfn6_t)d3d_device7_Clear,
    .SetTransform = (comfn2_t)d3d_device7_SetTransform,
    .GetTransform = (comfn2_t)d3d_device7_GetTransform,
    .SetViewport = (comfn1_t)d3d_device7_SetViewport,
    .MultiplyTransform = (comfn2_t)d3d_device7_MultiplyTransform,
    .GetViewport = (comfn1_t)d3d_device7_GetViewport,
    .SetMaterial = (comfn1_t)d3d_device7_SetMaterial,
    .GetMaterial = (comfn1_t)d3d_device7_GetMaterial,
    .SetLight = (comfn2_t)d3d_device7_SetLight,
    .GetLight = (comfn2_t)d3d_device7_GetLight,
    .SetRenderState = d3d_device7_SetRenderState,
    .GetRenderState = (comfn2_t)d3d_device7_GetRenderState,
    .BeginStateBlock = d3d_device7_BeginStateBlock,
    .EndStateBlock = (comfn1_t)d3d_device7_EndStateBlock,
    .PreLoad = d3d_device7_PreLoad,
    .DrawPrimitive = (comfn5_t)d3d_device7_DrawPrimitive,
    .DrawIndexedPrimitive = (comfn7_t)d3d_device7_DrawIndexedPrimitive,
    .SetClipStatus = (comfn1_t)d3d_device7_SetClipStatus,
    .GetClipStatus = (comfn1_t)d3d_device7_GetClipStatus,
    .DrawPrimitiveStrided = (comfn5_t)d3d_device7_DrawPrimitiveStrided,
    .DrawIndexedPrimitiveStrided = (comfn7_t)d3d_device7_DrawIndexedPrimitiveStrided,
    .DrawPrimitiveVB = (comfn5_t)d3d_device7_DrawPrimitiveVB,
    .DrawIndexedPrimitiveVB = (comfn7_t)d3d_device7_DrawIndexedPrimitiveVB,
    .ComputeSphereVisibility = (comfn5_t)d3d_device7_ComputeSphereVisibility,
    .GetTexture = (comfn2_t)d3d_device7_GetTexture,
    .SetTexture = (comfn2_t)d3d_device7_SetTexture,
    .GetTextureStageState = (comfn3_t)d3d_device7_GetTextureStageState,
    .SetTextureStageState = d3d_device7_SetTextureStageState,
    .ValidateDevice = (comfn1_t)d3d_device7_ValidateDevice,
    .ApplyStateBlock = (comfn1_t)d3d_device7_ApplyStateBlock,
    .CaptureStateBlock = (comfn1_t)d3d_device7_CaptureStateBlock,
    .DeleteStateBlock = (comfn1_t)d3d_device7_DeleteStateBlock,
    .CreateStateBlock = (comfn2_t)d3d_device7_CreateStateBlock,
    .Load = d3d_device7_Load,
    .LightEnable = d3d_device7_LightEnable,
    .GetLightEnable = (comfn2_t)d3d_device7_GetLightEnable,
    .SetClipPlane = (comfn2_t)d3d_device7_SetClipPlane,
    .GetClipPlane = (comfn2_t)d3d_device7_GetClipPlane,
    .GetInfo = fail3, /* D3DDEVICEINFOID debug queries -- optional, no real driver requires supporting these */
};

d3d_device7_t *d3d_device7_create(void *d3d7_owner)
{
    d3d_device7_t *dev = calloc(1, sizeof(*dev));
    dev->lpVtbl = &g_d3d_device7_vtbl;
    dev->refcount = 1;
    dev->d3d7_owner = d3d7_owner;
    d3d_state_init(&dev->state);
    dev->state_generation = 1;
    /* GL-thread-exclusive replay shadow -- see d3d_device7_t's own comment
       on replay_shadow. A plain calloc is enough: its lpVtbl/refcount/
       cmdbuf_slots/etc. are never touched by anything (it's never a real
       COM object, never recorded into), only state/state_generation/
       render_target/textures, which cmdbuf_flush_impl (d3d7_cmdbuf.c)
       fully overwrites at the start of every replay before anything reads
       them. */
    dev->replay_shadow = calloc(1, sizeof(*dev->replay_shadow));
    return dev;
}

/* IDirect3DDevice3 delegation surface (the DX6-era D3D3 bridge) now
   lives in d3d7_d3d3_bridge.c -- see d3d7.h's own header comment and
   [[project_swep1rcr_port_scoping]]. */

/* --- IDirect3D7 (8 methods after IUnknown) --- */

typedef struct {
    comfn2_t QueryInterface;
    comfn0_t AddRef;
    comfn0_t Release;
    comfn2_t EnumDevices;
    comfn3_t CreateDevice;
    comfn3_t CreateVertexBuffer;
    comfn3_t EnumZBufferFormats;
    comfn0_t EvictManagedTextures;
} d3d7_vtbl_t;

typedef struct {
    const d3d7_vtbl_t *lpVtbl;
    uint32_t refcount;
} d3d7_t;

static uint32_t WINAPI d3d7_QueryInterface(void *self, guid_t *riid, void **out)
{
    (void)riid;
    d3d7_t *d3d = (d3d7_t *)self;
    d3d->refcount++;
    if (out != NULL) *out = self;
    return DD_OK;
}
static uint32_t WINAPI d3d7_AddRef(void *self) { return ++((d3d7_t *)self)->refcount; }
static uint32_t WINAPI d3d7_Release(void *self)
{
    d3d7_t *d3d = (d3d7_t *)self;
    uint32_t rc = --d3d->refcount;
    if (rc == 0) { gfx_native_freed(d3d); free(d3d); }
    return rc;
}
/* king.exe's own device-init code specifically checks for RGB565 or
   ARGB4444 texture support (see docs/game_code/king.exe.c) and treats
   finding neither as fatal ("can not find 565 or 4444 texture format",
   confirmed via the game's own warn.log) -- reporting one device that
   claims every capability bit (0xFF-filled desc) is simplest and
   correct: king.exe's own device-init code (FUN_005d6b5f/nearby) only
   accepts a device whose enumeration callback actually ran and marked
   one accepted, so zero enumerated devices is fatal too. */
static uint32_t WINAPI d3d7_EnumDevices(void *self, void *cb_, void *ctx)
{
    (void)self;
    typedef uint32_t (WINAPI *cb_t)(char *, char *, uint8_t *, void *);
    if (cb_ == NULL) return DD_OK;
    uint8_t desc[256];
    memset(desc, 0xFF, sizeof(desc));
    char description[] = "Direct3D HAL";
    char name[] = "Direct3D HAL";
    ((cb_t)cb_)(description, name, desc, ctx);
    return DD_OK;
}

/* Same reasoning as d3d_device7_EnumTextureFormats above -- offering a
   plausible Z-buffer depth is what satisfies this check
   ("can not enum z formats" in the game's own warn.log otherwise).
   Reporting just one fixed 16-bit format is still a simplification --
   real depth-test/write/func against an actual depth/stencil
   renderbuffer IS genuinely real elsewhere (render.c's gl_target_ensure
   /apply_render_state), this just doesn't let the game pick among
   multiple real formats. */
static uint32_t WINAPI d3d7_EnumZBufferFormats(void *self, guid_t *device_iid, void *cb_, void *ctx)
{
    (void)self; (void)device_iid;
    typedef uint32_t (WINAPI *cb_t)(ddpixelformat_t *, void *);
    if (cb_ == NULL) return DD_OK;

    ddpixelformat_t fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.dwSize = sizeof(fmt);
    fmt.dwFlags = DDPF_ZBUFFER;
    fmt.dwRGBBitCount = 16;    /* dwZBufferBitDepth (same union slot) */
    ((cb_t)cb_)(&fmt, ctx);
    return DD_OK;
}

static uint32_t WINAPI d3d7_CreateDevice(void *self, guid_t *rclsid, void *surface, void **out_device)
{
    (void)rclsid;
    d3d_device7_t *dev = d3d_device7_create(self);
    /* Real CreateDevice takes a reference on the initial render-target
       surface, same as a later explicit SetRenderTarget call would. */
    if (surface != NULL) {
        com_addref(surface);
        dev->render_target = surface;
    }
    if (out_device != NULL) *out_device = dev;
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[d3d7-trace] CreateDevice: d3d7=%p surface=%p -> device=%p\n", self, surface, (void *)dev);
    }
    return DD_OK;
}

static uint32_t WINAPI d3d7_CreateVertexBuffer(void *self, d3dvertexbufferdesc_t *desc, void **out_vb, uint32_t flags)
{
    (void)self; (void)flags;
    if (desc == NULL || out_vb == NULL) return DDERR_INVALIDPARAMS;
    *out_vb = vb7_create(desc);
    /* Inventory of every vertex buffer the game ever creates, cross-
       referenceable against DrawPrimitiveVB/DrawIndexedPrimitiveVB's own
       trace (both print the same vb=%p) to find any created here that
       never shows up drawn there -- confirms "never reaches a real
       draw call" for a specific static mesh without rendering anything
       to check it. */
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[d3d7-trace] CreateVertexBuffer: vb=%p fvf=0x%08x num_vertices=%u\n",
                *out_vb, desc->fvf, desc->num_vertices);
    }
    return DD_OK;
}

/* Nothing here ever occupies real video memory (no rasterizer, no real
   texture uploads), so evicting it back is trivially, correctly a no-op
   success rather than the previous generic "unimplemented" failure. */
static uint32_t WINAPI d3d7_EvictManagedTextures(void *self) { (void)self; return DD_OK; }

static const d3d7_vtbl_t g_d3d7_vtbl = {
    .QueryInterface = (comfn2_t)d3d7_QueryInterface,
    .AddRef = d3d7_AddRef,
    .Release = d3d7_Release,
    .EnumDevices = (comfn2_t)d3d7_EnumDevices,
    .CreateDevice = (comfn3_t)d3d7_CreateDevice,
    .CreateVertexBuffer = (comfn3_t)d3d7_CreateVertexBuffer,
    .EnumZBufferFormats = (comfn3_t)d3d7_EnumZBufferFormats,
    .EvictManagedTextures = d3d7_EvictManagedTextures,
};

shim_fn_t shim_d3d7_lookup(const char *name, uint32_t ordinal)
{
    (void)name; (void)ordinal;
    return NULL;
}

void *d3d7_create(void)
{
    d3d7_t *d3d = calloc(1, sizeof(*d3d));
    d3d->lpVtbl = &g_d3d7_vtbl;
    d3d->refcount = 1;
    return d3d;
}
