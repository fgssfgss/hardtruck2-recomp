#include "d3d7_internal.h"
#include "activity_sampler.h"
#include "gl_thread.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Phase 2 rendering architecture: command buffer (see
 * [[project_phase2_command_buffer]] for the full design writeup, and
 * [[project_phase1_gl_state_cache]] for Phase 1, the GL state cache).
 *
 * Clear()/DrawPrimitive-family calls issued between BeginScene and
 * EndScene are recorded here (deep-copied vertex/index/rect data,
 * snapshotted device state) instead of issuing real GL calls
 * immediately, then replayed in the exact same order at EndScene -- see
 * cmdbuf_flush's own comment below for why this is behavior-preserving.
 * Every pointer a command owns (vertex/index/rect data, and every
 * sd.*.data stream for a strided draw) is a fresh malloc'd COPY taken at
 * record time, never the game's own memory -- real D3D7 vertex/index/
 * rect pointers are only guaranteed valid for the duration of the call
 * that received them.
 *
 * Split out of d3d7.c (originally over 4100 lines) purely for
 * readability -- d3d7_internal.h is the private, shared-not-public
 * header that makes this possible without changing d3d7.c's own
 * behavior or its public API (d3d7.h).
 */

/* Phase 3b (rendering-architecture plan): primitive merger. Real values,
   matching the numeric D3DPRIMITIVETYPE constants d3d7.c's own
   d3d_draw_dispatch_and_cleanup switch already uses. Only these 3
   topologies have no inter-vertex connectivity constraint between
   consecutive primitives, so concatenating several draws' worth of
   vertices/indices back to back is structurally safe -- LINESTRIP(3)/
   TRIANGLESTRIP(5)/TRIANGLEFAN(6) would need primitive-restart-index
   infrastructure that doesn't exist here, so a run containing one
   simply never extends (replayed individually, unchanged from before
   this phase). */
#define D3DPT_POINTLIST    1u
#define D3DPT_LINELIST     2u
#define D3DPT_TRIANGLELIST 4u

/* Full representable range of a uint16_t index (0..65535) -- a merged
   run's cumulative vertex count must never exceed this, real or
   synthesized indices would silently wrap mod 65536 and address the
   wrong vertex otherwise. See cmdbuf_flush's own run-building loop. */
#define MERGE_MAX_VERTICES 65536u

/* Phase 4 (state-sorted merging): render-state indices/values matching
   d3d7.c's own D3DRENDERSTATE_ and D3DCMP_ defines exactly -- duplicated
   here for the same reason D3DPT_POINTLIST/etc above already are (this
   file has no shared header with d3d7.c's render-state constants). */
#define D3DRENDERSTATE_ZENABLE          7u
#define D3DRENDERSTATE_ZWRITEENABLE     14u
#define D3DRENDERSTATE_ZFUNC            23u
#define D3DRENDERSTATE_ALPHABLENDENABLE 27u
#define D3DRENDERSTATE_STENCILENABLE    52u
#define D3DCMP_LESS      2u
#define D3DCMP_LESSEQUAL 4u

typedef enum { CMD_CLEAR, CMD_DRAW_FLAT, CMD_DRAW_STRIDED } render_cmd_kind_t;

struct render_cmd_s {
    render_cmd_kind_t kind;
    const d3d_state_t *state;       /* borrowed from dev->state_snapshots -- see that field's own comment; never owned/freed by this command itself */
    uint64_t state_generation;      /* dev->state_generation at record time -- cmds_mergeable compares this instead of memcmp'ing the whole state */
    void *render_target;            /* AddRef'd at record time, Release'd after replay/discard */
    void *textures[MAX_TEX_STAGES]; /* ditto */
    union {
        struct {
            uint32_t flags, color, zbits, stencil, rect_count;
            ddraw_rect_t *rects; /* owned copy, NULL if rect_count==0 */
        } clear;
        struct {
            uint32_t prim_type, fvf, vertex_count, index_count;
            uint8_t *vertices;   /* owned copy, vertex_count*fvf_stride(fvf) bytes */
            uint16_t *indices;   /* owned copy, NULL if index_count==0 */
        } draw_flat;
        struct {
            uint32_t prim_type, fvf, vertex_count, index_count;
            d3ddrawprimitivestrideddata_t sd; /* every non-NULL .data below is an owned copy */
            uint16_t *indices;                /* owned copy, NULL if index_count==0 */
        } draw_strided;
    } u;
};

/* Phase 2 (async/double-buffered pipelining, see [[project_gl_issuer_thread]]):
   every function below that used to read/write dev->cmd_buf/order/
   state_snapshots/etc directly now goes through one of TWO cmdbuf_slot_t
   instances (d3d7_internal.h) instead -- whichever dev->cmdbuf_recording_
   slot currently points at, for anything RECORDING (this function and
   everything below it up through cmdbuf_record_draw_strided), or an
   EXPLICIT slot pointer handed to it (for anything REPLAYING, i.e.
   cmdbuf_build_order/cmdbuf_flush_impl and everything they call) -- the
   two roles must never accidentally resolve to the same slot at the same
   time, which is exactly why replay takes its slot as an explicit
   parameter rather than also calling this helper. */
static cmdbuf_slot_t *cmdbuf_active_slot(d3d_device7_t *dev)
{
    return &dev->cmdbuf_slots[dev->cmdbuf_recording_slot];
}

static void cmdbuf_ensure_capacity(cmdbuf_slot_t *slot)
{
    if (slot->cmd_count < slot->cmd_capacity) return;
    uint32_t new_cap = slot->cmd_capacity == 0 ? 16u : slot->cmd_capacity * 2u;
    slot->cmd_buf = realloc(slot->cmd_buf, (size_t)new_cap * sizeof(*slot->cmd_buf));
    slot->cmd_capacity = new_cap;
}

/* Appends snap to slot's per-scene snapshot pool (grown the same
   realloc-doubling way cmd_buf/order already are) -- ownership of snap
   transfers to the pool; it's freed in bulk at the end of
   cmdbuf_flush/cmdbuf_discard, never by an individual command. */
static void cmdbuf_add_state_snapshot(cmdbuf_slot_t *slot, d3d_state_t *snap)
{
    if (slot->state_snapshot_count >= slot->state_snapshot_capacity) {
        uint32_t new_cap = slot->state_snapshot_capacity == 0 ? 8u : slot->state_snapshot_capacity * 2u;
        slot->state_snapshots = realloc(slot->state_snapshots, (size_t)new_cap * sizeof(*slot->state_snapshots));
        slot->state_snapshot_capacity = new_cap;
    }
    slot->state_snapshots[slot->state_snapshot_count++] = snap;
}

/* Frees every snapshot in slot's per-scene state-snapshot pool and resets
   the dedup-tracking fields -- called once every recorded command has
   either been replayed (cmdbuf_flush_impl) or dropped (cmdbuf_discard),
   never while any command might still reference one of these snapshots.
   Keeps the pool array's own capacity (same reuse-across-scenes
   convention as cmd_buf/order) -- only its count resets to empty. */
static void cmdbuf_free_state_snapshots(cmdbuf_slot_t *slot)
{
    for (uint32_t i = 0; i < slot->state_snapshot_count; i++) free(slot->state_snapshots[i]);
    slot->state_snapshot_count = 0;
    slot->cmdbuf_last_snapshot = NULL;
    slot->cmdbuf_last_snapshot_generation = 0;
}

/* Common to every recorded command: snapshot state/render_target/textures
   exactly as they are right now. render_target/textures are AddRef'd --
   a recorded command must keep its own reference alive until it's
   actually replayed or discarded, even if the game itself Releases its
   own reference to that surface in the meantime (real COM lifetime
   rules, same convention SetRenderTarget/SetTexture already use).

   State itself is NOT copied unconditionally anymore: state_generation
   is bumped on every actual dev->state mutator (see d3d_state_t's own
   comment) and is monotonically non-decreasing for the device's whole
   lifetime, so two commands recorded back to back with an unchanged
   generation are, by construction, byte-identical in their state --
   provably true, not a heuristic, the same invariant cmds_mergeable and
   the Phase 3 uniform-upload cache already trust elsewhere. Comparing
   only against the MOST RECENTLY taken snapshot (not searching the
   whole pool) is sufficient specifically because of that monotonicity:
   if the incoming generation doesn't match the last one taken, it is
   guaranteed to be a brand new, higher generation this pool has never
   seen, never a match further back in the pool. A real trace found
   ALPHABLENDENABLE (the most frequently toggled state this shim tracks)
   flips only ~9-14 times per scene against ~4000 draws, so the pool
   ends up holding on the order of a few dozen snapshots per scene, not
   one per draw. */
static void cmdbuf_snapshot_common(d3d_device7_t *dev, cmdbuf_slot_t *slot, render_cmd_t *cmd)
{
    if (slot->cmdbuf_last_snapshot != NULL && slot->cmdbuf_last_snapshot_generation == dev->state_generation) {
        cmd->state = slot->cmdbuf_last_snapshot;
    } else {
        d3d_state_t *snap = malloc(sizeof(*snap));
        *snap = dev->state;
        cmdbuf_add_state_snapshot(slot, snap);
        slot->cmdbuf_last_snapshot = snap;
        slot->cmdbuf_last_snapshot_generation = dev->state_generation;
        cmd->state = snap;
    }
    cmd->state_generation = dev->state_generation;
    cmd->render_target = dev->render_target;
    if (cmd->render_target != NULL) com_addref(cmd->render_target);
    for (int i = 0; i < MAX_TEX_STAGES; i++) {
        cmd->textures[i] = dev->textures[i];
        if (cmd->textures[i] != NULL) com_addref(cmd->textures[i]);
    }
}

/* Releases a recorded command's AddRef'd surfaces and frees every owned
   copy it holds -- called once a command has been replayed (cmdbuf_flush)
   or is being dropped without replay (cmdbuf_discard). Safe to call on a
   command whose owned pointers are still NULL (kind-specific fields not
   populated for that draw shape) since free(NULL) is a no-op. */
static void cmdbuf_free_command(render_cmd_t *cmd)
{
    if (cmd->render_target != NULL) com_release(cmd->render_target);
    for (int i = 0; i < MAX_TEX_STAGES; i++) {
        if (cmd->textures[i] != NULL) com_release(cmd->textures[i]);
    }
    switch (cmd->kind) {
        case CMD_CLEAR:
            free(cmd->u.clear.rects);
            break;
        case CMD_DRAW_FLAT:
            free(cmd->u.draw_flat.vertices);
            free(cmd->u.draw_flat.indices);
            break;
        case CMD_DRAW_STRIDED:
            free((void *)(uintptr_t)cmd->u.draw_strided.sd.position.data);
            free((void *)(uintptr_t)cmd->u.draw_strided.sd.normal.data);
            free((void *)(uintptr_t)cmd->u.draw_strided.sd.diffuse.data);
            free((void *)(uintptr_t)cmd->u.draw_strided.sd.specular.data); /* always NULL (never copied) -- free(NULL) is safe */
            for (int i = 0; i < 8; i++) free((void *)(uintptr_t)cmd->u.draw_strided.sd.texcoord[i].data);
            free(cmd->u.draw_strided.indices);
            break;
    }
}

/* Real, live-crashed gap: every count/stride this whole file deep-copies
   from (vertex_count, index_count, a strided stream's own per-attribute
   stride, a Clear() rect count) is a fully game-controlled, untrusted
   value with no D3D7-spec-mandated upper bound -- SIGSEGV inside a
   memcpy fed by one of these (cmdbuf_record_draw_strided ->
   strided_stream_copy) was live-reported after ~30 minutes of real
   king.exe gameplay, with no prior indication of what corrupted the
   count/pointer (a genuine game bug, a box86 stack-layout edge case
   under a long-running session, or heap corruption elsewhere manifesting
   here -- never pinned down further, and doesn't need to be for this fix
   to be correct: nothing in this file should ever have trusted an
   unbounded, unchecked multiply-then-malloc-then-memcpy chain regardless
   of what feeds it). Two independent gaps existed together: (1) no guard
   against 32-bit size_t overflow (element_size*count wrapping to a small
   value while the ORIGINAL, un-wrapped count is still what gets stored
   and later replayed against -- a heap-buffer-overflow-on-replay bug
   distinct from this one), and (2) malloc's return value was never
   checked before being handed to memcpy as the destination. Returns 0
   (caller must treat as "reject") if element_size*count would overflow,
   or exceeds a generous sanity cap no real D3D7-era content could
   plausibly need in one single stream/array. */
#define CMDBUF_MAX_COPY_BYTES (256u * 1024u * 1024u) /* 256MB -- real per-draw content needs a tiny fraction of this; large enough to never reject anything legitimate, small enough to catch a corrupted count immediately instead of attempting a multi-hundred-MB-or-more allocation */
static int cmdbuf_safe_alloc_size(uint32_t element_size, uint32_t count, size_t *out_bytes)
{
    size_t bytes = (size_t)element_size * (size_t)count;
    if (count != 0 && bytes / count != element_size) {
        fprintf(stderr, "FATAL-AVOIDED: cmdbuf allocation size overflow (element_size=%u count=%u) -- rejecting\n",
                element_size, count);
        return 0;
    }
    if (bytes > CMDBUF_MAX_COPY_BYTES) {
        fprintf(stderr, "FATAL-AVOIDED: cmdbuf allocation size too large (%zu bytes, element_size=%u count=%u) "
                        "-- rejecting\n", bytes, element_size, count);
        return 0;
    }
    *out_bytes = bytes;
    return 1;
}

void cmdbuf_record_clear(d3d_device7_t *dev, uint32_t count, const ddraw_rect_t *rp,
                          uint32_t flags, uint32_t color, uint32_t z, uint32_t stencil)
{
    cmdbuf_slot_t *slot = cmdbuf_active_slot(dev);
    cmdbuf_ensure_capacity(slot);
    render_cmd_t *cmd = &slot->cmd_buf[slot->cmd_count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->kind = CMD_CLEAR;
    cmdbuf_snapshot_common(dev, slot, cmd);
    cmd->u.clear.flags = flags;
    cmd->u.clear.color = color;
    cmd->u.clear.zbits = z;
    cmd->u.clear.stencil = stencil;
    if (rp != NULL && count > 0) {
        size_t rbytes;
        if (cmdbuf_safe_alloc_size((uint32_t)sizeof(*rp), count, &rbytes)) {
            cmd->u.clear.rects = malloc(rbytes);
            if (cmd->u.clear.rects != NULL) {
                cmd->u.clear.rect_count = count;
                memcpy(cmd->u.clear.rects, rp, rbytes);
            } else {
                fprintf(stderr, "FATAL-AVOIDED: cmdbuf_record_clear rect malloc(%zu) failed -- clearing full target instead\n", rbytes);
            }
        }
        /* else: cmdbuf_safe_alloc_size already logged; falls through to a
           full-target clear (rect_count stays 0), matching this
           command's own existing "no rects" convention -- safer than a
           partial/wrong-sized rect-scoped clear. */
    }
}

void cmdbuf_record_draw_flat(d3d_device7_t *dev, uint32_t prim_type, uint32_t fvf,
                              const uint8_t *vertices, uint32_t vertex_count,
                              const uint16_t *indices, uint32_t index_count)
{
    uint32_t stride = fvf_stride(fvf);
    if (stride == 0) return; /* matches draw_flat_execute_now's own no-op for an invalid fvf -- nothing to copy */

    /* Validated BEFORE reserving a command slot -- a corrupted/oversized
       vertex_count cleanly rejects the whole draw with nothing to unwind
       (see cmdbuf_safe_alloc_size's own comment for the real live crash
       this guards against). Vertices are mandatory (there's nothing to
       draw without them), unlike the optional index array below. */
    size_t vbytes;
    if (!cmdbuf_safe_alloc_size(stride, vertex_count, &vbytes)) return;

    cmdbuf_slot_t *slot = cmdbuf_active_slot(dev);
    cmdbuf_ensure_capacity(slot);
    render_cmd_t *cmd = &slot->cmd_buf[slot->cmd_count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->kind = CMD_DRAW_FLAT;
    cmdbuf_snapshot_common(dev, slot, cmd);
    cmd->u.draw_flat.prim_type = prim_type;
    cmd->u.draw_flat.fvf = fvf;
    cmd->u.draw_flat.vertex_count = vertex_count;
    cmd->u.draw_flat.vertices = malloc(vbytes != 0 ? vbytes : 1);
    if (cmd->u.draw_flat.vertices == NULL) {
        fprintf(stderr, "FATAL-AVOIDED: cmdbuf_record_draw_flat vertex malloc(%zu) failed -- dropping this draw\n", vbytes);
        /* cmdbuf_snapshot_common above already real-AddRef'd render_target/
           textures[] for this slot -- must release them (cmdbuf_free_command,
           safe on an otherwise-still-zeroed command since its other owned
           pointers are all still NULL) before undoing the slot reservation,
           or this leaks one real reference per rejected draw. */
        cmdbuf_free_command(cmd);
        slot->cmd_count--;
        return;
    }
    memcpy(cmd->u.draw_flat.vertices, vertices, vbytes);
    if (indices != NULL && index_count > 0) {
        size_t ibytes;
        if (cmdbuf_safe_alloc_size((uint32_t)sizeof(uint16_t), index_count, &ibytes)) {
            cmd->u.draw_flat.indices = malloc(ibytes);
            if (cmd->u.draw_flat.indices != NULL) {
                cmd->u.draw_flat.index_count = index_count;
                memcpy(cmd->u.draw_flat.indices, indices, ibytes);
            } else {
                fprintf(stderr, "FATAL-AVOIDED: cmdbuf_record_draw_flat index malloc(%zu) failed -- drawing non-indexed instead\n", ibytes);
            }
        }
        /* else: cmdbuf_safe_alloc_size already logged; falls through to a
           non-indexed draw of this same, already-valid vertex data --
           visually wrong (ignores the game's real index order) but not
           unsafe, and strictly better than the alternative of dropping
           an otherwise-good draw over a corrupted index array alone. */
    }
}

/* Deep-copies one D3DDP_PTRSTRIDE stream (vertex_count * effective bytes-
   per-vertex, where "effective" is the real stride if given, or
   natural_size if stride==0 -- a real, legal D3DDP_PTRSTRIDE convention
   meaning "tightly packed, use this attribute's natural size", matching
   how glVertexAttribPointer itself already treats a 0 stride). The
   ORIGINAL stride value (including 0) is preserved in the returned
   descriptor so replay reads the copy exactly the same way GL would have
   read the original. Returns {NULL, 0} if src has no data (matching the
   "attribute not present" case draw_strided_execute_now already relies
   on to skip a stream entirely), OR if the real, live-crashed allocation
   overflow/malloc-failure guard rejects it (see cmdbuf_safe_alloc_size's
   own comment) -- the two cases are deliberately indistinguishable to
   the caller for the 4 OPTIONAL streams (normal/diffuse/texcoord0/1:
   "treat as absent" is always safe for them, already-handled). They are
   NOT safe to conflate for the position stream specifically, which is
   mandatory -- cmdbuf_record_draw_strided checks its own result for NULL
   and rejects the whole command rather than letting a NULL position
   reach replay, which has no guard for it. */
static d3ddp_ptrstride_t strided_stream_copy(const d3ddp_ptrstride_t *src, uint32_t vertex_count, uint32_t natural_size)
{
    d3ddp_ptrstride_t out = { NULL, 0 };
    if (src == NULL || src->data == NULL) return out;
    uint32_t effective = src->stride != 0 ? src->stride : natural_size;
    size_t bytes;
    if (!cmdbuf_safe_alloc_size(effective, vertex_count, &bytes)) return out;
    void *copy = malloc(bytes != 0 ? bytes : 1);
    if (copy == NULL) {
        fprintf(stderr, "FATAL-AVOIDED: strided_stream_copy malloc(%zu) failed -- rejecting\n", bytes);
        return out;
    }
    memcpy(copy, src->data, bytes);
    out.data = copy;
    out.stride = src->stride;
    return out;
}

void cmdbuf_record_draw_strided(d3d_device7_t *dev, uint32_t prim_type, uint32_t fvf,
                                 const d3ddrawprimitivestrideddata_t *sd, uint32_t vertex_count,
                                 const uint16_t *indices, uint32_t index_count)
{
    fvf_layout_t layout = decode_fvf(fvf);
    if (layout.stride == 0) return; /* matches draw_strided_execute_now's own no-op for an invalid fvf */

    cmdbuf_slot_t *slot = cmdbuf_active_slot(dev);
    cmdbuf_ensure_capacity(slot);
    render_cmd_t *cmd = &slot->cmd_buf[slot->cmd_count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->kind = CMD_DRAW_STRIDED;
    cmdbuf_snapshot_common(dev, slot, cmd);
    cmd->u.draw_strided.prim_type = prim_type;
    cmd->u.draw_strided.fvf = fvf;
    cmd->u.draw_strided.vertex_count = vertex_count;

    cmd->u.draw_strided.sd.position = strided_stream_copy(&sd->position, vertex_count,
        (uint32_t)((layout.is_rhw ? 4u : 3u) * sizeof(float)));
    if (cmd->u.draw_strided.sd.position.data == NULL) {
        /* Position is mandatory -- real D3D7 semantics, and
           draw_strided_execute_now's own present[0]=1 unconditionally
           assumes it's there (see that function's own header comment).
           A NULL here means strided_stream_copy either found no real
           source data at all (shouldn't happen for a spec-conformant
           call) or rejected a corrupted/oversized count -- either way
           there's nothing safe to draw. Reject the whole command instead
           of letting a NULL position stream reach replay, which has no
           guard for it (this is the exact live-crashed shape -- see
           cmdbuf_safe_alloc_size's own comment). cmdbuf_snapshot_common
           above already real-AddRef'd render_target/textures[] for this
           slot -- must release them (cmdbuf_free_command; safe here since
           sd.position.data is the only owned pointer that could possibly
           be non-NULL yet, and it's the NULL we just checked) before
           undoing the slot reservation, or this leaks one real reference
           per rejected draw. */
        cmdbuf_free_command(cmd);
        slot->cmd_count--;
        return;
    }
    cmd->u.draw_strided.sd.normal = strided_stream_copy(&sd->normal, vertex_count, (uint32_t)(3u * sizeof(float)));
    cmd->u.draw_strided.sd.diffuse = strided_stream_copy(&sd->diffuse, vertex_count, 4u);
    cmd->u.draw_strided.sd.texcoord[0] = strided_stream_copy(&sd->texcoord[0], vertex_count,
        (uint32_t)((layout.tex_floats[0] < 2 ? layout.tex_floats[0] : 2) * sizeof(float)));
    cmd->u.draw_strided.sd.texcoord[1] = strided_stream_copy(&sd->texcoord[1], vertex_count,
        (uint32_t)((layout.tex_floats[1] < 2 ? layout.tex_floats[1] : 2) * sizeof(float)));
    /* sd.specular and sd.texcoord[2..7] stay {NULL,0} -- draw_*_execute_now
       never reads them (see d3d_draw_common_strided's own comment), so
       there's nothing to copy or replay. */

    if (indices != NULL && index_count > 0) {
        size_t ibytes;
        if (cmdbuf_safe_alloc_size((uint32_t)sizeof(uint16_t), index_count, &ibytes)) {
            cmd->u.draw_strided.indices = malloc(ibytes);
            if (cmd->u.draw_strided.indices != NULL) {
                cmd->u.draw_strided.index_count = index_count;
                memcpy(cmd->u.draw_strided.indices, indices, ibytes);
            } else {
                fprintf(stderr, "FATAL-AVOIDED: cmdbuf_record_draw_strided index malloc(%zu) failed -- drawing non-indexed instead\n", ibytes);
            }
        }
        /* else: cmdbuf_safe_alloc_size already logged; falls through to a
           non-indexed draw of this same, already-valid vertex data --
           same tradeoff as cmdbuf_record_draw_flat's own indices path. */
    }
}

/* --- Phase 3b: primitive merger -----------------------------------
   Collapses a RUN of adjacent, state-identical CMD_DRAW_FLAT/
   CMD_DRAW_STRIDED commands into one real GL draw call at replay time.
   CMD_CLEAR never participates. See the Phase 3b plan
   ([[project_phase3b_primitive_merger]]) for the full design writeup;
   this section implements it exactly. */

/* Small kind-agnostic accessors -- never called for CMD_CLEAR. */
static uint32_t cmd_prim_type(const render_cmd_t *cmd)
{ return cmd->kind == CMD_DRAW_FLAT ? cmd->u.draw_flat.prim_type : cmd->u.draw_strided.prim_type; }
static uint32_t cmd_fvf(const render_cmd_t *cmd)
{ return cmd->kind == CMD_DRAW_FLAT ? cmd->u.draw_flat.fvf : cmd->u.draw_strided.fvf; }
static uint32_t cmd_vertex_count(const render_cmd_t *cmd)
{ return cmd->kind == CMD_DRAW_FLAT ? cmd->u.draw_flat.vertex_count : cmd->u.draw_strided.vertex_count; }
static uint32_t cmd_index_count(const render_cmd_t *cmd)
{ return cmd->kind == CMD_DRAW_FLAT ? cmd->u.draw_flat.index_count : cmd->u.draw_strided.index_count; }
static int cmd_has_indices(const render_cmd_t *cmd)
{ return cmd->kind == CMD_DRAW_FLAT ? cmd->u.draw_flat.indices != NULL : cmd->u.draw_strided.indices != NULL; }

static int prim_type_mergeable(uint32_t prim_type)
{ return prim_type == D3DPT_POINTLIST || prim_type == D3DPT_LINELIST || prim_type == D3DPT_TRIANGLELIST; }

/* Vertices/indices per primitive for the 3 mergeable topologies -- a
   command's own count must be an exact multiple of this before it can
   start or extend a run, or a merged run could synthesize a primitive
   spanning the tail of one original draw and the head of the next,
   which never existed in either original draw. */
static uint32_t prim_group_size(uint32_t prim_type)
{
    switch (prim_type) {
        case D3DPT_TRIANGLELIST: return 3u;
        case D3DPT_LINELIST:     return 2u;
        default:                 return 1u; /* D3DPT_POINTLIST -- always clean */
    }
}

static int cmd_draw_count_is_clean(const render_cmd_t *cmd)
{
    uint32_t count = cmd_has_indices(cmd) ? cmd_index_count(cmd) : cmd_vertex_count(cmd);
    uint32_t group = prim_group_size(cmd_prim_type(cmd));
    return group <= 1u || (count % group) == 0u;
}

/* Full eligibility check between two commands. Equality is transitive
   over every field checked here, so cmdbuf_flush only ever needs to
   compare each new candidate against the RUN'S FIRST command, not the
   immediately-preceding one -- sufficient and simpler. */
static int cmds_mergeable(const render_cmd_t *a, const render_cmd_t *b)
{
    if (a->kind != b->kind) return 0;
    uint32_t a_prim = cmd_prim_type(a), b_prim = cmd_prim_type(b);
    if (a_prim != b_prim || !prim_type_mergeable(a_prim)) return 0;
    if (cmd_fvf(a) != cmd_fvf(b)) return 0;
    if (!cmd_draw_count_is_clean(a) || !cmd_draw_count_is_clean(b)) return 0;
    if (a->render_target != b->render_target) return 0;
    if (memcmp(a->textures, b->textures, sizeof(a->textures)) != 0) return 0;
    /* Same-generation means provably zero dev->state mutations occurred
       between recording a and b, hence their d3d_state_t snapshots are
       byte-identical -- see the generation-counter comment at d3d_state_t's
       own definition (d3d7_internal.h) for the full invariant this relies
       on. Replaces a ~6KB memcmp/call with an int compare -- confirmed a
       real, substantial cost in a perf.data capture of real gameplay. */
    if (a->state_generation != b->state_generation) return 0;
    if (a->kind == CMD_DRAW_STRIDED) {
        /* Stream presence must match exactly, not just fvf -- a real
           D3D7 app can legally claim an attribute via fvf flags while
           still passing a NULL pointer for that specific stream, and
           draw_strided_execute_now already treats that as "attribute
           absent" (see its own has_normal/has_diffuse/tex_count checks
           in d3d7.c), so fvf equality alone doesn't guarantee this. */
        const d3ddrawprimitivestrideddata_t *sa = &a->u.draw_strided.sd, *sb = &b->u.draw_strided.sd;
        if ((sa->position.data    != NULL) != (sb->position.data    != NULL)) return 0;
        if ((sa->normal.data      != NULL) != (sb->normal.data      != NULL)) return 0;
        if ((sa->diffuse.data     != NULL) != (sb->diffuse.data     != NULL)) return 0;
        if ((sa->texcoord[0].data != NULL) != (sb->texcoord[0].data != NULL)) return 0;
        if ((sa->texcoord[1].data != NULL) != (sb->texcoord[1].data != NULL)) return 0;
    }
    return 1;
}

/* --- Phase 4: state-sorted merging -------------------------------
   Phase 3b only merges ADJACENT same-state commands -- if the game
   interleaves a different draw (e.g. a different bound texture) between
   two otherwise-identical draws, the run breaks even though dev->state
   never actually changed (SetTexture doesn't bump state_generation --
   textures are a separate per-device field, not part of d3d_state_t).
   Phase 4 reorders WITHIN safety bounds so non-adjacent same-state
   commands become adjacent and merge too. See
   [[project_phase4_state_sorted_merging]] for the full design writeup
   and the two-round review that shaped it -- this is the first change
   in this whole rendering-optimization family that is NOT
   order-preserving, and the safety envelope below is deliberately
   conservative. */

/* A draw command is reorder-safe (may be moved within its own zone,
   see below) iff it's fully opaque, depth-tested-and-written with a
   MONOTONIC depth comparison, and never touches the stencil buffer --
   the standard "any submission order produces the same final image"
   graphics-engine invariant. CMD_CLEAR is never reorder-safe (always a
   hard barrier, unchanged since Phase 3b -- it already can't join or be
   crossed by a merge run).

   ZFUNC specifically needs its own gate, not just ZENABLE/ZWRITEENABLE:
   D3DCMP_ALWAYS with ZWRITEENABLE=1 is "last submitted wins" (a pure
   painter's-algorithm dependency on order), and D3DCMP_EQUAL is a
   classic decal/coplanar-detail-pass technique (base geometry writes
   depth, a second pass tests EQUAL against it) -- both are genuinely
   order-dependent even though every other criterion here holds.
   D3DCMP_LESS/LESSEQUAL (the engine's own default, d3d_state_init) are
   the only two where the final image is provably invariant to
   submission order for opaque, depth-written geometry.

   Every OTHER render state this shim implements (fog/lighting/alpha-
   test/colorkey/clip-planes/cull-mode/dither/texture-stage-combiner)
   only affects a fragment's color, never WHICH fragment wins at a
   pixel -- so once this 5-state gate holds, none of them can introduce
   order-dependence. */
static int cmd_is_reorder_safe(const render_cmd_t *cmd)
{
    if (cmd->kind == CMD_CLEAR) return 0;
    const uint32_t *rs = cmd->state->render_state;
    if (rs[D3DRENDERSTATE_ZENABLE] == 0) return 0;
    if (rs[D3DRENDERSTATE_ZWRITEENABLE] == 0) return 0;
    if (rs[D3DRENDERSTATE_ALPHABLENDENABLE] != 0) return 0;
    if (rs[D3DRENDERSTATE_STENCILENABLE] != 0) return 0;
    uint32_t zfunc = rs[D3DRENDERSTATE_ZFUNC];
    if (zfunc != D3DCMP_LESS && zfunc != D3DCMP_LESSEQUAL) return 0;
    return 1;
}

/* Native quicksort, replacing libc's qsort() for both call sites in this
   file (here and order_sort below) -- see
   [[project_cmdbuf_flush_perf_investigation]]: a real BOX86_DYNAREC_LOG=2
   capture found qsort's own comparator callbacks (the old ptr_compare/
   order_compare this replaces) dominating ALL box86-translated dynarec
   block executions in a real king.exe session, and cmdbuf_flush (which
   calls both, once per EndScene) measured up to 613ms/frame in a heavy
   scene. libc's qsort() can never inline a comparator passed as a
   runtime function pointer -- every single comparison is a real indirect
   call. A hand-written sort calling a `static` comparison function
   directly (not through a pointer) lets the compiler inline it, and
   avoids qsort()'s own generic, element-size-agnostic swap machinery
   (memcpy/memmove at a runtime-supplied size) in favor of real, fixed-
   type swaps.

   Standard insertion-sort-cutoff quicksort: median-of-three pivot
   (avoids O(n^2) on already-sorted or reverse-sorted input, both
   realistic here -- e.g. a scene whose draws already happen to be
   grouped by texture), Sedgewick/Bentley-McIlroy 3-way partition,
   "recurse into the smaller partition, loop into the larger" (bounds
   recursion depth to O(log n) regardless of pivot quality, no explicit
   stack needed). Every comparator here (ptr_less below, order_less in
   order_sort) has its own final tiebreak on original index/pointer
   value, making it a strict total order -- any correct comparison sort,
   this one included, MUST therefore produce byte-identical output to
   libc's qsort for the same input, so every existing pixel-regression
   test covering Phase 4 reordering remains valid unchanged. */
#define SORT_INSERTION_CUTOFF 16

static int ptr_less(void *a, void *b) { return a < b; }

static void ptr_insertion_sort(void **a, int lo, int hi)
{
    for (int i = lo + 1; i <= hi; i++) {
        void *key = a[i];
        int j = i - 1;
        while (j >= lo && ptr_less(key, a[j])) { a[j + 1] = a[j]; j--; }
        a[j + 1] = key;
    }
}

static void ptr_sort_range(void **a, int lo, int hi)
{
    while (hi - lo + 1 > SORT_INSERTION_CUTOFF) {
        int mid = lo + (hi - lo) / 2;
        if (ptr_less(a[mid], a[lo])) { void *t = a[mid]; a[mid] = a[lo]; a[lo] = t; }
        if (ptr_less(a[hi], a[lo])) { void *t = a[hi]; a[hi] = a[lo]; a[lo] = t; }
        if (ptr_less(a[hi], a[mid])) { void *t = a[hi]; a[hi] = a[mid]; a[mid] = t; }
        void *pivot = a[mid];
        a[mid] = a[hi - 1]; a[hi - 1] = pivot;
        int i = lo, j = hi - 1;
        for (;;) {
            while (ptr_less(a[++i], pivot)) {}
            while (ptr_less(pivot, a[--j])) {}
            if (i >= j) break;
            void *t = a[i]; a[i] = a[j]; a[j] = t;
        }
        void *t = a[i]; a[i] = a[hi - 1]; a[hi - 1] = t;
        if (i - lo < hi - i) { ptr_sort_range(a, lo, i - 1); lo = i + 1; }
        else { ptr_sort_range(a, i + 1, hi); hi = i - 1; }
    }
    ptr_insertion_sort(a, lo, hi);
}

static void ptr_sort(void **a, uint32_t n)
{
    if (n > 1) ptr_sort_range(a, 0, (int)n - 1);
}

void d3d7_cmdbuf_ptr_sort_for_test(void **a, uint32_t n)
{
    ptr_sort(a, n);
}

/* Builds the sorted, deduped set of every render_target that's sampled
   as a texture ANYWHERE in this scene's cmd_buf. A command whose OWN
   render_target is in this set is additionally excluded from reordering
   (a render-to-texture write sampled elsewhere is a read-after-write
   hazard if reordered across) -- conservatively treats the WHOLE
   surface as off-limits to reordering for the rest of this flush,
   rather than tracking exactly which specific draws read vs write it.

   Only the WRITER needs flagging here, not the reader: a writer that's
   excluded from reordering becomes a barrier (see cmdbuf_build_order
   below), and a barrier fences BOTH directions -- a zone's permutation
   can never move anything across a barrier's position. So any reader of
   this surface, wherever it originally sat relative to the write, stays
   confined to the correct side of that barrier and can never be moved
   across it either. This is a load-bearing property of how this
   pre-scan and the zone mechanism compose -- don't flag readers too,
   it isn't needed and won't change the outcome, and doing so would only
   fragment zones further for no correctness benefit. */
static void build_hot_render_targets(cmdbuf_slot_t *slot, void ***out_hot, uint32_t *out_hot_count)
{
    uint32_t max_count = slot->cmd_count * (uint32_t)MAX_TEX_STAGES;
    void **hot = max_count > 0 ? malloc((size_t)max_count * sizeof(void *)) : NULL;
    uint32_t count = 0;
    for (uint32_t i = 0; i < slot->cmd_count; i++) {
        for (int t = 0; t < MAX_TEX_STAGES; t++) {
            void *tex = slot->cmd_buf[i].textures[t];
            if (tex != NULL) hot[count++] = tex;
        }
    }
    if (count > 1) {
        ptr_sort(hot, count);
        uint32_t w = 1;
        for (uint32_t r = 1; r < count; r++) {
            if (hot[r] != hot[w - 1]) hot[w++] = hot[r];
        }
        count = w;
    }
    *out_hot = hot;
    *out_hot_count = count;
}

static int is_hot_render_target(void *rt, void **hot, uint32_t hot_count)
{
    if (rt == NULL || hot_count == 0) return 0;
    uint32_t lo = 0, hi = hot_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (hot[mid] == rt) return 1;
        if (hot[mid] < rt) lo = mid + 1; else hi = mid;
    }
    return 0;
}

/* Composite sort key for the within-zone regroup below -- covers exactly
   the pairwise fields cmds_mergeable() checks (state_generation, kind,
   prim_type, fvf, render_target, textures[8], and for strided the 5
   stream-presence booleans), so sorting by this key brings every
   cmds_mergeable-transitive-equivalence-class together contiguously.
   This is a coarsening of cmds_mergeable's true classes (it doesn't
   encode prim_type_mergeable/cmd_draw_count_is_clean, which are
   per-element gates, not pairwise comparisons) -- harmless, since
   cmdbuf_flush's own unchanged run-extension loop still rechecks
   cmds_mergeable pairwise as the final arbiter; a same-key group that
   isn't actually mergeable simply fails to extend, exactly like today.
   Original index is the final tiebreak, making the sort's result
   deterministic regardless of the underlying algorithm's stability.

   order_compare below takes the slot being replayed directly as a
   parameter now (not a static "current context" global) -- the native
   sort's own comparison calls (order_sort_range) are real, direct calls
   with the slot already in scope, unlike libc's qsort()/qsort_r(), which
   would have needed either the old static-global workaround or the
   non-portable qsort_r GNU extension just to pass it through a comparator
   with a fixed (const void *, const void *) signature. See ptr_sort's own
   comment above for the full native-sort rationale.

   strided_stream_ptr's real definition lives further down (alongside
   strided_natural_size, its usual partner for the merge/repack code),
   but order_compare needs it earlier -- both are declared in
   d3d7_internal.h (shared with d3d7.c's own VBO-upload repack code, see
   that file's draw_strided_execute_now) rather than forward-declared
   locally. */

static int order_compare(cmdbuf_slot_t *slot, uint32_t a, uint32_t b)
{
    const render_cmd_t *ca = &slot->cmd_buf[a];
    const render_cmd_t *cb = &slot->cmd_buf[b];

    if (ca->state_generation != cb->state_generation)
        return (ca->state_generation > cb->state_generation) - (ca->state_generation < cb->state_generation);
    if (ca->kind != cb->kind) return (ca->kind > cb->kind) - (ca->kind < cb->kind);
    uint32_t pa_prim = cmd_prim_type(ca), pb_prim = cmd_prim_type(cb);
    if (pa_prim != pb_prim) return (pa_prim > pb_prim) - (pa_prim < pb_prim);
    uint32_t pa_fvf = cmd_fvf(ca), pb_fvf = cmd_fvf(cb);
    if (pa_fvf != pb_fvf) return (pa_fvf > pb_fvf) - (pa_fvf < pb_fvf);
    if (ca->render_target != cb->render_target)
        return (ca->render_target > cb->render_target) - (ca->render_target < cb->render_target);
    int tex_cmp = memcmp(ca->textures, cb->textures, sizeof(ca->textures));
    if (tex_cmp != 0) return tex_cmp;
    if (ca->kind == CMD_DRAW_STRIDED) {
        for (int w = 0; w < 5; w++) {
            int pa_present = strided_stream_ptr(w, &ca->u.draw_strided.sd)->data != NULL;
            int pb_present = strided_stream_ptr(w, &cb->u.draw_strided.sd)->data != NULL;
            if (pa_present != pb_present) return pa_present ? 1 : -1;
        }
    }
    return (a > b) - (a < b); /* final tiebreak -- original index */
}

static void order_insertion_sort(cmdbuf_slot_t *slot, uint32_t *a, int lo, int hi)
{
    for (int i = lo + 1; i <= hi; i++) {
        uint32_t key = a[i];
        int j = i - 1;
        while (j >= lo && order_compare(slot, a[j], key) > 0) { a[j + 1] = a[j]; j--; }
        a[j + 1] = key;
    }
}

static void order_sort_range(cmdbuf_slot_t *slot, uint32_t *a, int lo, int hi)
{
    while (hi - lo + 1 > SORT_INSERTION_CUTOFF) {
        int mid = lo + (hi - lo) / 2;
        if (order_compare(slot, a[mid], a[lo]) < 0) { uint32_t t = a[mid]; a[mid] = a[lo]; a[lo] = t; }
        if (order_compare(slot, a[hi], a[lo]) < 0) { uint32_t t = a[hi]; a[hi] = a[lo]; a[lo] = t; }
        if (order_compare(slot, a[hi], a[mid]) < 0) { uint32_t t = a[hi]; a[hi] = a[mid]; a[mid] = t; }
        uint32_t pivot = a[mid];
        a[mid] = a[hi - 1]; a[hi - 1] = pivot;
        int i = lo, j = hi - 1;
        for (;;) {
            while (order_compare(slot, a[++i], pivot) < 0) {}
            while (order_compare(slot, pivot, a[--j]) < 0) {}
            if (i >= j) break;
            uint32_t t = a[i]; a[i] = a[j]; a[j] = t;
        }
        uint32_t t = a[i]; a[i] = a[hi - 1]; a[hi - 1] = t;
        if (i - lo < hi - i) { order_sort_range(slot, a, lo, i - 1); lo = i + 1; }
        else { order_sort_range(slot, a, i + 1, hi); hi = i - 1; }
    }
    order_insertion_sort(slot, a, lo, hi);
}

static void order_sort(cmdbuf_slot_t *slot, uint32_t *a, uint32_t n)
{
    if (n > 1) order_sort_range(slot, a, 0, (int)n - 1);
}

/* Escape hatch for real-device A/B measurement: LONGROAD2_CMDBUF_NO_REORDER
   skips Phase 4 (this whole file's build_hot_render_targets/order_sort
   machinery) entirely -- slot->order stays the identity permutation, so
   cmdbuf_flush's run-extension loop only ever merges commands that were
   ALREADY adjacent in submission order (exactly Phase 3b's own original
   behavior, before Phase 4 existed). Requested after a real
   BOX86_DYNAREC_LOG=2 + LONGROAD2_PERF_TRACE capture
   ([[project_cmdbuf_flush_perf_investigation]]) found a heavy scene
   where draws==gl_draws (4771 both) even WITH reordering running at full
   cost -- i.e. zero merging benefit that frame despite paying the full
   sort -- raising the real question of whether Phase 4 is worth its own
   cost at all for this game, independent of whether the sort itself is
   fast. Native quicksort (order_sort below) already replaced libc's
   qsort() for when reordering IS enabled; this flag is for measuring
   with it skipped altogether. */
static int cmdbuf_reorder_disabled(void)
{
    static int disabled = -1;
    if (disabled < 0) disabled = env_flag_enabled("LONGROAD2_CMDBUF_NO_REORDER");
    return disabled;
}

/* Builds slot->order[0..slot->cmd_count), an index permutation over
   cmd_buf, so that non-adjacent commands sharing everything
   cmds_mergeable checks become adjacent (and thus mergeable) without
   physically moving the ~6KB render_cmd_t structs themselves (only
   4-byte indices move). Barriers (CMD_CLEAR, not-reorder-safe commands,
   or a render_target that's sampled as a texture elsewhere in this
   scene) are never moved -- a "zone" is a maximal run of consecutive
   non-barrier commands, and only a zone's own index sub-range is ever
   permuted, so nothing can cross a barrier's original position. */
static void cmdbuf_build_order(cmdbuf_slot_t *slot)
{
    uint32_t n = slot->cmd_count;
    if (n > slot->order_capacity) {
        uint32_t new_cap = slot->order_capacity == 0 ? 16u : slot->order_capacity * 2u;
        while (new_cap < n) new_cap *= 2u;
        slot->order = realloc(slot->order, (size_t)new_cap * sizeof(*slot->order));
        slot->order_capacity = new_cap;
    }
    for (uint32_t i = 0; i < n; i++) slot->order[i] = i;

    if (cmdbuf_reorder_disabled()) return;

    void **hot = NULL;
    uint32_t hot_count = 0;
    build_hot_render_targets(slot, &hot, &hot_count);

    uint32_t zone_start = 0;
    int in_zone = 0;
    for (uint32_t i = 0; i <= n; i++) {
        int eligible = i < n &&
            cmd_is_reorder_safe(&slot->cmd_buf[i]) &&
            !is_hot_render_target(slot->cmd_buf[i].render_target, hot, hot_count);
        if (eligible && !in_zone) {
            zone_start = i;
            in_zone = 1;
        } else if (!eligible && in_zone) {
            if (i - zone_start > 1) {
                order_sort(slot, &slot->order[zone_start], i - zone_start);
            }
            in_zone = 0;
        }
    }

    free(hot);
}

/* Builds one concatenated vertex/index buffer from run[0..run_len) and
   issues a single draw_flat_execute_now call in place of run_len
   individual ones. Safe raw memcpy concatenation of vertices (no
   repacking needed, unlike the strided case below) -- same fvf across
   the whole run (cmds_mergeable) guarantees identical stride via
   fvf_stride, since decode_fvf is a pure function of fvf alone. Frees
   its own scratch buffers before returning; the run's N ORIGINAL
   commands are freed separately by the caller (cmdbuf_flush), exactly
   like the non-merged path already does -- this function never touches
   that bookkeeping. */
static void merge_and_execute_flat_run(d3d_device7_t *replay_shadow, render_cmd_t **run, uint32_t run_len)
{
    uint32_t prim_type = run[0]->u.draw_flat.prim_type;
    uint32_t fvf = run[0]->u.draw_flat.fvf;
    uint32_t stride = fvf_stride(fvf);

    uint32_t total_vertices = 0;
    int any_indexed = 0;
    for (uint32_t k = 0; k < run_len; k++) {
        total_vertices += run[k]->u.draw_flat.vertex_count;
        if (run[k]->u.draw_flat.indices != NULL) any_indexed = 1;
    }

    uint8_t *merged_vertices = malloc((size_t)total_vertices * stride);
    uint16_t *merged_indices = NULL;
    uint32_t merged_index_count = 0;
    if (any_indexed) {
        /* Every command needs indices in the merged result once any one
           of them does -- a non-indexed member's index_count here is
           its own vertex_count, since it'll get synthesized sequential
           indices (0..vertex_count-1) below. */
        uint32_t total_indices = 0;
        for (uint32_t k = 0; k < run_len; k++) {
            total_indices += run[k]->u.draw_flat.indices != NULL
                ? run[k]->u.draw_flat.index_count : run[k]->u.draw_flat.vertex_count;
        }
        merged_indices = malloc((size_t)total_indices * sizeof(uint16_t));
        merged_index_count = total_indices;
    }

    uint32_t voff = 0, ioff = 0;
    for (uint32_t k = 0; k < run_len; k++) {
        render_cmd_t *c = run[k];
        memcpy(merged_vertices + (size_t)voff * stride, c->u.draw_flat.vertices,
               (size_t)c->u.draw_flat.vertex_count * stride);
        if (any_indexed) {
            if (c->u.draw_flat.indices != NULL) {
                /* Real indices: offset by every prior command's vertex
                   count already folded in, so they address the right
                   position in the WHOLE concatenated vertex array. */
                for (uint32_t v = 0; v < c->u.draw_flat.index_count; v++)
                    merged_indices[ioff++] = (uint16_t)(c->u.draw_flat.indices[v] + voff);
            } else {
                for (uint32_t v = 0; v < c->u.draw_flat.vertex_count; v++)
                    merged_indices[ioff++] = (uint16_t)(voff + v);
            }
        }
        voff += c->u.draw_flat.vertex_count;
    }

    /* Calls the _impl body directly, not the gl_thread_call-dispatching
       wrapper -- this whole replay already runs as one gl_thread_post job
       (see cmdbuf_flush), and replay_shadow (never the real device) is
       what must be passed here -- see d3d_device7_t's own comment on
       replay_shadow. */
    draw_flat_execute_now_impl(replay_shadow, prim_type, fvf, merged_vertices, total_vertices, merged_indices, merged_index_count);
    free(merged_vertices);
    free(merged_indices);
}

/* Per-stream natural-size formulas, verbatim from
   cmdbuf_record_draw_strided's own calls to strided_stream_copy above.
   `which`: 0=position 1=normal 2=diffuse 3=texcoord[0] 4=texcoord[1].
   Declared in d3d7_internal.h -- also used by d3d7.c's
   draw_strided_execute_now to repack a strided draw's independent
   streams into one VBO upload. */
uint32_t strided_natural_size(int which, const fvf_layout_t *layout)
{
    switch (which) {
        case 0: return (uint32_t)((layout->is_rhw ? 4u : 3u) * sizeof(float));
        case 1: return (uint32_t)(3u * sizeof(float));
        case 2: return 4u; /* diffuse -- packed D3DCOLOR bytes, not floats */
        case 3: return (uint32_t)((layout->tex_floats[0] < 2 ? layout->tex_floats[0] : 2) * sizeof(float));
        case 4: return (uint32_t)((layout->tex_floats[1] < 2 ? layout->tex_floats[1] : 2) * sizeof(float));
        default: return 0u;
    }
}
const d3ddp_ptrstride_t *strided_stream_ptr(int which, const d3ddrawprimitivestrideddata_t *sd)
{
    switch (which) {
        case 0: return &sd->position;
        case 1: return &sd->normal;
        case 2: return &sd->diffuse;
        case 3: return &sd->texcoord[0];
        case 4: return &sd->texcoord[1];
        default: return NULL;
    }
}

/* Reads `count` vertices from src at ITS OWN effective stride (stride
   !=0 ? stride : natural_size, matching strided_stream_copy's own
   existing convention) and writes them into dst starting at vertex
   offset dst_voffset, always at natural_size spacing -- a REPACK, not a
   raw copy, since two commands sharing this run can legally have used
   different original strides for the same attribute (stride is an
   independent per-call parameter, not derived from fvf), and one
   glVertexAttribPointer call for the whole merged buffer can only
   express a single stride. Declared in d3d7_internal.h -- also reused
   by d3d7.c's draw_strided_execute_now for the exact same repack need,
   just for one command's own streams instead of a merged run's. */
void merge_copy_stream(uint8_t *dst, uint32_t dst_voffset,
                        const d3ddp_ptrstride_t *src, uint32_t count, uint32_t natural_size)
{
    uint32_t effective = src->stride != 0 ? src->stride : natural_size;
    const uint8_t *sp = (const uint8_t *)src->data;
    uint8_t *dp = dst + (size_t)dst_voffset * natural_size;
    for (uint32_t v = 0; v < count; v++) {
        memcpy(dp + (size_t)v * natural_size, sp + (size_t)v * effective, natural_size);
    }
}

/* Strided sibling of merge_and_execute_flat_run -- same vertex-count/
   indexing logic, but each present stream (shared presence pattern
   guaranteed by cmds_mergeable) is independently repacked via
   merge_copy_stream. The merged d3ddrawprimitivestrideddata_t is
   memset to zero first so untouched fields (specular, texcoord[2..7])
   stay {NULL,0} exactly like a recorded command's own struct -- neither
   is read downstream today, but this is explicit rather than assumed. */
static void merge_and_execute_strided_run(d3d_device7_t *replay_shadow, render_cmd_t **run, uint32_t run_len)
{
    uint32_t prim_type = run[0]->u.draw_strided.prim_type;
    uint32_t fvf = run[0]->u.draw_strided.fvf;
    fvf_layout_t layout = decode_fvf(fvf);

    uint32_t total_vertices = 0;
    int any_indexed = 0;
    for (uint32_t k = 0; k < run_len; k++) {
        total_vertices += run[k]->u.draw_strided.vertex_count;
        if (run[k]->u.draw_strided.indices != NULL) any_indexed = 1;
    }

    int present[5];
    for (int w = 0; w < 5; w++) present[w] = strided_stream_ptr(w, &run[0]->u.draw_strided.sd)->data != NULL;

    uint8_t *stream_buf[5] = { NULL, NULL, NULL, NULL, NULL };
    for (int w = 0; w < 5; w++) {
        if (!present[w]) continue;
        stream_buf[w] = malloc((size_t)total_vertices * strided_natural_size(w, &layout));
    }

    uint16_t *merged_indices = NULL;
    uint32_t merged_index_count = 0;
    if (any_indexed) {
        uint32_t total_indices = 0;
        for (uint32_t k = 0; k < run_len; k++) {
            total_indices += run[k]->u.draw_strided.indices != NULL
                ? run[k]->u.draw_strided.index_count : run[k]->u.draw_strided.vertex_count;
        }
        merged_indices = malloc((size_t)total_indices * sizeof(uint16_t));
        merged_index_count = total_indices;
    }

    uint32_t voff = 0, ioff = 0;
    for (uint32_t k = 0; k < run_len; k++) {
        render_cmd_t *c = run[k];
        uint32_t vc = c->u.draw_strided.vertex_count;
        for (int w = 0; w < 5; w++) {
            if (!present[w]) continue;
            merge_copy_stream(stream_buf[w], voff, strided_stream_ptr(w, &c->u.draw_strided.sd),
                               vc, strided_natural_size(w, &layout));
        }
        if (any_indexed) {
            if (c->u.draw_strided.indices != NULL) {
                for (uint32_t v = 0; v < c->u.draw_strided.index_count; v++)
                    merged_indices[ioff++] = (uint16_t)(c->u.draw_strided.indices[v] + voff);
            } else {
                for (uint32_t v = 0; v < vc; v++)
                    merged_indices[ioff++] = (uint16_t)(voff + v);
            }
        }
        voff += vc;
    }

    d3ddrawprimitivestrideddata_t merged_sd;
    memset(&merged_sd, 0, sizeof(merged_sd));
    if (present[0]) merged_sd.position    = (d3ddp_ptrstride_t){ stream_buf[0], strided_natural_size(0, &layout) };
    if (present[1]) merged_sd.normal      = (d3ddp_ptrstride_t){ stream_buf[1], strided_natural_size(1, &layout) };
    if (present[2]) merged_sd.diffuse     = (d3ddp_ptrstride_t){ stream_buf[2], strided_natural_size(2, &layout) };
    if (present[3]) merged_sd.texcoord[0] = (d3ddp_ptrstride_t){ stream_buf[3], strided_natural_size(3, &layout) };
    if (present[4]) merged_sd.texcoord[1] = (d3ddp_ptrstride_t){ stream_buf[4], strided_natural_size(4, &layout) };

    /* See merge_and_execute_flat_run's own comment on why this calls the
       _impl body directly, passing replay_shadow. */
    draw_strided_execute_now_impl(replay_shadow, prim_type, fvf, &merged_sd, total_vertices, merged_indices, merged_index_count);

    for (int w = 0; w < 5; w++) free(stream_buf[w]);
    free(merged_indices);
}

/* Replays every recorded command in the exact order it was issued, using
   replay_shadow (a GL-thread-EXCLUSIVE d3d_device7_t, see its own
   comment) as the state/target/texture view every execute_now call reads
   -- each command (or, since Phase 3b, each merged RUN of commands)
   temporarily loads its own state/target/texture into replay_shadow just
   long enough for its own execute_now call to read them (the same fields
   apply_render_state/ensure_rendering_target/bind_texture_stages already
   read today), matching real immediate-mode semantics exactly: draw N
   always used the state that was live when draw N itself was issued,
   never anything issued after it. A run's commands are, by construction
   (cmds_mergeable), state-identical to run[0], so loading run[0]'s
   snapshot once for the whole run is exactly equivalent to loading each
   member's own snapshot N times.

   Phase 2 (async/double-buffered pipelining): unlike the original
   single-buffered design, this no longer touches the real device's own
   state/render_target/textures at all (no "borrow, then restore the real
   live values" dance) -- replay_shadow starts this function completely
   fresh (state_generation forced to a sentinel below, guaranteed to
   differ from every real command's, so the very first command's state
   always loads for real) and is simply abandoned when this function
   returns; the real dev is free to be concurrently recording (and
   mutating its OWN state/render_target/textures for) the NEXT scene on
   the game thread the entire time this runs on the GL thread -- see
   [[project_gl_issuer_thread]]'s Phase 2 design. */
static void cmdbuf_flush_impl(cmdbuf_slot_t *slot, d3d_device7_t *replay_shadow)
{
    /* Phase 4: builds slot->order, an index permutation over cmd_buf, so
       non-adjacent same-state commands within a safe zone become
       adjacent below -- see cmdbuf_build_order's own comment for the
       full safety envelope. Every slot->cmd_buf[...] index access below
       goes through slot->order[...] instead of the raw index directly;
       everything else about this loop (run-length extension via
       cmds_mergeable against run[0], state/render_target/textures
       loading, execute-now dispatch, per-command cleanup) is unchanged
       from Phase 3b. */
    /* Diagnostic split of cmdbuf_flush's own total (see cmdbuf_flush's
       own wrapper below) -- isolates the qsort-based reordering step
       specifically from the merge/replay loop that follows, to settle
       which half of a real 600ms+ cmdbuf_flush capture is the actual
       cost before changing either. Not a permanent counter -- folded
       back out once a real capture answers the question. */
    activity_sampler_enter(ACTIVITY_CMDBUF_SORT);
    double sort_t0 = d3d7_perf_trace_enabled() ? d3d7_perf_now_ms() : 0.0;
    cmdbuf_build_order(slot);
    if (d3d7_perf_trace_enabled()) {
        fprintf(stderr, "[cmdbuf-sort-diag] cmd_count=%u sort_ms=%.2f\n",
                slot->cmd_count, d3d7_perf_now_ms() - sort_t0);
    }
    activity_sampler_leave();

    /* Sentinel, not a real generation (real ones start at 1 and only
       increase, see d3d_device7_create) -- guarantees the very first
       command's `replay_shadow->state_generation != cmd->state_generation`
       check below is true even though replay_shadow is otherwise
       zeroed/stale from whatever the LAST flush left in it. */
    replay_shadow->state_generation = (uint64_t)-1;
    replay_shadow->render_target = NULL;
    memset(replay_shadow->textures, 0, sizeof(replay_shadow->textures));

    uint32_t i = 0;
    while (i < slot->cmd_count) {
        render_cmd_t *cmd = &slot->cmd_buf[slot->order[i]];

        uint32_t run_len = 1;
        if (cmd->kind != CMD_CLEAR &&
            prim_type_mergeable(cmd_prim_type(cmd)) && cmd_draw_count_is_clean(cmd)) {
            uint32_t total_vertices = cmd_vertex_count(cmd);
            while (i + run_len < slot->cmd_count) {
                render_cmd_t *next = &slot->cmd_buf[slot->order[i + run_len]];
                if (next->kind == CMD_CLEAR || !cmds_mergeable(cmd, next)) break;
                uint32_t next_vc = cmd_vertex_count(next);
                if (total_vertices + next_vc > MERGE_MAX_VERTICES) break;
                total_vertices += next_vc;
                run_len++;
            }
        }

        /* d3d_state_t is a ~6.3KB struct -- skip the copy entirely when
           this command's snapshot generation matches what's already
           loaded (byte-identical by construction: state_generation is
           bumped on every actual mutator, see d3d_state_t's own comment,
           the same invariant cmds_mergeable/the uniform-upload cache
           already trust elsewhere in this file/d3d7.c). This is the
           common case: Phase 3b/4 merging already collapses runs that
           are ALSO texture/prim_type-identical into one load, but a
           same-generation stretch broken only by an unmergeable field
           (different bound texture, a non-mergeable prim type like
           TRIANGLESTRIP) previously still re-copied identical state on
           every single one of those iterations. render_target/textures
           aren't part of state_generation's contract (SetTexture/
           SetRenderTarget don't bump it) so those stay unconditional --
           they're just a few pointers, not worth guarding. */
        if (replay_shadow->state_generation != cmd->state_generation) {
            replay_shadow->state = *cmd->state;
            replay_shadow->state_generation = cmd->state_generation;
        }
        replay_shadow->render_target = cmd->render_target;
        memcpy(replay_shadow->textures, cmd->textures, sizeof(replay_shadow->textures));

        if (run_len == 1) {
            switch (cmd->kind) {
                case CMD_CLEAR:
                    clear_execute_now_impl(replay_shadow, cmd->u.clear.rect_count, cmd->u.clear.rects,
                                       cmd->u.clear.flags, cmd->u.clear.color, cmd->u.clear.zbits, cmd->u.clear.stencil);
                    break;
                case CMD_DRAW_FLAT:
                    draw_flat_execute_now_impl(replay_shadow, cmd->u.draw_flat.prim_type, cmd->u.draw_flat.fvf,
                                           cmd->u.draw_flat.vertices, cmd->u.draw_flat.vertex_count,
                                           cmd->u.draw_flat.indices, cmd->u.draw_flat.index_count);
                    break;
                case CMD_DRAW_STRIDED:
                    draw_strided_execute_now_impl(replay_shadow, cmd->u.draw_strided.prim_type, cmd->u.draw_strided.fvf,
                                              &cmd->u.draw_strided.sd, cmd->u.draw_strided.vertex_count,
                                              cmd->u.draw_strided.indices, cmd->u.draw_strided.index_count);
                    break;
            }
            cmdbuf_free_command(cmd);
        } else {
            /* run's members may no longer be contiguous in slot->cmd_buf
               (Phase 4 permutes indices, not the ~6KB structs themselves)
               -- gather pointers into a small transient array, only ever
               allocated for an actual multi-command run. */
            render_cmd_t **run_ptrs = malloc((size_t)run_len * sizeof(*run_ptrs));
            for (uint32_t k = 0; k < run_len; k++) run_ptrs[k] = &slot->cmd_buf[slot->order[i + k]];
            if (cmd->kind == CMD_DRAW_FLAT) {
                merge_and_execute_flat_run(replay_shadow, run_ptrs, run_len);
            } else {
                merge_and_execute_strided_run(replay_shadow, run_ptrs, run_len);
            }
            free(run_ptrs);
            /* Every one of the run's N ORIGINAL commands still gets
               freed exactly once each -- merging only changes what gets
               REPLAYED, never skips this existing per-command cleanup
               (releases each one's own independently-AddRef'd
               render_target/textures, frees its own owned buffers). */
            for (uint32_t k = 0; k < run_len; k++) cmdbuf_free_command(&slot->cmd_buf[slot->order[i + k]]);
        }

        i += run_len;
    }

    slot->cmd_count = 0;
    cmdbuf_free_state_snapshots(slot);

    /* TEMP DIAGNOSTIC -- see d3d7.c's own replay_diag_accum_t comment.
       Reported (and reset) once per real cmdbuf_flush call, same
       cadence as [cmdbuf-sort-diag] above, so the two lines directly
       correlate against the SAME flush. */
    if (d3d7_perf_trace_enabled()) {
        double prepare_ms, upload_ms, attrib_ms, dispatch_ms;
        long draw_count;
        d3d7_replay_diag_report_and_reset(&prepare_ms, &upload_ms, &attrib_ms, &dispatch_ms, &draw_count);
        fprintf(stderr, "[replay-diag] draws=%ld prepare_ms=%.2f upload_ms=%.2f attrib_ms=%.2f dispatch_ms=%.2f\n",
                draw_count, prepare_ms, upload_ms, attrib_ms, dispatch_ms);
    }
}

/* Phase 2 (async/double-buffered pipelining): what travels from EndScene
   (game thread) to the GL thread's async replay -- just the slot being
   replayed and the device's own persistent replay_shadow (see its
   comment). Heap-allocated (gl_thread_post's own contract: unlike
   gl_thread_call, the poster does not block, so a stack-allocated arg
   would be gone before this ever runs) and freed by the job itself once
   it's done. Perf timing moved IN HERE (from cmdbuf_flush below) since
   the actual replay now happens later/elsewhere -- see cmdbuf_flush's own
   comment on why timing the post call itself would no longer measure
   anything meaningful. */
typedef struct {
    cmdbuf_slot_t *slot;
    d3d_device7_t *replay_shadow;
} cmdbuf_flush_job_t;

static void cmdbuf_flush_job_run(void *arg)
{
    cmdbuf_flush_job_t *job = (cmdbuf_flush_job_t *)arg;
    double t0 = d3d7_perf_trace_enabled() ? d3d7_perf_now_ms() : 0.0;
    cmdbuf_flush_impl(job->slot, job->replay_shadow);
    if (d3d7_perf_trace_enabled()) d3d7_perf_record_cmdbuf_flush(d3d7_perf_now_ms() - t0);
    free(job);
}

void cmdbuf_flush(d3d_device7_t *dev)
{
    cmdbuf_slot_t *cur_slot = cmdbuf_active_slot(dev);
    /* An empty flush (nothing recorded this scene) shouldn't count as a
       real call, or need a slot hand-off/flip at all. */
    if (cur_slot->cmd_count == 0) return;

    cmdbuf_flush_job_t *job = malloc(sizeof(*job));
    job->slot = cur_slot;
    job->replay_shadow = dev->replay_shadow;

    /* Flip BEFORE posting -- cmdbuf_record_* calls issued by the game
       thread the MOMENT this function returns (the very next
       DrawPrimitive of the next scene, potentially) must land in the
       OTHER slot, never this one, which now belongs exclusively to the
       just-posted async job until it finishes. */
    dev->cmdbuf_recording_slot ^= 1;

    /* Async -- does NOT wait for replay to finish, which is the entire
       point of Phase 2 (the game can go on recording the next scene
       while this one replays). Real backpressure still applies: this
       call blocks if the GL thread is still busy with an EARLIER posted
       job, and by the time it returns, that earlier job (whichever one
       most recently used the slot dev->cmdbuf_recording_slot now points
       at) is guaranteed finished -- see gl_thread_post's own comment for
       the exact argument. That's what makes it safe for recording to
       start immediately into the slot just flipped to, with no separate
       "wait for my slot" call needed here. */
    gl_thread_post(cmdbuf_flush_job_run, job);
}



/* Drops every recorded command WITHOUT replaying it -- defensive teardown
   only (a fresh BeginScene, or full device Release); should be a no-op
   in the normal case since EndScene always flushes first. Only ever
   touches the device's CURRENT recording slot -- the other slot, if it
   has a real async job in flight, is that job's own business (see
   d3d_device7_Release's own drain-before-free for how that's handled at
   device teardown). */
void cmdbuf_discard(d3d_device7_t *dev)
{
    cmdbuf_slot_t *slot = cmdbuf_active_slot(dev);
    for (uint32_t i = 0; i < slot->cmd_count; i++) cmdbuf_free_command(&slot->cmd_buf[i]);
    slot->cmd_count = 0;
    cmdbuf_free_state_snapshots(slot);
}
