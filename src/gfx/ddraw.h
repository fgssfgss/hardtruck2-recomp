#ifndef LONGROAD2_SHIM_DDRAW_H
#define LONGROAD2_SHIM_DDRAW_H

/*
 * ddraw/ddraw7 shim: DirectDraw7 -- the real 2D/HUD rendering path
 * (surface blitting), highest priority module in this project. D3D7
 * (src/shim/d3d7/d3d7.c) is the real 3D-world renderer -- GOAL.md's
 * original "Finding 1" (D3D never driven) was retracted once live
 * tracing reached real gameplay; see GOAL.md/AGENT.md for the numbers.
 *
 * Surfaces are CPU-side buffers; Lock/Unlock return pointers into them;
 * Blt performs rect copies honoring color-key; present-time compositing
 * uploads the composited buffer as a GLES texture via src/render/ and
 * draws it full-screen through SDL2's GL context each frame. Surfaces
 * also interoperate with D3D7 rendering (surface7_t's gl_target_t +
 * surface_sync_t sync_state tri-state, see ddraw.c) -- a surface can be
 * a D3D7 render target or texture-stage source and still be Blt/Locked
 * from plain DirectDraw code without either side seeing stale pixels.
 *
 * Confirmed call list, including the hot path (IsLost ~75k/session,
 * Blt ~8k, Lock/Unlock ~2.6k each): docs/api-surface/ddraw.md
 */

#include "shim_common.h"

shim_fn_t shim_ddraw_lookup(const char *name, uint32_t ordinal);

/* Test-only introspection into g_perf's own blt_calls/bltfast_calls split
   (LONGROAD2_PERF_TRACE's own "blt_copy"/"bltfast" fields) -- proves
   surface7_Blt and surface7_BltFast land in separate counters, see
   [[project_ddraw_bltfast_gpu_path_investigation]]. Requires
   LONGROAD2_PERF_TRACE=1 to observe anything (same gate the real
   increment sites already check). */
void ddraw_perf_reset_for_test(void);
long ddraw_perf_get_blt_calls_for_test(void);
long ddraw_perf_get_bltfast_calls_for_test(void);

/* Test-only introspection (mirrors d3d7.h's d3d7_perf_reset_for_test
   pattern): proves ddraw_gl_get_texture's native RGB565 fast path was
   ACTUALLY taken, not just that the rendered color happens to be right
   -- the CPU fallback path produces the same visible result for this
   format, so a color-only check alone can't distinguish the two. */
void ddraw_native565_reset_for_test(void);
long ddraw_native565_upload_count_for_test(void);

/* Test-only: forces a surface into the exact "no valid pixel source"
   state surface7_present's own no_valid_source guard exists for --
   sync_state==SURFACE_GPU_AHEAD (->buffer is stale by definition) with
   ->width/->height set to something ->gl was never resized to match.
   Real gameplay trace already showed this arises from a menu<->gameplay
   resolution switch (see surface7_sync_from_gpu's own comment); this
   lets a test reproduce the resulting STATE directly rather than needing
   to replay that exact, hard-to-control real-world trigger sequence
   through the public API. Only touches sync_state/width/height --
   ->gl itself is left exactly as whatever real D3D7 rendering already
   put there, which is the whole point (a real, valid, correctly-sized-
   for-its-OWN-content GPU texture that the surface's CURRENT width/
   height no longer agrees with). */
void ddraw_force_gpu_ahead_size_mismatch_for_test(void *surface, uint32_t fake_width, uint32_t fake_height);

#endif /* LONGROAD2_SHIM_DDRAW_H */
