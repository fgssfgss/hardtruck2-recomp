#ifndef LONGROAD2_D3D7_INTERNAL_H
#define LONGROAD2_D3D7_INTERNAL_H

/*
 * Private to the d3d7 module -- shared by d3d7.c, d3d7_cmdbuf.c (Phase 2
 * command buffer, see [[project_phase2_command_buffer]]), and
 * d3d7_d3d3_bridge.c (the D3D3/DX6-era IDirect3DDevice3 delegation
 * surface, see [[project_swep1rcr_port_scoping]]) -- three files that
 * together implement one logical module and need to reach into each
 * other's normally-static internals (the full d3d_device7_t layout, the
 * real d3d_device7_* methods, the real GL-issuing execute_now functions).
 * NOT part of the module's public API: d3d7.h is what ddraw.c and
 * everything else outside this directory actually includes. Splitting
 * d3d7.c this way (originally over 4100 lines) was purely for
 * readability -- no behavior change, verified via the full existing test
 * suite before and after.
 */

#include "d3d7.h"
#include "ddraw_types.h"
#include <stdint.h>

/* GetEnv-based debug/rollback switch helper (d3d7.c) -- treats VAR=0 the
   same as unset, so a shell script exporting VAR=0 to mean "disabled"
   behaves as expected. Shared here (not duplicated like ddraw.c's own
   separate copy) since d3d7_cmdbuf.c/d3d7_d3d3_bridge.c are genuinely
   part of this same module, just split across files for size -- unlike
   ddraw.c/d3d7.c, which are independent shim components. */
int env_flag_enabled(const char *name);

typedef uint32_t (WINAPI *comfn0_t)(void *t);
typedef uint32_t (WINAPI *comfn1_t)(void *t, uint32_t a);
typedef uint32_t (WINAPI *comfn2_t)(void *t, uint32_t a, uint32_t b);
typedef uint32_t (WINAPI *comfn3_t)(void *t, uint32_t a, uint32_t b, uint32_t c);
typedef uint32_t (WINAPI *comfn5_t)(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
typedef uint32_t (WINAPI *comfn6_t)(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f);
typedef uint32_t (WINAPI *comfn7_t)(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g);

/* Real D3DVERTEXBUFFERDESC -- identical layout in both D3D3 and D3D7 (the
   two versions' CreateVertexBuffer differ in surrounding arg count, not
   this struct), confirmed against a real MinGW d3dtypes.h. Shared here
   (not just d3d7.c) so d3d7_d3d3_bridge.c can build one when relaying
   IDirect3D3::CreateVertexBuffer through to vb7_create. */
#pragma pack(push, 1)
typedef struct {
    uint32_t size, caps, fvf, num_vertices;
} d3dvertexbufferdesc_t;
#pragma pack(pop)

/* IDirect3DVertexBuffer7's real object -- moved here (from d3d7.c) so
   d3d7_d3d3_bridge.c can build one directly for IDirect3D3::CreateVertexBuffer
   (D3D3 wraps this same real storage behind its own, differently-shaped
   IDirect3DVertexBuffer tear-off object in ddraw.c -- see
   [[project_swep1rcr_port_scoping]] -- rather than duplicating a second
   real vertex-storage implementation). */
typedef struct {
    comfn2_t QueryInterface;
    comfn0_t AddRef;
    comfn0_t Release;
    comfn3_t Lock;
    comfn0_t Unlock;
    comfn7_t ProcessVertices;
    comfn1_t GetVertexBufferDesc;
    comfn2_t Optimize;
    comfn7_t ProcessVerticesStrided;
} vb7_vtbl_t;

typedef struct {
    const vb7_vtbl_t *lpVtbl;
    uint32_t refcount;
    uint32_t fvf;
    uint32_t num_vertices;
    uint32_t stride;
    uint8_t *data;
    uint32_t data_size;
    int locked;
} vb7_t;

vb7_t *vb7_create(const d3dvertexbufferdesc_t *desc);
uint32_t WINAPI vb7_AddRef(void *self);
uint32_t WINAPI vb7_Release(void *self);
uint32_t WINAPI vb7_Lock(void *self, uint32_t flags, void **out_data, uint32_t *out_size);
uint32_t WINAPI vb7_Unlock(void *self);
uint32_t WINAPI vb7_GetVertexBufferDesc(void *self, d3dvertexbufferdesc_t *out_desc);
uint32_t WINAPI d3d_device7_DrawPrimitiveVB(void *self, uint32_t a, uintptr_t b, uint32_t c, uint32_t d, uint32_t e);
uint32_t WINAPI d3d_device7_DrawIndexedPrimitiveVB3(void *self, uint32_t a, uintptr_t b, uintptr_t c, uint32_t d, uint32_t e);


typedef struct {
    uint32_t flags, status;
    float minx, maxx, miny, maxy, minz, maxz;
} d3dclipstatus_t;

#define MAX_TRANSFORMS   32  /* real range used: WORLD..WORLD3 (1-6), TEXTURE0-7 (16-23) */
#define MAX_LIGHTS       16
#define MAX_RENDERSTATES 256 /* real range used: up to ~152 (CLIPPLANEENABLE) */
#define MAX_TEX_STAGES   8
#define MAX_TSS_STATES   32  /* real range used: up to 24 (TEXTURETRANSFORMFLAGS) */

/* Moved here (from d3d7.c) so d3d7_shader.c can read them too --
   compute_shader_key needs the render-state flags below, and the
   fixed-function-combiner codegen work these support needs the full
   D3DTSS_/D3DTOP_/D3DTA_ sets. Real values, cross-checked against
   wine-staging's d3dtypes.h, same as every other D3D constant in this
   file. */
#define D3DRENDERSTATE_ALPHATESTENABLE  15u
#define D3DRENDERSTATE_ALPHAFUNC        25u
#define D3DRENDERSTATE_FOGENABLE        28u
#define D3DRENDERSTATE_FOGTABLEMODE     35u
#define D3DRENDERSTATE_FOGVERTEXMODE    140u
#define D3DRENDERSTATE_CLIPPING         136u
#define D3DRENDERSTATE_CLIPPLANEENABLE  152u
#define D3DRENDERSTATE_LIGHTING         137u
#define D3DRENDERSTATE_SPECULARENABLE   29u

#define D3DFOG_NONE   0u
#define D3DFOG_EXP    1u
#define D3DFOG_EXP2   2u
#define D3DFOG_LINEAR 3u

/* Real D3DCLIPPLANE0..5 bit values -- a bitmask of which of the 6 real
   SetClipPlane() slots (index 0-5) CLIPPLANEENABLE actually turns on. */
#define D3DCLIPPLANE0 0x01u
#define D3DCLIPPLANE1 0x02u
#define D3DCLIPPLANE2 0x04u
#define D3DCLIPPLANE3 0x08u
#define D3DCLIPPLANE4 0x10u
#define D3DCLIPPLANE5 0x20u

#define D3DCMP_NEVER        1u
#define D3DCMP_LESS         2u
#define D3DCMP_EQUAL        3u
#define D3DCMP_LESSEQUAL    4u
#define D3DCMP_GREATER      5u
#define D3DCMP_NOTEQUAL     6u
#define D3DCMP_GREATEREQUAL 7u
#define D3DCMP_ALWAYS       8u

/* D3DTEXTURESTAGESTATETYPE -- indices into dev->state.tss_state[stage][*] */
#define D3DTSS_COLOROP       1u
#define D3DTSS_COLORARG1     2u
#define D3DTSS_COLORARG2     3u
#define D3DTSS_ALPHAOP       4u
#define D3DTSS_ALPHAARG1     5u
#define D3DTSS_ALPHAARG2     6u
#define D3DTSS_TEXCOORDINDEX 11u
#define D3DTSS_ADDRESSU      13u
#define D3DTSS_ADDRESSV      14u
#define D3DTSS_MAGFILTER     16u
#define D3DTSS_MINFILTER     17u
/* Also real values, also currently no pipeline effect -- same
   diagnostic-only reasoning as d3d7.c's own render-state block.
   BUMPENVMAT00/01/10/11 + BUMPENVLSCALE/BUMPENVLOFFSET feed
   D3DTOP_BUMPENVMAP(LUMINANCE), which the shader combiner falls back to
   SELECTARG1 for -- these are named here so a trace can show whether
   that fallback path is ever actually reached. TEXTURETRANSFORMFLAGS
   drives projected/transformed texture coordinates, not implemented. */
#define D3DTSS_BUMPENVMAT00       7u
#define D3DTSS_BUMPENVMAT01       8u
#define D3DTSS_BUMPENVMAT10       9u
#define D3DTSS_BUMPENVMAT11       10u
#define D3DTSS_ADDRESS            12u
#define D3DTSS_BORDERCOLOR        15u
#define D3DTSS_MIPFILTER          18u
#define D3DTSS_MIPMAPLODBIAS      19u
#define D3DTSS_MAXMIPLEVEL        20u
#define D3DTSS_MAXANISOTROPY      21u
#define D3DTSS_BUMPENVLSCALE      22u
#define D3DTSS_BUMPENVLOFFSET     23u
#define D3DTSS_TEXTURETRANSFORMFLAGS 24u

/* D3DTEXTUREOP -- D3D7's real, complete enum (1-24; no MULTIPLYADD/LERP/
   TEMP register -- those are D3D8+ additions, not present here). */
#define D3DTOP_DISABLE                   1u
#define D3DTOP_SELECTARG1                2u
#define D3DTOP_SELECTARG2                3u
#define D3DTOP_MODULATE                  4u
#define D3DTOP_MODULATE2X                5u
#define D3DTOP_MODULATE4X                6u
#define D3DTOP_ADD                       7u
#define D3DTOP_ADDSIGNED                 8u
#define D3DTOP_ADDSIGNED2X               9u
#define D3DTOP_SUBTRACT                  10u
#define D3DTOP_ADDSMOOTH                 11u
#define D3DTOP_BLENDDIFFUSEALPHA         12u
#define D3DTOP_BLENDTEXTUREALPHA         13u
#define D3DTOP_BLENDFACTORALPHA          14u
#define D3DTOP_BLENDTEXTUREALPHAPM       15u
#define D3DTOP_BLENDCURRENTALPHA         16u
#define D3DTOP_PREMODULATE               17u
#define D3DTOP_MODULATEALPHA_ADDCOLOR    18u
#define D3DTOP_MODULATECOLOR_ADDALPHA    19u
#define D3DTOP_MODULATEINVALPHA_ADDCOLOR 20u
#define D3DTOP_MODULATEINVCOLOR_ADDALPHA 21u
#define D3DTOP_BUMPENVMAP                22u
#define D3DTOP_BUMPENVMAPLUMINANCE       23u
#define D3DTOP_DOTPRODUCT3               24u

/* D3DTA_* -- select mask (low 4 bits) plus two OR'd-in modifier bits. */
#define D3DTA_SELECTMASK     0x0Fu
#define D3DTA_DIFFUSE        0x00u
#define D3DTA_CURRENT        0x01u
#define D3DTA_TEXTURE        0x02u
#define D3DTA_TFACTOR        0x03u
#define D3DTA_SPECULAR       0x04u
#define D3DTA_COMPLEMENT     0x10u
#define D3DTA_ALPHAREPLICATE 0x20u
#define MAX_CLIP_PLANES  32  /* D3DMAXUSERCLIPPLANES */

/* Everything a state block can capture/restore -- kept as one embedded
   struct so CaptureStateBlock/ApplyStateBlock are a single real struct
   copy instead of ~8 parallel memcpys, and so Phase 2's render_cmd_t
   (d3d7_cmdbuf.c) can snapshot/restore a whole device's state the same
   way. Deliberately excludes texture stage bindings (dev->textures[]):
   real D3D7 state blocks don't capture those either -- see d3d7.c's own
   history comment on this if picked up again.

   IMPORTANT: d3d_device7_t.state_generation (below) must be bumped by
   EVERY function that writes any field of this struct -- d3d7.c's
   d3d_draw_prepare (uniform-cache dedup) and d3d7_cmdbuf.c's
   cmds_mergeable (merge-eligibility) both compare generation numbers
   instead of memcmp'ing this ~6KB struct, so a missed mutator here is a
   silent stale-cache bug, not a compile error. Current mutators: every
   SetTransform/MultiplyTransform/SetViewport/SetMaterial/SetLight/
   LightEnable/SetRenderState/SetTextureStageState/SetClipStatus/
   SetClipPlane, ApplyStateBlock's whole-struct write, and
   d3d_device7_create's initial value -- see each one's own bump in
   d3d7.c. Unlike memcmp (self-verifying regardless of how this struct is
   extended), this invariant is NOT enforced by the compiler: if you add a
   10th mutator, it must bump state_generation too. */
typedef struct {
    d3dmatrix_t transforms[MAX_TRANSFORMS];
    d3dmaterial7_t material;
    d3dlight7_t lights[MAX_LIGHTS];
    uint8_t light_valid[MAX_LIGHTS];
    uint8_t light_enabled[MAX_LIGHTS];
    uint32_t render_state[MAX_RENDERSTATES];
    uint32_t tss_state[MAX_TEX_STAGES][MAX_TSS_STATES];
    d3dviewport7_t viewport;
    d3dclipstatus_t clip_status;
    float clip_planes[MAX_CLIP_PLANES][4];
    uint32_t clip_plane_enable;
} d3d_state_t;

typedef struct {
    comfn2_t QueryInterface;
    comfn0_t AddRef;
    comfn0_t Release;
    comfn1_t GetCaps;
    comfn2_t EnumTextureFormats;
    comfn0_t BeginScene;
    comfn0_t EndScene;
    comfn1_t GetDirect3D;
    comfn2_t SetRenderTarget;
    comfn1_t GetRenderTarget;
    comfn6_t Clear;
    comfn2_t SetTransform;
    comfn2_t GetTransform;
    comfn1_t SetViewport;
    comfn2_t MultiplyTransform;
    comfn1_t GetViewport;
    comfn1_t SetMaterial;
    comfn1_t GetMaterial;
    comfn2_t SetLight;
    comfn2_t GetLight;
    comfn2_t SetRenderState;
    comfn2_t GetRenderState;
    comfn0_t BeginStateBlock;
    comfn1_t EndStateBlock;
    comfn1_t PreLoad;
    comfn5_t DrawPrimitive;
    comfn7_t DrawIndexedPrimitive;
    comfn1_t SetClipStatus;
    comfn1_t GetClipStatus;
    comfn5_t DrawPrimitiveStrided;
    comfn7_t DrawIndexedPrimitiveStrided;
    comfn5_t DrawPrimitiveVB;
    comfn7_t DrawIndexedPrimitiveVB;
    comfn5_t ComputeSphereVisibility;
    comfn2_t GetTexture;
    comfn2_t SetTexture;
    comfn3_t GetTextureStageState;
    comfn3_t SetTextureStageState;
    comfn1_t ValidateDevice;
    comfn1_t ApplyStateBlock;
    comfn1_t CaptureStateBlock;
    comfn1_t DeleteStateBlock;
    comfn2_t CreateStateBlock;
    comfn5_t Load;
    comfn2_t LightEnable;
    comfn2_t GetLightEnable;
    comfn2_t SetClipPlane;
    comfn2_t GetClipPlane;
    comfn3_t GetInfo;
} d3d_device7_vtbl_t;

/* Real D3D7 D3DDP_PTRSTRIDE/D3DDRAWPRIMITIVESTRIDEDDATA layout (cross-
   checked against d3dtypes.h): unlike the flat, tightly-packed FVF array
   DrawPrimitive/DrawIndexedPrimitive use, each attribute stream here is
   independently sourced -- its own base pointer AND its own stride, not
   necessarily related to any other attribute's. 8 texcoord slots
   (D3DDP_MAXTEXCOORD), matching this file's own MAX_TEX_STAGES/
   tex_offset[8] elsewhere. */
typedef struct {
    const void *data;
    uint32_t stride;
} d3ddp_ptrstride_t;
typedef struct {
    d3ddp_ptrstride_t position;
    d3ddp_ptrstride_t normal;
    d3ddp_ptrstride_t diffuse;
    d3ddp_ptrstride_t specular;
    d3ddp_ptrstride_t texcoord[8];
} d3ddrawprimitivestrideddata_t;

/* Phase 2 command buffer's recorded-command type is private to
   d3d7_cmdbuf.c -- an incomplete type here is enough since d3d_device7_t
   only ever holds a pointer to it. */
typedef struct render_cmd_s render_cmd_t;

/* Phase 2 rendering-architecture "Phase 2" (the async/double-buffered
   pipelining sub-phase, see [[project_gl_issuer_thread]]): everything a
   scene's recording needs, now split out of d3d_device7_t into its own
   struct so a device can own TWO of them (cmdbuf_slots[2] below) and
   ping-pong which one is being recorded into vs. replayed, instead of one
   shared set that recording and replay would otherwise have to take
   turns owning synchronously. Every field here is EXACTLY what used to
   live directly on d3d_device7_t -- same growth/reuse-across-scenes
   discipline, just now duplicated once. */
typedef struct {
    render_cmd_t *cmd_buf;
    uint32_t cmd_count, cmd_capacity;
    uint32_t *order;
    uint32_t order_capacity;
    d3d_state_t **state_snapshots;
    uint32_t state_snapshot_count, state_snapshot_capacity;
    d3d_state_t *cmdbuf_last_snapshot;
    uint64_t cmdbuf_last_snapshot_generation;
} cmdbuf_slot_t;

typedef struct d3d_device7 {
    const d3d_device7_vtbl_t *lpVtbl;
    uint32_t refcount;
    void *d3d7_owner;

    void *render_target;   /* IDirectDrawSurface7* -- opaque, AddRef'd/Release'd generically */
    void *textures[MAX_TEX_STAGES]; /* ditto */

    int in_scene;
    int in_state_block_record; /* Begin/EndStateBlock bookkeeping only -- see EndStateBlock */

    d3d_state_t state;
    uint64_t state_generation; /* bumped by every dev->state mutator -- see the comment at d3d_state_t's own definition above */

    /* Phase 2 command buffer (d3d7_cmdbuf.c): Clear()/DrawPrimitive-
       family calls issued between BeginScene and EndScene are recorded
       into cmdbuf_slots[cmdbuf_recording_slot] instead of issuing real GL
       calls immediately, then handed off to the dedicated GL thread
       (gl_thread_post, ASYNC -- EndScene does not wait for replay to
       finish) at EndScene. cmdbuf_recording_slot flips to the other slot
       right before that hand-off, so recording the NEXT scene can start
       immediately, concurrently with the GL thread still replaying THIS
       one -- real depth-2 pipelining. gl_thread_post's own single-slot
       FIFO backpressure (see gl_thread.h) is what guarantees the slot
       being flipped INTO is never still in use by an older, not-yet-
       finished replay by the time recording starts writing into it
       again -- see d3d7_cmdbuf.c's cmdbuf_flush for the exact argument. */
    cmdbuf_slot_t cmdbuf_slots[2];
    int cmdbuf_recording_slot;

    /* GL-thread-EXCLUSIVE shadow of state/render_target/textures/
       state_generation, used ONLY during an async cmdbuf_flush replay
       (d3d7_cmdbuf.c's cmdbuf_flush_impl) -- NEVER read or written by the
       game/recording thread. Necessary because replay can now run
       concurrently with the game thread recording (and mutating dev's
       OWN, real state/render_target/textures for) the NEXT scene -- see
       [[project_gl_issuer_thread]]'s Phase 2 design for the full
       reasoning. Every real-GL-touching function in the replay call tree
       (clear_execute_now_impl, draw_flat_execute_now_impl,
       draw_strided_execute_now_impl, and everything they call --
       ensure_rendering_target_impl, apply_render_state,
       apply_device_viewport, bind_texture_stages, d3d_draw_prepare, the
       uniform-upload functions, ensure_shader_program) only ever reads
       dev->state/render_target/textures (confirmed by grep across both
       d3d7.c and d3d7_shader.c -- nothing else is touched), so passing
       this shadow instead of the real dev to all of them during replay is
       sufficient to fully decouple the two. Allocated once at device
       creation (d3d_device7_create), freed at device destruction
       (d3d_device7_Release) -- its own cmdbuf_slots/cmdbuf_recording_slot
       fields are always zeroed and unused (nothing during replay reads
       them), only state/state_generation/render_target/textures are ever
       touched. */
    struct d3d_device7 *replay_shadow;
} d3d_device7_t;

/* FVF decoding (d3d7.c) -- needed by d3d7_cmdbuf.c to size vertex-data
   copies and pick the right natural per-attribute size for a strided
   stream whose real stride is 0. */
typedef struct {
    int stride;
    int is_rhw;       /* XYZRHW -- already screen-space/pre-transformed */
    int has_normal;
    int normal_offset;
    int has_diffuse;
    int diffuse_offset;
    int has_specular;
    int specular_offset;
    int tex_count;
    int tex_offset[8];
    int tex_floats[8]; /* 1..4 */
} fvf_layout_t;

fvf_layout_t decode_fvf(uint32_t fvf);
uint32_t fvf_stride(uint32_t fvf);

/* Strided-stream helpers (d3d7_cmdbuf.c) -- shared with d3d7.c's own
   draw_strided_execute_now, which reuses them to repack a single
   command's independent position/normal/diffuse/texcoord streams into
   one contiguous, natural-stride buffer for a real VBO upload (see that
   function's own comment). `which`: 0=position 1=normal 2=diffuse
   3=texcoord[0] 4=texcoord[1]. */
uint32_t strided_natural_size(int which, const fvf_layout_t *layout);
const d3ddp_ptrstride_t *strided_stream_ptr(int which, const d3ddrawprimitivestrideddata_t *sd);
void merge_copy_stream(uint8_t *dst, uint32_t dst_voffset,
                        const d3ddp_ptrstride_t *src, uint32_t count, uint32_t natural_size);

/* Generic COM AddRef/Release through just the first 3 (always IUnknown)
   vtable slots -- d3d7.c's own comment on this (com_addref/com_release)
   explains why this works without needing to know a surface's concrete
   layout. Needed by d3d7_cmdbuf.c to hold a real reference on a
   recorded command's render target/textures until it's replayed. */
void com_addref(void *obj);
void com_release(void *obj);

/* Real GL-issuing logic (d3d7.c) -- the actual Clear()/draw bodies. The
   plain names are the gl_thread_call-dispatching wrappers (unchanged by
   Phase 2), used by d3d7.c's own immediate/not-in-scene call sites,
   always passed the REAL dev (safe -- see gl_thread_call's own
   synchronous, blocking semantics). The _impl names are the real bodies,
   called DIRECTLY (bypassing the wrapper's dispatch, since the whole
   replay already runs as one gl_thread_post job) from d3d7_cmdbuf.c's
   cmdbuf_flush_impl/merge_and_execute_*_run, always passed dev->
   replay_shadow, never the real dev -- see d3d_device7_t's own comment on
   replay_shadow for why. */
uint32_t clear_execute_now(d3d_device7_t *dev, uint32_t count, const ddraw_rect_t *rp,
                            uint32_t flags, uint32_t color, uint32_t z, uint32_t stencil);
void draw_flat_execute_now(d3d_device7_t *dev, uint32_t prim_type, uint32_t fvf,
                            const uint8_t *vertices, uint32_t vertex_count,
                            const uint16_t *indices, uint32_t index_count);
void draw_strided_execute_now(d3d_device7_t *dev, uint32_t prim_type, uint32_t fvf,
                               const d3ddrawprimitivestrideddata_t *sd, uint32_t vertex_count,
                               const uint16_t *indices, uint32_t index_count);
uint32_t clear_execute_now_impl(d3d_device7_t *dev, uint32_t count, const ddraw_rect_t *rp,
                                 uint32_t flags, uint32_t color, uint32_t z, uint32_t stencil);
void draw_flat_execute_now_impl(d3d_device7_t *dev, uint32_t prim_type, uint32_t fvf,
                                 const uint8_t *vertices, uint32_t vertex_count,
                                 const uint16_t *indices, uint32_t index_count);
void draw_strided_execute_now_impl(d3d_device7_t *dev, uint32_t prim_type, uint32_t fvf,
                                    const d3ddrawprimitivestrideddata_t *sd, uint32_t vertex_count,
                                    const uint16_t *indices, uint32_t index_count);

/* Phase 2 command buffer (d3d7_cmdbuf.c) -- called from d3d7.c's
   Clear()/draw wrappers and BeginScene/EndScene/Release. */
/* Shared with d3d7_cmdbuf.c specifically so cmdbuf_flush below can time
   itself with the same trace-gated clock_gettime idiom every other perf
   counter in this module uses, rather than duplicating the env-cache
   logic in a second file. See d3d7.c's own comment on these two. */
double d3d7_perf_now_ms(void);
int d3d7_perf_trace_enabled(void);
/* Records one cmdbuf_flush call's wall time into g_d3d7_perf (d3d7.c),
   summed into d3d7_perf_frame_report_t's cmdbuf_flush_ms/_calls -- added
   after a real BOX86_DYNAREC_LOG=2 capture showed cmdbuf_build_order's
   qsort comparators (ptr_compare/order_compare) dominating dynarec block
   execution counts, and this function's own body (build_order + the
   full merge/replay loop, including the real draw_flat_execute_now/
   draw_strided_execute_now GL-issuing calls at replay time) had never
   been wall-clock timed at all -- only cmdbuf_flush's downstream GPU-
   completion cost was ever captured, via the trailing glFinish() in
   d3d7_perf_report_for_frame. See [[project_box86_dynarec_log_analysis]]. */
void d3d7_perf_record_cmdbuf_flush(double ms);
/* TEMP DIAGNOSTIC -- see its own definition comment in d3d7.c. */
void d3d7_replay_diag_report_and_reset(double *prepare_ms, double *upload_ms, double *attrib_ms,
                                        double *dispatch_ms, long *draw_count);

void cmdbuf_flush(d3d_device7_t *dev);
void cmdbuf_discard(d3d_device7_t *dev);
void cmdbuf_record_clear(d3d_device7_t *dev, uint32_t count, const ddraw_rect_t *rp,
                          uint32_t flags, uint32_t color, uint32_t z, uint32_t stencil);
void cmdbuf_record_draw_flat(d3d_device7_t *dev, uint32_t prim_type, uint32_t fvf,
                              const uint8_t *vertices, uint32_t vertex_count,
                              const uint16_t *indices, uint32_t index_count);
void cmdbuf_record_draw_strided(d3d_device7_t *dev, uint32_t prim_type, uint32_t fvf,
                                 const d3ddrawprimitivestrideddata_t *sd, uint32_t vertex_count,
                                 const uint16_t *indices, uint32_t index_count);

/* Real d3d_device7_* methods + constructor (d3d7.c), needed by
   d3d7_d3d3_bridge.c to implement d3d7.h's public d3d7_device_*
   delegation wrappers by forwarding to these -- no logic duplicated. */
d3d_device7_t *d3d_device7_create(void *d3d7_owner);
uint32_t WINAPI d3d_device7_Release(void *self);
uint32_t WINAPI d3d_device7_BeginScene(void *self);
uint32_t WINAPI d3d_device7_EndScene(void *self);
uint32_t WINAPI d3d_device7_SetRenderTarget(void *self, void *surface, uint32_t flags);
uint32_t WINAPI d3d_device7_GetRenderTarget(void *self, void **out_surface);
uint32_t WINAPI d3d_device7_EnumTextureFormats(void *self, void *cb_, void *ctx);
uint32_t WINAPI d3d_device7_SetRenderState(void *self, uint32_t state, uint32_t value);
uint32_t WINAPI d3d_device7_GetRenderState(void *self, uint32_t state, uint32_t *out_value);
uint32_t WINAPI d3d_device7_SetTextureStageState(void *self, uint32_t stage, uint32_t state, uint32_t value);
uint32_t WINAPI d3d_device7_GetTextureStageState(void *self, uint32_t stage, uint32_t state, uint32_t *out_value);
uint32_t WINAPI d3d_device7_SetTexture(void *self, uint32_t stage, void *texture);
uint32_t WINAPI d3d_device7_GetTexture(void *self, uint32_t stage, void **out_texture);
uint32_t WINAPI d3d_device7_SetTransform(void *self, uint32_t state, d3dmatrix_t *matrix);
uint32_t WINAPI d3d_device7_GetTransform(void *self, uint32_t state, d3dmatrix_t *out_matrix);
uint32_t WINAPI d3d_device7_MultiplyTransform(void *self, uint32_t state, d3dmatrix_t *matrix);
uint32_t WINAPI d3d_device7_SetViewport(void *self, d3dviewport7_t *vp);
uint32_t WINAPI d3d_device7_GetViewport(void *self, d3dviewport7_t *out_vp);
uint32_t WINAPI d3d_device7_SetMaterial(void *self, d3dmaterial7_t *mat);
uint32_t WINAPI d3d_device7_GetMaterial(void *self, d3dmaterial7_t *out_mat);
uint32_t WINAPI d3d_device7_SetLight(void *self, uint32_t index, d3dlight7_t *light);
uint32_t WINAPI d3d_device7_GetLight(void *self, uint32_t index, d3dlight7_t *out_light);
uint32_t WINAPI d3d_device7_LightEnable(void *self, uint32_t index, uint32_t enable);
uint32_t WINAPI d3d_device7_GetLightEnable(void *self, uint32_t index, uint32_t *out_enable);
uint32_t WINAPI d3d_device7_DrawPrimitive(void *self, uint32_t a, uint32_t b, uintptr_t c, uint32_t d, uint32_t e);
uint32_t WINAPI d3d_device7_DrawIndexedPrimitive(void *self, uint32_t a, uint32_t b, uintptr_t c, uint32_t d,
                                                  uintptr_t e, uint32_t f, uint32_t g);
uint32_t WINAPI d3d_device7_DrawPrimitiveStrided(void *self, uint32_t a, uint32_t b, uintptr_t c, uint32_t d, uint32_t e);
uint32_t WINAPI d3d_device7_DrawIndexedPrimitiveStrided(void *self, uint32_t a, uint32_t b, uintptr_t c, uint32_t d,
                                                         uintptr_t e, uint32_t f, uint32_t g);
uint32_t WINAPI d3d_device7_ValidateDevice(void *self, uint32_t *out_passes);

#endif /* LONGROAD2_D3D7_INTERNAL_H */
