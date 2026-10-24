#include "ddraw.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ddraw_types.h"
#include "gfx_mem.h"

#include "d3d7.h"
#include "kernel32.h"
#include "activity_sampler.h"
#include "platform.h"
#include "render.h"
#include "gl_thread.h"

/*
 * DirectDraw7 COM objects. Only DirectDrawCreateEx/DirectDrawEnumerateA
 * are real PE imports (see docs/api-surface/ddraw.md) -- everything else
 * here is a vtable method reached via QueryInterface, never through
 * shim_ddraw_lookup().
 *
 * Real surface bookkeeping (create/lock/unlock/refcounts/attachment/
 * palette/color-key/caps all genuinely tracked) plus a real CPU-side
 * pixel-copying blitter: Blt/BltFast do format-converting rect copies
 * (honoring src color-key and DDBLT_COLORFILL), Flip swaps front/back
 * buffer contents, and any write to a DDSCAPS_PRIMARYSURFACE surface
 * pushes a frame to src/render/'s GLES present pipeline
 * (render_present_rgba). No attempt at matching real blit performance or
 * every DDBLTFX effect (ROP/alpha/z-buffer/mirroring) -- those flags are
 * accepted and ignored, same as a HEL software blitter silently not
 * supporting an effect it can't do.
 *
 * Vtable layouts cross-checked against
 * /usr/include/wine-staging-11.13/wine/windows/ddraw.h.
 */

/* TEMP DIAGNOSTIC (opt-in via LONGROAD2_DDRAW_TRACE): traces surface
   creation/attachment/blit activity to find out what the game's software
   3D renderer actually does (or doesn't) with DirectDraw surfaces --
   see the missing-3D-world investigation. Remove once resolved. */
/* Shared by every LONGROAD2_* boolean debug switch below: unset OR set to
   "0" both mean off, any other value means on -- getenv() != NULL alone
   (this file's original pattern) treats VAR=0 the same as VAR=1, since
   both merely SET the variable; a user reasonably expecting VAR=0 to
   mean "disabled" (a genuinely more common shell convention than "unset
   it to disable") got the opposite. */
static int env_flag_enabled(const char *name)
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

/* --- Perf trace (opt-in via LONGROAD2_PERF_TRACE) ---
   Requested in place of a real profiler (not available on the target
   machine): a lightweight, always-compiled-in timer around the handful
   of functions already suspected of being the "laggy/99% CPU" cost --
   the GPU->CPU readback + per-pixel format conversion
   (surface7_sync_from_gpu), the CPU-side present conversion + upload
   (surface7_present), and the Blt/BltFast per-pixel copy loops
   themselves. Accumulates per-category wall-clock time and call counts
   between Flip() calls (this game's own natural frame boundary,
   confirmed elsewhere in this file) and prints one summary line per
   frame, rather than one line per call -- a call-per-line trace at
   these call frequencies (Blt alone: ~100k+/session) would be its own
   performance problem and mostly noise. */
static int perf_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = env_flag_enabled("LONGROAD2_PERF_TRACE");
    return enabled;
}

static double perf_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* CLOCK_THREAD_CPUTIME_ID, not CLOCK_MONOTONIC: real CPU time actually
   CONSUMED by this thread (the one calling Flip(), i.e. the game's own
   main thread -- game logic, input, and every x86 instruction box86
   translates/executes for it all run here, entirely outside anything
   else this trace line instruments), as opposed to wall-clock time
   elapsed regardless of whether this thread was ever scheduled to run
   during it. Diffed the same way real_wall_ms is (see
   perf_report_and_reset_if_due) -- the two together answer "is the real_
   wall/unaccounted gap genuine computation (box86 translating/running
   the game's own code, expected to show up here) or a blocking wait
   somewhere still uninstrumented (would show real_wall growing while
   this stays small)," see
   [[project_render_ground_truth_frame_timer]]. king.exe is confirmed to
   spawn additional real OS threads of its own (CreateThread, see
   [[project_thread_naming]]) -- this deliberately measures only the ONE
   thread driving the render loop, not the whole process, to isolate
   what's actually blocking frame-to-frame progress from unrelated
   background thread activity (audio, etc.) that wouldn't hold this
   thread back at all. */
static double thread_cpu_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
typedef struct {
    double sync_ms, present_ms, blt_ms;
    long sync_calls, present_calls, blt_calls;
    long blt_pixels;
    /* present_ms/present_calls above cover the CPU-buffer paths (RGBA and
       paletted) only -- this pair is separate so a LONGROAD2_PERF_TRACE
       capture can directly show the GPU-resident-direct path (see
       surface7_present's use_gpu_direct) actually taking over from the
       CPU one, the same way sync_from_gpu's own call count was what
       first exposed the minimap bug (see
       [[project_ddraw_gpu_blt_glreadpixels_elimination]]). */
    double present_gpu_ms;
    long present_gpu_calls;
    /* Separate from blt_ms/blt_calls/blt_pixels above -- surface7_BltFast
       (IDirectDrawSurface7::BltFast, a real, separate COM method from
       Blt, invisible to any PE-import-based call trace since it's a
       vtable dispatch) has NO GPU-accelerated path at all: always a CPU
       per-pixel/per-row loop, always an eager surface7_sync_from_gpu on
       both surfaces first. Used to feed into the SAME blt_ms/blt_calls
       counters as Blt's own (mostly GPU-capable) path, making a
       LONGROAD2_PERF_TRACE capture unable to tell "many cheap GPU Blts"
       apart from "a couple of expensive CPU BltFasts" -- exactly the
       ambiguity that made an early "x2 calls, 65620px, 18-21ms" reading
       look inexplicable until this split (see
       [[project_ddraw_bltfast_gpu_path_investigation]]). */
    double bltfast_ms;
    long bltfast_calls;
    long bltfast_pixels;
} perf_accum_t;
static perf_accum_t g_perf;
/* d3d (the accumulated 3D-scene stats for everything since the last
   report, including its own glFinish() timing) is pulled by the caller
   via d3d7_perf_report_for_frame BEFORE this prints -- see that
   function's own comment for why this replaced d3d7.c doing its own
   independent, per-EndScene glFinish()+print+reset. Folding it into
   THIS line (rather than two separate trace lines) is what makes one
   frame's worth of 3D-scene work and ddraw's own Blt/present/sync work
   directly readable together instead of needing to correlate two
   independently-cadenced trace streams by eye. */
static void perf_report_and_reset_if_due(const d3d7_perf_frame_report_t *d3d)
{
    if (!perf_trace_enabled()) return;
    /* real_wall_ms: ACTUAL wall-clock time elapsed since the last report,
       independent of what any individual bucket below claims to measure
       -- ground truth, not a sum of parts. Every other field on this
       line is "time spent inside a specific, instrumented call" -- this
       is "how much real time passed, period," so unaccounted (real_wall
       minus total) directly answers "is there real cost happening
       somewhere this line doesn't instrument at all" without having to
       add and rule out one more speculative bucket at a time (see
       [[project_ddraw_bltfast_gpu_path_investigation]]'s own msgpump_wait/
       bltfast additions, both of which came back clean while the game
       still felt far slower than `total` implied -- this is the direct
       way to settle whether more such buckets are still missing, instead
       of guessing which one). -1.0 sentinel (not 0.0) for "no previous
       report yet," same convention g_primary_last_present_ms already
       uses -- the FIRST report in a session has no real interval to
       measure and must not print a bogus ~large first-ever delta. */
    static double g_last_report_ms = -1.0;
    double now_ms = perf_now_ms();
    double real_wall_ms = (g_last_report_ms < 0.0) ? 0.0 : (now_ms - g_last_report_ms);
    g_last_report_ms = now_ms;

    /* thread_cpu: real CPU time THIS thread actually consumed since the
       last report -- see thread_cpu_now_ms's own comment for why this,
       not real_wall_ms, is what actually distinguishes "genuine
       computation" from "blocked/waiting." Same -1.0-sentinel,
       diff-since-last-report shape as real_wall_ms above. */
    static double g_last_thread_cpu_ms = -1.0;
    double thread_cpu_now = thread_cpu_now_ms();
    double thread_cpu_ms = (g_last_thread_cpu_ms < 0.0) ? 0.0 : (thread_cpu_now - g_last_thread_cpu_ms);
    g_last_thread_cpu_ms = thread_cpu_now;

    /* msgpump_wait: real time blocked in platform_get_message's own
       SDL_Delay(1) wait loop since the last report -- see that function's
       own comment for why this is worth watching (real GetMessage
       semantics, genuinely exercised by king.exe, completely outside
       every other bucket on this line). Pulled the same way d3d's own
       report is: read+reset right here, once per real Flip(). */
    double msgpump_wait_ms = 0.0;
    long msgpump_wait_calls = 0;
    platform_perf_report_for_frame(&msgpump_wait_ms, &msgpump_wait_calls);

    /* cs_wait/wfso_wait: real time blocked acquiring a critical section
       or waiting on an event/thread handle, since the last report --
       see kernel32.c's own EnterCriticalSection/WaitForSingleObject
       comments for why (user's own hypothesis: real contention, made
       proportionally worse because a held section's own protected work
       takes far longer under box86 emulation than natively). Deliberately
       NOT folded into total/unaccounted below: unlike every other field
       on this line (scoped to whichever thread calls Flip()), these two
       counters are process-wide -- ANY thread's Enter/WaitForSingleObject
       call counts, including king.exe's own worker threads -- so mixing
       them into a total meant to reconcile ONE thread's own real_wall
       vs. thread_cpu would misattribute cost across threads. Read
       standalone, alongside those two, not as part of the same sum. */
    double cs_wait_ms = 0.0; long cs_calls = 0, cs_contended = 0;
    double wfso_wait_ms = 0.0; long wfso_calls = 0, wfso_contended = 0;
    kernel32_perf_report_for_frame(&cs_wait_ms, &cs_calls, &cs_contended, &wfso_wait_ms, &wfso_calls, &wfso_contended);

    /* d3d_calls: in-function wall time + call count across the 9 D3D7
       fixed-function state-setter vtable methods (SetRenderState,
       SetTextureStageState, SetTransform, SetViewport, SetMaterial,
       SetLight, LightEnable, SetTexture, MultiplyTransform) -- see
       d3d7.c's own g_d3d7_perf comment. Thread-scoped like gpu_finish_ms
       (these all run synchronously on whichever thread calls the D3D7
       vtable, same as every other bucket folded into total_ms below),
       unlike cs_enter/wfso above -- folded into total_ms so a real
       capture directly shows how much of the previous `unaccounted` gap
       this closes, same as msgpump_wait's own addition did. */
    /* draw_dispatch: d3d_draw_common/d3d_draw_common_strided's own
       in-function wall time -- see d3d7.h's D3D7_TEST_CALL_DRAWCOMMON*
       comment. A real BOX86_DYNAREC_LOG=2 capture found these two
       functions dominate ALL box86-translated code execution across an
       entire king.exe session (~100% of dynarec blocks entered, vs the
       state-setter calls above which measured cheap) -- this is the
       direct, targeted measurement that capture motivated. Thread-scoped,
       folded into total_ms same as d3d_calls/state_call_ms above. */
    /* cmdbuf_flush: full wall time of Phase 2/3b/4's per-scene replay --
       build_order's qsort (ptr_compare/order_compare, the two functions a
       real BOX86_DYNAREC_LOG=2 capture found dominating ALL dynarec block
       executions, see [[project_box86_dynarec_log_analysis]]) plus the
       merge/replay loop's real draw_flat_execute_now/draw_strided_
       execute_now GL-issuing calls -- never wall-clock timed before this
       (only its downstream GPU-completion cost, via gpu_finish_ms's
       trailing glFinish(), was ever captured). Thread-scoped, folded into
       total_ms same as the other d3d7.c buckets above. */
    double total_ms = g_perf.sync_ms + g_perf.present_ms + g_perf.present_gpu_ms + g_perf.blt_ms + g_perf.bltfast_ms +
                       d3d->gpu_finish_ms + msgpump_wait_ms + d3d->state_call_ms + d3d->draw_dispatch_ms +
                       d3d->cmdbuf_flush_ms;
    fprintf(stderr, "[perf-trace] frame: sync_from_gpu=%.2fms(x%ld) present=%.2fms(x%ld) "
                    "present_gpu=%.2fms(x%ld) "
                    "blt_copy=%.2fms(x%ld, %ld px) "
                    "bltfast=%.2fms(x%ld, %ld px) "
                    "d3d7_gpu_finish=%.2fms(draws=%ld verts=%ld idx=%ld uniform_up=%ld uniform_skip=%ld gl_draws=%ld) "
                    "msgpump_wait=%.2fms(x%ld) "
                    "cs_enter=%.2fms(x%ld, %ld contended) wfso=%.2fms(x%ld, %ld contended) "
                    "d3d_calls=%.2fms(x%ld) "
                    "draw_dispatch=%.2fms(x%ld) "
                    "cmdbuf_flush=%.2fms(x%ld) "
                    "total=%.2fms real_wall=%.2fms unaccounted=%.2fms thread_cpu=%.2fms\n",
            g_perf.sync_ms, g_perf.sync_calls, g_perf.present_ms, g_perf.present_calls,
            g_perf.present_gpu_ms, g_perf.present_gpu_calls,
            g_perf.blt_ms, g_perf.blt_calls, g_perf.blt_pixels,
            g_perf.bltfast_ms, g_perf.bltfast_calls, g_perf.bltfast_pixels,
            d3d->gpu_finish_ms, d3d->draw_calls, d3d->vertices, d3d->indices,
            d3d->uniform_uploads, d3d->uniform_skips, d3d->gl_draws,
            msgpump_wait_ms, msgpump_wait_calls,
            cs_wait_ms, cs_calls, cs_contended, wfso_wait_ms, wfso_calls, wfso_contended,
            d3d->state_call_ms, d3d->state_call_count,
            d3d->draw_dispatch_ms, d3d->draw_dispatch_count,
            d3d->cmdbuf_flush_ms, d3d->cmdbuf_flush_count,
            total_ms, real_wall_ms, real_wall_ms - total_ms, thread_cpu_ms);
    memset(&g_perf, 0, sizeof(g_perf));

    /* Cooperative activity sampler (no-op / all-zero unless
       LONGROAD2_ACTIVITY_SAMPLE=1 -- see activity_sampler.h). Every
       bucket above measures OUR OWN code's wall time precisely; ACTIVITY_
       GAME below is the first thing that samples the GAPS between them
       directly, rather than inferring them as unaccounted's own
       subtraction. */
    unsigned long activity_counts[ACTIVITY_COUNT];
    unsigned long activity_total = 0;
    activity_sampler_report_and_reset(activity_counts, &activity_total);
    if (activity_total > 0) {
        static const char *const k_activity_names[ACTIVITY_COUNT] = {
            "game", "state_calls", "draw_dispatch", "cmdbuf_sort",
            "cmdbuf_prepare", "cmdbuf_upload", "cmdbuf_attrib", "cmdbuf_dispatch",
            "gpu_finish", "blt", "present", "msgpump",
            "prep_ensure_rt", "prep_ensure_shader", "prep_render_state",
            "prep_uniform_upload", "prep_bind_texture", "heap",
        };
        fprintf(stderr, "[perf-trace] activity samples=%lu:", activity_total);
        for (int i = 0; i < ACTIVITY_COUNT; i++) {
            fprintf(stderr, " %s=%.1f%%", k_activity_names[i],
                    100.0 * (double)activity_counts[i] / (double)activity_total);
        }
        fprintf(stderr, "\n");
    }
}

/* Real DirectDraw hardware: the primary surface is just memory: the
   display scans it out at its own pace, independent of how often the
   CPU writes to it. Blt() to the primary surface does not inherently
   wait for vsync. This shim's surface7_present (called from every
   primary-surface Blt/BltFast, see below) instead does a full CPU pixel
   conversion AND a real, vsync-blocking SDL_GL_SwapWindow every single
   time -- fine for the common case of one such Blt per real frame, but
   LONGROAD2_PERF_TRACE caught a real capture where this game's own
   loading-screen animation does ~140-180 consecutive full-surface Blts
   straight to the primary surface before ever calling Flip() (no
   separate "present" call exists in this game beyond Blt/Flip
   themselves, see the file header comment) -- 1.3-2.9 REAL SECONDS
   blocked in swaps for what the game intends as one screen's worth of
   animation frames, confirmed twice independently.

   FIRST ATTEMPT at this (a plain per-logical-frame counter, reset only
   at Flip) shipped and broke the game outright: "screen doesn't update
   in menu, can't load." Root cause of THAT bug: at least one menu/
   loading screen drives ALL of its own updates through Blt-to-primary
   alone and apparently never calls Flip() at all while it's up -- so a
   counter that only resets at Flip never got a fresh budget again once
   the cap was hit, and the screen froze at whatever the 4th present
   showed, forever. A pure time-based rate limit doesn't work either: a
   single blocked present already costs more wall-clock time than any
   reasonable window, so by the time the NEXT call is even reached,
   "enough time has passed" is already true again for every subsequent
   call too -- it wouldn't cap anything.

   Current design: a count-based cap per burst (bounds the zero-real-
   time storm, which is the actual cost problem) PLUS an independent
   staleness check against the time of the last REAL present (not the
   window start) that force-allows one through and resets the count if
   too long has passed -- this is what guarantees a Flip-less screen can
   never get stuck: once capped, subsequent Blts return fast (no
   present, no vsync block), so real time keeps advancing between them,
   and the staleness check eventually fires. Every Blt still updates the
   CPU buffer immediately regardless of any of this, so whichever
   present actually goes through always shows the latest composited
   content -- this only skips REDUNDANT intermediate presents, never the
   final state. */
static int g_primary_burst_count = 0;
static double g_primary_last_present_ms = -1.0; /* -1: no real present yet */
#define MAX_PRIMARY_PRESENTS_PER_BURST 4
#define PRIMARY_PRESENT_STALE_MS 50.0 /* >= this long since the last real present forces one through -- 20Hz floor, comfortably above "looks frozen" */

static int primary_present_allowed(void)
{
    double now = perf_now_ms();
    if (g_primary_last_present_ms < 0.0 || (now - g_primary_last_present_ms) >= PRIMARY_PRESENT_STALE_MS) {
        g_primary_burst_count = 0;
    }
    if (g_primary_burst_count >= MAX_PRIMARY_PRESENTS_PER_BURST) return 0;
    g_primary_burst_count++;
    g_primary_last_present_ms = now;
    return 1;
}

typedef uint32_t (WINAPI *comfn0_t)(void *t);
typedef uint32_t (WINAPI *comfn1_t)(void *t, uint32_t a);
typedef uint32_t (WINAPI *comfn2_t)(void *t, uint32_t a, uint32_t b);
typedef uint32_t (WINAPI *comfn3_t)(void *t, uint32_t a, uint32_t b, uint32_t c);
typedef uint32_t (WINAPI *comfn4_t)(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
typedef uint32_t (WINAPI *comfn5_t)(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
typedef uint32_t (WINAPI *comfn6_t)(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f);
typedef uint32_t (WINAPI *comfn7_t)(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g);

static uint32_t WINAPI fail0(void *t)
{ (void)t; fprintf(stderr, "[ddraw] unimplemented 0-arg method called\n"); return DDERR_GENERIC; }
static uint32_t WINAPI fail1(void *t, uint32_t a)
{ (void)t; (void)a; fprintf(stderr, "[ddraw] unimplemented 1-arg method called\n"); return DDERR_GENERIC; }
static uint32_t WINAPI fail2(void *t, uint32_t a, uint32_t b)
{ (void)t; (void)a; (void)b; fprintf(stderr, "[ddraw] unimplemented 2-arg method called\n"); return DDERR_GENERIC; }
static uint32_t WINAPI fail3(void *t, uint32_t a, uint32_t b, uint32_t c)
{ (void)t; (void)a; (void)b; (void)c; fprintf(stderr, "[ddraw] unimplemented 3-arg method called\n"); return DDERR_GENERIC; }
static uint32_t WINAPI fail4(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ (void)t; (void)a; (void)b; (void)c; (void)d; fprintf(stderr, "[ddraw] unimplemented 4-arg method called\n"); return DDERR_GENERIC; }
static uint32_t WINAPI fail5(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{ (void)t; (void)a; (void)b; (void)c; (void)d; (void)e; fprintf(stderr, "[ddraw] unimplemented 5-arg method called\n"); return DDERR_GENERIC; }

/* Named 4-arg unimplemented traps -- the generic fail4 above doesn't say
   WHICH of the several real comfn4-shaped methods was actually called,
   and a live report showed "unimplemented 4-arg method called" firing
   twice right after Flip with no way to tell which. These give a real
   answer the next time it fires, without needing to name all ~90 other
   generic-fail-mapped slots in this file too. */
static uint32_t WINAPI fail4_named(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d, const char *name)
{ (void)t; (void)a; (void)b; (void)c; (void)d;
  fprintf(stderr, "[ddraw] unimplemented method called: %s\n", name); return DDERR_GENERIC; }
static uint32_t WINAPI fail4_SetPrivateData(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ return fail4_named(t, a, b, c, d, "IDirectDrawSurface7::SetPrivateData"); }
static uint32_t WINAPI fail4_EnumSurfaces_v1(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ return fail4_named(t, a, b, c, d, "IDirectDraw::EnumSurfaces (v1)"); }
static uint32_t WINAPI fail4_EnumSurfaces_v2(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ return fail4_named(t, a, b, c, d, "IDirectDraw2::EnumSurfaces (v2)"); }
static uint32_t WINAPI fail4_EnumSurfaces_v4(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ return fail4_named(t, a, b, c, d, "IDirectDraw4::EnumSurfaces (v4)"); }
static uint32_t WINAPI fail4_EnumSurfaces_v7(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ return fail4_named(t, a, b, c, d, "IDirectDraw7::EnumSurfaces (v7)"); }
static uint32_t WINAPI fail4_TransformVertices(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ return fail4_named(t, a, b, c, d, "IDirect3DDevice::TransformVertices (execute-buffer legacy)"); }
static uint32_t WINAPI fail4_CreateDevice_v1(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ return fail4_named(t, a, b, c, d, "IDirect3D::CreateDevice (v1 -- shouldn't be reachable, real v1 has none)"); }

/* Trivial "succeed, do nothing" stubs -- used by the D3D1/2/3-era
   IDirect3D and IDirect3DDevice3 legacy objects below (see
   [[project_swep1rcr_port_scoping]]), where a real Windows game expects
   BeginScene/SetRenderState/DrawPrimitive/etc. to succeed even though
   this pass deliberately does not rasterize anything for the DX6-era
   execute-buffer pipeline -- unlike ddraw's own fail0..fail5 above
   (genuinely unexpected calls this shim doesn't know how to handle at
   all), these are EXPECTED, high-frequency per-frame calls that a real
   driver would satisfy trivially, so failing them loudly would make a
   game abort rendering entirely instead of just seeing a blank/inert
   scene. */
static uint32_t WINAPI ok0(void *t) { (void)t; return DD_OK; }
static uint32_t WINAPI ok1(void *t, uint32_t a) { (void)t; (void)a; return DD_OK; }
static uint32_t WINAPI ok2(void *t, uint32_t a, uint32_t b) { (void)t; (void)a; (void)b; return DD_OK; }
static uint32_t WINAPI ok3(void *t, uint32_t a, uint32_t b, uint32_t c) { (void)t; (void)a; (void)b; (void)c; return DD_OK; }
static uint32_t WINAPI ok4(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ (void)t; (void)a; (void)b; (void)c; (void)d; return DD_OK; }
static uint32_t WINAPI ok5(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{ (void)t; (void)a; (void)b; (void)c; (void)d; (void)e; return DD_OK; }
static uint32_t WINAPI ok7(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g)
{ (void)t; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; return DD_OK; }

/* TEMP DIAGNOSTIC (opt-in via LONGROAD2_DDRAW_TRACE): investigating a real,
   live black-screen report -- these trivial "succeed, do nothing" stubs are
   shared across many different vtable slots (see the comment above ok0..ok7
   itself), so a bare hit inside ok0..ok7 can't say WHICH real method got
   called. One small named trampoline per real vtable slot, each just
   logging its own name then delegating to the real underlying body, so a
   capture can show definitively whether this game's own code ever reaches
   any of these legacy D3D1-3 execute-buffer methods at all (this game's
   real rendering traces as IDirect3DDevice7, not this legacy path -- these
   should almost certainly never fire for it, and confirming that live
   rules the whole class out rather than leaving it a guess). Remove once
   resolved. */
static uint32_t WINAPI log_FlipToGDISurface(void *t)
{ if (ddraw_trace_enabled()) fprintf(stderr, "[ddraw-trace] STUB: IDirectDraw::FlipToGDISurface(t=%p)\n", t); return ok0(t); }
static uint32_t WINAPI log_light_Initialize(void *t, uint32_t a)
{ if (ddraw_trace_enabled()) fprintf(stderr, "[ddraw-trace] STUB: IDirect3DLight::Initialize(t=%p)\n", t); return ok1(t, a); }
static uint32_t WINAPI log_viewport_Initialize(void *t, uint32_t a)
{ if (ddraw_trace_enabled()) fprintf(stderr, "[ddraw-trace] STUB: IDirect3DViewport::Initialize(t=%p)\n", t); return ok1(t, a); }
static uint32_t WINAPI log_viewport_GetViewport_v1(void *t, uint32_t a)
{ if (ddraw_trace_enabled()) fprintf(stderr, "[ddraw-trace] STUB: IDirect3DViewport::GetViewport(v1, t=%p)\n", t); return ok1(t, a); }
static uint32_t WINAPI log_viewport_SetViewport_v1(void *t, uint32_t a)
{ if (ddraw_trace_enabled()) fprintf(stderr, "[ddraw-trace] STUB: IDirect3DViewport::SetViewport(v1, t=%p)\n", t); return ok1(t, a); }
static uint32_t WINAPI log_viewport_SetBackgroundDepth(void *t, uint32_t a)
{ if (ddraw_trace_enabled()) fprintf(stderr, "[ddraw-trace] STUB: IDirect3DViewport::SetBackgroundDepth(t=%p)\n", t); return ok1(t, a); }
static uint32_t WINAPI log_viewport_Clear(void *t, uint32_t a, uint32_t b, uint32_t c)
{ if (ddraw_trace_enabled()) fprintf(stderr, "[ddraw-trace] STUB: IDirect3DViewport::Clear(t=%p count=%u)\n", t, a); return ok3(t, a, b, c); }
static uint32_t WINAPI log_viewport_SetBackgroundDepth2(void *t, uint32_t a)
{ if (ddraw_trace_enabled()) fprintf(stderr, "[ddraw-trace] STUB: IDirect3DViewport::SetBackgroundDepth2(t=%p)\n", t); return ok1(t, a); }
static uint32_t WINAPI log_device3_AddViewport(void *t, uint32_t a)
{ if (ddraw_trace_enabled()) fprintf(stderr, "[ddraw-trace] STUB: IDirect3DDevice3::AddViewport(t=%p)\n", t); return ok1(t, a); }
static uint32_t WINAPI log_device3_DeleteViewport(void *t, uint32_t a)
{ if (ddraw_trace_enabled()) fprintf(stderr, "[ddraw-trace] STUB: IDirect3DDevice3::DeleteViewport(t=%p)\n", t); return ok1(t, a); }
static uint32_t WINAPI log_device3_Begin(void *t, uint32_t a, uint32_t b, uint32_t c)
{ if (ddraw_trace_enabled()) fprintf(stderr, "[ddraw-trace] STUB: IDirect3DDevice3::Begin(t=%p)\n", t); return ok3(t, a, b, c); }
static uint32_t WINAPI log_device3_BeginIndexed(void *t, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{ if (ddraw_trace_enabled()) fprintf(stderr, "[ddraw-trace] STUB: IDirect3DDevice3::BeginIndexed(t=%p)\n", t); return ok5(t, a, b, c, d, e); }
static uint32_t WINAPI log_device3_Vertex(void *t, uint32_t a)
{ if (ddraw_trace_enabled()) fprintf(stderr, "[ddraw-trace] STUB: IDirect3DDevice3::Vertex(t=%p)\n", t); return ok1(t, a); }
static uint32_t WINAPI log_device3_Index(void *t, uint32_t a)
{ if (ddraw_trace_enabled()) fprintf(stderr, "[ddraw-trace] STUB: IDirect3DDevice3::Index(t=%p)\n", t); return ok1(t, a); }
static uint32_t WINAPI log_device3_End(void *t, uint32_t a)
{ if (ddraw_trace_enabled()) fprintf(stderr, "[ddraw-trace] STUB: IDirect3DDevice3::End(t=%p)\n", t); return ok1(t, a); }
static uint32_t WINAPI log_device3_SetClipStatus(void *t, uint32_t a)
{ if (ddraw_trace_enabled()) fprintf(stderr, "[ddraw-trace] STUB: IDirect3DDevice3::SetClipStatus(t=%p)\n", t); return ok1(t, a); }
static uint32_t WINAPI log_device3_GetClipStatus(void *t, uint32_t a)
{ if (ddraw_trace_enabled()) fprintf(stderr, "[ddraw-trace] STUB: IDirect3DDevice3::GetClipStatus(t=%p)\n", t); return ok1(t, a); }
static uint32_t WINAPI log_EvictManagedTextures(void *t)
{ if (ddraw_trace_enabled()) fprintf(stderr, "[ddraw-trace] STUB: IDirect3D7::EvictManagedTextures(t=%p)\n", t); return ok0(t); }

/* IID_IDirectDraw7 / IID_IDirect3D7 -- cross-checked against
   /usr/lib/mingw64-toolchain/x86_64-w64-mingw32/include/{ddraw,d3d}.h */
static const guid_t IID_IDirectDraw7 =
    { 0x15e65ec0, 0x3b9c, 0x11d2, { 0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b } };
static const guid_t IID_IDirect3D7 =
    { 0xf5049e77, 0x4861, 0x11d2, { 0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8 } };
static const guid_t IID_IDirectDrawSurface7 =
    { 0x06675a80, 0x3b9b, 0x11d2, { 0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b } };
/* Real hardware commonly supports gamma-ramp control on the primary
   surface, so real code that QueryInterfaces for this often doesn't
   check the HRESULT before using the returned pointer -- a real, live
   crash confirmed this exact pattern (see gamma_control_create's own
   comment below). */
static const guid_t IID_IDirectDrawGammaControl =
    { 0x69c11c3e, 0xb46b, 0x11d1, { 0xad, 0x7a, 0x00, 0xc0, 0x4f, 0xc2, 0x9b, 0x4e } };

/* --- DirectDraw 1/2/4 and Direct3D 1/2/3 legacy IIDs -- SWEP1RCR
   (Star Wars Episode I: Racer, 1999, DX6-era) support -- see
   [[project_swep1rcr_port_scoping]]. Cross-checked against the same
   mingw ddraw.h/d3d.h headers as the block above. */
static const guid_t IID_IDirectDraw =
    { 0x6C14DB80, 0xA733, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const guid_t IID_IDirectDraw2 =
    { 0xB3A6F3E0, 0x2B43, 0x11CF, { 0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56 } };
static const guid_t IID_IDirectDraw4 =
    { 0x9c59509a, 0x39bd, 0x11d1, { 0x8c, 0x4a, 0x00, 0xc0, 0x4f, 0xd9, 0x30, 0xc5 } };
static const guid_t IID_IDirectDrawSurface =
    { 0x6C14DB81, 0xA733, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const guid_t IID_IDirectDrawSurface2 =
    { 0x57805885, 0x6eec, 0x11cf, { 0x94, 0x41, 0xa8, 0x23, 0x03, 0xc1, 0x0e, 0x27 } };
static const guid_t IID_IDirectDrawSurface3 =
    { 0xDA044E00, 0x69B2, 0x11D0, { 0xA1, 0xD5, 0x00, 0xAA, 0x00, 0xB8, 0xDF, 0xBB } };
static const guid_t IID_IDirectDrawSurface4 =
    { 0x0B2B8630, 0xAD35, 0x11D0, { 0x8E, 0xA6, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B } };
static const guid_t IID_IDirect3D =
    { 0x3BBA0080, 0x2421, 0x11CF, { 0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56 } };
static const guid_t IID_IDirect3D2 =
    { 0x6aae1ec1, 0x662a, 0x11d0, { 0x88, 0x9d, 0x00, 0xaa, 0x00, 0xbb, 0xb7, 0x6a } };
static const guid_t IID_IDirect3D3 =
    { 0xbb223240, 0xe72b, 0x11d0, { 0xa9, 0xb4, 0x00, 0xaa, 0x00, 0xc0, 0x99, 0x3e } };
static const guid_t IID_IDirect3DDevice3 =
    { 0xb0ab3b60, 0x33d7, 0x11d1, { 0xa9, 0x81, 0x00, 0xc0, 0x4f, 0xd7, 0xb1, 0x74 } };
static const guid_t IID_IDirect3DRGBDevice =
    { 0xA4665C60, 0x2673, 0x11CF, { 0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56 } };

/* IDirect3DLight/IDirect3DMaterial3/IDirect3DViewport3/IDirect3DTexture2 --
   real Viewport/Light/Material/Texture COM objects for the DX6-era D3D3
   pipeline (SWEP1RCR, see [[project_swep1rcr_port_scoping]]). Cross-checked
   against the same mingw ddraw.h/d3d.h headers as the block above, plus a
   compiled sizeof/offsetof probe against them for the wire structs these
   objects' methods take (see d3dlight_wire_t/d3dmaterial_wire_t/
   d3dviewport2_wire_t below). Note IID_IDirect3DViewport3 differs from
   IID_IDirect3DDevice3 above only in the last hex digit of Data1 (61 vs
   60) -- an easy transcription trap. */
static const guid_t IID_IDirect3DLight =
    { 0x4417C142, 0x33AD, 0x11CF, { 0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E } };
static const guid_t IID_IDirect3DMaterial3 =
    { 0xca9c46f4, 0xd3c5, 0x11d1, { 0xb7, 0x5a, 0x00, 0x60, 0x08, 0x52, 0xb3, 0x12 } };
static const guid_t IID_IDirect3DViewport3 =
    { 0xb0ab3b61, 0x33d7, 0x11d1, { 0xa9, 0x81, 0x00, 0xc0, 0x4f, 0xd7, 0xb1, 0x74 } };
static const guid_t IID_IDirect3DTexture2 =
    { 0x93281502, 0x8CF8, 0x11D0, { 0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29 } };

/* VIDEO CARD D3DEVICE GUID */
static const guid_t IID_D3DDEVICE_D3DUID = { 0xaeb2cdd4, 0x6e41, 0x43ea, { 0x94, 0x1c, 0x83, 0x61, 0xcc, 0x76, 0x07, 0x81 } };

/* ===================== IDirectDrawPalette ===================== */

typedef struct {
    comfn2_t QueryInterface;
    comfn0_t AddRef;
    comfn0_t Release;
    comfn1_t GetCaps;
    comfn4_t GetEntries;
    comfn3_t Initialize;
    comfn4_t SetEntries;
} palette7_vtbl_t;

typedef struct {
    uint8_t r, g, b, flags;
} paletteentry_t;

#define PALETTE_LUT_BITS 5 /* 5 bits/channel = 32768-entry RGB->nearest-index cache */
#define PALETTE_LUT_SIZE (1u << (PALETTE_LUT_BITS * 3))

typedef struct {
    const palette7_vtbl_t *lpVtbl;
    uint32_t refcount;
    paletteentry_t entries[256];
    uint8_t lut[PALETTE_LUT_SIZE]; /* rebuilt lazily, see palette7_ensure_lut */
    int lut_valid;
} palette7_t;

static uint32_t WINAPI palette7_QueryInterface(void *self, guid_t *riid, void **out)
{
    (void)riid;
    palette7_t *p = (palette7_t *)self;
    p->refcount++;
    if (out != NULL) *out = self;
    return DD_OK;
}
static uint32_t WINAPI palette7_AddRef(void *self) { return ++((palette7_t *)self)->refcount; }
static uint32_t WINAPI palette7_Release(void *self)
{
    palette7_t *p = (palette7_t *)self;
    uint32_t rc = --p->refcount;
    if (rc == 0) { gfx_native_freed(p); free(p); }
    return rc;
}
static uint32_t WINAPI palette7_GetCaps(void *self, uint32_t *out_caps)
{
    (void)self;
    if (out_caps != NULL) *out_caps = DDPCAPS_8BIT;
    return DD_OK;
}
static uint32_t WINAPI palette7_GetEntries(void *self, uint32_t flags, uint32_t base, uint32_t count, paletteentry_t *out)
{
    (void)flags;
    palette7_t *p = (palette7_t *)self;
    if (out == NULL || base + count > 256) return DDERR_INVALIDPARAMS;
    memcpy(out, &p->entries[base], count * sizeof(paletteentry_t));
    return DD_OK;
}
static uint32_t WINAPI palette7_Initialize(void *self, void *ddraw, uint32_t flags, paletteentry_t *color_table)
{
    (void)ddraw; (void)flags;
    palette7_t *p = (palette7_t *)self;
    if (color_table != NULL) memcpy(p->entries, color_table, sizeof(p->entries));
    return DD_OK;
}
static uint32_t WINAPI palette7_SetEntries(void *self, uint32_t flags, uint32_t base, uint32_t count, paletteentry_t *in)
{
    (void)flags;
    palette7_t *p = (palette7_t *)self;
    if (in == NULL || base + count > 256) return DDERR_INVALIDPARAMS;
    memcpy(&p->entries[base], in, count * sizeof(paletteentry_t));
    p->lut_valid = 0;
    return DD_OK;
}

static const palette7_vtbl_t g_palette7_vtbl = {
    .QueryInterface = (comfn2_t)palette7_QueryInterface,
    .AddRef = palette7_AddRef,
    .Release = palette7_Release,
    .GetCaps = (comfn1_t)palette7_GetCaps,
    .GetEntries = (comfn4_t)palette7_GetEntries,
    .Initialize = (comfn3_t)palette7_Initialize,
    .SetEntries = (comfn4_t)palette7_SetEntries,
};

static palette7_t *palette7_create(uint32_t flags, const paletteentry_t *color_table)
{
    (void)flags;
    palette7_t *p = calloc(1, sizeof(*p));
    p->lpVtbl = &g_palette7_vtbl;
    p->refcount = 1;
    if (color_table != NULL) memcpy(p->entries, color_table, sizeof(p->entries));
    return p;
}

/* Real DirectDraw's palette is effectively one hardware register for
   whatever's actually on screen (see surface7_SetPalette's own comment
   below), not an independent per-surface-object property. That comment's
   propagation-to-attached-surfaces logic only reaches the primary's back
   buffer/Z-buffer, though -- it was never meant to (and cannot) reach
   independently created surfaces the game never explicitly attaches or
   calls SetPalette on, which is the normal, expected way to create sprite/
   font/UI atlas surfaces in real DirectDraw code (their raw index bytes
   are still meant to be read through whatever palette is actually live,
   they just don't carry a pointer to it themselves). unpack_rgba's
   pal==NULL case falls back to this instead of literal grayscale, for
   exactly that reason -- see its own comment. Sets/clears only from
   surface7_SetPalette, which owns a matching AddRef/Release pair. */
static palette7_t *g_current_palette = NULL;

/* ===================== IDirectDrawClipper ===================== */
/* Zero confirmed calls (not even CreateClipper appears in
   docs/api-surface/ddraw.md's call tables) -- kept minimal on purpose. */

typedef struct {
    comfn2_t QueryInterface;
    comfn0_t AddRef;
    comfn0_t Release;
    comfn3_t GetClipList;
    comfn1_t GetHWnd;
    comfn2_t Initialize;
    comfn1_t IsClipListChanged;
    comfn2_t SetClipList;
    comfn2_t SetHWnd;
} clipper7_vtbl_t;

typedef struct {
    const clipper7_vtbl_t *lpVtbl;
    uint32_t refcount;
} clipper7_t;

static uint32_t WINAPI clipper7_QueryInterface(void *self, guid_t *riid, void **out)
{
    (void)riid;
    clipper7_t *c = (clipper7_t *)self;
    c->refcount++;
    if (out != NULL) *out = self;
    return DD_OK;
}
static uint32_t WINAPI clipper7_AddRef(void *self) { return ++((clipper7_t *)self)->refcount; }
static uint32_t WINAPI clipper7_Release(void *self)
{
    clipper7_t *c = (clipper7_t *)self;
    uint32_t rc = --c->refcount;
    if (rc == 0) { gfx_native_freed(c); free(c); }
    return rc;
}

static const clipper7_vtbl_t g_clipper7_vtbl = {
    .QueryInterface = (comfn2_t)clipper7_QueryInterface,
    .AddRef = clipper7_AddRef,
    .Release = clipper7_Release,
    .GetClipList = fail3,
    .GetHWnd = fail1,
    .Initialize = fail2,
    .IsClipListChanged = fail1,
    .SetClipList = fail2,
    .SetHWnd = fail2,
};

static clipper7_t *clipper7_create(void)
{
    clipper7_t *c = calloc(1, sizeof(*c));
    c->lpVtbl = &g_clipper7_vtbl;
    c->refcount = 1;
    return c;
}

/* ===================== IDirectDrawSurface7 ===================== */

typedef struct surface7 surface7_t;

/* Defined much further below (needs surface7_t and friends fully defined
   first); forward-declared here so surface7_Unlock can call it too,
   alongside surface7_Blt/BltFast/Flip further down which already do. */
static void surface7_present(surface7_t *s);
static inline uint32_t surface7_read_raw(const surface7_t *s, uint32_t x, uint32_t y);

typedef struct {
    comfn2_t QueryInterface;
    comfn0_t AddRef;
    comfn0_t Release;
    comfn1_t AddAttachedSurface;
    comfn1_t AddOverlayDirtyRect;
    comfn5_t Blt;
    comfn3_t BltBatch;
    comfn5_t BltFast;
    comfn2_t DeleteAttachedSurface;
    comfn2_t EnumAttachedSurfaces;
    comfn3_t EnumOverlayZOrders;
    comfn2_t Flip;
    comfn2_t GetAttachedSurface;
    comfn1_t GetBltStatus;
    comfn1_t GetCaps;
    comfn1_t GetClipper;
    comfn2_t GetColorKey;
    comfn1_t GetDC;
    comfn1_t GetFlipStatus;
    comfn2_t GetOverlayPosition;
    comfn1_t GetPalette;
    comfn1_t GetPixelFormat;
    comfn1_t GetSurfaceDesc;
    comfn2_t Initialize;
    comfn0_t IsLost;
    comfn4_t Lock;
    comfn1_t ReleaseDC;
    comfn0_t Restore;
    comfn1_t SetClipper;
    comfn2_t SetColorKey;
    comfn2_t SetOverlayPosition;
    comfn1_t SetPalette;
    comfn1_t Unlock;
    comfn5_t UpdateOverlay;
    comfn1_t UpdateOverlayDisplay;
    comfn2_t UpdateOverlayZOrder;
    comfn1_t GetDDInterface;
    comfn1_t PageLock;
    comfn1_t PageUnlock;
    comfn2_t SetSurfaceDesc;
    comfn4_t SetPrivateData;
    comfn3_t GetPrivateData;
    comfn1_t FreePrivateData;
    comfn1_t GetUniquenessValue;
    comfn0_t ChangeUniquenessValue;
    comfn1_t SetPriority;
    comfn1_t GetPriority;
    comfn1_t SetLOD;
    comfn1_t GetLOD;
} surface7_vtbl_t;

/* Forward declarations: surface7_QueryInterface below needs to reference
   these (DirectDrawSurface 1/2/3/4 legacy support, see
   [[project_swep1rcr_port_scoping]]), but their real (initialized)
   definitions live further down. */
static const surface7_vtbl_t g_surface7_vtbl;
static const surface7_vtbl_t g_surface_legacy_vtbl;
/* Real (initialized) definition is much further down, alongside
   surface7_create -- surface7_SetSurfaceDesc below needs it too. */
static uint32_t surface7_bpp_from_pixfmt(const ddpixelformat_t *fmt, uint32_t flags);

/* IDirect3DTexture2 -- a genuinely DIFFERENT, much NARROWER real vtable
   shape (6 slots: QueryInterface/AddRef/Release/GetHandle/
   PaletteChanged/Load) than IDirectDrawSurface's (40 slots, real Lock/
   Unlock at offsets 100/128). A first attempt at this swapped
   g_surface_texture2_vtbl directly onto the SAME shared surface7_t's
   lpVtbl field -- exactly the shared-vtable/shared-refcount bug already
   fixed once for ddraw7_t/d3d123_t (see [[project_swep1rcr_port_scoping]]
   and [[feedback_diagnose_exact_mechanism_not_guard]]), just missed here
   too: the game keeps using its ORIGINAL surface pointer for Lock/Unlock
   after QueryInterface-ing it for IID_IDirect3DTexture2, and since
   that's the SAME object whose lpVtbl just got mutated to the 6-slot
   struct, calling Lock/Unlock through it read 100/128 bytes into a
   24-byte static const -- past the end, into whatever static data
   happens to follow it in memory, then jumped there. Real, independent
   tear-off object instead (own vtable pointer, own refcount, an AddRef'd
   back-reference to the real surface for GetHandle/Load) -- same pattern
   as d3d_light_t/d3d_material3_t/d3d_viewport3_t. */
typedef struct {
    comfn2_t QueryInterface;
    comfn0_t AddRef;
    comfn0_t Release;
    comfn2_t GetHandle;
    comfn2_t PaletteChanged;
    comfn1_t Load;
} d3d_texture2_vtbl_t;
typedef struct {
    const d3d_texture2_vtbl_t *lpVtbl;
    atomic_uint refcount;
    surface7_t *surface; /* AddRef'd */
} d3d_texture2_t;
static const d3d_texture2_vtbl_t g_surface_texture2_vtbl;

/* IDirectDrawGammaControl -- same independent-tear-off pattern as
   d3d_texture2_t above (own vtable, own refcount, an AddRef'd back-
   reference to the real surface), not a shared-vtable mutation. Real
   GetGammaRamp/SetGammaRamp take (DWORD dwFlags, LPDDGAMMARAMP
   lpRampData); DDGAMMARAMP is exactly 3 uint16_t[256] arrays (red/
   green/blue). No real hardware gamma DAC exists to actually apply a
   ramp to (this loader renders via GLES, not a real display gamma
   ramp) -- SetGammaRamp still stores the real values (so a later
   GetGammaRamp reflects them, a real, correct round-trip), it just has
   no effect on rendered output. */
typedef struct {
    uint16_t red[256];
    uint16_t green[256];
    uint16_t blue[256];
} ddgammaramp_t;
typedef struct {
    comfn2_t QueryInterface;
    comfn0_t AddRef;
    comfn0_t Release;
    comfn2_t GetGammaRamp;
    comfn2_t SetGammaRamp;
} gamma_control_vtbl_t;
typedef struct {
    const gamma_control_vtbl_t *lpVtbl;
    atomic_uint refcount;
    surface7_t *surface; /* AddRef'd */
    ddgammaramp_t ramp;
} gamma_control_t;
static const gamma_control_vtbl_t g_gamma_control_vtbl;
static void *gamma_control_create(surface7_t *surface);

/* IDirect3DVertexBuffer (D3D3-era, NOT the same real interface/vtable as
   D3D7's IDirect3DVertexBuffer7) -- real 8-slot vtable (QI/AddRef/Release/
   Lock/Unlock/ProcessVertices/GetVertexBufferDesc/Optimize, verified
   against a real MinGW d3d.h). Wraps a real IDirect3DVertexBuffer7 object
   (see d3d7.h's d3d7_vb_* functions) rather than reimplementing vertex
   storage a second time -- same "own tear-off, own refcount, real object
   wrapped underneath" pattern as d3d_texture2_t above (own independent
   refcount per [[feedback_diagnose_exact_mechanism_not_guard]]'s "every
   QueryInterface'd object owns its own refcount" mandate, NOT sharing/
   mutating the wrapped vb7_t's vtable in place). See
   [[project_swep1rcr_port_scoping]]'s "no 3D in gameplay" investigation. */
typedef struct {
    comfn2_t QueryInterface;
    comfn0_t AddRef;
    comfn0_t Release;
    comfn3_t Lock;
    comfn0_t Unlock;
    comfn7_t ProcessVertices;
    comfn1_t GetVertexBufferDesc;
    comfn2_t Optimize;
} d3d_vertexbuffer3_vtbl_t;
typedef struct {
    const d3d_vertexbuffer3_vtbl_t *lpVtbl;
    atomic_uint refcount;
    void *inner_vb; /* real vb7_t*, via d3d7.h's opaque d3d7_vb_* functions -- AddRef'd */
} d3d_vertexbuffer3_t;
/* Real body further down (needs surface7_AddRef, defined after
   surface7_QueryInterface below). */
static void *d3d_texture2_create(surface7_t *surface);

/* A real complex surface can have more than one thing attached at once
   (e.g. a back buffer AND a Z-buffer, or an overlay) -- confirmed this
   was NOT just a theoretical gap (see docs/api-surface/ddraw.md's open
   item): a single-slot model meant a second AddAttachedSurface call
   silently clobbered the first attachment, and king.exe's own warn.log
   showed repeated "can not GetAttachedSurface 40"/"...80" (DDSCAPS_
   OFFSCREENPLAIN / DDSCAPS_OVERLAY) once that started actually being
   exercised (previously masked behind other crashes). 4 slots is
   generous -- real DirectDraw complex-surface chains this game plausibly
   builds (primary+backbuffer, or a render target+zbuffer) never need
   more than 2. */
#define DDRAW_MAX_ATTACHED 4

/* A surface's pixel content can live in two places -- the CPU-side
   ->buffer (written by Lock/Unlock, Blt/BltFast, colorfill) and the
   GL-side ->gl (written by a D3D7 draw, or the GPU-accelerated Blt/
   colorfill paths in this file) -- and only one of them can be the
   more current at any moment: syncing one side FROM the other always
   happens before anything writes the destination side, everywhere in
   this file (surface7_upload_to_gl before a GPU write on top of
   pending CPU content, surface7_sync_from_gpu before a CPU write/read
   needs current pixels). So "cpu is newer" and "gpu is newer" are two
   readings of the same one relationship, never independently true --
   this tri-state enum makes that the only representable shape instead
   of relying on every call site to manually avoid setting both of a
   former separate int pair. */
typedef enum {
    SURFACE_SYNCED = 0,  /* ->buffer and ->gl agree -- either can be read as-is */
    SURFACE_CPU_AHEAD,   /* ->buffer is newer; ->gl needs surface7_upload_to_gl first */
    SURFACE_GPU_AHEAD,   /* ->gl is newer; ->buffer needs surface7_sync_from_gpu first */
} surface_sync_t;

struct surface7 {
    const surface7_vtbl_t *lpVtbl;
    uint32_t refcount;
    uint32_t width, height;
    uint32_t bpp;   /* bytes per pixel */
    uint32_t pitch;
    uint8_t *buffer;
    int buffer_is_external; /* set via SetSurfaceDesc(DDSD_LPSURFACE) -- ->buffer points at
                                caller-owned memory; must never free() or reallocate it */
    uint32_t caps;
    ddpixelformat_t pixfmt;
    ddcolorkey_t ck_destblt, ck_srcblt;
    int has_ck_destblt, has_ck_srcblt;
    surface7_t *attached[DDRAW_MAX_ATTACHED];
    palette7_t *palette;
    int lost; /* always 0 here -- we never invalidate surfaces */
    int ever_presented_nonzero; /* primary surface only -- see surface7_present's
                          own comment on the all-zero-Lock/Unlock-write guard */

    gl_target_t gl;   /* GL FBO/texture backing this surface -- lazily
                          created the first time it's used as a D3D7
                          render target (see d3d7_surface_interop_t) */
    surface_sync_t sync_state; /* which of ->buffer/->gl is currently
                          authoritative -- see surface_sync_t's own
                          comment. Replaces a former separate
                          gpu_ahead/cpu_ahead int pair: the two were
                          never independent (one side being newer always
                          meant the other wasn't), and every call site
                          already had to manually maintain "never both
                          true" by careful ordering -- a real, if never
                          triggered, footgun the old code's own comments
                          flagged as "a contradiction". A tri-state enum
                          makes that invalid combination unrepresentable
                          instead of just manually avoided. */
};

/* Defined below (needs pack_rgba/surface7_write_raw); forward-declared
   here since Lock/GetSurfaceDesc/Blt/BltFast/Flip all need to call it
   before touching ->buffer, and they come first in the file. */
static void surface7_sync_from_gpu(surface7_t *s);
/* Same reason: surface7_Blt's new GPU-accelerated path needs these two
   (ensure/upload a real GL texture for a Blt source, and resolve its
   real color-key range if any) but both are defined later in the file,
   originally written for the D3D7 texture-stage interop. */
static unsigned ddraw_gl_get_texture(void *surface_, int *out_w, int *out_h);
static int ddraw_gl_get_color_key(void *surface_, uint8_t out_low[4], uint8_t out_high[4]);
/* Same reason: surface7_Blt's GPU path needs to push the DESTINATION's
   existing CPU-side content into ->gl before drawing on top of it (see
   the call site) -- ddraw_gl_bind_render_target already does exactly
   this for the D3D7-render-target-binding case, for the same underlying
   reason (a colorkey-discarded/partial draw must show what was really
   there before, not a blank freshly-allocated texture). */
static void surface7_upload_to_gl(surface7_t *s);

static void surface7_fill_desc(const surface7_t *s, ddsurfacedesc2_t *desc)
{
    uint32_t requested_size = desc->dwSize;
    memset(desc, 0, requested_size < sizeof(*desc) ? requested_size : sizeof(*desc));
    desc->dwSize = requested_size;
    desc->dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
    desc->dwHeight = s->height;
    desc->dwWidth = s->width;
    desc->lPitchOrLinearSize = s->pitch;
    desc->lpSurface = gfx_guest_va(s->buffer);
    desc->ddpfPixelFormat = s->pixfmt;
    desc->ddsCaps.dwCaps = s->caps;
    if (s->has_ck_destblt) { desc->dwFlags |= DDSD_CKDESTBLT; desc->ddckCKDestBlt = s->ck_destblt; }
    if (s->has_ck_srcblt) { desc->dwFlags |= DDSD_CKSRCBLT; desc->ddckCKSrcBlt = s->ck_srcblt; }
}

static uint32_t WINAPI surface7_QueryInterface(void *self, guid_t *riid, void **out)
{
    surface7_t *s = (surface7_t *)self;
    if (out == NULL) return DDERR_INVALIDPARAMS;
    if (ddraw_trace_enabled() && riid != NULL) {
        fprintf(stderr, "[ddraw-trace] surface7_QueryInterface: surface=%p riid={%08x-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x}\n",
                self, riid->Data1, riid->Data2, riid->Data3,
                riid->Data4[0], riid->Data4[1], riid->Data4[2], riid->Data4[3],
                riid->Data4[4], riid->Data4[5], riid->Data4[6], riid->Data4[7]);
    }
    if (riid == NULL || guid_equal(riid, &IID_IDirectDrawSurface7)) {
        s->lpVtbl = &g_surface7_vtbl;
        s->refcount++;
        *out = self;
        return DD_OK;
    }
    /* DX6-era legacy surfaces (SWEP1RCR, see
       [[project_swep1rcr_port_scoping]]) -- same underlying surface7_t
       object (all real Blt/Lock/Flip/etc. state genuinely shared, not
       re-derived), just a different vtable "view" swapped in for
       whichever version was actually requested -- same pattern already
       used for IID_IDirectDrawSurface4 below and for the DirectDraw main
       object further down. */
    if (guid_equal(riid, &IID_IDirectDrawSurface4)) {
        s->lpVtbl = &g_surface7_vtbl;
        s->refcount++;
        *out = self;
        return DD_OK;
    }
    if (guid_equal(riid, &IID_IDirectDrawSurface) || guid_equal(riid, &IID_IDirectDrawSurface2) ||
        guid_equal(riid, &IID_IDirectDrawSurface3)) {
        s->lpVtbl = &g_surface_legacy_vtbl;
        s->refcount++;
        *out = self;
        return DD_OK;
    }
    /* IDirect3DTexture2 -- how a texture-capable DDraw surface actually
       reaches IDirect3DDevice3::SetTexture in real DX6-era code (SetTexture
       takes an IDirect3DTexture2*, not a raw IDirectDrawSurface*). Real,
       independent tear-off object (own vtable, own refcount) -- see
       d3d_texture2_t's own comment above for why this must NOT mutate
       `s->lpVtbl` in place the way the DirectDraw version chain does. */
    if (guid_equal(riid, &IID_IDirect3DTexture2)) {
        *out = d3d_texture2_create(s);
        return DD_OK;
    }
    if (guid_equal(riid, &IID_IDirectDrawGammaControl)) {
        *out = gamma_control_create(s);
        return DD_OK;
    }
    *out = NULL;
    if (riid != NULL) {
        fprintf(stderr, "[ddraw] surface7_QueryInterface: unrecognized riid={%08x-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x} on surface=%p\n",
                riid->Data1, riid->Data2, riid->Data3,
                riid->Data4[0], riid->Data4[1], riid->Data4[2], riid->Data4[3],
                riid->Data4[4], riid->Data4[5], riid->Data4[6], riid->Data4[7], self);
    }
    return DDERR_NOINTERFACE;
}
static uint32_t WINAPI surface7_AddRef(void *self) { return ++((surface7_t *)self)->refcount; }
static uint32_t WINAPI surface7_Release(void *self)
{
    surface7_t *s = (surface7_t *)self;
    uint32_t rc = --s->refcount;
    if (rc == 0) {
        if (s->palette != NULL) palette7_Release(s->palette);
        for (int i = 0; i < DDRAW_MAX_ATTACHED; i++) {
            if (s->attached[i] != NULL) surface7_Release(s->attached[i]);
        }
        gl_target_destroy(&s->gl);
        if (!s->buffer_is_external) gfx_guest_free(s->buffer);
        gfx_native_freed(s);
        free(s);
    }
    return rc;
}

static uint32_t WINAPI surface7_IsLost(void *self)
{
    /* Hottest call in the whole game (~75k/session per docs/api-surface/
       ddraw.md) -- must stay this cheap. We never invalidate surfaces
       (no real exclusive-mode device loss to model), so always DD_OK. */
    (void)self;
    return DD_OK;
}

/* Real semantics: report whether an earlier async Blt/Flip has finished
   (DDGBS_ISBLTDONE/DDGFS_ISFLIPDONE) or whether a new one could be
   started right now (DDGBS_CANBLT/DDGFS_CANFLIP) -- DD_OK means "yes",
   DDERR_WASSTILLDRAWING means "not yet". Our Blt/Flip implementations
   are synchronous (done by the time they return, no real hardware queue
   to drain), so the answer is unconditionally "yes" regardless of which
   flag was asked about.

   This matters far beyond just returning the right boolean: king.exe's
   own present loop (docs/game_code/king.exe.c, vtable offset 0x48 in the
   disassembly is confirmed GetFlipStatus, called with DDGFS_CANFLIP)
   spin-polls this in a tight `do { } while (GetFlipStatus(...) != DD_OK)`
   loop before every flip. Returning failure here unconditionally (the
   previous generic fail1 stub) meant that loop never terminated --
   confirmed via a live run pegging one core at >100M calls/sec and never
   returning to the message loop, which is consistent with the menu
   rendering once and never being able to present a second frame. */
static uint32_t WINAPI surface7_GetBltStatus(void *self, uint32_t flags)
{
    (void)self; (void)flags;
    return DD_OK;
}
static uint32_t WINAPI surface7_GetFlipStatus(void *self, uint32_t flags)
{
    (void)self; (void)flags;
    return DD_OK;
}

/* Real semantics: hands back a GDI DC targeting this surface's pixels --
   a common DX6-era technique is to GetDC a system-memory surface, draw
   into it via BitBlt/TextOut/etc., ReleaseDC, then Blt the result into a
   real video-memory texture (SWEP1RCR's own "allocate system texture"
   pattern does exactly this, see [[project_swep1rcr_port_scoping]]). No
   real GDI rasterization backs this HDC -- gdi32.c's own drawing
   functions never dereference their HDC parameter (same "opaque, never-
   dereferenced sentinel" convention as every DC gdi32.c itself hands
   out), so GDI calls made against it are harmless no-ops, not crashes;
   content drawn this way won't actually reach the surface's pixels. The
   previous hard failure here was a confirmed bug regardless of whether
   any specific crash traces back to it: a caller checking the HRESULT
   takes the wrong branch, and a caller skipping the check uses an
   uninitialized HDC -- returning success with a safe, stable handle
   (the surface pointer itself) fixes both. */
static uint32_t WINAPI surface7_GetDC(void *self, void **out_hdc)
{
    if (out_hdc == NULL) return DDERR_INVALIDPARAMS;
    *out_hdc = self;
    return DD_OK;
}
static uint32_t WINAPI surface7_ReleaseDC(void *self, void *hdc)
{ (void)self; (void)hdc; return DD_OK; }

static uint32_t WINAPI surface7_Lock(void *self, ddraw_rect_t *rect, ddsurfacedesc2_t *out_desc, uint32_t flags, void *event)
{
    (void)rect; (void)event;
    surface7_t *s = (surface7_t *)self;
    /* A real full-session gameplay trace (see
       [[project_ddraw_gpu_blt_glreadpixels_elimination]]) showed zero
       Lock calls at all across 145k Blt/3.8k Flip/~2M DrawPrimitive in
       one session (startup, loading, menus, driving, clean exit), so
       this game genuinely never calls IDirectDrawSurface7::Lock in
       practice -- but removing the sync below anyway (tried once, as
       part of eliminating every glReadPixels call site not tied to
       screenshotting) turned out to have a real cost, not just a
       theoretical one: this project's OWN test suite calls Lock
       ~40+ times specifically to verify a GPU render/Blt/colorfill
       really landed, and every one of those checks would start reading
       stale bytes the moment the sync was gone -- not some hypothetical
       future path, but this codebase's existing, working verification
       methodology, broken immediately. Kept the sync given that: zero
       real-world benefit (never called by the actual game) against a
       real, immediate cost (breaks how this project checks its own
       rendering correctness) is not a good trade. The trace line below
       stays regardless -- it's free (gated behind the already-necessary
       LONGROAD2_DDRAW_TRACE) and answers the READONLY/WRITEONLY question
       if this is ever revisited -- decoded explicitly (READONLY=%d
       WRITEONLY=%d) rather than just the raw hex so a captured log can
       be grepped directly (grep -c "READONLY=1", etc.). */
    int trace = ddraw_trace_enabled();
    int pre_sync_state = s->sync_state;
    if (trace) {
        fprintf(stderr, "[ddraw-trace] Lock: surface=%p(caps=0x%08x,%ux%u) flags=0x%08x "
                        "[READONLY=%d WRITEONLY=%d] pre_sync_state=%d(0=SYNCED,1=CPU_AHEAD,2=GPU_AHEAD) "
                        "gl.tex=%u gl.fbo=%u gl.w=%d gl.h=%d\n",
                (void *)s, s->caps, s->width, s->height, flags,
                (flags & DDLOCK_READONLY) != 0, (flags & DDLOCK_WRITEONLY) != 0,
                pre_sync_state, s->gl.tex, s->gl.fbo, s->gl.w, s->gl.h);
    }
    surface7_sync_from_gpu(s);
    if (trace && (s->caps & DDSCAPS_PRIMARYSURFACE)) {
        uint64_t nonzero_bytes = 0;
        size_t total = (size_t)s->pitch * s->height;
        if (s->buffer != NULL) {
            for (size_t i = 0; i < total; i++) if (s->buffer[i] != 0) nonzero_bytes++;
        }
        fprintf(stderr, "[ddraw-trace] Lock: post-sync buffer=%zu bytes, nonzero=%llu (%.1f%%)\n",
                total, (unsigned long long)nonzero_bytes,
                total > 0 ? (100.0 * (double)nonzero_bytes / (double)total) : 0.0);
    }
    if (out_desc != NULL) {
        surface7_fill_desc(s, out_desc);
        /* hardtruck2-recomp: a rect Lock points at the rect's first pixel,
           as on Windows (LongRoad2 always handed back the surface base). */
        if (rect != NULL && s->buffer != NULL && rect->left >= 0 && rect->top >= 0)
            out_desc->lpSurface += (uint32_t)rect->top * s->pitch + (uint32_t)rect->left * s->bpp;
    }
    return DD_OK;
}
static uint32_t WINAPI surface7_Unlock(void *self, void *surface_data)

{
    (void)surface_data;
    surface7_t *s = (surface7_t *)self;
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] Unlock: surface=%p(caps=0x%08x,%ux%u)\n", (void *)s, s->caps, s->width, s->height);
    }
    /* Real Lock() flags (DDLOCK_READONLY etc.) aren't tracked (see
       surface7_Lock -- (void)flags), so conservatively assume any
       Lock/Unlock pair could have written -- correctness over a minor
       redundant-upload cost the next time this surface samples as a
       D3D7 texture. */
    s->sync_state = SURFACE_CPU_AHEAD;
    /* Porsche.exe's own FMV player writes decoded video frames straight
       into the primary surface via Lock/memcpy/Unlock -- no Blt at all
       (confirmed via a real LONGROAD2_DDRAW_TRACE capture: 140+ Lock/
       Unlock cycles on the primary surface with zero further Flip/Blt
       calls for the rest of the session). Blt/BltFast/Flip already push
       a primary-surface write to the screen (see their own matching
       "if ((d->caps & DDSCAPS_PRIMARYSURFACE) ..." block); Unlock never
       did, so every one of those decoded frames was silently dropped --
       the real root cause of a real "intro video is just a black,
       unresponsive screen" report, not an input-subsystem bug. */
    if ((s->caps & DDSCAPS_PRIMARYSURFACE)) {
        int allowed = primary_present_allowed();
        if (ddraw_trace_enabled()) {
            /* A 2-pixel sample (corner+center) can't distinguish "this
               buffer is genuinely all zero" from "it has real content,
               just not at those two exact points" -- scan every byte of
               the real backing buffer instead, so this answers that
               question definitively regardless of where in the frame
               any real content might be. */
            uint64_t nonzero_bytes = 0;
            uint8_t first_nonzero = 0;
            uint32_t first_nonzero_off = 0;
            size_t total = (size_t)s->pitch * s->height;
            if (s->buffer != NULL) {
                for (size_t i = 0; i < total; i++) {
                    if (s->buffer[i] != 0) {
                        if (nonzero_bytes == 0) { first_nonzero = s->buffer[i]; first_nonzero_off = (uint32_t)i; }
                        nonzero_bytes++;
                    }
                }
            }
            fprintf(stderr, "[ddraw-trace] Unlock: primary present %s -- buffer=%zu bytes, nonzero=%llu "
                            "(%.1f%%), first_nonzero=0x%02x@%u, bpp=%u\n",
                    allowed ? "FIRING" : "SKIPPED(rate-limited)", total, (unsigned long long)nonzero_bytes,
                    total > 0 ? (100.0 * (double)nonzero_bytes / (double)total) : 0.0,
                    first_nonzero, first_nonzero_off, s->bpp);
        }
        if (allowed) surface7_present(s);
    }
    return DD_OK;
}
static uint32_t WINAPI surface7_GetSurfaceDesc(void *self, ddsurfacedesc2_t *out_desc)
{
    surface7_t *s = (surface7_t *)self;
    if (out_desc == NULL) return DDERR_INVALIDPARAMS;
    /* Same reasoning as surface7_Lock right above -- see its own comment.
       GetSurfaceDesc hands out the same s->buffer via lpSurface that
       Lock does, so removing this sync carries the identical real cost
       (this project's own test suite reading stale bytes) for the same
       zero real-world benefit. */
    surface7_sync_from_gpu(s);
    surface7_fill_desc(s, out_desc);
    /* TEMP DIAGNOSTIC (opt-in via LONGROAD2_DDRAW_TRACE): previously
       untraced -- see [[project_swep1rcr_port_scoping]]'s crash #3
       investigation, which found a completely silent gap between a
       texture's last Lock/Unlock and a later crash, with no visibility
       into whether GetSurfaceDesc/GetCaps/GetPixelFormat/GetColorKey
       fired in between. */
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] GetSurfaceDesc: surface=%p %ux%u bpp=%u pitch=%d\n",
                (void *)s, out_desc->dwWidth, out_desc->dwHeight,
                out_desc->ddpfPixelFormat.dwRGBBitCount, out_desc->lPitchOrLinearSize);
    }
    return DD_OK;
}
static uint32_t WINAPI surface7_GetCaps(void *self, ddscaps2_t *out_caps)
{
    surface7_t *s = (surface7_t *)self;
    if (out_caps == NULL) return DDERR_INVALIDPARAMS;
    memset(out_caps, 0, sizeof(*out_caps));
    out_caps->dwCaps = s->caps;
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] GetCaps: surface=%p caps=0x%08x\n", (void *)s, out_caps->dwCaps);
    }
    return DD_OK;
}
/* IDirectDrawSurface/2/3::GetCaps real callers pass a plain DDSCAPS (one
   DWORD, no dwCaps2/3/4) -- unlike ddsurfacedesc2_t's dwSize, DDSCAPS
   carries no self-describing size, so surface7_GetCaps's unconditional
   sizeof(ddscaps2_t) write can't safely serve these legacy versions
   (would write 12 bytes past the caller's real, smaller buffer). Used by
   g_surface_legacy_vtbl below; v4/v7 (real DDSCAPS2 callers) keep using
   surface7_GetCaps unchanged. */
static uint32_t WINAPI surface_legacy_GetCaps1(void *self, uint32_t *out_caps1)
{
    if (out_caps1 == NULL) return DDERR_INVALIDPARAMS;
    *out_caps1 = ((surface7_t *)self)->caps;
    return DD_OK;
}
static uint32_t WINAPI surface7_GetPixelFormat(void *self, ddpixelformat_t *out_fmt)
{
    surface7_t *s = (surface7_t *)self;
    if (out_fmt == NULL) return DDERR_INVALIDPARAMS;
    uint32_t sz = out_fmt->dwSize;
    *out_fmt = s->pixfmt;
    out_fmt->dwSize = sz;
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] GetPixelFormat: surface=%p bpp=%u R=0x%08x G=0x%08x B=0x%08x A=0x%08x\n",
                (void *)s, out_fmt->dwRGBBitCount, out_fmt->dwRBitMask, out_fmt->dwGBitMask,
                out_fmt->dwBBitMask, out_fmt->dwRGBAlphaBitMask);
    }
    return DD_OK;
}

static uint32_t WINAPI surface7_SetColorKey(void *self, uint32_t flags, ddcolorkey_t *key)
{
    surface7_t *s = (surface7_t *)self;
    /* TEMP DIAGNOSTIC (opt-in via LONGROAD2_DDRAW_TRACE): the ONE call
       that decides whether bind_texture_stages' colorkey discard ever
       fires for a given surface (see the matching trace this turn added
       to bind_texture_stages[0] in d3d7.c) -- was never traced before,
       so there was no way to see WHICH surfaces get a real key, or
       whether a surface later bound as a 3D texture (ground/cabin) is
       one of them. */
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] SetColorKey: surface=%p flags=0x%08x%s\n",
                (void *)s, flags,
                key != NULL ? "" : " (key=NULL, clearing)");
        if (key != NULL) {
            fprintf(stderr, "  low=0x%08x high=0x%08x\n", key->dwColorSpaceLowValue, key->dwColorSpaceHighValue);
        }
    }
    if (flags & 0x2u /* DDCKEY_DESTBLT */) {
        if (key != NULL) { s->ck_destblt = *key; s->has_ck_destblt = 1; } else s->has_ck_destblt = 0;
    } else /* DDCKEY_SRCBLT default */ {
        if (key != NULL) { s->ck_srcblt = *key; s->has_ck_srcblt = 1; } else s->has_ck_srcblt = 0;
    }
    return DD_OK;
}
static uint32_t WINAPI surface7_GetColorKey(void *self, uint32_t flags, ddcolorkey_t *out_key)
{
    surface7_t *s = (surface7_t *)self;
    if (out_key == NULL) return DDERR_INVALIDPARAMS;
    uint32_t result = DD_OK;
    if (flags & 0x2u) {
        if (!s->has_ck_destblt) result = DDERR_GENERIC;
        else *out_key = s->ck_destblt;
    } else {
        if (!s->has_ck_srcblt) result = DDERR_GENERIC;
        else *out_key = s->ck_srcblt;
    }
    if (ddraw_trace_enabled()) {
        if (result == DD_OK) {
            fprintf(stderr, "[ddraw-trace] GetColorKey: surface=%p flags=0x%08x low=0x%08x high=0x%08x\n",
                    (void *)s, flags, out_key->dwColorSpaceLowValue, out_key->dwColorSpaceHighValue);
        } else {
            fprintf(stderr, "[ddraw-trace] GetColorKey: surface=%p flags=0x%08x -> no key set\n", (void *)s, flags);
        }
    }
    return result;
}

static uint32_t WINAPI surface7_AddAttachedSurface(void *self, surface7_t *attachment)
{
    surface7_t *s = (surface7_t *)self;
    if (attachment == NULL) return DDERR_INVALIDPARAMS;

    /* Real semantics: attaching where one of the same caps is already
       attached replaces it, rather than growing without bound (a caller
       re-attaching, e.g. after resizing, is a real usage pattern). */
    int free_slot = -1;
    for (int i = 0; i < DDRAW_MAX_ATTACHED; i++) {
        if (s->attached[i] != NULL && s->attached[i]->caps == attachment->caps) {
            surface7_Release(s->attached[i]);
            free_slot = i;
            break;
        }
        if (free_slot < 0 && s->attached[i] == NULL) free_slot = i;
    }
    if (free_slot < 0) {
        fprintf(stderr, "[ddraw] AddAttachedSurface: all %d slots full, dropping attachment "
                         "(caps=0x%08x)\n", DDRAW_MAX_ATTACHED, attachment->caps);
        return DDERR_GENERIC;
    }
    surface7_AddRef(attachment);
    s->attached[free_slot] = attachment;

    /* Same reasoning as surface7_SetPalette's propagation: cover the
       reverse ordering too (SetPalette on the primary called before its
       back buffer/Z-buffer existed to be attached to it). */
    if (s->palette != NULL && attachment->palette != s->palette) {
        palette7_AddRef(s->palette);
        if (attachment->palette != NULL) palette7_Release(attachment->palette);
        attachment->palette = s->palette;
    }

    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] AddAttachedSurface: %p (caps=0x%08x) -> %p (caps=0x%08x) slot %d\n",
                (void *)s, s->caps, (void *)attachment, attachment->caps, free_slot);
    }
    return DD_OK;
}
/* Real semantics: returns the attached surface whose OWN ddsCaps has (at
   least) every bit set in *caps -- NOT identity/exact-match, since
   callers commonly ask with just the one or two bits they care about
   (e.g. DDSCAPS_BACKBUFFER) rather than the attachment's full cap set. A
   NULL caps pointer (not part of the real API, but defensive) falls back
   to the first attachment. */
static uint32_t WINAPI surface7_GetAttachedSurface(void *self, ddscaps2_t *caps, surface7_t **out)
{
    surface7_t *s = (surface7_t *)self;
    if (out == NULL) return DDERR_INVALIDPARAMS;
    uint32_t want = caps != NULL ? caps->dwCaps : 0;
    for (int i = 0; i < DDRAW_MAX_ATTACHED; i++) {
        surface7_t *a = s->attached[i];
        if (a != NULL && (want == 0 || (a->caps & want) == want)) {
            surface7_AddRef(a);
            *out = a;
            if (ddraw_trace_enabled()) {
                fprintf(stderr, "[ddraw-trace] GetAttachedSurface: %p want=0x%08x -> FOUND %p (caps=0x%08x)\n",
                        (void *)s, want, (void *)a, a->caps);
            }
            return DD_OK;
        }
    }
    *out = NULL;
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] GetAttachedSurface: %p want=0x%08x -> NOT FOUND (have:", (void *)s, want);
        for (int i = 0; i < DDRAW_MAX_ATTACHED; i++) {
            if (s->attached[i] != NULL) fprintf(stderr, " 0x%08x", s->attached[i]->caps);
        }
        fprintf(stderr, ")\n");
    }
    return DDERR_GENERIC;
}
static uint32_t WINAPI surface7_EnumAttachedSurfaces(void *self, void *ctx, void *cb_)
{
    typedef uint32_t (WINAPI *cb_t)(surface7_t *, ddsurfacedesc2_t *, void *);
    surface7_t *s = (surface7_t *)self;
    if (cb_ == NULL) return DD_OK;
    for (int i = 0; i < DDRAW_MAX_ATTACHED; i++) {
        surface7_t *a = s->attached[i];
        if (a == NULL) continue;
        ddsurfacedesc2_t desc;
        desc.dwSize = sizeof(desc);
        surface7_fill_desc(a, &desc);
        surface7_AddRef(a);
        if (((cb_t)cb_)(a, &desc, ctx) == 0) break; /* DDENUMRET_CANCEL */
    }
    return DD_OK;
}
static uint32_t WINAPI surface7_DeleteAttachedSurface(void *self, uint32_t flags, surface7_t *attachment)
{
    (void)flags;
    surface7_t *s = (surface7_t *)self;
    for (int i = 0; i < DDRAW_MAX_ATTACHED; i++) {
        if (s->attached[i] != NULL && (attachment == NULL || s->attached[i] == attachment)) {
            surface7_Release(s->attached[i]);
            s->attached[i] = NULL;
            return DD_OK;
        }
    }
    return DDERR_GENERIC;
}

/* Real signature: SetSurfaceDesc(LPDDSURFACEDESC2 lpDDSurfaceDesc, DWORD
   dwFlags) -- dwFlags is reserved (real docs: "must be 0"), the real
   work is reformatting/resizing THIS surface in place per whichever
   DDSD_* fields the caller's desc sets. Real, common use: a plain
   surface created generically, then reformatted right before use as a
   texture (confirmed live: this was one of two "unimplemented 2-arg
   method" calls firing on every single textured draw for a real game,
   right between Blt-ing content into a fresh surface and SetTexture
   using it). */
static uint32_t WINAPI surface7_SetSurfaceDesc(void *self, ddsurfacedesc2_t *desc, uint32_t flags)
{
    (void)flags;
    surface7_t *s = (surface7_t *)self;
    if (desc == NULL) return DDERR_INVALIDPARAMS;

    uint32_t new_width = (desc->dwFlags & DDSD_WIDTH) ? desc->dwWidth : s->width;
    uint32_t new_height = (desc->dwFlags & DDSD_HEIGHT) ? desc->dwHeight : s->height;
    ddpixelformat_t new_pixfmt = s->pixfmt;
    uint32_t new_bpp = s->bpp;
    if (desc->dwFlags & DDSD_PIXELFORMAT) {
        new_pixfmt = desc->ddpfPixelFormat;
        new_bpp = surface7_bpp_from_pixfmt(&new_pixfmt, DDSD_PIXELFORMAT);
    }

    if (desc->dwFlags & DDSD_LPSURFACE) {
        /* Real semantics: point this surface directly at caller-owned
           memory instead of our own allocated buffer -- confirmed live
           to be the ACTUAL, real, confirmed-needed case (a first version
           of this function guessed it was a width/height/pixelformat
           reformat instead and failed this branch outright; a real
           captured trace showed EVERY texture-prep step hitting exactly
           this path). DDSD_PITCH is documented as required alongside
           DDSD_LPSURFACE, but a second real capture showed a real caller
           omitting it anyway -- real Windows is apparently more lenient
           than its own docs here, so this falls back to computing a
           standard tightly-packed pitch (width*bpp, the same default
           the internally-allocated path already uses) rather than
           rejecting the call outright. */
        if (desc->lpSurface == 0) return DDERR_INVALIDPARAMS;
        if (!s->buffer_is_external) gfx_guest_free(s->buffer);
        s->buffer = (uint8_t *)gfx_guest_ptr(desc->lpSurface);
        s->buffer_is_external = 1;
        s->pitch = (desc->dwFlags & DDSD_PITCH) ? desc->lPitchOrLinearSize : new_width * new_bpp;
        s->width = new_width;
        s->height = new_height;
        s->pixfmt = new_pixfmt;
        s->bpp = new_bpp;
        s->sync_state = SURFACE_CPU_AHEAD;

        if (ddraw_trace_enabled()) {
            fprintf(stderr, "[ddraw-trace] SetSurfaceDesc: surface=%p -> EXTERNAL buffer=%p "
                            "%ux%u bpp=%u pitch=%u\n", self, (void *)s->buffer, s->width, s->height,
                    s->bpp, s->pitch);
        }
        return DD_OK;
    }

    uint32_t new_pitch = (desc->dwFlags & DDSD_PITCH) ? desc->lPitchOrLinearSize : new_width * new_bpp;

    if (s->buffer_is_external || new_width != s->width || new_height != s->height || new_pitch != s->pitch) {
        uint8_t *new_buffer = gfx_guest_alloc((size_t)new_pitch * (new_height != 0 ? new_height : 1));
        if (new_buffer == NULL) return DDERR_OUTOFMEMORY;
        if (!s->buffer_is_external) gfx_guest_free(s->buffer);
        s->buffer = new_buffer;
        s->buffer_is_external = 0;
    }
    s->width = new_width;
    s->height = new_height;
    s->pixfmt = new_pixfmt;
    s->bpp = new_bpp;
    s->pitch = new_pitch;
    s->sync_state = SURFACE_CPU_AHEAD;

    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] SetSurfaceDesc: surface=%p -> %ux%u bpp=%u pitch=%u\n",
                self, s->width, s->height, s->bpp, s->pitch);
    }
    return DD_OK;
}

static uint32_t WINAPI surface7_SetPalette(void *self, palette7_t *palette)
{
    surface7_t *s = (surface7_t *)self;
    if (palette != NULL) palette7_AddRef(palette);
    if (s->palette != NULL) palette7_Release(s->palette);
    s->palette = palette;

    /* Real DirectDraw's palette is effectively one hardware register for
       whatever's actually on screen, not an independent per-surface-
       object property -- a game that calls SetPalette only on the
       primary (the typical, documented pattern) still expects its BACK
       BUFFER's pixels (what D3D7 actually renders into, and what
       pack_rgba/surface7_sync_from_gpu need a real palette for) to use
       that same palette. Propagate to whatever's already attached
       (back buffer, Z-buffer) so those surfaces' own ->palette isn't
       silently NULL forever, quietly falling back to index 0. */
    if (palette != NULL) {
        for (int i = 0; i < DDRAW_MAX_ATTACHED; i++) {
            surface7_t *att = s->attached[i];
            if (att != NULL && att->palette != palette) {
                palette7_AddRef(palette);
                if (att->palette != NULL) palette7_Release(att->palette);
                att->palette = palette;
            }
        }

        /* Same reasoning, one step further: also becomes the process-wide
           fallback unpack_rgba uses for any OTHER paletted surface that
           never gets its own SetPalette call at all (see g_current_palette's
           own comment) -- not just this surface's directly attached ones. */
        palette7_AddRef(palette);
        if (g_current_palette != NULL) palette7_Release(g_current_palette);
        g_current_palette = palette;
    }
    return DD_OK;
}
static uint32_t WINAPI surface7_GetPalette(void *self, palette7_t **out)
{
    surface7_t *s = (surface7_t *)self;
    if (out == NULL) return DDERR_INVALIDPARAMS;
    if (s->palette == NULL) { *out = NULL; return DDERR_GENERIC; }
    palette7_AddRef(s->palette);
    *out = s->palette;
    return DD_OK;
}

static uint32_t WINAPI surface7_Restore(void *self) { (void)self; return DD_OK; }

/* ----- pixel-format conversion + the real blitter -----
   Every surface stores pixels in its own native format (native RGB
   masks, or an 8-bit palette index) exactly as king.exe wrote them via
   Lock -- games of this era mix bit depths freely (8-bit paletted sprite
   atlases blitted onto a 16/32-bit back buffer is standard practice), so
   Blt/BltFast/present all go through a shared RGBA8888 intermediate
   rather than assuming every surface matches. */

static inline int pixfmt_equal(const ddpixelformat_t *a, const ddpixelformat_t *b)
{
    return a->dwFlags == b->dwFlags && a->dwRGBBitCount == b->dwRGBBitCount &&
           a->dwRBitMask == b->dwRBitMask && a->dwGBitMask == b->dwGBitMask &&
           a->dwBBitMask == b->dwBBitMask && a->dwRGBAlphaBitMask == b->dwRGBAlphaBitMask;
}

static inline uint8_t unpack_channel(uint32_t raw, uint32_t mask)
{
    if (mask == 0) return 0;
    int shift = __builtin_ctz(mask);
    int bits = __builtin_popcount(mask);
    uint32_t val = (raw & mask) >> shift;
    switch (bits) {
        case 8: return (uint8_t)val;
        case 5: return (uint8_t)((val << 3) | (val >> 2));
        case 6: return (uint8_t)((val << 2) | (val >> 4));
        default: return (uint8_t)((val * 255u) / ((1u << bits) - 1u));
    }
}
static inline uint32_t pack_channel(uint8_t v, uint32_t mask)
{
    if (mask == 0) return 0;
    int shift = __builtin_ctz(mask);
    int bits = __builtin_popcount(mask);
    uint32_t maxval = (1u << bits) - 1u;
    uint32_t scaled = ((uint32_t)v * maxval + 127u) / 255u;
    return (scaled << shift) & mask;
}

/* out[4] = R,G,B,A. Palette lookup falls back to grayscale-of-index when
   no palette is bound yet (matches showing "something" over showing
   nothing while the game hasn't called SetPalette yet). */
static void unpack_rgba(uint32_t raw, const ddpixelformat_t *fmt, const palette7_t *pal, uint8_t out[4])
{
    if (fmt->dwFlags & DDPF_PALETTEINDEXED8) {
        uint8_t idx = (uint8_t)raw;
        /* A surface with no palette of its own (pal==NULL) is the normal,
           expected case for independently created sprite/font/UI atlas
           surfaces in real DirectDraw code -- see g_current_palette's own
           comment. Its index bytes are still meant to be read through
           whatever palette is actually live process-wide, not treated as
           genuinely colorless; falling back to literal index-as-gray here
           was fine as long as every paletted surface Blitted this way
           still got re-expanded through a real, correct, LIVE palette at
           present time (the old always-CPU paletted-present path did
           exactly that every frame) -- but the GPU Blt/present paths
           (see surface7_Blt, surface7_present) now bake this conversion
           once and reuse it verbatim, so a literal gray bake here becomes
           literal gray on screen forever. Confirmed via a real trace
           (docs/api-surface/raw/winedebug.log) that this game calls
           SetPalette exactly once, well before any Blt -- so
           g_current_palette is reliably set by the time any of this
           matters in practice. */
        const palette7_t *use_pal = pal != NULL ? pal : g_current_palette;
        if (use_pal != NULL) {
            out[0] = use_pal->entries[idx].r;
            out[1] = use_pal->entries[idx].g;
            out[2] = use_pal->entries[idx].b;
        } else {
            out[0] = out[1] = out[2] = idx;
        }
        out[3] = 255;
        return;
    }
    out[0] = unpack_channel(raw, fmt->dwRBitMask);
    out[1] = unpack_channel(raw, fmt->dwGBitMask);
    out[2] = unpack_channel(raw, fmt->dwBBitMask);
    out[3] = ((fmt->dwFlags & DDPF_ALPHAPIXELS) && fmt->dwRGBAlphaBitMask != 0)
             ? unpack_channel(raw, fmt->dwRGBAlphaBitMask) : 255;
}

/* Rebuilds palette7_t's RGB->nearest-index LUT (see PALETTE_LUT_BITS) --
   O(32768 * 256) brute-force nearest-color search, but only when the
   palette actually changed (SetEntries invalidates lut_valid), not per
   pixel. This turned out to matter for real: the D3D7 rendering
   backend's GPU->CPU sync (surface7_sync_from_gpu) can target an 8-bit
   PALETTED render surface (confirmed via this game's own real usage --
   it runs its actual 3D driving view through a paletted DirectDraw
   surface, not a 16/32-bit one), so "pack an RGBA color into this
   surface's format" genuinely needs a real answer here now, not the
   admittedly-reasonable-at-the-time guess that it never would. */
static void palette7_ensure_lut(palette7_t *pal)
{
    if (pal->lut_valid) return;
    for (uint32_t r5 = 0; r5 < (1u << PALETTE_LUT_BITS); r5++) {
        for (uint32_t g5 = 0; g5 < (1u << PALETTE_LUT_BITS); g5++) {
            for (uint32_t b5 = 0; b5 < (1u << PALETTE_LUT_BITS); b5++) {
                int r = (int)(r5 << (8 - PALETTE_LUT_BITS));
                int g = (int)(g5 << (8 - PALETTE_LUT_BITS));
                int b = (int)(b5 << (8 - PALETTE_LUT_BITS));
                int best_idx = 0, best_dist = 0x7FFFFFFF;
                for (int i = 0; i < 256; i++) {
                    int dr = r - pal->entries[i].r, dg = g - pal->entries[i].g, db = b - pal->entries[i].b;
                    int dist = dr * dr + dg * dg + db * db;
                    if (dist < best_dist) { best_dist = dist; best_idx = i; }
                }
                uint32_t lut_idx = (r5 << (PALETTE_LUT_BITS * 2)) | (g5 << PALETTE_LUT_BITS) | b5;
                pal->lut[lut_idx] = (uint8_t)best_idx;
            }
        }
    }
    pal->lut_valid = 1;
}

static uint8_t rgba_to_palette_index(const uint8_t in[4], palette7_t *pal)
{
    /* Same g_current_palette fallback as unpack_rgba, and for the same
       reason: a destination surface with no palette of its own (normal
       for an independently created offscreen/scratch surface) still
       needs a real answer here, not index 0 for every packed color --
       index 0 is just whatever color happens to sit in that palette slot,
       almost never black-by-convention or otherwise meaningful. */
    if (pal == NULL) pal = g_current_palette;
    if (pal == NULL) return 0; /* truly no palette set anywhere yet -- no meaningful answer possible */
    palette7_ensure_lut(pal);
    uint32_t r5 = in[0] >> (8 - PALETTE_LUT_BITS);
    uint32_t g5 = in[1] >> (8 - PALETTE_LUT_BITS);
    uint32_t b5 = in[2] >> (8 - PALETTE_LUT_BITS);
    return pal->lut[(r5 << (PALETTE_LUT_BITS * 2)) | (g5 << PALETTE_LUT_BITS) | b5];
}

static uint32_t pack_rgba(const uint8_t in[4], const ddpixelformat_t *fmt, palette7_t *pal)
{
    if (fmt->dwFlags & DDPF_PALETTEINDEXED8) return rgba_to_palette_index(in, pal);
    uint32_t raw = pack_channel(in[0], fmt->dwRBitMask) | pack_channel(in[1], fmt->dwGBitMask) |
                   pack_channel(in[2], fmt->dwBBitMask);
    if ((fmt->dwFlags & DDPF_ALPHAPIXELS) && fmt->dwRGBAlphaBitMask != 0) {
        raw |= pack_channel(in[3], fmt->dwRGBAlphaBitMask);
    }
    return raw;
}

/* Per-pixel hot loops (surface7_convert_to_rgba, surface7_sync_from_gpu,
   Blt/BltFast's CPU fallbacks, d3d_texture2_Load) call unpack_rgba/
   pack_rgba once per pixel, and unpack_channel/pack_channel recompute
   __builtin_ctz/__builtin_popcount + a bit-width switch EVERY pixel even
   though a single surface's mask/shift/bit-count never change mid-loop --
   confirmed a real ~14% of total CPU in a HardTruck2 perf.data capture.
   pixfmt_decode hoists that constant-per-call work out; *_fast variants
   below take the decoded shift/bits directly instead of re-deriving them.
   Deliberately a stack-local decode, not a persistent cache on surface7_t:
   ddpixelformat_t is a real packed DirectDraw ABI struct handed to game
   code verbatim (GetPixelFormat/GetSurfaceDesc/Lock), so it can't grow new
   fields, and surface7_t->pixfmt isn't write-once -- surface7_Flip swaps
   front/back pixfmt on every Flip(), so any persistent cache would need an
   equivalent swap there too or go stale. Computing fresh at the top of
   each hot function avoids that invalidation hazard entirely. */
typedef struct {
    uint32_t r_mask, g_mask, b_mask, a_mask; /* a_mask=0 means "no alpha", mirrors DDPF_ALPHAPIXELS-gated behavior below */
    int r_shift, r_bits, g_shift, g_bits, b_shift, b_bits, a_shift, a_bits;
} pixfmt_decoded_t;

static void pixfmt_decode(const ddpixelformat_t *fmt, pixfmt_decoded_t *out)
{
    out->r_mask = fmt->dwRBitMask;
    out->r_shift = out->r_mask ? __builtin_ctz(out->r_mask) : 0;
    out->r_bits = out->r_mask ? __builtin_popcount(out->r_mask) : 0;
    out->g_mask = fmt->dwGBitMask;
    out->g_shift = out->g_mask ? __builtin_ctz(out->g_mask) : 0;
    out->g_bits = out->g_mask ? __builtin_popcount(out->g_mask) : 0;
    out->b_mask = fmt->dwBBitMask;
    out->b_shift = out->b_mask ? __builtin_ctz(out->b_mask) : 0;
    out->b_bits = out->b_mask ? __builtin_popcount(out->b_mask) : 0;
    int has_alpha = (fmt->dwFlags & DDPF_ALPHAPIXELS) && fmt->dwRGBAlphaBitMask != 0;
    out->a_mask = has_alpha ? fmt->dwRGBAlphaBitMask : 0;
    out->a_shift = out->a_mask ? __builtin_ctz(out->a_mask) : 0;
    out->a_bits = out->a_mask ? __builtin_popcount(out->a_mask) : 0;
}

/* Exact standard RGB565 bit layout (R:11-15, G:5-10, B:0-4, no alpha) --
   bit-for-bit identical to GL_UNSIGNED_SHORT_5_6_5, so a surface this
   matches can skip the CPU unpack_rgba conversion entirely and upload
   its raw buffer straight into a native GL_RGB565 texture (see
   ddraw_gl_get_texture, gl_target_ensure_native565/gl_target_upload_
   native565 in render.c). Deliberately an exact-match check, not a
   "close enough" one -- any other 16-bit layout (XRGB1555, ARGB1555,
   ARGB4444, XRGB4444) still goes through the general CPU path unchanged. */
static int pixfmt_is_native_rgb565(const ddpixelformat_t *fmt)
{
    return (fmt->dwFlags & DDPF_RGB) != 0 && (fmt->dwFlags & DDPF_ALPHAPIXELS) == 0 &&
           fmt->dwRGBBitCount == 16 &&
           fmt->dwRBitMask == 0xF800u && fmt->dwGBitMask == 0x07E0u && fmt->dwBBitMask == 0x001Fu;
}

/* Test-only introspection into g_perf's own blt_calls/bltfast_calls split
   ([[project_ddraw_bltfast_gpu_path_investigation]]) -- lets a test prove
   Blt and BltFast really do land in separate counters now, not just that
   the trace line LOOKS right. Requires LONGROAD2_PERF_TRACE=1 (same gate
   perf_trace_enabled() already checks at every real increment site --
   these read/reset the exact same g_perf fields, not a parallel copy). */
void ddraw_perf_reset_for_test(void)
{
    g_perf.blt_calls = 0;
    g_perf.bltfast_calls = 0;
}
long ddraw_perf_get_blt_calls_for_test(void) { return g_perf.blt_calls; }
long ddraw_perf_get_bltfast_calls_for_test(void) { return g_perf.bltfast_calls; }

/* Test-only introspection (mirrors d3d7.c's d3d7_perf_reset_for_test
   pattern): lets render_backend_test.c prove the native565 upload path
   was ACTUALLY taken, not just that the rendered color happens to be
   right -- the CPU fallback path produces the same visible result for
   this format, so a color-only check can't distinguish the two. */
static long g_native565_upload_count = 0;

void ddraw_native565_reset_for_test(void)
{
    g_native565_upload_count = 0;
}
long ddraw_native565_upload_count_for_test(void)
{
    return g_native565_upload_count;
}

static inline uint8_t unpack_channel_fast(uint32_t raw, uint32_t mask, int shift, int bits)
{
    if (mask == 0) return 0;
    uint32_t val = (raw & mask) >> shift;
    switch (bits) {
        case 8: return (uint8_t)val;
        case 5: return (uint8_t)((val << 3) | (val >> 2));
        case 6: return (uint8_t)((val << 2) | (val >> 4));
        default: return (uint8_t)((val * 255u) / ((1u << bits) - 1u));
    }
}
static inline uint32_t pack_channel_fast(uint8_t v, uint32_t mask, int shift, int bits)
{
    if (mask == 0) return 0;
    uint32_t maxval = (1u << bits) - 1u;
    uint32_t scaled = ((uint32_t)v * maxval + 127u) / 255u;
    return (scaled << shift) & mask;
}

static void unpack_rgba_fast(uint32_t raw, const pixfmt_decoded_t *pd, const ddpixelformat_t *fmt,
                              const palette7_t *pal, uint8_t out[4])
{
    if (fmt->dwFlags & DDPF_PALETTEINDEXED8) {
        /* Identical to unpack_rgba's own palette branch -- see its comment
           for why the g_current_palette fallback matters. */
        uint8_t idx = (uint8_t)raw;
        const palette7_t *use_pal = pal != NULL ? pal : g_current_palette;
        if (use_pal != NULL) {
            out[0] = use_pal->entries[idx].r;
            out[1] = use_pal->entries[idx].g;
            out[2] = use_pal->entries[idx].b;
        } else {
            out[0] = out[1] = out[2] = idx;
        }
        out[3] = 255;
        return;
    }
    out[0] = unpack_channel_fast(raw, pd->r_mask, pd->r_shift, pd->r_bits);
    out[1] = unpack_channel_fast(raw, pd->g_mask, pd->g_shift, pd->g_bits);
    out[2] = unpack_channel_fast(raw, pd->b_mask, pd->b_shift, pd->b_bits);
    out[3] = pd->a_mask ? unpack_channel_fast(raw, pd->a_mask, pd->a_shift, pd->a_bits) : 255;
}
static uint32_t pack_rgba_fast(const uint8_t in[4], const pixfmt_decoded_t *pd, const ddpixelformat_t *fmt, palette7_t *pal)
{
    if (fmt->dwFlags & DDPF_PALETTEINDEXED8) return rgba_to_palette_index(in, pal);
    uint32_t raw = pack_channel_fast(in[0], pd->r_mask, pd->r_shift, pd->r_bits) |
                   pack_channel_fast(in[1], pd->g_mask, pd->g_shift, pd->g_bits) |
                   pack_channel_fast(in[2], pd->b_mask, pd->b_shift, pd->b_bits);
    if (pd->a_mask) raw |= pack_channel_fast(in[3], pd->a_mask, pd->a_shift, pd->a_bits);
    return raw;
}

static inline uint32_t surface7_read_raw(const surface7_t *s, uint32_t x, uint32_t y)
{
    const uint8_t *p = s->buffer + (size_t)y * s->pitch + (size_t)x * s->bpp;
    switch (s->bpp) {
        case 1: return p[0];
        case 2: return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        case 3: return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
        default: return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
}
static inline void surface7_write_raw(surface7_t *s, uint32_t x, uint32_t y, uint32_t raw)
{
    uint8_t *p = s->buffer + (size_t)y * s->pitch + (size_t)x * s->bpp;
    switch (s->bpp) {
        case 4: p[3] = (uint8_t)(raw >> 24); /* fallthrough */
        case 3: p[2] = (uint8_t)(raw >> 16); /* fallthrough */
        case 2: p[1] = (uint8_t)(raw >> 8);  /* fallthrough */
        default: p[0] = (uint8_t)raw;
    }
}

/* Pulls back GL-rendered pixels (from a D3D7 draw that used this surface
   as its render target) into ->buffer, converting from the GL FBO's
   RGBA8888 into this surface's own native pixel format -- every direct
   reader/writer of ->buffer (Lock, GetSurfaceDesc, Blt/BltFast as either
   source or destination, Flip) needs to see the latest pixels regardless
   of whether they came from D3D7 or plain DirectDraw, so this is called
   at the top of each of those instead of just once at EndScene: Clear()
   alone (no BeginScene/EndScene) is a real, common D3D7 usage pattern
   too. */
static void surface7_sync_from_gpu(surface7_t *s)
{
    if (s == NULL) return;
    /* Phase 2 (async cmdbuf_flush, see [[project_gl_issuer_thread]]):
       s->sync_state only becomes SURFACE_GPU_AHEAD when a real D3D7 draw
       actually marks this surface dirty (d3d_draw_dispatch_and_cleanup's
       own mark_gpu_dirty call) -- and that now happens INSIDE an async
       replay job (gl_thread_post from cmdbuf_flush), not synchronously at
       EndScene anymore. A caller landing here right after EndScene (Lock,
       GetSurfaceDesc, Blt, ...) could otherwise read s->sync_state before
       that job has even started, see a stale (pre-render) value, and
       silently skip the sync this render actually needs -- confirmed
       live via render_backend_test's own "Clear/Draw ordering" milestone
       reading back a Locked surface before its own just-EndScene'd
       content had actually replayed. Draining first guarantees any
       replay that might still flip THIS surface's sync_state has already
       done so by the time the check below runs. */
    gl_thread_wait_idle();
    if (s->sync_state != SURFACE_GPU_AHEAD) return;
    s->sync_state = SURFACE_SYNCED;
    if (s->width == 0 || s->height == 0) return;

    /* s->gl (the GPU FBO/texture) is tied to object identity, not to
       s->width/s->height -- Flip() swaps width/height/buffer/etc.
       between the front/back surface *objects* but deliberately leaves
       each object's ->gl alone (see the comment in surface7_Flip). Any
       3D draw call re-syncs ->gl to the CURRENT dimensions via
       ensure_rendering_target -> gl_target_ensure, but this function
       runs on the Blt/BltFast/Flip/Lock path and never does -- so if a
       Flip() changed s->width/s->height to something ->gl was never
       resized for (confirmed via real gameplay trace: the same surface
       pointer reported 800x600 at CreateSurface time and 640x480 later,
       once it had been through the menu<->gameplay resolution switch),
       gl_target_read_rgba below would fill a buffer sized for
       s->width*s->height using ->gl's own (mismatched) w/h as its
       stride/size -- a real heap-buffer overflow when ->gl is larger,
       and in every case a reinterpretation of the pixels at the wrong
       row stride, which looks exactly like the sheared/mirrored/
       duplicated corruption this project has been chasing. There is no
       valid GPU-side content for this surface's current identity/size
       in that case, so just drop it instead of reinterpreting garbage --
       resizing ->gl first would only recreate an empty texture and lose
       the real pixels anyway, not recover them. */
    if (s->gl.w != (int)s->width || s->gl.h != (int)s->height) {
        if (ddraw_trace_enabled()) {
            fprintf(stderr, "[ddraw-trace] surface7_sync_from_gpu: surface=%p dimension mismatch "
                            "gl=%dx%d surface=%ux%u -- dropping stale GPU content instead of "
                            "reinterpreting it at the wrong stride\n",
                    (void *)s, s->gl.w, s->gl.h, s->width, s->height);
        }
        return;
    }

    double perf_t0 = perf_trace_enabled() ? perf_now_ms() : 0.0;

    size_t need = (size_t)s->width * s->height * 4;
    uint8_t *rgba = malloc(need);
    if (rgba == NULL) return;
    gl_target_read_rgba(&s->gl, rgba);

    pixfmt_decoded_t s_pd;
    pixfmt_decode(&s->pixfmt, &s_pd);
    for (uint32_t y = 0; y < s->height; y++) {
        for (uint32_t x = 0; x < s->width; x++) {
            surface7_write_raw(s, x, y, pack_rgba_fast(rgba + ((size_t)y * s->width + x) * 4, &s_pd, &s->pixfmt, s->palette));
        }
    }
    free(rgba);

    if (perf_trace_enabled()) {
        g_perf.sync_ms += perf_now_ms() - perf_t0;
        g_perf.sync_calls++;
    }
}

/* Converts the whole surface to RGBA8888 and hands it to src/render/'s
   GLES pipeline. Called whenever a primary surface's pixels change
   (Blt/BltFast targeting it directly, or Flip) -- see docs/api-surface/
   ddraw.md's per-frame hot path table: this game has no separate
   "present"/"swap chain" call beyond Blt and Flip themselves. */
static uint8_t *g_present_scratch = NULL;
static size_t g_present_scratch_cap = 0;

/* Grows (never shrinks) and returns the shared RGBA staging buffer --
   used both for presenting a primary surface and for uploading a
   surface's CPU pixels into a GL texture (ddraw_gl_get_texture);
   neither path overlaps the other's use of it within a single call. */
static uint8_t *ensure_present_scratch(size_t need)
{
    if (need > g_present_scratch_cap) {
        uint8_t *grown = realloc(g_present_scratch, need);
        if (grown == NULL) return NULL;
        g_present_scratch = grown;
        g_present_scratch_cap = need;
    }
    return g_present_scratch;
}

/* Converts the whole surface to a tightly packed top-down RGBA8888
   buffer -- shared by the present path below and the D3D7 texture
   upload path (ddraw_gl_get_texture), both of which need the same
   native-format-agnostic conversion. */
static void surface7_convert_to_rgba(const surface7_t *s, uint8_t *out)
{
    uint8_t *dst = out;
    pixfmt_decoded_t s_pd;
    pixfmt_decode(&s->pixfmt, &s_pd);
    for (uint32_t y = 0; y < s->height; y++) {
        for (uint32_t x = 0; x < s->width; x++) {
            unpack_rgba_fast(surface7_read_raw(s, x, y), &s_pd, &s->pixfmt, s->palette, dst);
            dst += 4;
        }
    }
}

/* This game's own primary/3D-render-target surface is confirmed real
   8bpp palette-indexed content (docs/game_code notes, and this whole
   project's own rendering-bug history) -- the surface7_convert_to_rgba
   CPU path below does a full per-pixel unpack_rgba (palette lookup +
   channel pack) loop for every one of those pixels, every single
   displayed frame. render_present_paletted does the same lookup on the
   GPU instead: the raw index bytes upload as-is (no CPU transform at
   all, and 1/4 the bytes of a pre-converted RGBA buffer), and only a
   tiny 256-entry palette needs converting on the CPU. Kept as a genuine
   fallback, not a special case that could silently stop covering real
   content: any OTHER pixel format (16/24/32-bit RGB, seen for some non-
   3D surfaces) still goes through the original, already-proven
   surface7_convert_to_rgba + render_present_rgba path unchanged. */
static uint8_t g_present_palette_scratch[256 * 4];

static void surface7_present(surface7_t *s)
{
    if (s == NULL || s->width == 0 || s->height == 0 || !render_is_initialized()) return;

    /* Phase 2 (async cmdbuf_flush, see [[project_gl_issuer_thread]]): the
       use_gpu_direct/no_valid_source decision just below reads s->sync_
       state and s->gl.tex/w/h DIRECTLY, on whichever thread calls Flip()
       -- unlike surface7_sync_from_gpu's own check (also drained, see its
       comment), there's no other function in between to catch this. A
       real D3D7 render into this surface may still be replaying
       asynchronously on the GL thread when Flip() is called right after
       EndScene, so this decision itself needs the same drain, not just
       whichever branch it ends up choosing. */
    gl_thread_wait_idle();

    activity_sampler_enter(ACTIVITY_PRESENT);
    double perf_t0 = perf_trace_enabled() ? perf_now_ms() : 0.0;

    /* GPU-resident-direct path, tried first: skip the CPU convert+upload
       below entirely when s->gl already has this surface's real, current
       content and nothing has touched ->buffer since -- sync_state !=
       SURFACE_CPU_AHEAD is the existing, already-maintained signal for
       exactly that (Blt/Unlock/colorfill all set SURFACE_CPU_AHEAD
       whenever they write ->buffer directly; a successful GPU Blt/clear
       or a D3D7 render leaves it at SURFACE_GPU_AHEAD -- see
       surface7_Blt and ddraw_gl_mark_gpu_dirty). The explicit s->gl.w/h
       check guards the same dimension-mismatch case surface7_sync_from_gpu
       already defends against (a Flip changing s->width/height out from
       under a ->gl that hasn't been resized to match). Correct even for a
       nominally palette-indexed surface -- GPU rendering only ever
       produces real colors in ->gl, never raw index bytes, regardless of
       the surface's own DirectDraw pixel format (see
       render_present_gl_target's own doc comment). The CPU-buffer path
       in the else branch below is now purely a failure-fallback safety
       net (a dimension mismatch, or SURFACE_CPU_AHEAD genuinely true),
       not a flag-selected alternative. */
    int use_gpu_direct = s->sync_state != SURFACE_CPU_AHEAD && s->gl.tex != 0 &&
                         s->gl.w == (int)s->width && s->gl.h == (int)s->height;

    /* Real, live-confirmed root cause of a black-screen/game-frame flicker
       (menu<->gameplay resolution switch, see surface7_sync_from_gpu's own
       comment on the exact "800x600 at CreateSurface, 640x480 later"
       trace this was diagnosed from): SURFACE_GPU_AHEAD means ->buffer is
       KNOWN stale relative to ->gl -- the whole point of that state. If
       ->gl also doesn't match this surface's CURRENT width/height (the
       same dimension-mismatch condition use_gpu_direct above already
       checks, and surface7_sync_from_gpu already refuses to read from),
       there is no correct pixel source left: ->gl can't be trusted (wrong
       size) and ->buffer can't be trusted (sync_state says so itself).
       Previously this fell through to the "else" branch below anyway,
       which presented ->buffer regardless -- for a render target that's
       ONLY ever GPU-rendered into (never Blt/Locked by the CPU), ->buffer
       is still its original calloc()'d zero content from surface7_create,
       i.e. solid black. The very next real D3D7 draw re-syncs ->gl to the
       current dimensions (ensure_rendering_target -> gl_target_ensure),
       so this condition is transient -- one bad Flip, then correct again
       -- which is exactly what produces a black/game-frame ALTERNATION
       rather than a permanently broken screen. Skipping the present
       entirely here (not calling any render_present_* function at all)
       just leaves the window showing whatever it last successfully
       presented, which is always strictly better than replacing a real
       frame with black. */
    int no_valid_source = s->sync_state == SURFACE_GPU_AHEAD && s->gl.tex != 0 &&
                          (s->gl.w != (int)s->width || s->gl.h != (int)s->height);
    if (no_valid_source) {
        if (ddraw_trace_enabled()) {
            fprintf(stderr, "[ddraw-trace] surface7_present: surface=%p no valid pixel source "
                            "(gl=%dx%d surface=%ux%u, GPU-ahead) -- holding last frame instead of "
                            "presenting stale/black content\n",
                    (void *)s, s->gl.w, s->gl.h, s->width, s->height);
        }
        activity_sampler_leave();
        return;
    }

    /* TEMP DIAGNOSTIC (opt-in via LONGROAD2_DDRAW_TRACE): investigating a
       real, live black-screen report where D3D7 draws/Clear/Flip are all
       genuinely happening (confirmed via existing traces) yet the window
       shows solid black -- narrowing down whether surface7_present picks
       the GPU-direct path at all, and what it thinks the surface's real
       dimensions/GL state are when it does. Remove once resolved. */
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] surface7_present: surface=%p %ux%u sync_state=%d "
                        "gl.tex=%u gl.w=%d gl.h=%d -> %s\n",
                (void *)s, s->width, s->height, (int)s->sync_state,
                s->gl.tex, s->gl.w, s->gl.h, use_gpu_direct ? "GPU-direct" : "CPU-buffer");
    }

    int did_gpu_present = 0;
    if (use_gpu_direct) {
        render_present_gl_target(&s->gl);

        did_gpu_present = 1;
        /* A real D3D7 GPU-rendered present unambiguously means this
           surface has shown real content -- see the all-zero-Lock/Unlock-
           write guard below, in the CPU-buffer branch, for why this flag
           exists. Must be set here too, not just there: D3D7 rendering
           and a direct CPU Lock/Unlock write to the SAME primary surface
           are two independent paths (confirmed live, both active in the
           same session), and the guard needs to know real content came
           through EITHER one, not just its own branch. */
        if (s->caps & DDSCAPS_PRIMARYSURFACE) s->ever_presented_nonzero = 1;
    } else {
        /* Sync lazily, HERE, only on the path that's actually about to
           read ->buffer -- callers (Flip, Blt's direct-primary-present
           case) used to have to pre-sync defensively before calling this
           at all, which would have silently defeated use_gpu_direct
           above by forcing sync_state to SURFACE_SYNCED (making
           SURFACE_CPU_AHEAD moot either way) before this function ever
           got a say --
           exactly the same "eager sync fights a newer GPU-aware path"
           shape as the surface7_Blt bug this whole plan started from.
           Callers no longer need (or should) sync before calling this. */
        surface7_sync_from_gpu(s);

        /* Real, live-confirmed root cause of a black-screen report where
           D3D7 is genuinely rendering real content (confirmed via other
           traces) but the window still shows solid black: this game's own
           video/movie codec (established earlier, closed-source, unfixable
           from this side) writes GENUINELY ALL-ZERO pixel data via a
           direct Lock/memcpy/Unlock straight to the PRIMARY surface, on
           its own independent schedule, interleaved with the D3D7 device's
           own real BeginScene/.../EndScene/Flip cycle on the SAME primary
           -- confirmed via a live capture showing this exact "buffer=...
           nonzero=0 (0.0%)" pattern immediately followed by a real present
           of that all-zero content, racing against and overwriting
           whatever the D3D7 path had just rendered a moment before (both
           paths ultimately call SDL_GL_SwapWindow, so whichever runs last
           wins the visible frame). Wine's own real driver never receives
           an all-zero Lock/Unlock write like this for the primary in the
           first place (the game's own broken codec presumably produces
           SOMETHING non-zero on a real system, or isn't exercised the same
           way) -- but regardless of why, presenting known-all-zero content
           over a real, already-rendered frame is never correct, matching
           the exact same "hold the last good frame instead of replacing it
           with black" precedent the no_valid_source check above already
           established for a different case. Scoped to the primary surface
           only (never a texture or an intentionally-cleared render
           target, both of which can be legitimately all-zero) and skipped
           entirely once the surface has ever shown real content, so this
           can't mask a genuinely-intended very first black frame either. */
        if ((s->caps & DDSCAPS_PRIMARYSURFACE) && s->buffer != NULL) {
            size_t total = (size_t)s->pitch * s->height;
            int all_zero = 1;
            for (size_t i = 0; i < total; i++) {
                if (s->buffer[i] != 0) { all_zero = 0; break; }
            }
            if (all_zero && s->ever_presented_nonzero) {
                if (ddraw_trace_enabled()) {
                    fprintf(stderr, "[ddraw-trace] surface7_present: surface=%p primary buffer is "
                                    "entirely zero -- holding last frame instead of presenting black "
                                    "over previously-real content\n", (void *)s);
                }
                activity_sampler_leave();
                return;
            }
            if (!all_zero) s->ever_presented_nonzero = 1;
        }

        if ((s->pixfmt.dwFlags & DDPF_PALETTEINDEXED8) && s->palette != NULL) {
            const paletteentry_t *entries = s->palette->entries;
            for (int i = 0; i < 256; i++) {
                g_present_palette_scratch[i * 4 + 0] = entries[i].r;
                g_present_palette_scratch[i * 4 + 1] = entries[i].g;
                g_present_palette_scratch[i * 4 + 2] = entries[i].b;
                g_present_palette_scratch[i * 4 + 3] = 0xFFu; /* peFlags is not alpha -- always fully opaque */
            }
            render_present_paletted(s->buffer, (int)s->width, (int)s->height, (int)s->pitch,
                                     g_present_palette_scratch);
        } else {
            uint8_t *scratch = ensure_present_scratch((size_t)s->width * s->height * 4);
            if (scratch == NULL) { activity_sampler_leave(); return; }
            surface7_convert_to_rgba(s, scratch);
            render_present_rgba(scratch, (int)s->width, (int)s->height, (int)s->width);
        }
    }

    if (perf_trace_enabled()) {
        if (did_gpu_present) {
            g_perf.present_gpu_ms += perf_now_ms() - perf_t0;
            g_perf.present_gpu_calls++;
        } else {
            g_perf.present_ms += perf_now_ms() - perf_t0;
            g_perf.present_calls++;
        }
    }
    activity_sampler_leave();
}

void ddraw_force_gpu_ahead_size_mismatch_for_test(void *surface, uint32_t fake_width, uint32_t fake_height)
{
    surface7_t *s = (surface7_t *)surface;
    if (s == NULL) return;
    s->sync_state = SURFACE_GPU_AHEAD;
    s->width = fake_width;
    s->height = fake_height;
}

static uint32_t WINAPI surface7_Blt(void *self, ddraw_rect_t *dst_rect, surface7_t *src, ddraw_rect_t *src_rect, uint32_t flags, void *fx_)
{
    surface7_t *d = (surface7_t *)self;
    ddbltfx_t *fx = (ddbltfx_t *)fx_;
    /* Neither d nor src is synced here -- see the surface7_sync_from_gpu
       calls further down (colorfill branch, and right before the CPU
       copy-loop fallback), each placed exactly where a CPU write/read of
       ->buffer is about to happen. A real game (this game's own minimap,
       confirmed via LONGROAD2_PERF_TRACE: up to ~48 Blt calls in one
       frame, all targeting the same destination surface -- one layered
       icon per Blt) can call Blt on the SAME destination (and, in
       principle, the same source) many times within one frame; once the
       GPU path (below) starts succeeding, it sets d->sync_state to
       SURFACE_GPU_AHEAD, and an eager, unconditional sync here would
       force a full glReadPixels readback of the WHOLE surface on every
       subsequent Blt just to immediately discard that CPU copy again --
       confirmed live for d as
       47 real (non-no-op) syncs in a single ~150ms frame, over 90% of
       that frame's total time, on readbacks nothing ever used. src's own
       eager sync never showed up as a real cost in that same capture
       (this game's real Blt sources are static assets, not recent render
       targets), but it's the identical shape of waste on general
       principle: the GPU path never reads src->buffer either (it uses
       ddraw_gl_get_texture(src), a GL texture handle, not the CPU
       buffer), so syncing src before knowing whether the GPU path will
       even be attempted is exactly as pointless as syncing d was. */

    if (ddraw_trace_enabled()) {
        /* dst_rect/src_rect logged RAW (as the game supplied them, before
           any clamping below) -- and fx->dwDDFX too, since Blt mirror/
           rotate effects (DDBLTFX_MIRRORUPDOWN etc.) are parsed nowhere
           in this shim (see the comment where dw/dh get computed below);
           if a game relies on that flag for a real effect, this is the
           only way to see it's even being requested. */
        fprintf(stderr, "[ddraw-trace] Blt: dst=%p(caps=0x%08x,%ux%u) src=%p(caps=0x%08x,%ux%u) "
                        "flags=0x%08x ddfx=0x%08x\n",
                (void *)d, d->caps, d->width, d->height,
                (void *)src, src != NULL ? src->caps : 0, src != NULL ? src->width : 0, src != NULL ? src->height : 0,
                flags, fx != NULL ? fx->dwDDFX : 0);
        if (dst_rect != NULL) {
            fprintf(stderr, "  dst_rect=(l=%d,t=%d,r=%d,b=%d)\n",
                    dst_rect->left, dst_rect->top, dst_rect->right, dst_rect->bottom);
        } else {
            fprintf(stderr, "  dst_rect=(null, full surface)\n");
        }
        if (src_rect != NULL) {
            fprintf(stderr, "  src_rect=(l=%d,t=%d,r=%d,b=%d)\n",
                    src_rect->left, src_rect->top, src_rect->right, src_rect->bottom);
        } else if (src != NULL) {
            fprintf(stderr, "  src_rect=(null, full surface)\n");
        }
    }

    ddraw_rect_t dr = dst_rect != NULL ? *dst_rect : (ddraw_rect_t){ 0, 0, (int32_t)d->width, (int32_t)d->height };
    if (dr.left < 0) dr.left = 0;
    if (dr.top < 0) dr.top = 0;
    if (dr.right > (int32_t)d->width) dr.right = (int32_t)d->width;
    if (dr.bottom > (int32_t)d->height) dr.bottom = (int32_t)d->height;
    int32_t dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    int did_gpu = 0; /* set true by either the colorfill GPU-clear or the real-Blt GPU-blit path below -- gates the sync_state bookkeeping after this whole block, since either GPU path writes ->gl, not ->buffer */

    /* activity_sampler tag is intentionally NOT leave()'d on this
       function's own early-return paths below (there are several,
       scattered through a long function) -- same accepted imprecision as
       d3d_draw_prepare's early return in draw_flat_execute_now: this is
       a statistical diagnostic, and any stuck tag self-corrects at the
       next tagged call elsewhere, which in real gameplay is always soon. */
    activity_sampler_enter(ACTIVITY_BLT);
    double perf_t0 = perf_trace_enabled() ? perf_now_ms() : 0.0;
    if (dw > 0 && dh > 0) {
        if (src == NULL || (flags & DDBLT_COLORFILL)) {
            uint32_t fill = fx != NULL ? fx->dwFillColor : 0;
            /* A fill rect covering the ENTIRE surface has no "untouched
               region" for either sync direction to preserve -- every pixel
               is about to be overwritten regardless of which branch below
               runs, so neither the GPU path's pre-upload nor the CPU
               fallback's pre-sync needs to run first. dr is already
               clamped to surface bounds above, so an overhanging dst_rect
               that clamps down to exactly the full surface correctly
               counts too. A real trace (SWEP1RCR) showed this exact
               pattern -- a full-surface DDBLT_COLORFILL every frame --
               driving the majority of that game's CPU cost via the
               now-skipped round trip. */
            int full_surface = (dr.left == 0 && dr.top == 0 &&
                                 dw == (int32_t)d->width && dh == (int32_t)d->height);
            /* DDSCAPS_ZBUFFER surfaces are never read through ->gl by
               anything that affects the rendered frame -- this shim's real
               GL depth-testing lives entirely in the render target's own
               gl_target_t.depth_rb (see gl_target_ensure/d3d_device7_
               SetRenderTarget), never via a DirectDraw Z-buffer surface
               object. Skipping the GPU attempt for such surfaces avoids a
               pointless GPU round-trip on every clear (confirmed hot: the
               SWEP1RCR trace above), and as a side effect avoids a latent
               corruption bug for a genuinely DDPF_ZBUFFER-formatted surface
               (all-zero RGB masks would otherwise make unpack_rgba below
               flatten any real fill value to black before glClearColor). */
            int try_gpu = (d->caps & DDSCAPS_ZBUFFER) == 0;
            if (try_gpu && render_is_initialized()) {
                gl_target_ensure(&d->gl, (int)d->width, (int)d->height, 1);
                if (d->gl.fbo != 0) {
                    /* A sub-rect fill needs the untouched regions to
                       already be correct in ->gl before clearing just
                       the fill rect -- same reasoning as the real-Blt
                       GPU path's own upload-if-needed line right below. */
                    if (!full_surface && d->sync_state == SURFACE_CPU_AHEAD) surface7_upload_to_gl(d);
                    uint8_t rgba[4];
                    unpack_rgba(fill, &d->pixfmt, d->palette, rgba);
                    did_gpu = gl_target_clear_rect(&d->gl, dr.left, dr.top, dw, dh, rgba);
                    if (did_gpu) d->sync_state = SURFACE_GPU_AHEAD;
                }
            }
            if (!did_gpu) {
                /* GPU path didn't run (render not initialized yet, FBO
                   creation failed, or this is a Z-buffer surface) -- this
                   loop writes d->buffer directly and only touches the fill
                   rect, so a sub-rect fill genuinely needs d synced first:
                   any pixels outside the fill rect must already be
                   current. A full-surface fill has no such pixels. */
                if (!full_surface) surface7_sync_from_gpu(d);
                /* fill and d->bpp are loop-invariant for the whole call --
                   switch on bpp ONCE here instead of once per pixel inside
                   surface7_write_raw. A real perf.data capture of SWEP1RCR
                   found this exact loop (this surface is almost always the
                   DDSCAPS_ZBUFFER one, routed here on purpose since its GPU
                   round-trip was pure waste -- see full_surface's own
                   comment above) at >50% of TOTAL session CPU once that
                   round-trip was eliminated: the per-pixel function-call +
                   switch overhead became the new bottleneck. Byte-wise
                   writes (no uint32_t* pointer-cast) deliberately match
                   surface7_write_raw's own convention -- avoids any
                   strict-aliasing question, and the win here is purely
                   from hoisting the switch out of the pixel loop, not from
                   wider stores. */
                uint8_t b0 = (uint8_t)fill, b1 = (uint8_t)(fill >> 8),
                        b2 = (uint8_t)(fill >> 16), b3 = (uint8_t)(fill >> 24);
                switch (d->bpp) {
                    case 1:
                        for (int32_t y = 0; y < dh; y++) {
                            memset(d->buffer + (size_t)(dr.top + y) * d->pitch + (size_t)dr.left, b0, (size_t)dw);
                        }
                        break;
                    case 2:
                        for (int32_t y = 0; y < dh; y++) {
                            uint8_t *row = d->buffer + (size_t)(dr.top + y) * d->pitch + (size_t)dr.left * 2;
                            for (int32_t x = 0; x < dw; x++) { row[0] = b0; row[1] = b1; row += 2; }
                        }
                        break;
                    case 3:
                        for (int32_t y = 0; y < dh; y++) {
                            uint8_t *row = d->buffer + (size_t)(dr.top + y) * d->pitch + (size_t)dr.left * 3;
                            for (int32_t x = 0; x < dw; x++) { row[0] = b0; row[1] = b1; row[2] = b2; row += 3; }
                        }
                        break;
                    case 4:
                        for (int32_t y = 0; y < dh; y++) {
                            uint8_t *row = d->buffer + (size_t)(dr.top + y) * d->pitch + (size_t)dr.left * 4;
                            for (int32_t x = 0; x < dw; x++) { row[0] = b0; row[1] = b1; row[2] = b2; row[3] = b3; row += 4; }
                        }
                        break;
                    default:
                        /* Not a bpp this codebase ever actually constructs
                           (surface7_bpp_from_pixfmt only ever produces
                           1/2/3/4) -- but nothing validates dwRGBBitCount
                           in CreateSurface either, so fall back to the
                           original per-pixel path instead of silently
                           misapplying a hardcoded 4-byte stride, exactly
                           preserving surface7_write_raw's own exact-match
                           switch behavior (1 byte, correctly strided) for
                           any bpp it wasn't designed for. */
                        for (int32_t y = 0; y < dh; y++) {
                            for (int32_t x = 0; x < dw; x++) {
                                surface7_write_raw(d, (uint32_t)(dr.left + x), (uint32_t)(dr.top + y), fill);
                            }
                        }
                        break;
                }
            }
        } else {
            ddraw_rect_t sr = src_rect != NULL ? *src_rect : (ddraw_rect_t){ 0, 0, (int32_t)src->width, (int32_t)src->height };
            /* Same clamping as dr above, applied to the SOURCE this time
               -- previously missing entirely. An out-of-range src_rect
               (any reason: a stale rect after a surface resize, a
               game-side off-by-one, a rect sized for a different
               surface) would otherwise compute sx/sy offsets outside
               src->buffer's real allocation, reading adjacent heap
               memory as pixel data -- a real, likely source of exactly
               the kind of "garbage" corruption this project chased
               through the rendering pipeline before finding nothing
               there: reading unrelated heap contents as pixels produces
               arbitrary-looking colors/streaks with no rhyme or reason,
               which is exactly what garbage memory looks like rendered
               as an image. */
            if (sr.left < 0) sr.left = 0;
            if (sr.top < 0) sr.top = 0;
            if (sr.right > (int32_t)src->width) sr.right = (int32_t)src->width;
            if (sr.bottom > (int32_t)src->height) sr.bottom = (int32_t)src->height;
            int32_t sw = sr.right - sr.left, sh = sr.bottom - sr.top;
            int use_ck = (flags & DDBLT_KEYSRC) && src->has_ck_srcblt;
            int same_fmt = src->bpp == d->bpp && pixfmt_equal(&src->pixfmt, &d->pixfmt);

            /* GPU-accelerated path, tried first for every real Blt:
               composites source->dest entirely on the GPU (gl_target_blit,
               render.c) instead of the CPU pixel-copy loop below. d is
               deliberately NOT synced anywhere above this point (see the
               comment at the top of this function) specifically so this
               path can mark the destination SURFACE_GPU_AHEAD and return
               without ever touching d->buffer -- a real game Blt'ing many
               layers onto the same destination in one frame (this game's
               minimap) stays entirely GPU-side across all of them instead
               of paying for a full readback between every pair. DDBLT_ROP
               is included here too (dwROP is read nowhere in this
               codebase, so a GPU-accelerated plain/keyed copy already
               produces the identical result the CPU path's own "ignore
               ROP" behavior does). The CPU pixel-copy loop below is now
               purely a failure-fallback safety net (GL resources not
               ready yet, dest with no palette/format info, etc), not a
               flag-selected alternative -- did_gpu itself is declared at
               the top of this whole function (outer scope), not here,
               since the sync_state bookkeeping after this if/else
               needs to see it too. */
            if (sw > 0 && sh > 0 && render_is_initialized()) {
                int src_tw = 0, src_th = 0;
                unsigned src_tex = ddraw_gl_get_texture(src, &src_tw, &src_th);
                if (src_tex != 0) {
                    gl_target_ensure(&d->gl, (int)d->width, (int)d->height, 1);
                    if (d->gl.fbo != 0) {
                        /* d->gl may just have been freshly (re)allocated above,
                           or may exist but be stale relative to d->buffer (e.g.
                           this destination's latest content came from an earlier
                           CPU-path Blt/colorfill/Lock+write, not a GPU render) --
                           either way, a colorkeyed/partial blit needs the REAL
                           current destination content already sitting in ->gl
                           before drawing on top of it, since any discarded
                           source texel must show what was really there, not a
                           blank/stale texture. Mirrors ddraw_gl_bind_render_
                           target's identical handling for the D3D7-render-
                           target-binding case. */
                        if (d->sync_state == SURFACE_CPU_AHEAD) surface7_upload_to_gl(d);
                        uint8_t ck_low[4] = {0}, ck_high[4] = {0};
                        if (use_ck) ddraw_gl_get_color_key(src, ck_low, ck_high);
                        did_gpu = gl_target_blit(&d->gl, dr.left, dr.top, dw, dh,
                                                  &src->gl, sr.left, sr.top, sw, sh,
                                                  use_ck, ck_low, ck_high);
                        if (did_gpu) d->sync_state = SURFACE_GPU_AHEAD;
                    }
                }
            }

            if (!did_gpu && sw > 0 && sh > 0) {
                /* GPU path didn't run (render not initialized yet, or
                   texture/FBO creation failed) -- this loop writes
                   d->buffer directly via surface7_write_raw below (same
                   "needs current content outside the touched rect"
                   reasoning as the colorfill branch above) and reads
                   src->buffer directly via surface7_read_raw, so both
                   need syncing exactly here, the one place this function
                   actually touches either buffer. */
                surface7_sync_from_gpu(d);
                surface7_sync_from_gpu(src);
                pixfmt_decoded_t src_pd, d_pd;
                pixfmt_decode(&src->pixfmt, &src_pd);
                pixfmt_decode(&d->pixfmt, &d_pd);
                for (int32_t y = 0; y < dh; y++) {
                    int32_t sy = sr.top + (y * sh) / dh;
                    for (int32_t x = 0; x < dw; x++) {
                        int32_t sx = sr.left + (x * sw) / dw;
                        uint32_t raw = surface7_read_raw(src, (uint32_t)sx, (uint32_t)sy);
                        if (use_ck && raw >= src->ck_srcblt.dwColorSpaceLowValue && raw <= src->ck_srcblt.dwColorSpaceHighValue) {
                            continue;
                        }
                        uint32_t wraw = raw;
                        if (!same_fmt) {
                            uint8_t rgba[4];
                            unpack_rgba_fast(raw, &src_pd, &src->pixfmt, src->palette, rgba);
                            wraw = pack_rgba_fast(rgba, &d_pd, &d->pixfmt, d->palette);
                        }
                        surface7_write_raw(d, (uint32_t)(dr.left + x), (uint32_t)(dr.top + y), wraw);
                    }
                }
            }
        }
    }
    /* Counts/pixels unconditional, only the perf_now_ms()-based timing
       gated -- matches d3d7.c's own g_d3d7_perf split exactly (draw_calls/
       vertices/indices always accumulate, only the expensive per-frame
       glFinish()+report is gated behind its own trace flag). Also what
       lets ddraw_perf_get_blt_calls_for_test/ddraw_perf_get_bltfast_
       calls_for_test observe real calls without needing LONGROAD2_PERF_
       TRACE=1 set before this process's first-ever perf_trace_enabled()
       call latches its own cache (env vars this codebase reads are
       cached on first check) -- a real problem hit writing this fix's
       own test, see [[project_ddraw_bltfast_gpu_path_investigation]]. */
    g_perf.blt_calls++;
    if (dw > 0 && dh > 0) g_perf.blt_pixels += (long)dw * (long)dh;
    if (perf_trace_enabled()) {
        g_perf.blt_ms += perf_now_ms() - perf_t0;
    }
    activity_sampler_leave();

    /* The GPU-blit path already set d->sync_state = SURFACE_GPU_AHEAD
       itself and wrote nothing to d->buffer -- setting SURFACE_CPU_AHEAD
       here too would be a lie (sync_state can only mean one side is
       newer) and would tell a later reader ->buffer is current when
       it's stale by exactly the content this Blt just drew. */
    if (!did_gpu) d->sync_state = SURFACE_CPU_AHEAD;
    if (ddraw_trace_enabled() && d->width <= 256 && d->buffer != NULL) {
        size_t total = (size_t)d->pitch * d->height;
        int all_zero = 1;
        for (size_t i = 0; i < total; i++) { if (d->buffer[i] != 0) { all_zero = 0; break; } }
        fprintf(stderr, "[ddraw-trace] surface7_Blt: dst=%p did_gpu=%d sync_state=%d dst_buffer_all_zero=%d\n",
                (void *)d, did_gpu, (int)d->sync_state, all_zero);
    }
    if ((d->caps & DDSCAPS_PRIMARYSURFACE) && primary_present_allowed()) {
        /* No pre-sync here anymore -- surface7_present(d) now decides
           for itself (Step 4 of [[project_ddraw_gpu_blt_glreadpixels_elimination]])
           whether it can present directly from d->gl or needs d->buffer
           synced first, and syncs lazily only in the latter case. A
           pre-sync here would force a full readback even when the GPU-
           resident-direct path was about to apply, the same class of
           self-defeating eager sync Bug 3 in that plan already fixed
           for the Blt-to-offscreen-target case. */
        surface7_present(d);
    }
    return DD_OK;
}

static uint32_t WINAPI surface7_BltFast(void *self, uint32_t x, uint32_t y, surface7_t *src, ddraw_rect_t *src_rect, uint32_t flags)
{
    surface7_t *d = (surface7_t *)self;
    if (src == NULL) return DD_OK;
    surface7_sync_from_gpu(d);
    surface7_sync_from_gpu(src);

    ddraw_rect_t sr = src_rect != NULL ? *src_rect : (ddraw_rect_t){ 0, 0, (int32_t)src->width, (int32_t)src->height };
    /* See the matching comment in surface7_Blt -- an out-of-range
       src_rect here would otherwise read outside src->buffer's real
       allocation. */
    if (sr.left < 0) sr.left = 0;
    if (sr.top < 0) sr.top = 0;
    if (sr.right > (int32_t)src->width) sr.right = (int32_t)src->width;
    if (sr.bottom > (int32_t)src->height) sr.bottom = (int32_t)src->height;
    int32_t dw = sr.right - sr.left, dh = sr.bottom - sr.top;
    if ((int32_t)x + dw > (int32_t)d->width) dw = (int32_t)d->width - (int32_t)x;
    if ((int32_t)y + dh > (int32_t)d->height) dh = (int32_t)d->height - (int32_t)y;

    activity_sampler_enter(ACTIVITY_BLT);
    double perf_t0 = perf_trace_enabled() ? perf_now_ms() : 0.0;
    if (dw > 0 && dh > 0) {
        int use_ck = (flags & DDBLTFAST_SRCCOLORKEY) && src->has_ck_srcblt;
        int same_fmt = src->bpp == d->bpp && pixfmt_equal(&src->pixfmt, &d->pixfmt);
        pixfmt_decoded_t src_pd, d_pd;
        pixfmt_decode(&src->pixfmt, &src_pd);
        pixfmt_decode(&d->pixfmt, &d_pd);

        for (int32_t row = 0; row < dh; row++) {
            if (same_fmt && !use_ck) {
                memcpy(d->buffer + (size_t)(y + (uint32_t)row) * d->pitch + (size_t)x * d->bpp,
                       src->buffer + (size_t)(sr.top + row) * src->pitch + (size_t)sr.left * src->bpp,
                       (size_t)dw * d->bpp);
                continue;
            }
            for (int32_t col = 0; col < dw; col++) {
                uint32_t raw = surface7_read_raw(src, (uint32_t)(sr.left + col), (uint32_t)(sr.top + row));
                if (use_ck && raw >= src->ck_srcblt.dwColorSpaceLowValue && raw <= src->ck_srcblt.dwColorSpaceHighValue) {
                    continue;
                }
                uint32_t wraw = raw;
                if (!same_fmt) {
                    uint8_t rgba[4];
                    unpack_rgba_fast(raw, &src_pd, &src->pixfmt, src->palette, rgba);
                    wraw = pack_rgba_fast(rgba, &d_pd, &d->pixfmt, d->palette);
                }
                surface7_write_raw(d, x + (uint32_t)col, y + (uint32_t)row, wraw);
            }
        }
    }
    /* Counts/pixels unconditional -- see surface7_Blt's own identical
       split and comment above for why. */
    g_perf.bltfast_calls++;
    if (dw > 0 && dh > 0) g_perf.bltfast_pixels += (long)dw * (long)dh;
    if (perf_trace_enabled()) {
        g_perf.bltfast_ms += perf_now_ms() - perf_t0;
    }
    activity_sampler_leave();

    d->sync_state = SURFACE_CPU_AHEAD;
    if ((d->caps & DDSCAPS_PRIMARYSURFACE) && primary_present_allowed()) {
        surface7_present(d);
    }
    return DD_OK;
}

static uint32_t WINAPI surface7_Flip(void *self, surface7_t *target, uint32_t flags)
{
    (void)flags;
    surface7_t *front = (surface7_t *)self;
    surface7_t *back = target;
    if (back == NULL) {
        for (int i = 0; i < DDRAW_MAX_ATTACHED; i++) {
            if (front->attached[i] != NULL && (front->attached[i]->caps & DDSCAPS_BACKBUFFER) != 0) {
                back = front->attached[i];
                break;
            }
        }
    }
    /* TEMP DIAGNOSTIC (opt-in via LONGROAD2_DDRAW_TRACE): Flip was the one
       call in this whole surface lifecycle with zero tracing anywhere --
       real gameplay logs showed a render-target surface's own width/
       height genuinely changing (800x600 -> 640x480) with only one
       CreateSurface on record for it, which only Flip's width/height
       swap could explain, but there was no way to see what it was
       actually being flipped against. Logging both sides' dimensions
       here closes that gap for the next capture. */
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] Flip: front=%p(%ux%u) target_param=%p back=%p(%ux%u)%s\n",
                (void *)front, front->width, front->height, (void *)target, (void *)back,
                back != NULL ? back->width : 0, back != NULL ? back->height : 0,
                (back != NULL && (front->width != back->width || front->height != back->height))
                    ? " <-- MISMATCHED SIZES" : "");
    }
    /* Phase 2 (async cmdbuf_flush, see [[project_gl_issuer_thread]]): the
       swap below moves ->gl/->sync_state between these two surface
       OBJECTS synchronously, on whichever thread calls Flip(). A D3D7
       device's own EndScene, called just before Flip() in the ordinary
       BeginScene/.../EndScene/Flip() sequence, may still have a real
       render into `back` replaying asynchronously on the GL thread at
       this exact moment -- that job updates `back`'s own ->gl/->sync_
       state by the surface pointer it captured when the draw was
       recorded, which is `back` specifically, not whichever surface
       happens to be called "back" after this swap runs. Draining first
       guarantees that update has already landed on `back` BEFORE the
       swap moves it onto `front` -- without this, the swap can run first
       and hand the (still-empty) OLD content to `front` while the real,
       just-rendered content the game actually meant to present lands on
       `back` a moment later, invisible until the NEXT Flip() moves it
       across for real one frame late. Confirmed live via present_flip_
       check's own "D3D7-rendered asymmetric content through a real
       Flip()" milestone. */
    gl_thread_wait_idle();
    if (back != NULL) {
        /* Real page-flipping swaps which physical buffer is "front" --
           the game keeps the same surface object pointers forever, so we
           swap buffer contents/metadata between the two objects instead
           of swapping the objects themselves. */
        uint8_t *tmp_buf = front->buffer; front->buffer = back->buffer; back->buffer = tmp_buf;
        uint32_t tmp_w = front->width; front->width = back->width; back->width = tmp_w;
        uint32_t tmp_h = front->height; front->height = back->height; back->height = tmp_h;
        uint32_t tmp_pitch = front->pitch; front->pitch = back->pitch; back->pitch = tmp_pitch;
        uint32_t tmp_bpp = front->bpp; front->bpp = back->bpp; back->bpp = tmp_bpp;
        ddpixelformat_t tmp_fmt = front->pixfmt; front->pixfmt = back->pixfmt; back->pixfmt = tmp_fmt;
        /* ->gl (the GPU FBO/texture -- just a handful of GLuint handles
           and ints, no GL calls involved in copying it) travels with the
           swap too, alongside sync_state: sync_state describes a
           relationship between ->buffer and ->gl, and both of those just
           moved, so a single swap of the one field that used to require
           two (cpu_ahead and gpu_ahead separately) keeps it correct.
           Previously ->gl was deliberately left tied to object identity
           (see surface7_sync_from_gpu's own comment, and the dimension-
           mismatch guard it added) and didn't need special handling
           because the two eager syncs above already forced both sides to
           a clean, already-flushed state before this point -- correct,
           but only by relying on paying for a real GPU->CPU readback on
           every single Flip. Now that gl/sync_state move as a pair, a
           surface's GPU-resident content follows its *logical role*
           (front vs back) through the flip the same way its buffer
           always has, which is what later lets those eager pre-swap
           syncs be removed instead of silently orphaning whichever
           side's GPU content doesn't get read back before the swap. This
           also structurally strengthens the s->gl.w != s->width
           dimension-mismatch guard in surface7_sync_from_gpu: gl and
           dimensions can no longer be split apart by a Flip, so that
           check becomes defense in depth rather than the only thing
           preventing a heap overflow. */
        gl_target_t tmp_gl = front->gl; front->gl = back->gl; back->gl = tmp_gl;
        surface_sync_t tmp_sync = front->sync_state; front->sync_state = back->sync_state; back->sync_state = tmp_sync;
    }

    /* Flip's own present is the authoritative "this frame is done" signal
       -- always goes through uncapped, unlike the Blt-to-primary path
       above. Reset the burst budget here too: a real frame boundary
       should get a fresh one immediately rather than wait on
       primary_present_allowed()'s own staleness timer -- though that
       timer is what actually protects a screen that never calls Flip()
       at all (see the comment above primary_present_allowed()). */
    g_primary_burst_count = 0;
    g_primary_last_present_ms = perf_now_ms();
    /* No sync here anymore (Step 4 of [[project_ddraw_gpu_blt_glreadpixels_elimination]]
       removed it): surface7_present(front) now syncs lazily, itself,
       only when it isn't taking the GPU-resident-direct path -- an eager
       sync here would force a full readback on every real Flip even
       when the direct path was about to apply, undoing the whole point
       of that path for exactly the reason Bug 3 in that plan already
       went through for Blt. */
    /* Pulled BEFORE surface7_present's own GPU work so its glFinish()
       isolates ONLY the 3D-scene backlog (every EndScene since the last
       Flip), not anything present/swap is about to add -- same
       separation of concerns d3d7.c's EndScene used to do on its own,
       see d3d7_perf_report_for_frame's own comment. */
    d3d7_perf_frame_report_t d3d_report;
    d3d7_perf_report_for_frame(&d3d_report);

    /* Real fix, not a diagnostic -- always runs, not gated behind any
       trace flag. Lets d3d_device7_BeginScene's own implicit depth
       clear (see its own comment in d3d7.c, and d3d7_reset_frame_depth_
       clear's own comment in d3d7.h) fire again for the NEXT real
       frame's first scene -- must happen exactly once per real Flip,
       same cadence as d3d7_perf_report_for_frame just above. */
    d3d7_reset_frame_depth_clear();

    surface7_present(front);
    /* Report+reset the perf accumulator here rather than on every Blt,
       so LONGROAD2_PERF_TRACE prints one line per displayed frame
       regardless of how many Blts happened inside it. */
    perf_report_and_reset_if_due(&d3d_report);
    return DD_OK;
}

static const surface7_vtbl_t g_surface7_vtbl = {
    .QueryInterface = (comfn2_t)surface7_QueryInterface,
    .AddRef = surface7_AddRef,
    .Release = surface7_Release,
    .AddAttachedSurface = (comfn1_t)surface7_AddAttachedSurface,
    .AddOverlayDirtyRect = fail1,
    .Blt = (comfn5_t)surface7_Blt,
    .BltBatch = fail3,
    .BltFast = (comfn5_t)surface7_BltFast,
    .DeleteAttachedSurface = (comfn2_t)surface7_DeleteAttachedSurface,
    .EnumAttachedSurfaces = (comfn2_t)surface7_EnumAttachedSurfaces,
    .EnumOverlayZOrders = fail3,
    .Flip = (comfn2_t)surface7_Flip,
    .GetAttachedSurface = (comfn2_t)surface7_GetAttachedSurface,
    .GetBltStatus = (comfn1_t)surface7_GetBltStatus,
    .GetCaps = (comfn1_t)surface7_GetCaps,
    .GetClipper = fail1,
    .GetColorKey = (comfn2_t)surface7_GetColorKey,
    .GetDC = (comfn1_t)surface7_GetDC,
    .GetFlipStatus = (comfn1_t)surface7_GetFlipStatus,
    .GetOverlayPosition = fail2,
    .GetPalette = (comfn1_t)surface7_GetPalette,
    .GetPixelFormat = (comfn1_t)surface7_GetPixelFormat,
    .GetSurfaceDesc = (comfn1_t)surface7_GetSurfaceDesc,
    .Initialize = fail2,
    .IsLost = surface7_IsLost,
    .Lock = (comfn4_t)surface7_Lock,
    .ReleaseDC = (comfn1_t)surface7_ReleaseDC,
    .Restore = surface7_Restore,
    .SetClipper = fail1,
    .SetColorKey = (comfn2_t)surface7_SetColorKey,
    .SetOverlayPosition = fail2,
    .SetPalette = (comfn1_t)surface7_SetPalette,
    .Unlock = (comfn1_t)surface7_Unlock,
    .UpdateOverlay = fail5,
    .UpdateOverlayDisplay = fail1,
    .UpdateOverlayZOrder = fail2,
    .GetDDInterface = fail1,
    .PageLock = fail1,
    .PageUnlock = fail1,
    .SetSurfaceDesc = (comfn2_t)surface7_SetSurfaceDesc,
    .SetPrivateData = fail4_SetPrivateData,
    .GetPrivateData = fail3,
    .FreePrivateData = fail1,
    .GetUniquenessValue = fail1,
    .ChangeUniquenessValue = fail0,
    .SetPriority = fail1,
    .GetPriority = fail1,
    .SetLOD = fail1,
    .GetLOD = fail1,
};

/* IDirectDrawSurface/2/3 (SWEP1RCR/DX6-era legacy surfaces, see
   [[project_swep1rcr_port_scoping]]) -- identical to g_surface7_vtbl in
   every real per-method behavior EXCEPT GetCaps (see
   surface_legacy_GetCaps1's own comment for why). Real v1/2/3 callers
   never index past their own, shorter method list (COM vtables are
   additive/prefix-compatible across these interface versions -- v2 adds
   GetDDInterface/PageLock/PageUnlock after v1's list, v3 adds
   SetSurfaceDesc after that), so v1 callers naturally never reach the
   v2/v3-only tail slots here -- one shared table for all three versions
   is correct, not just convenient. GetAttachedSurface is unchanged from
   v7's: it only READS caps->dwCaps as a filter (surface7_GetAttachedSurface
   never writes through that pointer), so the DDSCAPS-vs-DDSCAPS2 size
   difference that forced a separate GetCaps doesn't apply to it. */
static const surface7_vtbl_t g_surface_legacy_vtbl = {
    .QueryInterface = (comfn2_t)surface7_QueryInterface,
    .AddRef = surface7_AddRef,
    .Release = surface7_Release,
    .AddAttachedSurface = (comfn1_t)surface7_AddAttachedSurface,
    .AddOverlayDirtyRect = fail1,
    .Blt = (comfn5_t)surface7_Blt,
    .BltBatch = fail3,
    .BltFast = (comfn5_t)surface7_BltFast,
    .DeleteAttachedSurface = (comfn2_t)surface7_DeleteAttachedSurface,
    .EnumAttachedSurfaces = (comfn2_t)surface7_EnumAttachedSurfaces,
    .EnumOverlayZOrders = fail3,
    .Flip = (comfn2_t)surface7_Flip,
    .GetAttachedSurface = (comfn2_t)surface7_GetAttachedSurface,
    .GetBltStatus = (comfn1_t)surface7_GetBltStatus,
    .GetCaps = (comfn1_t)surface_legacy_GetCaps1,
    .GetClipper = fail1,
    .GetColorKey = (comfn2_t)surface7_GetColorKey,
    .GetDC = (comfn1_t)surface7_GetDC,
    .GetFlipStatus = (comfn1_t)surface7_GetFlipStatus,
    .GetOverlayPosition = fail2,
    .GetPalette = (comfn1_t)surface7_GetPalette,
    .GetPixelFormat = (comfn1_t)surface7_GetPixelFormat,
    .GetSurfaceDesc = (comfn1_t)surface7_GetSurfaceDesc,
    .Initialize = fail2,
    .IsLost = surface7_IsLost,
    .Lock = (comfn4_t)surface7_Lock,
    .ReleaseDC = (comfn1_t)surface7_ReleaseDC,
    .Restore = surface7_Restore,
    .SetClipper = fail1,
    .SetColorKey = (comfn2_t)surface7_SetColorKey,
    .SetOverlayPosition = fail2,
    .SetPalette = (comfn1_t)surface7_SetPalette,
    .Unlock = (comfn1_t)surface7_Unlock,
    .UpdateOverlay = fail5,
    .UpdateOverlayDisplay = fail1,
    .UpdateOverlayZOrder = fail2,
    .GetDDInterface = fail1,
    .PageLock = fail1,
    .PageUnlock = fail1,
    .SetSurfaceDesc = (comfn2_t)surface7_SetSurfaceDesc,
};

/* Forward declaration: real definition lives alongside d3d_device3_t
   (needs its full struct layout), same reason
   d3d_device3_register_material_handle is forward-declared for
   d3d_material3_GetHandle's sake -- d3d_texture2_GetHandle below is
   defined long before d3d_device3_t itself. */
static uint32_t d3d_device3_register_texture_handle(void *device, d3d_texture2_t *tex, uint32_t *out_handle);

static uint32_t WINAPI d3d_texture2_QueryInterface(void *self, guid_t *riid, void **out)
{
    d3d_texture2_t *t = (d3d_texture2_t *)self;
    if (out == NULL) return DDERR_INVALIDPARAMS;
    if (riid == NULL || guid_equal(riid, &IID_IDirect3DTexture2)) {
        atomic_fetch_add(&t->refcount, 1u);
        *out = self;
        return DD_OK;
    }
    /* Real DX6-era code commonly QueryInterfaces a texture BACK to its
       underlying DirectDrawSurface (e.g. to Lock its pixels for
       postprocessing after only keeping the IDirect3DTexture2* around,
       not the original surface pointer) -- root-caused live in
       [[project_swep1rcr_port_scoping]]'s crash #3 investigation: real
       GDB disassembly (not the unreliable Ghidra decompile) showed a
       3-arg QueryInterface(this=texture2, riid=IID_IDirectDrawSurface4,
       &out) call whose result was dereferenced WITHOUT checking the
       returned HRESULT -- confirmed independently by
       /home/andrew/sources/openswe1r's own working IDirect3DTexture2
       QueryInterface handler, which does exactly this for the SAME real
       GUID. Our old DDERR_NOINTERFACE/NULL response here was a
       guaranteed NULL-deref moments later. Delegate straight to the
       surface's own QueryInterface -- it already handles the full
       v1/2/3/4/7 DirectDrawSurface version chain via the same shared-
       object vtable-swap pattern (and already has the trace logging
       this call site was invisibly missing). */
    return surface7_QueryInterface(t->surface, riid, out);
}
static uint32_t WINAPI d3d_texture2_AddRef(void *self)
{ return atomic_fetch_add(&((d3d_texture2_t *)self)->refcount, 1u) + 1u; }
static uint32_t WINAPI d3d_texture2_Release(void *self)
{
    d3d_texture2_t *t = (d3d_texture2_t *)self;
    if (atomic_load(&t->refcount) == 0) {
        fprintf(stderr, "[ddraw] BUG: Release called on d3d_texture2 %p with refcount already 0 (over-release)\n", self);
        return 0;
    }
    uint32_t rc = atomic_fetch_sub(&t->refcount, 1u) - 1u;
    if (rc == 0) {
        surface7_Release(t->surface);
        free(t);
    }
    return rc;
}
/* Real DX6 SetTexture(stage, IDirect3DTexture2*) takes the object pointer
   directly, not a handle, so nothing in THIS shim's own API surface ever
   looks a texture up BY this handle -- but the handle VALUE ITSELF still
   has to look like a real one (a small, device-scoped opaque token,
   matching real D3D IM handle semantics -- see
   d3d_device3_register_material_handle's identical reasoning for
   materials) in case the game's own code treats it that way (bit-packs
   it with flags, range-checks it, etc.) -- a raw heap pointer would
   break any of that silently. */
static uint32_t WINAPI d3d_texture2_GetHandle(void *self, void *device, uint32_t *out_handle)
{
    if (out_handle == NULL) return DDERR_INVALIDPARAMS;
    if (device == NULL) return DDERR_INVALIDPARAMS;
    uint32_t hres = d3d_device3_register_texture_handle(device, (d3d_texture2_t *)self, out_handle);
    if (hres != DD_OK) return hres;
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] Texture2::GetHandle: texture2=%p surface=%p -> handle=0x%08x\n",
                self, (void *)((d3d_texture2_t *)self)->surface, *out_handle);
    }
    return DD_OK;
}
static uint32_t WINAPI d3d_texture2_PaletteChanged(void *self, uint32_t start, uint32_t count)
{ (void)self; (void)start; (void)count; return DD_OK; }
/* Real semantics: copies source_texture's image into self, converting
   between pixel formats/sizes as needed -- this is NOT always a no-op
   over an already-populated destination. SWEP1RCR's own UI/HUD icon
   atlas confirmed this live: CreateSurface(TEXTURE|SYSTEMMEMORY) makes a
   brand new, never-Blt'd/Locked destination surface (->buffer freshly
   calloc'd, all zero), then Load() is called immediately as the ONLY
   thing that ever puts real pixel data into it -- the old no-op left
   every one of these textures fully transparent black (zero RGBA), so
   the icons rendered but were invisible against the colorfilled
   background, looking like a solid black screen. Mirrors surface7_Blt's
   own CPU pixel-copy loop (unpack_rgba/pack_rgba through each surface's
   own real pixel format), with nearest-neighbor scaling if src/dst
   dimensions differ (real IDirect3DTexture2::Load allows mismatched
   sizes, unlike same-object Blt). */
static uint32_t WINAPI d3d_texture2_Load(void *self, void *source_texture)
{
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] Texture2::Load: dst_texture2=%p src=%p\n", self, source_texture);
    }
    d3d_texture2_t *dst_tex = (d3d_texture2_t *)self;
    d3d_texture2_t *src_tex = (d3d_texture2_t *)source_texture;
    if (dst_tex == NULL || src_tex == NULL) return DDERR_INVALIDPARAMS;
    surface7_t *d = dst_tex->surface, *s = src_tex->surface;
    if (d == NULL || s == NULL || d->width == 0 || d->height == 0 || s->width == 0 || s->height == 0) {
        return DDERR_INVALIDPARAMS;
    }

    surface7_sync_from_gpu(s);
    surface7_sync_from_gpu(d);
    pixfmt_decoded_t s_pd, d_pd;
    pixfmt_decode(&s->pixfmt, &s_pd);
    pixfmt_decode(&d->pixfmt, &d_pd);
    for (uint32_t y = 0; y < d->height; y++) {
        uint32_t sy = y * s->height / d->height;
        for (uint32_t x = 0; x < d->width; x++) {
            uint32_t sx = x * s->width / d->width;
            uint8_t rgba[4];
            unpack_rgba_fast(surface7_read_raw(s, sx, sy), &s_pd, &s->pixfmt, s->palette, rgba);
            surface7_write_raw(d, x, y, pack_rgba_fast(rgba, &d_pd, &d->pixfmt, d->palette));
        }
    }
    d->sync_state = SURFACE_CPU_AHEAD;
    return DD_OK;
}

static const d3d_texture2_vtbl_t g_surface_texture2_vtbl = {
    .QueryInterface = (comfn2_t)d3d_texture2_QueryInterface,
    .AddRef = d3d_texture2_AddRef,
    .Release = d3d_texture2_Release,
    .GetHandle = (comfn2_t)d3d_texture2_GetHandle,
    .PaletteChanged = d3d_texture2_PaletteChanged,
    .Load = (comfn1_t)d3d_texture2_Load,
};

static void *d3d_texture2_create(surface7_t *surface)
{
    d3d_texture2_t *t = calloc(1, sizeof(*t));
    t->lpVtbl = &g_surface_texture2_vtbl;
    atomic_init(&t->refcount, 1u);
    surface7_AddRef(surface);
    t->surface = surface;
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] Texture2::create: texture2=%p wraps surface=%p(caps=0x%08x,%ux%u)\n",
                (void *)t, (void *)surface, surface->caps, surface->width, surface->height);
    }
    return t;
}

static void ddgammaramp_fill_identity(ddgammaramp_t *r)
{
    /* Real default gamma ramp before any SetGammaRamp call -- linear
       0..65535 across each 256-entry channel (i*257 exactly spans that
       range, since 255*257==65535), matching what a freshly queried
       real gamma control interface reports on real hardware. */
    for (int i = 0; i < 256; i++) {
        uint16_t v = (uint16_t)(i * 257);
        r->red[i] = v;
        r->green[i] = v;
        r->blue[i] = v;
    }
}

static uint32_t WINAPI gamma_control_QueryInterface(void *self, guid_t *riid, void **out)
{
    gamma_control_t *g = (gamma_control_t *)self;
    if (out == NULL) return DDERR_INVALIDPARAMS;
    if (riid == NULL || guid_equal(riid, &IID_IDirectDrawGammaControl)) {
        atomic_fetch_add(&g->refcount, 1u);
        *out = self;
        return DD_OK;
    }
    /* Same "QueryInterface back to the underlying surface" real pattern
       already confirmed live for IDirect3DTexture2 (see
       d3d_texture2_QueryInterface's own comment) -- delegate rather than
       guess whether this specific tear-off also needs it. */
    return surface7_QueryInterface(g->surface, riid, out);
}
static uint32_t WINAPI gamma_control_AddRef(void *self)
{ return atomic_fetch_add(&((gamma_control_t *)self)->refcount, 1u) + 1u; }
static uint32_t WINAPI gamma_control_Release(void *self)
{
    gamma_control_t *g = (gamma_control_t *)self;
    if (atomic_load(&g->refcount) == 0) {
        fprintf(stderr, "[ddraw] BUG: Release called on gamma_control %p with refcount already 0 "
                        "(over-release)\n", self);
        return 0;
    }
    uint32_t rc = atomic_fetch_sub(&g->refcount, 1u) - 1u;
    if (rc == 0) {
        surface7_Release(g->surface);
        gfx_native_freed(g);
        free(g);
    }
    return rc;
}
static uint32_t WINAPI gamma_control_GetGammaRamp(void *self, uint32_t dwFlags, void *lpRampData)
{
    (void)dwFlags;
    gamma_control_t *g = (gamma_control_t *)self;
    if (lpRampData == NULL) return DDERR_INVALIDPARAMS;
    memcpy(lpRampData, &g->ramp, sizeof(g->ramp));
    return DD_OK;
}
static uint32_t WINAPI gamma_control_SetGammaRamp(void *self, uint32_t dwFlags, void *lpRampData)
{
    (void)dwFlags;
    gamma_control_t *g = (gamma_control_t *)self;
    if (lpRampData == NULL) return DDERR_INVALIDPARAMS;
    memcpy(&g->ramp, lpRampData, sizeof(g->ramp));
    return DD_OK;
}

static const gamma_control_vtbl_t g_gamma_control_vtbl = {
    .QueryInterface = (comfn2_t)gamma_control_QueryInterface,
    .AddRef = gamma_control_AddRef,
    .Release = gamma_control_Release,
    .GetGammaRamp = (comfn2_t)gamma_control_GetGammaRamp,
    .SetGammaRamp = (comfn2_t)gamma_control_SetGammaRamp,
};

static void *gamma_control_create(surface7_t *surface)
{
    gamma_control_t *g = calloc(1, sizeof(*g));
    g->lpVtbl = &g_gamma_control_vtbl;
    atomic_init(&g->refcount, 1u);
    surface7_AddRef(surface);
    g->surface = surface;
    ddgammaramp_fill_identity(&g->ramp);
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] GammaControl::create: gamma=%p wraps surface=%p\n",
                (void *)g, (void *)surface);
    }
    return g;
}

static uint32_t surface7_bpp_from_pixfmt(const ddpixelformat_t *fmt, uint32_t flags)
{
    if ((flags & DDSD_PIXELFORMAT) && (fmt->dwFlags & (DDPF_RGB | DDPF_ALPHAPIXELS)) && fmt->dwRGBBitCount != 0) {
        return (fmt->dwRGBBitCount + 7) / 8;
    }
    if ((flags & DDSD_PIXELFORMAT) && (fmt->dwFlags & DDPF_PALETTEINDEXED8)) {
        return 1;
    }
    return 4; /* default: 32bpp, matches GetDisplayMode's reported mode */
}

static surface7_t *surface7_create(uint32_t width, uint32_t height, uint32_t caps, const ddpixelformat_t *pixfmt, uint32_t desc_flags)
{
    surface7_t *s = calloc(1, sizeof(*s));
    s->lpVtbl = &g_surface7_vtbl;
    s->refcount = 1;
    s->width = width;
    s->height = height;
    s->caps = caps;
    if (pixfmt != NULL && (desc_flags & DDSD_PIXELFORMAT)) {
        s->pixfmt = *pixfmt;
    } else {
        s->pixfmt.dwSize = sizeof(ddpixelformat_t);
        s->pixfmt.dwFlags = DDPF_RGB;
        s->pixfmt.dwRGBBitCount = 32;
        s->pixfmt.dwRBitMask = 0x00FF0000u;
        s->pixfmt.dwGBitMask = 0x0000FF00u;
        s->pixfmt.dwBBitMask = 0x000000FFu;
    }
    s->bpp = surface7_bpp_from_pixfmt(&s->pixfmt, DDSD_PIXELFORMAT);
    s->pitch = width * s->bpp;
    s->buffer = gfx_guest_alloc((size_t)s->pitch * (height != 0 ? height : 1));
    return s;
}

/* ===================== D3D7 GL interop =====================
   Implements d3d7.h's d3d7_surface_interop_t -- see that header for why
   this is a registered callback table rather than a direct call (link
   cycle: shim_ddraw already links shim_d3d7). Registered once below in
   stub_DirectDrawCreateEx. */

/* Shared by both interop entry points below: pushes this surface's
   current CPU pixel content into its GL texture (creating/resizing it
   first if needed) and marks the two sides synced. Uses the same
   RGBA-staging buffer as the present path (surface7_convert_to_rgba/
   ensure_present_scratch) -- neither use overlaps within one call. */
static void surface7_upload_to_gl(surface7_t *s)
{
    uint8_t *scratch = ensure_present_scratch((size_t)s->width * s->height * 4);
    if (scratch == NULL) return;
    surface7_convert_to_rgba(s, scratch);
    /* TEMP DIAGNOSTIC (opt-in via LONGROAD2_DDRAW_TRACE): investigating a
       real, live black-screen report -- real draws/textures/state changes
       all genuinely happen (confirmed via other traces) yet the final
       presented frame reads back as exactly 0% nonblack (confirmed via a
       real glReadPixels scan). Narrowing down whether real game textures
       upload with real (opaque) alpha or come through zeroed -- combined
       with ALPHATESTENABLE+ALPHAFUNC=GREATER, an all-zero alpha channel
       would silently discard every single fragment, leaving the screen
       showing nothing but whatever Clear() last painted. Remove once
       resolved. */
    if (ddraw_trace_enabled()) {
        size_t total = (size_t)s->width * s->height;
        size_t nonzero_alpha = 0;
        for (size_t i = 0; i < total; i++) if (scratch[i * 4 + 3] != 0) nonzero_alpha++;
        fprintf(stderr, "[ddraw-trace] surface7_upload_to_gl: surface=%p %ux%u pixfmt.dwFlags=0x%08x "
                        "amask=0x%08x nonzero_alpha=%zu/%zu (%.1f%%)\n",
                (void *)s, s->width, s->height, s->pixfmt.dwFlags, s->pixfmt.dwRGBAlphaBitMask,
                nonzero_alpha, total, total ? 100.0 * (double)nonzero_alpha / (double)total : 0.0);
    }
    gl_target_upload_rgba(&s->gl, scratch, (int)s->width, (int)s->height);
    s->sync_state = SURFACE_SYNCED;
}

static unsigned ddraw_gl_bind_render_target(void *surface_, int *out_w, int *out_h)
{
    surface7_t *s = (surface7_t *)surface_;
    if (s == NULL || s->width == 0 || s->height == 0 || !render_is_initialized()) return 0;

    gl_target_ensure(&s->gl, (int)s->width, (int)s->height, 1);
    if (s->gl.fbo == 0) return 0;

    /* Preserve any pixels already drawn onto this surface via plain
       DirectDraw Blt (or an earlier D3D7 draw) before D3D7 starts
       rendering into it -- a game that Blts a static background and
       then only partially overdraws it with D3D7 (or that never Clears
       first) should still see that content, not a blank GPU texture. */
    if (s->sync_state == SURFACE_CPU_AHEAD) surface7_upload_to_gl(s);

    gl_target_bind(&s->gl);
    *out_w = (int)s->width;
    *out_h = (int)s->height;
    return 1;
}

static void ddraw_gl_mark_gpu_dirty(void *surface_)
{
    surface7_t *s = (surface7_t *)surface_;
    if (s != NULL) s->sync_state = SURFACE_GPU_AHEAD;
}

/* d3d7.c's g_surface_interop functions receive whatever a game passed to
   SetTexture as an opaque void* -- D3D7's own SetTexture takes a raw
   IDirectDrawSurface7* directly, but D3D3-era SetTexture (see
   [[project_swep1rcr_port_scoping]]) takes an IDirect3DTexture2*, a real,
   independent tear-off object (d3d_texture2_t), not a raw surface7_t*.
   Unwraps transparently by checking the object's own vtable pointer --
   d3d_texture2_t and surface7_t are never ambiguous this way since each
   one's lpVtbl is always one specific static const address. Skipping
   this would read surface7_t's field layout (width/height/gl/...) out of
   a d3d_texture2_t allocation that's nowhere near big enough to hold
   them -- a real out-of-bounds read/write, not just wrong output. */
static surface7_t *unwrap_texture2(void *obj)
{
    if (obj == NULL) return NULL;
    if (*(const void *const *)obj == (const void *)&g_surface_texture2_vtbl) {
        return ((d3d_texture2_t *)obj)->surface;
    }
    return (surface7_t *)obj;
}

static unsigned ddraw_gl_get_texture(void *surface_, int *out_w, int *out_h)
{
    surface7_t *s = (surface7_t *)unwrap_texture2(surface_);
    if (s == NULL || s->width == 0 || s->height == 0 || !render_is_initialized()) return 0;

    /* This same surface could have been a D3D7 render target very
       recently (or could be right now, for an unusual render-to-self
       case) -- but that's exactly the case that needs NO extra work
       here: a real render pass already left the correct content sitting
       in s->gl itself, which is precisely what gets returned below.
       Deliberately does NOT call surface7_sync_from_gpu first (used to,
       until this turn): pulling s->gl's content into s->buffer here
       would be pure waste, since this function returns a GL texture
       handle for use as a sampler, never reads s->buffer, and
       surface7_upload_to_gl right below only fires when s->sync_state
       is ALREADY SURFACE_CPU_AHEAD -- which a sync can't affect, since
       it only ever moves sync_state from SURFACE_GPU_AHEAD to
       SURFACE_SYNCED, never to SURFACE_CPU_AHEAD. So the removed call was a
       real glReadPixels for a value nothing here or in either caller
       (d3d7.c's bind_texture_stages, surface7_Blt's GPU-blt path) ever
       consumed. The vertex shader's v_texcoord0 flip (k_vs_src) still
       correctly handles the render-target-reused-as-sampler case with no
       special-casing needed: a real D3D7 render pass places content the
       game considers "top" (NDC y=+1) at this texture's highest GL row
       index, for the exact same reason gl_target_read_rgba/
       gl_target_upload_rgba need their own row-flip elsewhere (GL's
       bottom-up row addressing) -- so "top" ends up at GL texture
       coordinate T~=1 whether it got there via a real raster pass or via
       gl_target_upload_rgba's explicit flip below. One flip (T = 1 -
       game's V) compensates for both, consistently. */

    /* Native RGB565 fast path: skip the CPU unpack_rgba conversion
       entirely for a surface whose pixel format is bit-for-bit
       GL_UNSIGNED_SHORT_5_6_5 already -- real game textures (road/
       object surfaces) are commonly 16-bit. gl_target_ensure_native565
       itself refuses (returns 0) for any target that's ever been a
       render target (s->gl.fbo != 0), so this can never conflict with
       the RTT-sampler case handled by the general path below. */
    if (s->gl.fbo == 0 && pixfmt_is_native_rgb565(&s->pixfmt) &&
        gl_target_ensure_native565(&s->gl, (int)s->width, (int)s->height)) {
        /* TEMP DIAGNOSTIC (opt-in via LONGROAD2_DDRAW_TRACE): investigating
           a real, live "textures render as a solid white/blank rectangle
           instead of their real image" report -- this is the OTHER real
           texture-upload path (surface7_upload_to_gl's own similar
           diagnostic never fires for a surface that qualifies for this
           native565 fast path instead). Real, live-relevant history: this
           exact path had a previously-fixed row-warping bug that was never
           re-confirmed live (see [[project_native_rgb565_texture_upload]]).
           Scanning the RAW source buffer (before upload, not the GL
           texture after) checks whether the SOURCE data itself is real,
           varied image content or already blank/uniform at the point this
           shim reads it. Remove once resolved. */
        if (ddraw_trace_enabled() && s->sync_state == SURFACE_CPU_AHEAD && s->buffer != NULL) {
            size_t total = (size_t)s->pitch * s->height;
            uint16_t first = total >= 2 ? (uint16_t)(s->buffer[0] | (s->buffer[1] << 8)) : 0;
            int all_same = 1;
            for (size_t i = 0; i + 1 < total; i += 2) {
                uint16_t px = (uint16_t)(s->buffer[i] | (s->buffer[i + 1] << 8));
                if (px != first) { all_same = 0; break; }
            }
            fprintf(stderr, "[ddraw-trace] native565 upload: surface=%p %ux%u pitch=%u first_pixel=0x%04x "
                            "all_pixels_identical=%d buffer_is_external=%d\n",
                    (void *)s, s->width, s->height, s->pitch, first, all_same, s->buffer_is_external);
        }
        if (s->sync_state == SURFACE_CPU_AHEAD) {
            gl_target_upload_native565(&s->gl, s->buffer, (int)s->width, (int)s->height);
            s->sync_state = SURFACE_SYNCED;
            g_native565_upload_count++;
        }
        *out_w = (int)s->width;
        *out_h = (int)s->height;
        return s->gl.tex;
    }

    gl_target_ensure(&s->gl, (int)s->width, (int)s->height, 0);
    if (s->gl.tex == 0) return 0;
    if (s->sync_state == SURFACE_CPU_AHEAD) surface7_upload_to_gl(s);

    *out_w = (int)s->width;
    *out_h = (int)s->height;
    return s->gl.tex;
}

/* D3DRENDERSTATE_COLORKEYENABLE support: reuses this surface's own
   DDCKEY_SRCBLT (already real for plain Blt/BltFast, see surface7_Blt/
   BltFast's own use_ck checks) so 3D texturing honors the SAME key a
   game already set for 2D blits, rather than needing its own separate
   D3D-side color-key API (D3D7 doesn't have one -- this IS the real
   mechanism). unpack_rgba is the exact same conversion surface7_
   convert_to_rgba (and thus this surface's own GL texture upload) uses
   per-pixel, so the returned RGBA bytes are directly comparable against
   a texel actually sampled from that texture in the shader. */
static int ddraw_gl_get_color_key(void *surface_, uint8_t out_low[4], uint8_t out_high[4])
{
    surface7_t *s = (surface7_t *)unwrap_texture2(surface_);
    if (s == NULL || !s->has_ck_srcblt) return 0;
    unpack_rgba(s->ck_srcblt.dwColorSpaceLowValue, &s->pixfmt, s->palette, out_low);
    unpack_rgba(s->ck_srcblt.dwColorSpaceHighValue, &s->pixfmt, s->palette, out_high);
    return 1;
}

static const d3d7_surface_interop_t g_d3d7_interop = {
    .bind_render_target = ddraw_gl_bind_render_target,
    .mark_gpu_dirty = ddraw_gl_mark_gpu_dirty,
    .get_texture = ddraw_gl_get_texture,
    .get_color_key = ddraw_gl_get_color_key,
};

/* ===================== IDirectDraw7 ===================== */

typedef struct {
    comfn2_t QueryInterface;
    comfn0_t AddRef;
    comfn0_t Release;
    comfn0_t Compact;
    comfn3_t CreateClipper;
    comfn4_t CreatePalette;
    comfn3_t CreateSurface;
    comfn2_t DuplicateSurface;
    comfn4_t EnumDisplayModes;
    comfn4_t EnumSurfaces;
    comfn0_t FlipToGDISurface;
    comfn2_t GetCaps;
    comfn1_t GetDisplayMode;
    comfn2_t GetFourCCCodes;
    comfn1_t GetGDISurface;
    comfn1_t GetMonitorFrequency;
    comfn1_t GetScanLine;
    comfn1_t GetVerticalBlankStatus;
    comfn1_t Initialize;
    comfn0_t RestoreDisplayMode;
    comfn2_t SetCooperativeLevel;
    comfn5_t SetDisplayMode;
    comfn2_t WaitForVerticalBlank;
    comfn3_t GetAvailableVidMem;
    comfn2_t GetSurfaceFromDC;
    comfn0_t RestoreAllSurfaces;
    comfn0_t TestCooperativeLevel;
    comfn2_t GetDeviceIdentifier;
    comfn3_t StartModeTest;
    comfn2_t EvaluateMode;
} ddraw7_vtbl_t;

/* Real DirectDraw versions (v1/v2/v4/v7) obtained via QueryInterface are
   genuinely independent COM tear-off objects, each with its OWN
   AddRef/Release-managed lifetime -- NOT the same allocation with a
   mutated vtable pointer and a refcount shared across every version.
   This isn't COM pedantry: SWEP1RCR-era init code routinely does
   QueryInterface(IDirectDraw4), keeps using the ORIGINAL v1 pointer for
   more calls afterward, and Releases each handle independently at
   different times -- releasing one must not free or otherwise disturb
   the others. See [[project_swep1rcr_port_scoping]] and
   [[feedback_diagnose_exact_mechanism_not_guard]].

   The real mutable state (hwnd/coop_flags/mode_w/h/bpp) genuinely IS
   shared -- SetDisplayMode called through any one view must be visible
   through every other view of "the same object" -- so it lives in a
   separate ddraw_core_t, reference-counted by however many tear-offs
   currently point at it, and only freed once the last one is released. */
typedef struct {
    atomic_uint refcount; /* how many tear-offs (any version) reference this core */
    uint32_t hwnd;
    uint32_t coop_flags;
    uint32_t mode_w, mode_h; /* 0 = unset -- see ddraw7_SetDisplayMode */
    uint32_t mode_bpp;       /* 0 = unset -- see ddraw7_SetDisplayMode */
} ddraw_core_t;

typedef struct {
    const ddraw7_vtbl_t *lpVtbl;
    atomic_uint refcount; /* THIS tear-off's own count -- independent of every other view's */
    ddraw_core_t *core;
} ddraw7_t;

/* Forward declarations: ddraw7_QueryInterface below needs to reference
   these (DirectDraw 1/2/4 legacy support, see
   [[project_swep1rcr_port_scoping]]), but their real (initialized)
   definitions live further down, alongside g_ddraw7_vtbl -- same
   forward-reference need as g_surface7_vtbl/g_surface_legacy_vtbl above. */
static const ddraw7_vtbl_t g_ddraw7_vtbl;
static const ddraw7_vtbl_t g_ddraw1_vtbl;
static const ddraw7_vtbl_t g_ddraw2_vtbl;
static const ddraw7_vtbl_t g_ddraw4_vtbl;

/* Allocates a brand new DirectDraw tear-off bound to `core`, with its
   own independent refcount starting at 1 -- see the comment on
   ddraw7_t/ddraw_core_t above for why this exists instead of mutating
   and re-returning `self`. */
static ddraw7_t *ddraw_tearoff_create(ddraw_core_t *core, const ddraw7_vtbl_t *vtbl)
{
    ddraw7_t *view = malloc(sizeof(*view));
    view->lpVtbl = vtbl;
    atomic_init(&view->refcount, 1u);
    atomic_fetch_add(&core->refcount, 1u);
    view->core = core;
    return view;
}

/* IDirect3D/2/3 (DX6-era legacy Direct3D main objects, see
   [[project_swep1rcr_port_scoping]]) -- factories defined in the legacy
   Direct3D section further down this file, declared here so
   ddraw7_QueryInterface above can reach them (same forward-reference
   need as the vtable objects above). Each call creates a fully
   independent object (own vtable + own refcount, no link back to the
   ddraw7_t/ddraw_core_t that reached it) -- IDirect3D/2/3 have no real
   mutable state of their own to share between versions, so there's no
   core to tear off from, unlike DirectDraw above. */
static void *d3d1_create(void);
static void *d3d2_create(void);
static void *d3d3_create(void);

/* Real DirectDraw: a primary surface created without an explicit
   DDSD_PIXELFORMAT implicitly takes on the CURRENT display mode's pixel
   format -- games rely on this constantly, since asking for "the primary
   surface" is specifically asking for whatever mode you just set. Fills
   in real RGB565 for 16bpp (the standard/only 16-bit format any real
   DirectDraw driver actually offers -- RGB555 was already obsolete by
   this game's era) and paletted for 8bpp, matching what
   ddraw7_EnumDisplayModes above advertises. */
static void fill_pixfmt_for_bpp(ddpixelformat_t *pf, uint32_t bpp)
{
    memset(pf, 0, sizeof(*pf));
    pf->dwSize = sizeof(*pf);
    switch (bpp) {
        case 8:
            pf->dwFlags = DDPF_PALETTEINDEXED8;
            pf->dwRGBBitCount = 8;
            break;
        case 16:
            pf->dwFlags = DDPF_RGB;
            pf->dwRGBBitCount = 16;
            pf->dwRBitMask = 0xF800u;
            pf->dwGBitMask = 0x07E0u;
            pf->dwBBitMask = 0x001Fu;
            break;
        case 24:
            pf->dwFlags = DDPF_RGB;
            pf->dwRGBBitCount = 24;
            pf->dwRBitMask = 0x00FF0000u;
            pf->dwGBitMask = 0x0000FF00u;
            pf->dwBBitMask = 0x000000FFu;
            break;
        default: /* 32, or unknown -- same default surface7_create already used */
            pf->dwFlags = DDPF_RGB;
            pf->dwRGBBitCount = 32;
            pf->dwRBitMask = 0x00FF0000u;
            pf->dwGBitMask = 0x0000FF00u;
            pf->dwBBitMask = 0x000000FFu;
            break;
    }
}

static uint32_t WINAPI ddraw7_QueryInterface(void *self, guid_t *riid, void **out)
{
    ddraw7_t *dd = (ddraw7_t *)self;
    if (out == NULL) return DDERR_INVALIDPARAMS;
    if (guid_equal(riid, &IID_IDirectDraw7)) {
        *out = ddraw_tearoff_create(dd->core, &g_ddraw7_vtbl);
        return DD_OK;
    }
    if (guid_equal(riid, &IID_IDirect3D7)) {
        *out = d3d7_create();
        return *out != NULL ? DD_OK : DDERR_OUTOFMEMORY;
    }
    /* DX6-era legacy DirectDraw chain (SWEP1RCR, see
       [[project_swep1rcr_port_scoping]]) -- real, shared underlying
       state (hwnd/coop_flags/mode_w/h/bpp) via the common ddraw_core_t,
       but each requested version gets its OWN independent tear-off
       object/refcount (see ddraw_tearoff_create/ddraw7_t above), not a
       vtable swap on the same allocation. */
    if (guid_equal(riid, &IID_IDirectDraw4)) {
        *out = ddraw_tearoff_create(dd->core, &g_ddraw4_vtbl);
        return DD_OK;
    }
    if (guid_equal(riid, &IID_IDirectDraw2)) {
        *out = ddraw_tearoff_create(dd->core, &g_ddraw2_vtbl);
        return DD_OK;
    }
    if (guid_equal(riid, &IID_IDirectDraw)) {
        *out = ddraw_tearoff_create(dd->core, &g_ddraw1_vtbl);
        return DD_OK;
    }
    if (guid_equal(riid, &IID_IDirect3D3)) {
        *out = d3d3_create();
        return *out != NULL ? DD_OK : DDERR_OUTOFMEMORY;
    }
    if (guid_equal(riid, &IID_IDirect3D2)) {
        *out = d3d2_create();
        return *out != NULL ? DD_OK : DDERR_OUTOFMEMORY;
    }
    if (guid_equal(riid, &IID_IDirect3D)) {
        *out = d3d1_create();
        return *out != NULL ? DD_OK : DDERR_OUTOFMEMORY;
    }
    *out = NULL;
    return DDERR_NOINTERFACE;
}
static uint32_t WINAPI ddraw7_AddRef(void *self)
{
    return atomic_fetch_add(&((ddraw7_t *)self)->refcount, 1u) + 1u;
}
/* Releases THIS tear-off only -- other views obtained via QueryInterface
   (and the core they all share) are untouched unless this was the last
   tear-off standing. See ddraw7_t/ddraw_core_t's own comment above. */
static uint32_t WINAPI ddraw7_Release(void *self)
{
    ddraw7_t *dd = (ddraw7_t *)self;
    if (atomic_load(&dd->refcount) == 0) {
        fprintf(stderr, "[ddraw] BUG: Release called on ddraw7 tear-off %p with refcount already 0 (over-release)\n", self);
        return 0;
    }
    uint32_t rc = atomic_fetch_sub(&dd->refcount, 1u) - 1u;
    if (rc == 0) {
        ddraw_core_t *core = dd->core;
        gfx_native_freed(dd);
        free(dd);
        if (atomic_fetch_sub(&core->refcount, 1u) - 1u == 0) {
            free(core);
        }
    }
    return rc;
}

static uint32_t WINAPI ddraw7_SetCooperativeLevel(void *self, uint32_t hwnd, uint32_t flags)
{
    ddraw7_t *dd = (ddraw7_t *)self;
    dd->core->hwnd = hwnd;
    dd->core->coop_flags = flags;
    return DD_OK;
}
static uint32_t WINAPI ddraw7_SetDisplayMode(void *self, uint32_t w, uint32_t h, uint32_t bpp, uint32_t refresh, uint32_t flags)
{
    /* Real exclusive-mode SetDisplayMode changes the physical resolution;
       we don't have a "physical resolution" separate from our one SDL
       window, so resizing that window to match is the closest real
       equivalent -- and it's what makes CreateSurface's primary-surface
       sizing (below, w==0/h==0 falls back to this) actually match what
       the game asked for instead of the host desktop's real size.

       Real, live-traced root cause of the black-screen investigation:
       dx7z.dll (Porsche.exe's D3D7 middleware) calls this TWICE during its
       own window-init sequence -- once with a real, correct 640x480x16
       (mode index 1, backed by dx7z's own hardcoded mode table), then AGAIN
       moments later with a genuine (0,0,0) request (mode index 0, which
       dx7z's own mode table never populates -- confirmed live, its
       hardcoded population loop starts at table index 1, never writes
       index 0 at all). This second call is not a real "resize to 0x0"
       request; it's dx7z treating index 0 as a "keep the current mode"
       sentinel with nothing behind it to look up. This shim used to accept
       every call unconditionally, so the second (0,0,0) call clobbered the
       first call's good 640x480 back to zero, and every surface created
       afterward inherited that degenerate size -- live-traced all the way
       through to the game's own UI vertex-transform math collapsing every
       on-screen element to a single point. A 0-sized request now leaves
       the previously-set mode untouched, matching real DirectDraw's own
       "0 means no change" semantics for this call. */
    (void)refresh; (void)flags;
    ddraw7_t *dd = (ddraw7_t *)self;
    if (w != 0 && h != 0) {
        dd->core->mode_w = w;
        dd->core->mode_h = h;
        dd->core->mode_bpp = bpp;
        render_resize((int)w, (int)h);
    }
    return DD_OK;
}
/* IDirectDraw::SetDisplayMode (real v1 interface, SWEP1RCR/DX6-era, see
   [[project_swep1rcr_port_scoping]]) takes only 3 args -- dwRefreshRate/
   dwFlags were added starting IDirectDraw2. This is a genuine stdcall
   ABI difference, not just a struct-content one: reusing the 5-arg
   ddraw7_SetDisplayMode directly for v1's vtable slot would make this
   shim's function pop 8 bytes more off the stack than the real caller
   (compiled against the 3-arg v1 signature) actually pushed, corrupting
   the game's own stack on return. IDirectDraw2 onward already matches
   ddraw7_SetDisplayMode's 5-arg shape exactly, so only v1 needs this
   wrapper. */
static uint32_t WINAPI ddraw1_SetDisplayMode(void *self, uint32_t w, uint32_t h, uint32_t bpp)
{
    return ddraw7_SetDisplayMode(self, w, h, bpp, 0, 0);
}
static uint32_t WINAPI ddraw7_GetDisplayMode(void *self, ddsurfacedesc2_t *out_desc)
{
    if (out_desc == NULL) return DDERR_INVALIDPARAMS;
    ddraw7_t *dd = (ddraw7_t *)self;
    int w, h;
    if (dd->core->mode_w != 0 && dd->core->mode_h != 0) {
        w = (int)dd->core->mode_w;
        h = (int)dd->core->mode_h;
    } else {
        /* Real, live-traced root cause of a black-screen report: this used
           to fall back to platform_get_screen_size() -- the REAL host
           desktop resolution -- even when LONGROAD2_TARGET_RESOLUTION
           pins this process to a single, different size everywhere else.
           A game that calls GetDisplayMode before ever calling
           SetDisplayMode (confirmed real for Porsche.exe's own dx7z.dll
           hardware-detection sequence) would see the host's real size
           here and create its primary surface at THAT size, while
           anything else in this same process (the actual window,
           render_get_size, CreateSurface's own w==0/h==0 fallback
           elsewhere) all agree on the pinned target -- a real size
           mismatch between a primary surface and its own backbuffer,
           live-traced all the way to Flip()'s buffer-swap logic making
           the device's bound render target's reported dimensions
           alternate between the two sizes, with a stale/mismatched
           viewport on the frames where it didn't match, and ultimately
           every real 3D draw landing on a black screen. Preferring the
           pinned target here (when set) keeps every reported "current
           mode" in this process self-consistent, matching
           render_get_size's own "one resolution, always agreeing with
           itself" design. */
        render_get_target_resolution(&w, &h);
        if (w == 0 || h == 0) {
            w = 1027; h = 768;
            platform_get_screen_size(&w, &h);
        }
    }
    uint32_t sz = out_desc->dwSize;
    /* Bounded by the CALLER's own declared dwSize, not this shim's own
       (potentially larger) ddsurfacedesc2_t -- needed now that this same
       function also serves the legacy IDirectDraw/2::GetDisplayMode
       vtable slots (see the DirectDraw 1/2/4 legacy block further down),
       whose real callers pass a genuinely smaller DDSURFACEDESC (no
       DDSCAPS2/dwTextureStage tail). An unconditional sizeof(*out_desc)
       here would write past the end of that real, smaller buffer. */
    memset(out_desc, 0, sz < sizeof(*out_desc) ? sz : sizeof(*out_desc));
    out_desc->dwSize = sz;
    out_desc->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_PITCH;
    out_desc->dwWidth = (uint32_t)w;
    out_desc->dwHeight = (uint32_t)h;
    fill_pixfmt_for_bpp(&out_desc->ddpfPixelFormat, dd->core->mode_bpp);
    out_desc->lPitchOrLinearSize = (uint32_t)w * (out_desc->ddpfPixelFormat.dwRGBBitCount / 8);

    return DD_OK;
}
static uint32_t WINAPI ddraw7_RestoreDisplayMode(void *self) { (void)self; return DD_OK; }

/* Real hardware/HEL DirectDraw drivers enumerate every mode they support;
   king.exe's own mode-picking callback (see docs/game_code/king.exe.c,
   the function at 0x5d4240) matches width/height exactly and checks a
   specific bit depth (8/16/24) per call -- calling back with only the
   current desktop mode meant almost every EnumDisplayModes call (676/
   session per docs/api-surface/ddraw.md -- this runs far more than
   once) found nothing to accept, which the game treats as fatal
   ("can not find video mode", confirmed via its own warn.log). Offering
   a standard spread of resolutions x bit depths, like a real driver
   would, is what actually satisfies that callback. */
static const struct { uint32_t w, h; } g_common_modes[] = {
    { 640, 480 }, { 800, 600 }, { 1024, 768 },
};

static uint32_t WINAPI ddraw7_EnumDisplayModes(void *self, uint32_t flags, ddsurfacedesc2_t *filter, void *ctx, void *cb_)
{
    (void)self; (void)flags; (void)filter;
    typedef uint32_t (WINAPI *cb_t)(ddsurfacedesc2_t *, void *);
    if (cb_ == NULL) return DD_OK;

    static const uint32_t bpps[] = { 8, 16, 32 };
    for (size_t m = 0; m < sizeof(g_common_modes) / sizeof(g_common_modes[0]); m++) {
        for (size_t b = 0; b < sizeof(bpps) / sizeof(bpps[0]); b++) {
            ddsurfacedesc2_t desc;
            memset(&desc, 0, sizeof(desc));
            desc.dwSize = sizeof(desc);
            desc.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_PITCH | DDSD_REFRESHRATE;
            desc.dwWidth = g_common_modes[m].w;
            desc.dwHeight = g_common_modes[m].h;
            desc.ddpfPixelFormat.dwSize = sizeof(ddpixelformat_t);
            if (bpps[b] == 8) {
                desc.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
                desc.ddpfPixelFormat.dwRGBBitCount = 8;
                desc.ddpfPixelFormat.dwFourCC = 0;
                desc.ddpfPixelFormat.dwRBitMask = 0x00;
                desc.ddpfPixelFormat.dwGBitMask = 0x00;
                desc.ddpfPixelFormat.dwBBitMask = 0x00;
                desc.ddpfPixelFormat.dwRGBAlphaBitMask = 0x0;
            } else if (bpps[b] == 16) {
                desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
                desc.ddpfPixelFormat.dwRGBBitCount = 16;
                desc.ddpfPixelFormat.dwFourCC = 0;
                desc.ddpfPixelFormat.dwRBitMask = 0xF800; 
                desc.ddpfPixelFormat.dwGBitMask = 0x07E0;
                desc.ddpfPixelFormat.dwBBitMask = 0x001F;
                desc.ddpfPixelFormat.dwRGBAlphaBitMask = 0x0;
            } else {
                desc.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
                desc.ddpfPixelFormat.dwRGBBitCount = 32;
                desc.ddpfPixelFormat.dwFourCC = 0;
                desc.ddpfPixelFormat.dwRBitMask = 0x00ff0000;
                desc.ddpfPixelFormat.dwGBitMask = 0x0000ff00;
                desc.ddpfPixelFormat.dwBBitMask = 0x000000ff;
                desc.ddpfPixelFormat.dwRGBAlphaBitMask = 0xff000000;
            }
            desc.lPitchOrLinearSize = (desc.ddpfPixelFormat.dwRGBBitCount / 8) * desc.dwWidth;
            desc.lPitchOrLinearSize = (desc.lPitchOrLinearSize + 3) & ~3;
            if (((cb_t)cb_)(&desc, ctx) == 0) return DD_OK; /* DDENUMRET_CANCEL */
        }
    }
    return DD_OK;
}

static uint32_t WINAPI ddraw7_GetCaps(void *self, ddcaps_dx7_t *driver_caps, ddcaps_dx7_t *hel_caps)
{
    (void)self;
    ddcaps_dx7_t *targets[2] = { driver_caps, hel_caps };
    for (int i = 0; i < 2; i++) {
        ddcaps_dx7_t *c = targets[i];
        if (c == NULL) continue;
        uint32_t sz = c->dwSize;
        memset(c, 0, sz);
        c->dwSize = sz;
        c->dwCaps = DDCAPS_BLT | DDCAPS_BLTQUEUE | DDCAPS_GDI | DDCAPS_PALETTE | DDCAPS_3D | DDCAPS_BLTDEPTHFILL;
        c->dwCaps2 = DDCAPS2_CANRENDERWINDOWED;
        c->dwVidMemTotal = 64u * 1024 * 1024;
        c->dwVidMemFree = 64u * 1024 * 1024;
    }
    return DD_OK;
}
static uint32_t WINAPI ddraw7_GetAvailableVidMem(void *self, ddscaps2_t *caps, uint32_t *total, uint32_t *free_)
{
    (void)self; (void)caps;
    if (total != NULL) *total = 64u * 1024 * 1024;
    if (free_ != NULL) *free_ = 64u * 1024 * 1024;
    return DD_OK;
}
static uint32_t WINAPI ddraw7_GetFourCCCodes(void *self, uint32_t *num_codes, uint32_t *codes)
{
    (void)self; (void)codes;
    if (num_codes != NULL) *num_codes = 0; /* pure RGB -- no FourCC/YUV formats */
    return DD_OK;
}
static uint32_t WINAPI ddraw7_GetDeviceIdentifier(void *self, dddeviceidentifier2_t *out_id, uint32_t flags)
{
    (void)self; (void)flags;
    if (out_id == NULL) return DDERR_INVALIDPARAMS;
    memset(out_id, 0, sizeof(*out_id));
    out_id->dwVendorId = 0x10d9;
    out_id->dwDeviceId = 0x5144;
    out_id->dwSubSysId = 0x0001;
    out_id->dwRevision = 0x0001;
    out_id->liDriverVersion = 0xA;
    memcpy(&out_id->guidDeviceIdentifier, &IID_D3DDEVICE_D3DUID, sizeof(guid_t));
    strncpy(out_id->szDriver, "nv4_disp.dll", sizeof(out_id->szDriver) - 1);
    strncpy(out_id->szDescription, "DirectDraw HAL", sizeof(out_id->szDescription) - 1);
    return DD_OK;
}
/* IDirectDraw4::GetDeviceIdentifier (SWEP1RCR/DX6-era, see
   [[project_swep1rcr_port_scoping]]) takes a real DDDEVICEIDENTIFIER,
   which is 4 bytes SMALLER than DDDEVICEIDENTIFIER2 (v7 tacked on a
   trailing dwWHQLLevel field) -- unlike DDSURFACEDESC's dwSize, this
   struct carries no self-describing size, so ddraw7_GetDeviceIdentifier's
   unconditional sizeof(*out_id) write can't safely serve a real v4
   caller's smaller buffer. Builds the answer in a local, full-sized
   struct and copies only the v4-sized prefix out. */
static uint32_t WINAPI ddraw4_GetDeviceIdentifier(void *self, void *out_id_v4, uint32_t flags)
{
    dddeviceidentifier2_t full;
    uint32_t hr = ddraw7_GetDeviceIdentifier(self, &full, flags);
    if (hr == DD_OK && out_id_v4 != NULL) {
        memcpy(out_id_v4, &full, sizeof(full) - sizeof(full.dwWHQLLevel));
    }
    return hr;
}
static uint32_t WINAPI ddraw7_Initialize(void *self, guid_t *guid) { (void)self; (void)guid; return DD_OK; }

static uint32_t WINAPI ddraw7_CreateSurface(void *self, ddsurfacedesc2_t *desc, surface7_t **out_surface, void *outer)
{
    (void)outer;
    if (desc == NULL || out_surface == NULL) return DDERR_INVALIDPARAMS;
    ddraw7_t *dd = (ddraw7_t *)self;

    uint32_t w = (desc->dwFlags & DDSD_WIDTH) ? desc->dwWidth : 0;
    uint32_t h = (desc->dwFlags & DDSD_HEIGHT) ? desc->dwHeight : 0;
    if (w == 0 || h == 0) {
        /* No explicit size -- almost always the primary surface, which is
           implicitly "the current mode" per real DirectDraw semantics.
           Prefer whatever SetDisplayMode last established; then the
           pinned LONGROAD2_TARGET_RESOLUTION (same fix and same real,
           live-traced reason as ddraw7_GetDisplayMode's own fallback just
           above -- this is the actual code path that was creating a
           primary surface at the host's real desktop size while
           everything else in this process agreed on the pinned target,
           the root cause behind a real black-screen report); only fall
           back to the real host desktop size if neither is set. */
        int sw, sh;
        if (dd->core->mode_w != 0 && dd->core->mode_h != 0) {
            sw = (int)dd->core->mode_w; sh = (int)dd->core->mode_h;
        } else {
            render_get_target_resolution(&sw, &sh);
            if (sw == 0 || sh == 0) {
                sw = 1024; sh = 768;
                platform_get_screen_size(&sw, &sh);
            }
        }
        if (w == 0) w = (uint32_t)sw;
        if (h == 0) h = (uint32_t)sh;
    }

    /* A real driver fails a CreateSurface request it can't possibly back
       with memory rather than silently succeeding -- confirmed necessary
       via a live SWEP1RCR trace (see [[project_swep1rcr_port_scoping]]):
       its texture manager repeatedly calls CreateSurface with
       dwWidth=dwHeight=0xFFFFFFFF (2952 times in one session, always
       immediately followed by recreating the same address at a real
       size), and this shim previously let every one of those through.
       surface7_create's `width * bpp` pitch computation wraps in 32-bit
       arithmetic for a width this large, so the resulting calloc size is
       essentially random and typically still enormous -- calloc fails,
       s->buffer stays NULL, but CreateSurface still reports DD_OK with a
       "valid" surface. Nothing crashed immediately (whatever consumed
       these bogus surfaces before never happened to Lock/write them
       directly), until a texture-color-inversion pass did exactly that:
       Lock() handed back lpSurface=NULL with a garbage pitch, and the
       game wrote pixels straight through the NULL pointer. Rejecting
       absurd dimensions up front -- 16384 is already far beyond any real
       DX6/DX7-era surface or display mode -- matches real DirectDraw
       behavior and lets the game's own CreateSurface failure handling
       (which it must have, since this exact call succeeds harmlessly
       thousands of times per session in this shim today) run instead. */
    if (w > 16384 || h > 16384) {
        if (ddraw_trace_enabled()) {
            fprintf(stderr, "[ddraw-trace] CreateSurface: rejecting insane size %ux%u (flags=0x%08x)\n",
                    w, h, desc->dwFlags);
        }
        return DDERR_OUTOFMEMORY;
    }

    uint32_t caps = (desc->dwFlags & DDSD_CAPS) ? desc->ddsCaps.dwCaps : 0;

    /* No explicit pixel format -- almost always the primary surface,
       which implicitly takes on the CURRENT display mode's format (see
       fill_pixfmt_for_bpp above). Getting this wrong is not cosmetic:
       king.exe writes pixels through its own Lock()'d pointer using a
       stride it computes itself from the bpp IT asked SetDisplayMode
       for, so creating the surface at the wrong bpp (previously always
       32-bit regardless of what the game requested) desyncs every row
       write from where this shim's blitter/present path expects it --
       confirmed via a live run requesting 8bpp paletted mode: the
       resulting image showed content compressed into a fraction of the
       surface width with corrupted colors, exactly consistent with 1
       real scanline's worth of palette-index bytes only ever filling a
       quarter of a scanline we'd allocated at 4 bytes/pixel. */
    ddpixelformat_t resolved_pixfmt;
    const ddpixelformat_t *pixfmt_to_use = &desc->ddpfPixelFormat;
    uint32_t desc_flags_to_use = desc->dwFlags;
    if (!(desc->dwFlags & DDSD_PIXELFORMAT)) {
        if (caps & DDSCAPS_TEXTURE) {
            /* Real bug regression (SWEP1RCR.EXE, see
               [[project_swep1rcr_port_scoping]]'s "jet stream renders solid
               white, no alpha" investigation): a TEXTURE surface created
               without an explicit DDPIXELFORMAT used to fall through to the
               SAME "match the current display mode" default the PRIMARY
               surface correctly wants -- but a 16bpp display mode means
               RGB565 (zero alpha bits, see fill_pixfmt_for_bpp), and
               unlike the primary surface, nothing requires a texture to
               match the display mode's exact format at all. Any texture
               relying on this implicit default for real alpha data (a
               particle/trail effect fading via its own texture's alpha
               channel, exactly this game's own reported symptom) got
               permanently-opaque texels regardless of what the source
               asset actually contained -- the same root shape as the
               earlier "gasoline in water" bug (EnumTextureFormats only
               ever advertising RGB565), just reached through surfaces the
               game creates directly instead of ones picked from that
               enumeration. Default un-specified TEXTURE surfaces to a
               real alpha-capable 16-bit format (ARGB4444, already
               advertised by EnumTextureFormats) instead. */
            memset(&resolved_pixfmt, 0, sizeof(resolved_pixfmt));
            resolved_pixfmt.dwSize = sizeof(resolved_pixfmt);
            resolved_pixfmt.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
            resolved_pixfmt.dwRGBBitCount = 16;
            resolved_pixfmt.dwRBitMask = 0x0F00u;
            resolved_pixfmt.dwGBitMask = 0x00F0u;
            resolved_pixfmt.dwBBitMask = 0x000Fu;
            resolved_pixfmt.dwRGBAlphaBitMask = 0xF000u;
        } else {
            fill_pixfmt_for_bpp(&resolved_pixfmt, dd->core->mode_bpp);
        }
        pixfmt_to_use = &resolved_pixfmt;
        desc_flags_to_use = desc->dwFlags | DDSD_PIXELFORMAT;
    }
    surface7_t *surface = surface7_create(w, h, caps, pixfmt_to_use, desc_flags_to_use);

    /* A COMPLEX primary/offscreen surface with a requested back-buffer
       count gets its flip-chain partner created and attached right away
       -- real DirectDraw does the same so GetAttachedSurface(BACKBUFFER)
       and EnumAttachedSurfaces work immediately after CreateSurface. */
    if ((caps & DDSCAPS_COMPLEX) && (desc->dwFlags & DDSD_BACKBUFFERCOUNT) && desc->dwBackBufferCountOrDepth > 0) {
        uint32_t back_caps = (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER)) | DDSCAPS_BACKBUFFER;
        surface7_t *back = surface7_create(w, h, back_caps, &surface->pixfmt, DDSD_PIXELFORMAT);
        surface7_AddAttachedSurface(surface, back);
        surface7_Release(back); /* surface now owns the one ref via AddAttachedSurface's AddRef */
    }

    /* Same reasoning as the backbuffer chain above -- a DDSCAPS_TEXTURE |
       DDSCAPS_MIPMAP surface needs its sublevel chain built right away so
       repeated GetAttachedSurface(TEXTURE|MIPMAP) calls (real D3D7 usage:
       each level's GetAttachedSurface call walks to the next-smaller
       level, exactly like the backbuffer chain) find something instead
       of failing. Confirmed via real gameplay trace: textures created
       with caps=0x00405008 (COMPLEX|TEXTURE|VIDEOMEMORY|MIPMAP, e.g. a
       256x256 one) followed a few calls later by
       "GetAttachedSurface want=0x00401000 -> NOT FOUND" -- this shim
       never built the chain the game was walking. No actual mip data
       gets downsampled/generated here (this renderer doesn't sample
       mipmaps), just the attachment structure the game expects to be
       able to walk without erroring. */
    if ((caps & DDSCAPS_TEXTURE) && (caps & DDSCAPS_MIPMAP)) {
        uint32_t levels;
        if ((desc->dwFlags & DDSD_MIPMAPCOUNT) && desc->dwMipMapCountOrRefreshRateOrSrcVBHandle > 0) {
            levels = desc->dwMipMapCountOrRefreshRateOrSrcVBHandle;
        } else {
            uint32_t mw = w, mh = h;
            levels = 1;
            while (mw > 1 || mh > 1) {
                mw = mw > 1 ? mw / 2 : 1;
                mh = mh > 1 ? mh / 2 : 1;
                levels++;
            }
        }
        surface7_t *parent = surface;
        uint32_t mw = w, mh = h;
        for (uint32_t lvl = 1; lvl < levels; lvl++) {
            mw = mw > 1 ? mw / 2 : 1;
            mh = mh > 1 ? mh / 2 : 1;
            surface7_t *sub = surface7_create(mw, mh, caps, &surface->pixfmt, DDSD_PIXELFORMAT);
            surface7_AddAttachedSurface(parent, sub);
            surface7_Release(sub);
            parent = sub;
        }
    }

    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] CreateSurface: %p %ux%u caps=0x%08x bpp=%u (backbuffers=%u)\n",
                (void *)surface, w, h, caps, surface->bpp,
                (desc->dwFlags & DDSD_BACKBUFFERCOUNT) ? desc->dwBackBufferCountOrDepth : 0);
    }

    *out_surface = surface;
    return DD_OK;
}
/* IDirectDraw/2::CreateSurface (SWEP1RCR/DX6-era, see
   [[project_swep1rcr_port_scoping]]) -- real DirectDraw always hands
   back a v1-shaped IDirectDrawSurface from these two versions'
   CreateSurface regardless of which ddraw version created it (a real,
   long-standing DirectDraw quirk -- IDirectDrawSurface2/3 are only ever
   obtained by later QueryInterface'ing the returned v1 surface, never
   returned directly). ddraw7_CreateSurface's own reads (dwFlags/width/
   height/ddsCaps.dwCaps/pixel format/backbuffer+mipmap counts) are all
   at identical offsets in the real, smaller v1 DDSURFACEDESC, so no
   input adapter is needed -- only the result surface's vtable needs to
   be swapped to the legacy (narrow-GetCaps) one before returning. */
static uint32_t WINAPI ddraw_legacy_CreateSurface(void *self, ddsurfacedesc2_t *desc, surface7_t **out_surface, void *outer)
{
    uint32_t hr = ddraw7_CreateSurface(self, desc, out_surface, outer);
    if (hr == DD_OK && out_surface != NULL && *out_surface != NULL) {
        (*out_surface)->lpVtbl = &g_surface_legacy_vtbl;
    }
    return hr;
}
static uint32_t WINAPI ddraw7_CreatePalette(void *self, uint32_t flags, paletteentry_t *color_table, palette7_t **out_palette, void *outer)
{
    (void)self; (void)outer;
    if (out_palette == NULL) return DDERR_INVALIDPARAMS;
    *out_palette = palette7_create(flags, color_table);
    return DD_OK;
}
static uint32_t WINAPI ddraw7_CreateClipper(void *self, uint32_t flags, clipper7_t **out_clipper, void *outer)
{
    (void)self; (void)flags; (void)outer;
    if (out_clipper == NULL) return DDERR_INVALIDPARAMS;
    *out_clipper = clipper7_create();
    return DD_OK;
}

/* Real semantics: EnumSurfaces(dwFlags, lpDDSD, lpContext, lpCallback)
   either enumerates REAL currently-existing surfaces matching lpDDSD
   (DDENUMSURFACES_DOESEXIST/ALL) or just tests whether a surface with
   the given description COULD be created without creating one
   (DDENUMSURFACES_CANBECREATED) -- real, if less commonly used,
   confirmed live to be one of the "unimplemented 4-arg method" calls
   firing on a real game.

   This shim has no real hardware memory limit modeled anywhere else
   (CreateSurface always succeeds), so CANBECREATED can honestly always
   answer "yes" -- calls the callback once with a NULL surface (real
   semantics: nothing was actually created) and the caller's own
   requested desc echoed back.

   DOESEXIST/ALL would need a real registry of every live surface this
   ddraw7_t has ever created (CreateSurface doesn't track one today,
   and nothing else in this codebase needs one) -- rather than build
   that speculatively for a call whose actual real dwFlags in THIS
   game aren't confirmed yet, this returns DD_OK without invoking the
   callback (an honest "found nothing", not a lie) if CANBECREATED
   isn't set. If that turns out to matter, it'll surface as a new,
   different, discoverable symptom -- not a silent wrong answer. */
#define DDENUMSURFACES_ALL          0x00000001u
#define DDENUMSURFACES_MATCH        0x00000002u
#define DDENUMSURFACES_NOMATCH      0x00000004u
#define DDENUMSURFACES_CANBECREATED 0x00000008u
#define DDENUMSURFACES_DOESEXIST    0x00000010u
static uint32_t WINAPI ddraw7_EnumSurfaces(void *self, uint32_t flags, ddsurfacedesc2_t *desc,
                                            void *ctx, void *cb_)
{
    typedef uint32_t (WINAPI *cb_t)(surface7_t *, ddsurfacedesc2_t *, void *);
    (void)self;
    if (cb_ == NULL) return DD_OK;

    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] EnumSurfaces: flags=0x%08x desc=%p\n", flags, (void *)desc);
    }

    if (flags & DDENUMSURFACES_CANBECREATED) {
        ddsurfacedesc2_t echoed;
        memset(&echoed, 0, sizeof(echoed));
        if (desc != NULL) echoed = *desc;
        echoed.dwSize = sizeof(echoed);
        ((cb_t)cb_)(NULL, &echoed, ctx);
    }
    return DD_OK;
}

static const ddraw7_vtbl_t g_ddraw7_vtbl = {
    .QueryInterface = (comfn2_t)ddraw7_QueryInterface,
    .AddRef = ddraw7_AddRef,
    .Release = ddraw7_Release,
    .Compact = fail0,
    .CreateClipper = (comfn3_t)ddraw7_CreateClipper,
    .CreatePalette = (comfn4_t)ddraw7_CreatePalette,
    .CreateSurface = (comfn3_t)ddraw7_CreateSurface,
    .DuplicateSurface = fail2,
    .EnumDisplayModes = (comfn4_t)ddraw7_EnumDisplayModes,
    .EnumSurfaces = (comfn4_t)ddraw7_EnumSurfaces,
    /* Real semantics: hands the primary surface back to GDI, typically
       called when an app wants to show a normal Windows dialog/message
       box over its DirectDraw content, or at shutdown. No real GDI
       framebuffer exists behind any surface in this shim (same "opaque,
       never-dereferenced sentinel" reasoning as gdi32.c's own DCs), so
       there's nothing to actually hand over -- a bare success is the
       correct behavior, not a placeholder. SWEP1RCR calls this for real. */
    .FlipToGDISurface = log_FlipToGDISurface,
    .GetCaps = (comfn2_t)ddraw7_GetCaps,
    .GetDisplayMode = (comfn1_t)ddraw7_GetDisplayMode,
    .GetFourCCCodes = (comfn2_t)ddraw7_GetFourCCCodes,
    .GetGDISurface = fail1,
    .GetMonitorFrequency = fail1,
    .GetScanLine = fail1,
    .GetVerticalBlankStatus = fail1,
    .Initialize = (comfn1_t)ddraw7_Initialize,
    .RestoreDisplayMode = ddraw7_RestoreDisplayMode,
    .SetCooperativeLevel = (comfn2_t)ddraw7_SetCooperativeLevel,
    .SetDisplayMode = (comfn5_t)ddraw7_SetDisplayMode,
    .WaitForVerticalBlank = fail2,
    .GetAvailableVidMem = (comfn3_t)ddraw7_GetAvailableVidMem,
    .GetSurfaceFromDC = fail2,
    .RestoreAllSurfaces = fail0,
    .TestCooperativeLevel = fail0,
    .GetDeviceIdentifier = (comfn2_t)ddraw7_GetDeviceIdentifier,
    .StartModeTest = fail3,
    .EvaluateMode = fail2,
};

/* IDirectDraw/2/4 (DX6-era legacy DirectDraw, SWEP1RCR, see
   [[project_swep1rcr_port_scoping]]) -- real callers of any of these
   three never index past their own, shorter method list (same additive/
   prefix-compatible vtable convention as the surface interfaces above),
   so each of these is just g_ddraw7_vtbl with the handful of real
   per-version differences substituted in: v1's 3-arg SetDisplayMode and
   v1/v2's CreateSurface (always returns a v1-shaped surface, never v4/v7's)
   for g_ddraw1_vtbl/g_ddraw2_vtbl, and v4's narrower GetDeviceIdentifier
   for g_ddraw4_vtbl. The trailing StartModeTest/EvaluateMode slots (v7-only)
   are harmlessly unreachable from v1/v2/v4 callers. */
static const ddraw7_vtbl_t g_ddraw1_vtbl = {
    .QueryInterface = (comfn2_t)ddraw7_QueryInterface,
    .AddRef = ddraw7_AddRef,
    .Release = ddraw7_Release,
    .Compact = fail0,
    .CreateClipper = (comfn3_t)ddraw7_CreateClipper,
    .CreatePalette = (comfn4_t)ddraw7_CreatePalette,
    .CreateSurface = (comfn3_t)ddraw_legacy_CreateSurface,
    .DuplicateSurface = fail2,
    .EnumDisplayModes = (comfn4_t)ddraw7_EnumDisplayModes,
    .EnumSurfaces = fail4_EnumSurfaces_v1,
    .FlipToGDISurface = log_FlipToGDISurface,
    .GetCaps = (comfn2_t)ddraw7_GetCaps,
    .GetDisplayMode = (comfn1_t)ddraw7_GetDisplayMode,
    .GetFourCCCodes = (comfn2_t)ddraw7_GetFourCCCodes,
    .GetGDISurface = fail1,
    .GetMonitorFrequency = fail1,
    .GetScanLine = fail1,
    .GetVerticalBlankStatus = fail1,
    .Initialize = (comfn1_t)ddraw7_Initialize,
    .RestoreDisplayMode = ddraw7_RestoreDisplayMode,
    .SetCooperativeLevel = (comfn2_t)ddraw7_SetCooperativeLevel,
    .SetDisplayMode = (comfn5_t)ddraw1_SetDisplayMode, /* real ABI is 3-arg -- see ddraw1_SetDisplayMode's own comment; cast target is g_ddraw1_vtbl's comfn5_t field type, not the actual call */
    .WaitForVerticalBlank = fail2,
};

static const ddraw7_vtbl_t g_ddraw2_vtbl = {
    .QueryInterface = (comfn2_t)ddraw7_QueryInterface,
    .AddRef = ddraw7_AddRef,
    .Release = ddraw7_Release,
    .Compact = fail0,
    .CreateClipper = (comfn3_t)ddraw7_CreateClipper,
    .CreatePalette = (comfn4_t)ddraw7_CreatePalette,
    .CreateSurface = (comfn3_t)ddraw_legacy_CreateSurface,
    .DuplicateSurface = fail2,
    .EnumDisplayModes = (comfn4_t)ddraw7_EnumDisplayModes,
    .EnumSurfaces = fail4_EnumSurfaces_v2,
    .FlipToGDISurface = log_FlipToGDISurface,
    .GetCaps = (comfn2_t)ddraw7_GetCaps,
    .GetDisplayMode = (comfn1_t)ddraw7_GetDisplayMode,
    .GetFourCCCodes = (comfn2_t)ddraw7_GetFourCCCodes,
    .GetGDISurface = fail1,
    .GetMonitorFrequency = fail1,
    .GetScanLine = fail1,
    .GetVerticalBlankStatus = fail1,
    .Initialize = (comfn1_t)ddraw7_Initialize,
    .RestoreDisplayMode = ddraw7_RestoreDisplayMode,
    .SetCooperativeLevel = (comfn2_t)ddraw7_SetCooperativeLevel,
    .SetDisplayMode = (comfn5_t)ddraw7_SetDisplayMode,
    .WaitForVerticalBlank = fail2,
    .GetAvailableVidMem = (comfn3_t)ddraw7_GetAvailableVidMem,
};

static const ddraw7_vtbl_t g_ddraw4_vtbl = {
    .QueryInterface = (comfn2_t)ddraw7_QueryInterface,
    .AddRef = ddraw7_AddRef,
    .Release = ddraw7_Release,
    .Compact = fail0,
    .CreateClipper = (comfn3_t)ddraw7_CreateClipper,
    .CreatePalette = (comfn4_t)ddraw7_CreatePalette,
    .CreateSurface = (comfn3_t)ddraw7_CreateSurface,
    .DuplicateSurface = fail2,
    .EnumDisplayModes = (comfn4_t)ddraw7_EnumDisplayModes,
    .EnumSurfaces = fail4_EnumSurfaces_v4,
    .FlipToGDISurface = log_FlipToGDISurface,
    .GetCaps = (comfn2_t)ddraw7_GetCaps,
    .GetDisplayMode = (comfn1_t)ddraw7_GetDisplayMode,
    .GetFourCCCodes = (comfn2_t)ddraw7_GetFourCCCodes,
    .GetGDISurface = fail1,
    .GetMonitorFrequency = fail1,
    .GetScanLine = fail1,
    .GetVerticalBlankStatus = fail1,
    .Initialize = (comfn1_t)ddraw7_Initialize,
    .RestoreDisplayMode = ddraw7_RestoreDisplayMode,
    .SetCooperativeLevel = (comfn2_t)ddraw7_SetCooperativeLevel,
    .SetDisplayMode = (comfn5_t)ddraw7_SetDisplayMode,
    .WaitForVerticalBlank = fail2,
    .GetAvailableVidMem = (comfn3_t)ddraw7_GetAvailableVidMem,
    .GetSurfaceFromDC = fail2,
    .RestoreAllSurfaces = fail0,
    .TestCooperativeLevel = fail0,
    .GetDeviceIdentifier = (comfn2_t)ddraw4_GetDeviceIdentifier,
};

/* ===================== IDirect3D/2/3 + IDirect3DDevice3 (DX6-era legacy,
   SWEP1RCR) =====================
   See [[project_swep1rcr_port_scoping]] -- SWEP1RCR (Star Wars Episode I:
   Racer, 1999) is DX6-era and drives IDirectDraw4 + IDirect3D3 +
   IDirect3DDevice3 (confirmed via a real trace log) rather than D3D7's
   DrawPrimitive-only pipeline this codebase otherwise targets.

   UPDATE: no longer structure-only. IDirect3DDevice3's real per-frame
   surface (BeginScene/EndScene/SetRenderState/SetTransform/SetTexture/
   SetTextureStageState/DrawPrimitive(Strided)/etc.) now genuinely
   renders, by delegating to a real, standalone d3d_device7_t (see
   d3d7_device_create_standalone and the d3d7_device_* wrappers in
   d3d7.h) -- D3D3 and D3D7 share the exact same render-state/transform-
   state/FVF numeric values and fixed-function pipeline underneath, only
   reached through a different, older, more indirect COM surface in
   D3D3's case (separate IDirect3DViewport3/IDirect3DLight/
   IDirect3DMaterial3 objects rather than D3D7's flattened device-level
   Set*). Still explicitly out of scope: the D3D3 execute-buffer
   immediate-mode path (Begin/Vertex/Index/End stay no-op -- unconfirmed
   whether this game uses it at all) and DrawPrimitiveVB/
   DrawIndexedPrimitiveVB (D3D3 doesn't share D3D7's vertex-buffer
   object, and it's unconfirmed whether this game uses that path over
   plain DrawPrimitive/Strided).

   IDirect3DDevice2's real method layout is NOT prefix-compatible with
   Device3 (it has an extra SwapTextureHandles slot between GetCaps and
   GetStats that Device3 dropped, shifting every later offset) -- so
   unlike the DirectDraw/Surface legacy chains above, Device2 is NOT
   built out here: IDirect3D2::CreateDevice cleanly fails (DDERR_GENERIC)
   rather than risk handing back a Device3-shaped object through a
   Device2-shaped pointer, which would silently misalign every method
   call after GetCaps. IDirect3D (v1) has no CreateDevice at all in real
   Windows -- 3D device creation starts at v2. */

/* Real wire structs D3D3-era methods actually take -- genuinely
   different shapes from d3d7.h's d3dlight7_t/d3dmaterial7_t/
   d3dviewport7_t (D3D7's simplified, dwSize-less versions), confirmed
   byte-exact via a compiled sizeof/offsetof probe against real Wine/
   MinGW headers under Wine (not eyeballed -- see
   [[project_swep1rcr_port_scoping]] for what that caught: an initial
   guess had D3DLIGHT at 80 bytes matching D3DLIGHT2, when the real
   IDirect3DLight::SetLight/GetLight take the older, 76-byte D3DLIGHT
   with a single dcvColor rather than separate diffuse/specular/ambient). */
#pragma pack(push, 1)
typedef struct {
    uint32_t dwSize;
    uint32_t dltType;
    d3dcolorvalue_t dcvColor;
    d3dvector_t dvPosition;
    d3dvector_t dvDirection;
    float dvRange;
    float dvFalloff;
    float dvAttenuation0, dvAttenuation1, dvAttenuation2;
    float dvTheta;
    float dvPhi;
} d3dlight_wire_t; /* real D3DLIGHT, 76 bytes */

typedef struct {
    uint32_t dwSize;
    d3dcolorvalue_t dcvDiffuse;
    d3dcolorvalue_t dcvAmbient;
    d3dcolorvalue_t dcvSpecular;
    d3dcolorvalue_t dcvEmissive;
    float dvPower;
    uint32_t hTexture;
    uint32_t dwRampSize;
} d3dmaterial_wire_t; /* real D3DMATERIAL, 80 bytes */

typedef struct {
    uint32_t dwSize;
    uint32_t dwX, dwY, dwWidth, dwHeight;
    float dvClipX, dvClipY, dvClipWidth, dvClipHeight;
    float dvMinZ, dvMaxZ;
} d3dviewport2_wire_t; /* real D3DVIEWPORT2, 44 bytes */
#pragma pack(pop)

/* dcvColor (single light color, real D3DLIGHT semantics) becomes both
   diffuse and specular contribution in d3d7.h's 3-color d3dlight7_t --
   matches real fixed-function driver behavior for this era (ambient
   comes from the separate global D3DLIGHTSTATE_AMBIENT, not per-light).
   Round-tripped back out through the same field on GetLight. */
static void d3dlight_wire_to_internal(const d3dlight_wire_t *src, d3dlight7_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->type = src->dltType;
    dst->diffuse = src->dcvColor;
    dst->specular = src->dcvColor;
    dst->position = src->dvPosition;
    dst->direction = src->dvDirection;
    dst->range = src->dvRange;
    dst->falloff = src->dvFalloff;
    dst->attenuation0 = src->dvAttenuation0;
    dst->attenuation1 = src->dvAttenuation1;
    dst->attenuation2 = src->dvAttenuation2;
    dst->theta = src->dvTheta;
    dst->phi = src->dvPhi;
}
static void d3dlight_internal_to_wire(const d3dlight7_t *src, d3dlight_wire_t *dst)
{
    dst->dwSize = sizeof(*dst);
    dst->dltType = src->type;
    dst->dcvColor = src->diffuse;
    dst->dvPosition = src->position;
    dst->dvDirection = src->direction;
    dst->dvRange = src->range;
    dst->dvFalloff = src->falloff;
    dst->dvAttenuation0 = src->attenuation0;
    dst->dvAttenuation1 = src->attenuation1;
    dst->dvAttenuation2 = src->attenuation2;
    dst->dvTheta = src->theta;
    dst->dvPhi = src->phi;
}

static void d3dmaterial_wire_to_internal(const d3dmaterial_wire_t *src, d3dmaterial7_t *dst)
{
    dst->diffuse = src->dcvDiffuse;
    dst->ambient = src->dcvAmbient;
    dst->specular = src->dcvSpecular;
    dst->emissive = src->dcvEmissive;
    dst->power = src->dvPower;
}
static void d3dmaterial_internal_to_wire(const d3dmaterial7_t *src, d3dmaterial_wire_t *dst)
{
    dst->dwSize = sizeof(*dst);
    dst->dcvDiffuse = src->diffuse;
    dst->dcvAmbient = src->ambient;
    dst->dcvSpecular = src->specular;
    dst->dcvEmissive = src->emissive;
    dst->dvPower = src->power;
    dst->hTexture = 0;
    dst->dwRampSize = 0;
}

/* dvClipX/Y/Width/Height (D3DVIEWPORT2's separate clip region) has no
   equivalent in d3dviewport7_t -- real D3DVIEWPORT7 dropped that same
   concept too (pixel rect + depth range only), nothing downstream reads
   it either way. */
static void d3dviewport2_wire_to_internal(const d3dviewport2_wire_t *src, d3dviewport7_t *dst)
{
    dst->x = src->dwX;
    dst->y = src->dwY;
    dst->width = src->dwWidth;
    dst->height = src->dwHeight;
    dst->min_z = src->dvMinZ;
    dst->max_z = src->dvMaxZ;
}
static void d3dviewport2_internal_to_wire(const d3dviewport7_t *src, d3dviewport2_wire_t *dst)
{
    dst->dwSize = sizeof(*dst);
    dst->dwX = src->x;
    dst->dwY = src->y;
    dst->dwWidth = src->width;
    dst->dwHeight = src->height;
    dst->dvClipX = 0.0f;
    dst->dvClipY = 0.0f;
    dst->dvClipWidth = (float)src->width;
    dst->dvClipHeight = (float)src->height;
    dst->dvMinZ = src->min_z;
    dst->dvMaxZ = src->max_z;
}

/* ===================== IDirect3DLight ===================== */

typedef struct {
    comfn2_t QueryInterface;
    comfn0_t AddRef;
    comfn0_t Release;
    comfn1_t Initialize;
    comfn1_t SetLight;
    comfn1_t GetLight;
} d3d_light_vtbl_t;

typedef struct {
    const d3d_light_vtbl_t *lpVtbl;
    atomic_uint refcount;
    d3dlight7_t light;
    int light_set;
    /* Which device/slot this light is currently pushed into, if any --
       set by d3d_device3_activate_light, cleared by
       d3d_device3_deactivate_light (both defined near d3d_device3_t
       itself, forward-declared below). A light could in principle be on
       more than one viewport/device at once in real D3D3, but this shim
       (like every other object in it) only ever has one device alive
       per session, so tracking a single owner directly on the light is
       a deliberate, documented simplification. */
    void *assigned_device;
    int assigned_slot;
} d3d_light_t;

static const d3d_light_vtbl_t g_d3d_light_vtbl;

/* Forward-declared: real bodies live near d3d_device3_t further down,
   once its full definition (inner7, light_slot_owner[]) is known. */
static void d3d_device3_activate_light(void *device, d3d_light_t *light);
static void d3d_device3_deactivate_light(void *device, d3d_light_t *light);
static void d3d_device3_apply_viewport(void *device, const d3dviewport7_t *vp);

static uint32_t WINAPI d3d_light_QueryInterface(void *self, guid_t *riid, void **out)
{
    if (out == NULL) return DDERR_INVALIDPARAMS;
    if (riid == NULL || guid_equal(riid, &IID_IDirect3DLight)) {
        atomic_fetch_add(&((d3d_light_t *)self)->refcount, 1u);
        *out = self;
        return DD_OK;
    }
    *out = NULL;
    return DDERR_NOINTERFACE;
}
static uint32_t WINAPI d3d_light_AddRef(void *self)
{ return atomic_fetch_add(&((d3d_light_t *)self)->refcount, 1u) + 1u; }
static uint32_t WINAPI d3d_light_Release(void *self)
{
    d3d_light_t *l = (d3d_light_t *)self;
    if (atomic_load(&l->refcount) == 0) {
        fprintf(stderr, "[ddraw] BUG: Release called on d3d_light %p with refcount already 0 (over-release)\n", self);
        return 0;
    }
    uint32_t rc = atomic_fetch_sub(&l->refcount, 1u) - 1u;
    if (rc == 0) {
        if (l->assigned_device != NULL) d3d_device3_deactivate_light(l->assigned_device, l);
        free(l);
    }
    return rc;
}
static uint32_t WINAPI d3d_light_SetLight(void *self, d3dlight_wire_t *data)
{
    d3d_light_t *l = (d3d_light_t *)self;
    if (data == NULL) return DDERR_INVALIDPARAMS;
    d3dlight_wire_to_internal(data, &l->light);
    l->light_set = 1;
    /* Real semantics: changing an already-attached light's data updates
       what's actually lit immediately, not just on the next AddLight. */
    if (l->assigned_device != NULL) d3d_device3_activate_light(l->assigned_device, l);
    return DD_OK;
}
static uint32_t WINAPI d3d_light_GetLight(void *self, d3dlight_wire_t *out_data)
{
    d3d_light_t *l = (d3d_light_t *)self;
    if (out_data == NULL) return DDERR_INVALIDPARAMS;
    if (!l->light_set) return DDERR_GENERIC;
    d3dlight_internal_to_wire(&l->light, out_data);
    return DD_OK;
}
static const d3d_light_vtbl_t g_d3d_light_vtbl = {
    .QueryInterface = (comfn2_t)d3d_light_QueryInterface,
    .AddRef = d3d_light_AddRef,
    .Release = d3d_light_Release,
    .Initialize = log_light_Initialize, /* real IDirect3DLight::Initialize(IDirect3D*) -- nothing downstream needs to know */
    .SetLight = (comfn1_t)d3d_light_SetLight,
    .GetLight = (comfn1_t)d3d_light_GetLight,
};
static void *d3d_light_create(void)
{
    d3d_light_t *l = calloc(1, sizeof(*l));
    l->lpVtbl = &g_d3d_light_vtbl;
    atomic_init(&l->refcount, 1u);
    l->assigned_slot = -1;
    return l;
}

/* ===================== IDirect3DMaterial3 ===================== */

typedef struct {
    comfn2_t QueryInterface;
    comfn0_t AddRef;
    comfn0_t Release;
    comfn1_t SetMaterial;
    comfn1_t GetMaterial;
    comfn2_t GetHandle;
} d3d_material3_vtbl_t;

typedef struct {
    const d3d_material3_vtbl_t *lpVtbl;
    atomic_uint refcount;
    d3dmaterial7_t material;
} d3d_material3_t;

static const d3d_material3_vtbl_t g_d3d_material3_vtbl;

/* Forward-declared: real body lives near d3d_device3_t further down,
   once its handle table is known. */
static uint32_t d3d_device3_register_material_handle(void *device, d3d_material3_t *mat, uint32_t *out_handle);

static uint32_t WINAPI d3d_material3_QueryInterface(void *self, guid_t *riid, void **out)
{
    if (out == NULL) return DDERR_INVALIDPARAMS;
    if (riid == NULL || guid_equal(riid, &IID_IDirect3DMaterial3)) {
        atomic_fetch_add(&((d3d_material3_t *)self)->refcount, 1u);
        *out = self;
        return DD_OK;
    }
    *out = NULL;
    return DDERR_NOINTERFACE;
}
static uint32_t WINAPI d3d_material3_AddRef(void *self)
{ return atomic_fetch_add(&((d3d_material3_t *)self)->refcount, 1u) + 1u; }
static uint32_t WINAPI d3d_material3_Release(void *self)
{
    d3d_material3_t *m = (d3d_material3_t *)self;
    if (atomic_load(&m->refcount) == 0) {
        fprintf(stderr, "[ddraw] BUG: Release called on d3d_material3 %p with refcount already 0 (over-release)\n", self);
        return 0;
    }
    uint32_t rc = atomic_fetch_sub(&m->refcount, 1u) - 1u;
    if (rc == 0) free(m);
    return rc;
}
static uint32_t WINAPI d3d_material3_SetMaterial(void *self, d3dmaterial_wire_t *data)
{
    d3d_material3_t *m = (d3d_material3_t *)self;
    if (data == NULL) return DDERR_INVALIDPARAMS;
    d3dmaterial_wire_to_internal(data, &m->material);
    return DD_OK;
}
static uint32_t WINAPI d3d_material3_GetMaterial(void *self, d3dmaterial_wire_t *out_data)
{
    d3d_material3_t *m = (d3d_material3_t *)self;
    if (out_data == NULL) return DDERR_INVALIDPARAMS;
    d3dmaterial_internal_to_wire(&m->material, out_data);
    return DD_OK;
}
static uint32_t WINAPI d3d_material3_GetHandle(void *self, void *device, uint32_t *out_handle)
{
    if (device == NULL || out_handle == NULL) return DDERR_INVALIDPARAMS;
    return d3d_device3_register_material_handle(device, (d3d_material3_t *)self, out_handle);
}
static const d3d_material3_vtbl_t g_d3d_material3_vtbl = {
    .QueryInterface = (comfn2_t)d3d_material3_QueryInterface,
    .AddRef = d3d_material3_AddRef,
    .Release = d3d_material3_Release,
    .SetMaterial = (comfn1_t)d3d_material3_SetMaterial,
    .GetMaterial = (comfn1_t)d3d_material3_GetMaterial,
    .GetHandle = (comfn2_t)d3d_material3_GetHandle,
};
static void *d3d_material3_create(void)
{
    d3d_material3_t *m = calloc(1, sizeof(*m));
    m->lpVtbl = &g_d3d_material3_vtbl;
    atomic_init(&m->refcount, 1u);
    return m;
}

/* ===================== IDirect3DViewport3 ===================== */

#define MAX_VIEWPORT_LIGHTS 16 /* matches d3d7.c's own MAX_LIGHTS bound -- see [[project_swep1rcr_port_scoping]] */

typedef struct {
    comfn2_t QueryInterface;
    comfn0_t AddRef;
    comfn0_t Release;
    comfn1_t Initialize;
    comfn1_t GetViewport;
    comfn1_t SetViewport;
    comfn4_t TransformVertices;
    comfn2_t LightElements;
    comfn1_t SetBackground;
    comfn2_t GetBackground;
    comfn1_t SetBackgroundDepth;
    comfn2_t GetBackgroundDepth;
    comfn3_t Clear;
    comfn1_t AddLight;
    comfn1_t DeleteLight;
    comfn3_t NextLight;
    comfn1_t GetViewport2;
    comfn1_t SetViewport2;
    comfn1_t SetBackgroundDepth2;
    comfn2_t GetBackgroundDepth2;
    comfn6_t Clear2;
} d3d_viewport3_vtbl_t;

typedef struct {
    const d3d_viewport3_vtbl_t *lpVtbl;
    atomic_uint refcount;
    d3dviewport7_t viewport;
    int viewport_set;
    d3d_light_t *lights[MAX_VIEWPORT_LIGHTS];
    int light_count;
    uint32_t background_handle;
    /* Set by d3d_device3_SetCurrentViewport when this viewport becomes
       the device's active one, cleared when another becomes active --
       NOT AddRef'd, same "device owns/outlives its current viewport
       reference" convention already used for d3d_device3_t's own
       current_viewport field. */
    void *active_device;
} d3d_viewport3_t;

static const d3d_viewport3_vtbl_t g_d3d_viewport3_vtbl;

static uint32_t WINAPI d3d_viewport3_QueryInterface(void *self, guid_t *riid, void **out)
{
    if (out == NULL) return DDERR_INVALIDPARAMS;
    if (riid == NULL || guid_equal(riid, &IID_IDirect3DViewport3)) {
        atomic_fetch_add(&((d3d_viewport3_t *)self)->refcount, 1u);
        *out = self;
        return DD_OK;
    }
    *out = NULL;
    return DDERR_NOINTERFACE;
}
static uint32_t WINAPI d3d_viewport3_AddRef(void *self)
{ return atomic_fetch_add(&((d3d_viewport3_t *)self)->refcount, 1u) + 1u; }
static uint32_t WINAPI d3d_viewport3_Release(void *self)
{
    d3d_viewport3_t *vp = (d3d_viewport3_t *)self;
    if (atomic_load(&vp->refcount) == 0) {
        fprintf(stderr, "[ddraw] BUG: Release called on d3d_viewport3 %p with refcount already 0 (over-release)\n", self);
        return 0;
    }
    uint32_t rc = atomic_fetch_sub(&vp->refcount, 1u) - 1u;
    if (rc == 0) {
        for (int i = 0; i < vp->light_count; i++) {
            if (vp->lights[i] != NULL) d3d_light_Release(vp->lights[i]);
        }
        free(vp);
    }
    return rc;
}
static uint32_t WINAPI d3d_viewport3_GetViewport2(void *self, d3dviewport2_wire_t *out_data)
{
    d3d_viewport3_t *vp = (d3d_viewport3_t *)self;
    if (out_data == NULL) return DDERR_INVALIDPARAMS;
    if (!vp->viewport_set) return DDERR_GENERIC;
    d3dviewport2_internal_to_wire(&vp->viewport, out_data);
    return DD_OK;
}
static uint32_t WINAPI d3d_viewport3_SetViewport2(void *self, d3dviewport2_wire_t *data)
{
    d3d_viewport3_t *vp = (d3d_viewport3_t *)self;
    if (data == NULL) return DDERR_INVALIDPARAMS;
    d3dviewport2_wire_to_internal(data, &vp->viewport);
    vp->viewport_set = 1;
    if (vp->active_device != NULL) d3d_device3_apply_viewport(vp->active_device, &vp->viewport);
    return DD_OK;
}
static uint32_t WINAPI d3d_viewport3_AddLight(void *self, void *light_)
{
    d3d_viewport3_t *vp = (d3d_viewport3_t *)self;
    d3d_light_t *light = (d3d_light_t *)light_;
    if (light == NULL) return DDERR_INVALIDPARAMS;
    if (vp->light_count >= MAX_VIEWPORT_LIGHTS) return DDERR_GENERIC;
    d3d_light_AddRef(light);
    vp->lights[vp->light_count++] = light;
    if (vp->active_device != NULL) d3d_device3_activate_light(vp->active_device, light);
    return DD_OK;
}
static uint32_t WINAPI d3d_viewport3_DeleteLight(void *self, void *light_)
{
    d3d_viewport3_t *vp = (d3d_viewport3_t *)self;
    d3d_light_t *light = (d3d_light_t *)light_;
    if (light == NULL) return DDERR_INVALIDPARAMS;
    for (int i = 0; i < vp->light_count; i++) {
        if (vp->lights[i] != light) continue;
        if (vp->active_device != NULL) d3d_device3_deactivate_light(vp->active_device, light);
        d3d_light_Release(light);
        for (int j = i; j < vp->light_count - 1; j++) vp->lights[j] = vp->lights[j + 1];
        vp->light_count--;
        return DD_OK;
    }
    return DDERR_GENERIC; /* real semantics: light wasn't attached to this viewport */
}
static uint32_t WINAPI d3d_viewport3_NextLight(void *self, void *ref_, void **out, uint32_t flags)
{
    (void)flags;
    d3d_viewport3_t *vp = (d3d_viewport3_t *)self;
    if (out == NULL) return DDERR_INVALIDPARAMS;
    if (vp->light_count == 0) { *out = NULL; return DD_OK; }
    int idx = 0;
    if (ref_ != NULL) {
        int found = -1;
        for (int i = 0; i < vp->light_count; i++) if (vp->lights[i] == ref_) { found = i; break; }
        if (found < 0 || found + 1 >= vp->light_count) { *out = NULL; return DD_OK; }
        idx = found + 1;
    }
    *out = vp->lights[idx];
    d3d_light_AddRef(vp->lights[idx]);
    return DD_OK;
}
static uint32_t WINAPI d3d_viewport3_SetBackground(void *self, uint32_t handle)
{ ((d3d_viewport3_t *)self)->background_handle = handle; return DD_OK; }
static uint32_t WINAPI d3d_viewport3_GetBackground(void *self, uint32_t *out_handle, uint32_t *out_valid)
{
    d3d_viewport3_t *vp = (d3d_viewport3_t *)self;
    if (out_handle != NULL) *out_handle = vp->background_handle;
    if (out_valid != NULL) *out_valid = vp->background_handle != 0;
    return DD_OK;
}
/* GetBackgroundDepth/GetBackgroundDepth2 -- real signature shape is
   identical between v1/v3 (only the surface interface version pointed
   to differs, an ABI-irrelevant type-only distinction, same pointer
   width) -- shared by both vtable slots below. Always-null/invalid
   rather than leaving the caller's output pointers untouched. */
static uint32_t WINAPI d3d_viewport3_GetBackgroundDepth(void *self, void **out_surface, uint32_t *out_valid)
{
    (void)self;
    if (out_surface != NULL) *out_surface = NULL;
    if (out_valid != NULL) *out_valid = 0;
    return DD_OK;
}
static uint32_t WINAPI d3d_viewport3_Clear2(void *self, uint32_t count, uint32_t rects, uint32_t flags,
                                             uint32_t color, uint32_t z_bits, uint32_t stencil)
{
    /* DirectDraw's own Blt-based clear is this game's real clear path
       (visible working in the trace log) -- Viewport::Clear/Clear2
       staying a no-op is a deliberate, documented scope decision, not
       an oversight. See [[project_swep1rcr_port_scoping]]. */
    (void)self; (void)count; (void)rects; (void)flags; (void)color; (void)z_bits; (void)stencil;
    return DD_OK;
}
static const d3d_viewport3_vtbl_t g_d3d_viewport3_vtbl = {
    .QueryInterface = (comfn2_t)d3d_viewport3_QueryInterface,
    .AddRef = d3d_viewport3_AddRef,
    .Release = d3d_viewport3_Release,
    .Initialize = log_viewport_Initialize,           /* real IDirect3DViewport::Initialize(IDirect3D*) */
    .GetViewport = log_viewport_GetViewport_v1,          /* v1 D3DVIEWPORT -- D3D3-era games predominantly use v2 below */
    .SetViewport = log_viewport_SetViewport_v1,
    .TransformVertices = fail4_TransformVertices,  /* execute-buffer legacy, unconfirmed needed -- see [[project_swep1rcr_port_scoping]] */
    .LightElements = fail2,      /* ditto */
    .SetBackground = (comfn1_t)d3d_viewport3_SetBackground,
    .GetBackground = (comfn2_t)d3d_viewport3_GetBackground,
    .SetBackgroundDepth = log_viewport_SetBackgroundDepth,
    .GetBackgroundDepth = (comfn2_t)d3d_viewport3_GetBackgroundDepth,
    .Clear = log_viewport_Clear,
    .AddLight = (comfn1_t)d3d_viewport3_AddLight,
    .DeleteLight = (comfn1_t)d3d_viewport3_DeleteLight,
    .NextLight = (comfn3_t)d3d_viewport3_NextLight,
    .GetViewport2 = (comfn1_t)d3d_viewport3_GetViewport2,
    .SetViewport2 = (comfn1_t)d3d_viewport3_SetViewport2,
    .SetBackgroundDepth2 = log_viewport_SetBackgroundDepth2,
    .GetBackgroundDepth2 = (comfn2_t)d3d_viewport3_GetBackgroundDepth,
    .Clear2 = d3d_viewport3_Clear2,
};
static void *d3d_viewport3_create(void)
{
    d3d_viewport3_t *vp = calloc(1, sizeof(*vp));
    vp->lpVtbl = &g_d3d_viewport3_vtbl;
    atomic_init(&vp->refcount, 1u);
    return vp;
}

typedef struct {
    comfn2_t QueryInterface;
    comfn0_t AddRef;
    comfn0_t Release;
    comfn2_t EnumDevices;
    comfn2_t CreateLight;
    comfn2_t CreateMaterial;
    comfn2_t CreateViewport;
    comfn2_t FindDevice;
    comfn4_t CreateDevice;         /* v3's real 4-arg shape (rclsid,surface,device,outer); v2's real CreateDevice is 3-arg (no outer) -- cast to this field type regardless, same convention as ddraw1_SetDisplayMode above */
    comfn4_t CreateVertexBuffer;
    comfn3_t EnumZBufferFormats;
    comfn0_t EvictManagedTextures;
} d3d123_vtbl_t;

typedef struct {
    const d3d123_vtbl_t *lpVtbl;
    atomic_uint refcount; /* THIS object's own count -- see ddraw7_t's comment above for why versions don't share one */
} d3d123_t;

static const d3d123_vtbl_t g_d3d1_vtbl;
static const d3d123_vtbl_t g_d3d2_vtbl;
static const d3d123_vtbl_t g_d3d3_vtbl;

/* Same independent-tear-off reasoning as ddraw7_t above: each requested
   version is its own object with its own refcount, not a vtable swap on
   `self`. Unlike DirectDraw, IDirect3D/2/3 has no real mutable state of
   its own to share between versions, so there's no core to share --
   d3d1/2/3_create() below just allocate a standalone object each time. */
static uint32_t WINAPI d3d123_QueryInterface(void *self, guid_t *riid, void **out)
{
    (void)self;
    if (out == NULL) return DDERR_INVALIDPARAMS;
    if (guid_equal(riid, &IID_IDirect3D3)) { *out = d3d3_create(); return *out != NULL ? DD_OK : DDERR_OUTOFMEMORY; }
    if (guid_equal(riid, &IID_IDirect3D2)) { *out = d3d2_create(); return *out != NULL ? DD_OK : DDERR_OUTOFMEMORY; }
    if (guid_equal(riid, &IID_IDirect3D))  { *out = d3d1_create(); return *out != NULL ? DD_OK : DDERR_OUTOFMEMORY; }
    *out = NULL;
    return DDERR_NOINTERFACE;
}
static uint32_t WINAPI d3d123_AddRef(void *self)
{
    return atomic_fetch_add(&((d3d123_t *)self)->refcount, 1u) + 1u;
}
static uint32_t WINAPI d3d123_Release(void *self)
{
    d3d123_t *d3d = (d3d123_t *)self;
    if (atomic_load(&d3d->refcount) == 0) {
        fprintf(stderr, "[ddraw] BUG: Release called on d3d123 %p with refcount already 0 (over-release)\n", self);
        return 0;
    }
    uint32_t rc = atomic_fetch_sub(&d3d->refcount, 1u) - 1u;
    if (rc == 0) free(d3d);
    return rc;
}

/* Real D3DDEVICEDESC (what IDirect3D/2/3::EnumDevices hands the callback,
   NOT the same struct as D3D7's D3DDEVICEDESC7 -- see d3ddevicedesc7_t in
   d3d7.h) and its nested cap structs, confirmed byte-exact via a compiled
   sizeof/offsetof probe against real Wine/MinGW headers under Wine (same
   methodology as d3dlight_wire_t/d3dmaterial_wire_t above -- not
   eyeballed): D3DDEVICEDESC is 252 bytes, D3DPRIMCAPS 56, D3DLIGHTINGCAPS
   16, D3DTRANSFORMCAPS 8 -- all-DWORD/float/WORD fields with no gaps, so
   pack(1) reproduces the real layout exactly. */
#pragma pack(push, 1)
typedef struct {
    uint32_t dwSize;
    uint32_t dwCaps;
} d3dtransformcaps_wire_t; /* real D3DTRANSFORMCAPS, 8 bytes */

typedef struct {
    uint32_t dwSize;
    uint32_t dwCaps;
    uint32_t dwLightingModel;
    uint32_t dwNumLights;
} d3dlightingcaps_wire_t; /* real D3DLIGHTINGCAPS, 16 bytes */

typedef struct {
    uint32_t dwSize;
    uint32_t dwMiscCaps;
    uint32_t dwRasterCaps;
    uint32_t dwZCmpCaps;
    uint32_t dwSrcBlendCaps;
    uint32_t dwDestBlendCaps;
    uint32_t dwAlphaCmpCaps;
    uint32_t dwShadeCaps;
    uint32_t dwTextureCaps;
    uint32_t dwTextureFilterCaps;
    uint32_t dwTextureBlendCaps;
    uint32_t dwTextureAddressCaps;
    uint32_t dwStippleWidth;
    uint32_t dwStippleHeight;
} d3dprimcaps_wire_t; /* real D3DPRIMCAPS, 56 bytes */

typedef struct {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dcmColorModel;
    uint32_t dwDevCaps;
    d3dtransformcaps_wire_t dtcTransformCaps;
    uint32_t bClipping;
    d3dlightingcaps_wire_t dlcLightingCaps;
    d3dprimcaps_wire_t dpcLineCaps;
    d3dprimcaps_wire_t dpcTriCaps;
    uint32_t dwDeviceRenderBitDepth;
    uint32_t dwDeviceZBufferBitDepth;
    uint32_t dwMaxBufferSize;
    uint32_t dwMaxVertexCount;
    uint32_t dwMinTextureWidth, dwMinTextureHeight;
    uint32_t dwMaxTextureWidth, dwMaxTextureHeight;
    uint32_t dwMinStippleWidth, dwMaxStippleWidth;
    uint32_t dwMinStippleHeight, dwMaxStippleHeight;
    uint32_t dwMaxTextureRepeat;
    uint32_t dwMaxTextureAspectRatio;
    uint32_t dwMaxAnisotropy;
    float dvGuardBandLeft, dvGuardBandTop, dvGuardBandRight, dvGuardBandBottom;
    float dvExtentsAdjust;
    uint32_t dwStencilCaps;
    uint32_t dwFVFCaps;
    uint32_t dwTextureOpCaps;
    uint16_t wMaxTextureBlendStages;
    uint16_t wMaxSimultaneousTextures;
} d3ddevicedesc_wire_t; /* real D3DDEVICEDESC, 252 bytes */
#pragma pack(pop)

#define D3DDD_COLORMODEL           0x00000001u
#define D3DDD_DEVCAPS              0x00000002u
#define D3DDD_TRANSFORMCAPS        0x00000004u
#define D3DDD_LIGHTINGCAPS         0x00000008u
#define D3DDD_BCLIPPING            0x00000010u
#define D3DDD_LINECAPS             0x00000020u
#define D3DDD_TRICAPS              0x00000040u
#define D3DDD_DEVICERENDERBITDEPTH 0x00000080u
#define D3DDD_DEVICEZBUFFERBITDEPTH 0x00000100u
#define D3DDD_MAXBUFFERSIZE        0x00000200u
#define D3DDD_MAXVERTEXCOUNT       0x00000400u
#define D3DCOLOR_RGB_MODEL         2u
#define D3D3_DEVCAPS_FLOATTLVERTEX        0x00000001u
#define D3D3_DEVCAPS_EXECUTESYSTEMMEMORY  0x00000010u
#define D3D3_DEVCAPS_TLVERTEXSYSTEMMEMORY 0x00000040u
#define D3D3_DEVCAPS_DRAWPRIMTLVERTEX     0x00000400u
#define D3D3_DEVCAPS_CANRENDERAFTERFLIP   0x00000800u
#define D3D3_TRANSFORMCAPS_CLIP    0x00000001u
#define D3D3_LIGHTCAPS_POINT       0x00000001u
#define D3D3_LIGHTCAPS_SPOT        0x00000002u
#define D3D3_LIGHTCAPS_DIRECTIONAL 0x00000004u
#define D3D3_LIGHTCAPS_PARALLELPOINT 0x00000008u
#define D3D3_LIGHTINGMODEL_RGB     0x00000001u
#define D3D3_PRASTERCAPS_DITHER    0x00000001u
#define D3D3_PRASTERCAPS_ZTEST     0x00000010u
#define D3D3_PCMPCAPS_LESSEQUAL    0x00000008u
#define D3D3_PSHADECAPS_COLORGOURAUDRGB 0x00000008u
#define D3D3_PTEXTURECAPS_PERSPECTIVE 0x00000001u
#define D3D3_PTEXTURECAPS_ALPHA       0x00000004u
#define D3D3_PTBLENDCAPS_DECAL         0x00000001u
#define D3D3_PTBLENDCAPS_MODULATE      0x00000002u
#define D3D3_PTBLENDCAPS_DECALALPHA    0x00000004u
#define D3D3_PTBLENDCAPS_MODULATEALPHA 0x00000008u
#define D3D3_PTADDRESSCAPS_WRAP    0x00000001u
#define D3D3_PTADDRESSCAPS_CLAMP   0x00000004u

/* Real IDirect3D/2/3::EnumDevices callback is
   (GUID*, char*, char*, D3DDEVICEDESC*, D3DDEVICEDESC*, void*) -- unlike
   D3D7's simpler 4-arg callback (see d3d7_EnumDevices in d3d7.c), this
   takes TWO caps structs (HAL and HEL) plus a device GUID. Same
   "claim every capability" reasoning as d3d7_EnumDevices: reporting zero
   devices, like an empty capability set, is what real games from this
   era treat as fatal.

   Previously handed back a raw 0xFF-filled byte blob instead of a real
   D3DDEVICEDESC -- harmless for fields games never read, but a live
   SWEP1RCR trace (see [[project_swep1rcr_port_scoping]]) showed a
   texture-manager CreateSurface call with dwWidth=dwHeight=0xFFFFFFFF,
   and 0xFF-filling this struct means dwMaxTextureWidth/dwMaxTextureHeight
   -- and every other DWORD field -- ALSO read back as 0xFFFFFFFF. A
   texture manager sizing something off the device's own advertised max
   texture dimensions would get exactly that value, straight from us.
   Now fills in real, sane HAL-plausible caps (same numbers
   d3d_device7_GetCaps in d3d7.c already reports and has been live-tested
   with for king.exe: 2048 max texture size, 1 minimum, etc.) instead of
   a blob no real driver would ever produce. */
static void fill_d3d3_device_desc(d3ddevicedesc_wire_t *d)
{
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
    d->dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRANSFORMCAPS |
                 D3DDD_LIGHTINGCAPS | D3DDD_BCLIPPING | D3DDD_LINECAPS |
                 D3DDD_TRICAPS | D3DDD_DEVICERENDERBITDEPTH |
                 D3DDD_DEVICEZBUFFERBITDEPTH | D3DDD_MAXBUFFERSIZE |
                 D3DDD_MAXVERTEXCOUNT;
    d->dcmColorModel = D3DCOLOR_RGB_MODEL;
    d->dwDevCaps = D3D3_DEVCAPS_FLOATTLVERTEX | D3D3_DEVCAPS_EXECUTESYSTEMMEMORY |
                   D3D3_DEVCAPS_TLVERTEXSYSTEMMEMORY | D3D3_DEVCAPS_DRAWPRIMTLVERTEX |
                   D3D3_DEVCAPS_CANRENDERAFTERFLIP;

    d->dtcTransformCaps.dwSize = sizeof(d->dtcTransformCaps);
    d->dtcTransformCaps.dwCaps = D3D3_TRANSFORMCAPS_CLIP;
    d->bClipping = 1;

    d->dlcLightingCaps.dwSize = sizeof(d->dlcLightingCaps);
    d->dlcLightingCaps.dwCaps = D3D3_LIGHTCAPS_POINT | D3D3_LIGHTCAPS_SPOT |
                                 D3D3_LIGHTCAPS_DIRECTIONAL | D3D3_LIGHTCAPS_PARALLELPOINT;
    d->dlcLightingCaps.dwLightingModel = D3D3_LIGHTINGMODEL_RGB;
    d->dlcLightingCaps.dwNumLights = MAX_VIEWPORT_LIGHTS;

    d3dprimcaps_wire_t prim;
    memset(&prim, 0, sizeof(prim));
    prim.dwSize = sizeof(prim);
    prim.dwRasterCaps = D3D3_PRASTERCAPS_DITHER | D3D3_PRASTERCAPS_ZTEST;
    prim.dwZCmpCaps = D3D3_PCMPCAPS_LESSEQUAL;
    prim.dwShadeCaps = D3D3_PSHADECAPS_COLORGOURAUDRGB;
    prim.dwTextureCaps = D3D3_PTEXTURECAPS_PERSPECTIVE | D3D3_PTEXTURECAPS_ALPHA;
    prim.dwTextureBlendCaps = D3D3_PTBLENDCAPS_DECAL | D3D3_PTBLENDCAPS_MODULATE |
                               D3D3_PTBLENDCAPS_DECALALPHA | D3D3_PTBLENDCAPS_MODULATEALPHA;
    prim.dwTextureAddressCaps = D3D3_PTADDRESSCAPS_WRAP | D3D3_PTADDRESSCAPS_CLAMP;
    d->dpcLineCaps = prim;
    d->dpcTriCaps = prim;

    /* Real bit-flag values, not the literal depth numbers -- see d3d7.c's
       own d3d_device7_GetCaps for the full live-traced explanation (same
       bug, same fix, kept consistent across both the D3D7 and legacy D3D3
       caps-fill paths). */
    d->dwDeviceRenderBitDepth = 0x400u | 0x100u; /* DDBD_16 | DDBD_32 */
    d->dwDeviceZBufferBitDepth = 0x400u;         /* DDBD_16 */
    d->dwMaxBufferSize = 0;
    d->dwMaxVertexCount = 0xFFFFu;

    d->dwMinTextureWidth = 1; d->dwMinTextureHeight = 1;
    d->dwMaxTextureWidth = 2048; d->dwMaxTextureHeight = 2048;
    d->dwMinStippleWidth = 1; d->dwMaxStippleWidth = 1;
    d->dwMinStippleHeight = 1; d->dwMaxStippleHeight = 1;
    d->dwMaxTextureRepeat = 256;
    d->dwMaxTextureAspectRatio = 2048;
    d->dwMaxAnisotropy = 1;

    d->wMaxTextureBlendStages = 1;
    d->wMaxSimultaneousTextures = 1;
}

static uint32_t WINAPI d3d123_EnumDevices(void *self, void *cb_, void *ctx)
{
    (void)self;
    typedef uint32_t (WINAPI *cb_t)(const guid_t *, char *, char *, d3ddevicedesc_wire_t *, d3ddevicedesc_wire_t *, void *);
    if (cb_ == NULL) return DD_OK;
    d3ddevicedesc_wire_t hal_desc, hel_desc;
    fill_d3d3_device_desc(&hal_desc);
    fill_d3d3_device_desc(&hel_desc);
    char description[] = "longroad2 stub Direct3D device";
    char name[] = "RGB Emulation";
    ((cb_t)cb_)(&IID_IDirect3DRGBDevice, description, name, &hal_desc, &hel_desc, ctx);
    return DD_OK;
}

/* Real callback shape (DDPIXELFORMAT*, void*) is unchanged from D3D7's
   own EnumZBufferFormats (see d3d7_EnumZBufferFormats in d3d7.c) --
   same reasoning too: offering a plausible Z-buffer depth is what
   satisfies this check ("can not enum z formats" is fatal to real
   games from this era otherwise), one fixed 16-bit format is a
   simplification but real depth-test/write/func against an actual
   depth/stencil renderbuffer IS genuinely real elsewhere. */
static uint32_t WINAPI d3d123_EnumZBufferFormats(void *self, guid_t *device_iid, void *cb_, void *ctx)
{
    (void)self; (void)device_iid;
    typedef uint32_t (WINAPI *cb_t)(ddpixelformat_t *, void *);
    if (cb_ == NULL) return DD_OK;

    ddpixelformat_t fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.dwSize = sizeof(fmt);
    fmt.dwFlags = DDPF_ZBUFFER;
    fmt.dwRGBBitCount = 16; /* dwZBufferBitDepth (same union slot) */
    ((cb_t)cb_)(&fmt, ctx);
    return DD_OK;
}

/* --- IDirect3DDevice3 --- */

typedef struct {
    comfn2_t QueryInterface;
    comfn0_t AddRef;
    comfn0_t Release;
    comfn2_t GetCaps;
    comfn1_t GetStats;
    comfn1_t AddViewport;
    comfn1_t DeleteViewport;
    comfn3_t NextViewport;
    comfn2_t EnumTextureFormats;
    comfn0_t BeginScene;
    comfn0_t EndScene;
    comfn1_t GetDirect3D;
    comfn1_t SetCurrentViewport;
    comfn1_t GetCurrentViewport;
    comfn2_t SetRenderTarget;
    comfn1_t GetRenderTarget;
    comfn3_t Begin;
    comfn5_t BeginIndexed;
    comfn1_t Vertex;
    comfn1_t Index;
    comfn1_t End;
    comfn2_t GetRenderState;
    comfn2_t SetRenderState;
    comfn2_t GetLightState;
    comfn2_t SetLightState;
    comfn2_t SetTransform;
    comfn2_t GetTransform;
    comfn2_t MultiplyTransform;
    comfn5_t DrawPrimitive;
    comfn7_t DrawIndexedPrimitive;
    comfn1_t SetClipStatus;
    comfn1_t GetClipStatus;
    comfn5_t DrawPrimitiveStrided;
    comfn7_t DrawIndexedPrimitiveStrided;
    comfn5_t DrawPrimitiveVB;
    comfn5_t DrawIndexedPrimitiveVB;
    comfn5_t ComputeSphereVisibility;
    comfn2_t GetTexture;
    comfn2_t SetTexture;
    comfn3_t GetTextureStageState;
    comfn3_t SetTextureStageState;
    comfn1_t ValidateDevice;
} d3d_device3_vtbl_t;

#define MAX_MATERIAL_HANDLES 64
#define MAX_TEXTURE_HANDLES 64

typedef struct {
    const d3d_device3_vtbl_t *lpVtbl;
    atomic_uint refcount;
    d3d123_t *d3d_parent;      /* AddRef'd at CreateDevice time */
    surface7_t *render_target; /* AddRef'd -- mirrors d3d_device7_t's own render_target handling in d3d7.c */
    void *current_viewport;    /* opaque d3d_viewport3_t*, set by SetCurrentViewport -- NOT AddRef'd, see d3d_viewport3_t's own active_device comment */
    void *inner7;              /* real, standalone d3d_device7_t* this device delegates actual rendering to -- see d3d7.h */
    /* Which d3d_light_t currently occupies each of inner7's MAX_LIGHTS
       slots, if any -- see d3d_device3_activate_light/deactivate_light. */
    d3d_light_t *light_slot_owner[MAX_VIEWPORT_LIGHTS];
    /* Device-scoped material handle table for SetLightState(MATERIAL, handle)
       -- AddRef'd on first registration, Release'd at device teardown; see
       d3d_device3_register_material_handle's own comment for why this is a
       deliberate simplification vs. real global handle space. */
    d3d_material3_t *material_handles[MAX_MATERIAL_HANDLES];
    /* Same idea, for IDirect3DTexture2::GetHandle -- see
       d3d_device3_register_texture_handle's own comment for why a real
       small-integer handle table exists here at all (nothing in this
       codebase currently resolves a texture handle back to an object,
       unlike materials' SetLightState, but the HANDLE VALUE ITSELF must
       still be a small opaque token, not a raw pointer, to match real
       D3D IM semantics -- see [[project_swep1rcr_port_scoping]]). */
    d3d_texture2_t *texture_handles[MAX_TEXTURE_HANDLES];
} d3d_device3_t;

static const d3d_device3_vtbl_t g_d3d_device3_vtbl;

static uint32_t WINAPI d3d_device3_QueryInterface(void *self, guid_t *riid, void **out)
{
    d3d_device3_t *dev = (d3d_device3_t *)self;
    if (out == NULL) return DDERR_INVALIDPARAMS;
    if (guid_equal(riid, &IID_IDirect3DDevice3)) {
        atomic_fetch_add(&dev->refcount, 1u);
        *out = self;
        return DD_OK;
    }
    *out = NULL;
    return DDERR_NOINTERFACE;
}
static uint32_t WINAPI d3d_device3_AddRef(void *self)
{
    return atomic_fetch_add(&((d3d_device3_t *)self)->refcount, 1u) + 1u;
}
static uint32_t WINAPI d3d_device3_Release(void *self)
{
    d3d_device3_t *dev = (d3d_device3_t *)self;
    if (atomic_load(&dev->refcount) == 0) {
        fprintf(stderr, "[ddraw] BUG: Release called on d3d_device3 %p with refcount already 0 (over-release)\n", self);
        return 0;
    }
    uint32_t rc = atomic_fetch_sub(&dev->refcount, 1u) - 1u;
    if (rc == 0) {
        if (dev->d3d_parent != NULL) d3d123_Release(dev->d3d_parent);
        if (dev->render_target != NULL) surface7_Release(dev->render_target);
        if (dev->inner7 != NULL) d3d7_device_release(dev->inner7);
        for (int i = 0; i < MAX_MATERIAL_HANDLES; i++) {
            if (dev->material_handles[i] != NULL) d3d_material3_Release(dev->material_handles[i]);
        }
        for (int i = 0; i < MAX_TEXTURE_HANDLES; i++) {
            if (dev->texture_handles[i] != NULL) d3d_texture2_Release(dev->texture_handles[i]);
        }
        free(dev);
    }
    return rc;
}

/* Real, live-debugged bug (found tracing why real Wine renders Porsche.exe's
   main menu fully but this port shows black -- see
   [[project_porsche_intro_video_black_screen]]): this used to be a raw
   dwSize-bounded 0xFF-filled blob, same old pattern d3d123_EnumDevices's
   own callback (fill_d3d3_device_desc, right above) was ALREADY fixed away
   from -- but this GetCaps method, the one dx7z.dll's own decompiled
   "DX7_createD3D()" init routine actually calls directly
   (docs/game_code/dx7z.dll.c, vtable offset 0xc on what its own error
   string calls "IDirect3D3" but is really an IDirect3DDevice3*, matching
   real IDirect3DDevice3::GetCaps's vtable position exactly), was never
   updated to match. Every DWORD field of the returned D3DDEVICEDESC
   (dwMaxTextureWidth, dvMaxVertexW-adjacent fields, etc.) came back as
   raw 0xFFFFFFFF -- and dx7z.dll reads some of those fields DIRECTLY into
   float locals (_THRASH_setstate_8's own `fVar1 = DAT_60034004;`, fed
   straight from one of these caps fields), where the raw bit pattern
   0xFFFFFFFF decodes as a quiet NaN, not a large number -- corrupting
   every downstream computation that touches it. A real, well-known
   result of the earlier SWEP1RCR "gasoline in water" texture-distortion
   investigation already established the SAME 0xFF-fill idiom as a real
   bug elsewhere in this exact struct family; this call site was simply
   missed at the time. Fixed by reusing the same real, sane HAL-plausible
   caps fill_d3d3_device_desc already provides EnumDevices' callback with,
   for both the HAL and HEL (hardware-emulation-layer) output structs --
   real D3D7-class hardware reports the same real capabilities for both
   here. */
static uint32_t WINAPI d3d_device3_GetCaps(void *self, uint32_t *hal_desc, uint32_t *hel_desc)
{
    (void)self;
    if (hal_desc != NULL) fill_d3d3_device_desc((d3ddevicedesc_wire_t *)hal_desc);
    if (hel_desc != NULL) fill_d3d3_device_desc((d3ddevicedesc_wire_t *)hel_desc);
    return DD_OK;
}
static uint32_t WINAPI d3d_device3_NextViewport(void *self, void *ref, void **out, uint32_t flags)
{ (void)self; (void)ref; (void)flags; if (out != NULL) *out = NULL; return DDERR_GENERIC; }
/* Used to report a single fabricated RGB565-only format (via
   fill_pixfmt_for_bpp) -- a real, confirmed bug (see
   [[project_swep1rcr_port_scoping]]'s "gasoline in water" texture-
   distortion investigation): RGB565 has ZERO alpha bits, so ANY texture
   this device advertised support for via this call could never actually
   carry real alpha data, and a game whose own asset pipeline expects an
   alpha-capable 16-bit format (ARGB1555/ARGB4444, standard for this
   game's era) writing its own pixel data into a surface created at
   RGB565 gets every pixel's bits reinterpreted through the wrong 5-6-5
   channel boundaries -- exactly the shifting/scrambled color corruption
   reported live. d3d_device7_EnumTextureFormats (d3d7.c, king.exe's own
   D3D7 path) already offers a real, broad set of formats including
   proper alpha ones -- this delegates to that exact same, already-tested
   logic through inner7 (same pattern as every other D3D3->D3D7 bridge
   method in this file) instead of duplicating a second, narrower one. */
static uint32_t WINAPI d3d_device3_EnumTextureFormats(void *self, void *cb_, void *ctx)
{
    d3d_device3_t *dev = (d3d_device3_t *)self;
    return d3d7_device_enum_texture_formats(dev->inner7, cb_, ctx);
}
static uint32_t WINAPI d3d_device3_GetDirect3D(void *self, void **out_d3d)
{
    d3d_device3_t *dev = (d3d_device3_t *)self;
    if (out_d3d == NULL) return DDERR_INVALIDPARAMS;
    if (dev->d3d_parent != NULL) d3d123_AddRef(dev->d3d_parent);
    *out_d3d = dev->d3d_parent;
    return DD_OK;
}
/* ----- inner7 delegation helpers (forward-declared near d3d_light_t/
   d3d_viewport3_t further up -- real bodies here, now that inner7/
   light_slot_owner/material_handles are known). See
   [[project_swep1rcr_port_scoping]]. */
static void d3d_device3_activate_light(void *device_, d3d_light_t *light)
{
    d3d_device3_t *dev = (d3d_device3_t *)device_;
    if (!light->light_set) return; /* SetLight hasn't been called yet -- nothing real to push */
    int slot = light->assigned_slot;
    if (light->assigned_device != device_ || slot < 0) {
        slot = -1;
        for (int i = 0; i < MAX_VIEWPORT_LIGHTS; i++) {
            if (dev->light_slot_owner[i] == NULL) { slot = i; break; }
        }
        if (slot < 0) return; /* out of slots -- real hardware would also just not light this one */
        dev->light_slot_owner[slot] = light;
        light->assigned_device = device_;
        light->assigned_slot = slot;
    }
    d3d7_device_set_light(dev->inner7, (uint32_t)slot, &light->light);
    d3d7_device_light_enable(dev->inner7, (uint32_t)slot, 1);
}
static void d3d_device3_deactivate_light(void *device_, d3d_light_t *light)
{
    d3d_device3_t *dev = (d3d_device3_t *)device_;
    if (light->assigned_device != device_ || light->assigned_slot < 0) return;
    d3d7_device_light_enable(dev->inner7, (uint32_t)light->assigned_slot, 0);
    dev->light_slot_owner[light->assigned_slot] = NULL;
    light->assigned_device = NULL;
    light->assigned_slot = -1;
}
static void d3d_device3_apply_viewport(void *device_, const d3dviewport7_t *vp)
{
    d3d_device3_t *dev = (d3d_device3_t *)device_;
    d3dviewport7_t copy = *vp;
    d3d7_device_set_viewport(dev->inner7, &copy);
}
/* Real handles are never 0 (0 means "no material" to SetLightState).
   Idempotent like the real API (calling GetHandle twice on the same
   material returns the same handle). Fixed-size, device-scoped table --
   deliberate simplification vs. real global handle space: this shim, like
   every other object in it, only ever has one device alive per session. */
static uint32_t d3d_device3_register_material_handle(void *device_, d3d_material3_t *mat, uint32_t *out_handle)
{
    d3d_device3_t *dev = (d3d_device3_t *)device_;
    for (int i = 0; i < MAX_MATERIAL_HANDLES; i++) {
        if (dev->material_handles[i] == mat) { *out_handle = (uint32_t)i + 1; return DD_OK; }
    }
    for (int i = 0; i < MAX_MATERIAL_HANDLES; i++) {
        if (dev->material_handles[i] == NULL) {
            d3d_material3_AddRef(mat); /* kept alive for the handle's lifetime, released at device teardown */
            dev->material_handles[i] = mat;
            *out_handle = (uint32_t)i + 1;
            return DD_OK;
        }
    }
    return DDERR_OUTOFMEMORY;
}

/* Same shape as d3d_device3_register_material_handle right above --
   handle = index+1 (1-based, 0 stays reserved as "invalid" per real D3D
   handle convention), first-fit reuse of an empty slot, AddRef'd for
   the handle's lifetime and Release'd at device teardown (see
   d3d_device3_Release). See d3d_texture2_GetHandle's own comment for
   why this table exists at all despite nothing here resolving a handle
   back to a texture (unlike materials' SetLightState). */
static uint32_t d3d_device3_register_texture_handle(void *device_, d3d_texture2_t *tex, uint32_t *out_handle)
{
    d3d_device3_t *dev = (d3d_device3_t *)device_;
    for (int i = 0; i < MAX_TEXTURE_HANDLES; i++) {
        if (dev->texture_handles[i] == tex) { *out_handle = (uint32_t)i + 1; return DD_OK; }
    }
    for (int i = 0; i < MAX_TEXTURE_HANDLES; i++) {
        if (dev->texture_handles[i] == NULL) {
            d3d_texture2_AddRef(tex); /* kept alive for the handle's lifetime, released at device teardown */
            dev->texture_handles[i] = tex;
            *out_handle = (uint32_t)i + 1;
            return DD_OK;
        }
    }
    return DDERR_OUTOFMEMORY;
}

static uint32_t WINAPI d3d_device3_BeginScene(void *self)
{ return d3d7_device_begin_scene(((d3d_device3_t *)self)->inner7); }
static uint32_t WINAPI d3d_device3_EndScene(void *self)
{ return d3d7_device_end_scene(((d3d_device3_t *)self)->inner7); }

/* Real SetCurrentViewport(NULL) is legal (detaches). Pushes the newly-
   current viewport's stored D3DVIEWPORT2 (if any) and every light it has
   attached into inner7 -- see d3d_viewport3_t's own comment for why
   active_device isn't AddRef'd. */
static uint32_t WINAPI d3d_device3_SetCurrentViewport(void *self, void *viewport_)
{
    d3d_device3_t *dev = (d3d_device3_t *)self;
    d3d_viewport3_t *old = (d3d_viewport3_t *)dev->current_viewport;
    d3d_viewport3_t *vp = (d3d_viewport3_t *)viewport_;
    if (old != NULL && old != vp) old->active_device = NULL;
    dev->current_viewport = viewport_;
    if (vp != NULL) {
        vp->active_device = self;
        if (vp->viewport_set) d3d_device3_apply_viewport(self, &vp->viewport);
        for (int i = 0; i < vp->light_count; i++) {
            if (vp->lights[i] != NULL) d3d_device3_activate_light(self, vp->lights[i]);
        }
    }
    return DD_OK;
}
static uint32_t WINAPI d3d_device3_GetCurrentViewport(void *self, void **out_viewport)
{
    d3d_device3_t *dev = (d3d_device3_t *)self;
    if (out_viewport == NULL) return DDERR_INVALIDPARAMS;
    *out_viewport = dev->current_viewport;
    return DD_OK;
}
static uint32_t WINAPI d3d_device3_SetRenderTarget(void *self, surface7_t *surface, uint32_t flags)
{
    d3d_device3_t *dev = (d3d_device3_t *)self;
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] Device3::SetRenderTarget: device=%p surface=%p (was %p)\n",
                self, (void *)surface, (void *)dev->render_target);
    }
    if (surface != NULL) surface7_AddRef(surface);
    if (dev->render_target != NULL) surface7_Release(dev->render_target);
    dev->render_target = surface;
    d3d7_device_set_render_target(dev->inner7, surface, flags);
    return DD_OK;
}
static uint32_t WINAPI d3d_device3_GetRenderTarget(void *self, surface7_t **out_surface)
{
    d3d_device3_t *dev = (d3d_device3_t *)self;
    if (out_surface == NULL) return DDERR_INVALIDPARAMS;
    if (dev->render_target != NULL) surface7_AddRef(dev->render_target);
    *out_surface = dev->render_target;
    return DD_OK;
}

static uint32_t WINAPI d3d_device3_SetRenderState(void *self, uint32_t state, uint32_t value)
{ return d3d7_device_set_render_state(((d3d_device3_t *)self)->inner7, state, value); }
static uint32_t WINAPI d3d_device3_GetRenderState(void *self, uint32_t state, uint32_t *out_value)
{ return d3d7_device_get_render_state(((d3d_device3_t *)self)->inner7, state, out_value); }

/* Real D3DRENDERSTATETYPE values -- same numbers d3d7.c's own private
   D3DRENDERSTATE_* #defines use (cross-checked there against real
   headers already); duplicated here since these are just Microsoft's
   fixed enum values, not something that could drift between the files. */
#define D3DRENDERSTATE_AMBIENT_       139u
#define D3DRENDERSTATE_FOGVERTEXMODE_ 140u
#define D3DRENDERSTATE_COLORVERTEX_   141u
#define D3DRENDERSTATE_FOGSTART_      36u
#define D3DRENDERSTATE_FOGEND_        37u
#define D3DRENDERSTATE_FOGDENSITY_    38u

#define D3DLIGHTSTATE_MATERIAL    1u
#define D3DLIGHTSTATE_AMBIENT     2u
#define D3DLIGHTSTATE_COLORMODEL  3u
#define D3DLIGHTSTATE_FOGMODE     4u
#define D3DLIGHTSTATE_FOGSTART    5u
#define D3DLIGHTSTATE_FOGEND      6u
#define D3DLIGHTSTATE_FOGDENSITY  7u
#define D3DLIGHTSTATE_COLORVERTEX 8u

/* D3DLIGHTSTATETYPE has no 1:1 render-state array the way D3D7 flattens
   RenderState, so this dispatches by hand rather than delegating
   wholesale -- MATERIAL looks the handle up in the device's own table;
   AMBIENT/FOGMODE/FOGSTART/FOGEND/FOGDENSITY/COLORVERTEX forward to the
   equivalent already-real D3DRENDERSTATE_* slot d3d7.c's own pipeline
   already reads (FOGMODE -> FOGVERTEXMODE specifically, NOT FOGTABLEMODE
   -- d3d7.c's upload_fog_uniforms only falls back to FOGVERTEXMODE when
   FOGTABLEMODE is D3DFOG_NONE, so mapping to the wrong one would
   silently do nothing in the common case). COLORMODEL has no pipeline
   equivalent -- harmless no-op. */
static uint32_t WINAPI d3d_device3_SetLightState(void *self, uint32_t state, uint32_t value)
{
    d3d_device3_t *dev = (d3d_device3_t *)self;
    switch (state) {
        case D3DLIGHTSTATE_MATERIAL: {
            if (value == 0) return DD_OK; /* real semantics: 0 = "no material" */
            uint32_t idx = value - 1;
            if (idx >= MAX_MATERIAL_HANDLES || dev->material_handles[idx] == NULL) return DDERR_INVALIDPARAMS;
            return d3d7_device_set_material(dev->inner7, &dev->material_handles[idx]->material);
        }
        case D3DLIGHTSTATE_AMBIENT:
            return d3d7_device_set_render_state(dev->inner7, D3DRENDERSTATE_AMBIENT_, value);
        case D3DLIGHTSTATE_FOGMODE:
            return d3d7_device_set_render_state(dev->inner7, D3DRENDERSTATE_FOGVERTEXMODE_, value);
        case D3DLIGHTSTATE_FOGSTART:
            return d3d7_device_set_render_state(dev->inner7, D3DRENDERSTATE_FOGSTART_, value);
        case D3DLIGHTSTATE_FOGEND:
            return d3d7_device_set_render_state(dev->inner7, D3DRENDERSTATE_FOGEND_, value);
        case D3DLIGHTSTATE_FOGDENSITY:
            return d3d7_device_set_render_state(dev->inner7, D3DRENDERSTATE_FOGDENSITY_, value);
        case D3DLIGHTSTATE_COLORVERTEX:
            return d3d7_device_set_render_state(dev->inner7, D3DRENDERSTATE_COLORVERTEX_, value);
        default: /* COLORMODEL -- no pipeline equivalent */
            return DD_OK;
    }
}
static uint32_t WINAPI d3d_device3_GetLightState(void *self, uint32_t state, uint32_t *out_value)
{
    d3d_device3_t *dev = (d3d_device3_t *)self;
    if (out_value == NULL) return DDERR_INVALIDPARAMS;
    switch (state) {
        case D3DLIGHTSTATE_AMBIENT:
            return d3d7_device_get_render_state(dev->inner7, D3DRENDERSTATE_AMBIENT_, out_value);
        case D3DLIGHTSTATE_FOGMODE:
            return d3d7_device_get_render_state(dev->inner7, D3DRENDERSTATE_FOGVERTEXMODE_, out_value);
        case D3DLIGHTSTATE_FOGSTART:
            return d3d7_device_get_render_state(dev->inner7, D3DRENDERSTATE_FOGSTART_, out_value);
        case D3DLIGHTSTATE_FOGEND:
            return d3d7_device_get_render_state(dev->inner7, D3DRENDERSTATE_FOGEND_, out_value);
        case D3DLIGHTSTATE_FOGDENSITY:
            return d3d7_device_get_render_state(dev->inner7, D3DRENDERSTATE_FOGDENSITY_, out_value);
        case D3DLIGHTSTATE_COLORVERTEX:
            return d3d7_device_get_render_state(dev->inner7, D3DRENDERSTATE_COLORVERTEX_, out_value);
        default: /* MATERIAL (handle round-trip not needed) / COLORMODEL */
            *out_value = 0;
            return DD_OK;
    }
}

static uint32_t WINAPI d3d_device3_SetTransform(void *self, uint32_t state, d3dmatrix_t *matrix)
{ return d3d7_device_set_transform(((d3d_device3_t *)self)->inner7, state, matrix); }
static uint32_t WINAPI d3d_device3_GetTransform(void *self, uint32_t state, d3dmatrix_t *out_matrix)
{ return d3d7_device_get_transform(((d3d_device3_t *)self)->inner7, state, out_matrix); }
static uint32_t WINAPI d3d_device3_MultiplyTransform(void *self, uint32_t state, d3dmatrix_t *matrix)
{ return d3d7_device_multiply_transform(((d3d_device3_t *)self)->inner7, state, matrix); }

static uint32_t WINAPI d3d_device3_SetTexture(void *self, uint32_t stage, void *texture)
{
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] Device3::SetTexture: device=%p stage=%u texture=%p\n", self, stage, texture);
    }
    return d3d7_device_set_texture(((d3d_device3_t *)self)->inner7, stage, texture);
}
static uint32_t WINAPI d3d_device3_GetTexture(void *self, uint32_t stage, void **out_texture)
{ return d3d7_device_get_texture(((d3d_device3_t *)self)->inner7, stage, out_texture); }
static uint32_t WINAPI d3d_device3_SetTextureStageState(void *self, uint32_t stage, uint32_t state, uint32_t value)
{ return d3d7_device_set_texture_stage_state(((d3d_device3_t *)self)->inner7, stage, state, value); }
static uint32_t WINAPI d3d_device3_GetTextureStageState(void *self, uint32_t stage, uint32_t state, uint32_t *out_value)
{ return d3d7_device_get_texture_stage_state(((d3d_device3_t *)self)->inner7, stage, state, out_value); }

static uint32_t WINAPI d3d_device3_DrawPrimitive(void *self, uint32_t prim_type, uint32_t fvf,
                                                  uint32_t vertices, uint32_t count, uint32_t flags)
{ return d3d7_device_draw_primitive(((d3d_device3_t *)self)->inner7, prim_type, fvf, (void *)(uintptr_t)vertices, count, flags); }
static uint32_t WINAPI d3d_device3_DrawIndexedPrimitive(void *self, uint32_t prim_type, uint32_t fvf,
                                                         uint32_t vertices, uint32_t vertex_count,
                                                         uint32_t indices, uint32_t index_count, uint32_t flags)
{
    return d3d7_device_draw_indexed_primitive(((d3d_device3_t *)self)->inner7, prim_type, fvf,
                                               (void *)(uintptr_t)vertices, vertex_count,
                                               (void *)(uintptr_t)indices, index_count, flags);
}
static uint32_t WINAPI d3d_device3_DrawPrimitiveStrided(void *self, uint32_t prim_type, uint32_t fvf,
                                                         uint32_t strided_data, uint32_t count, uint32_t flags)
{
    return d3d7_device_draw_primitive_strided(((d3d_device3_t *)self)->inner7, prim_type, fvf,
                                               (void *)(uintptr_t)strided_data, count, flags);
}
static uint32_t WINAPI d3d_device3_DrawIndexedPrimitiveStrided(void *self, uint32_t prim_type, uint32_t fvf,
                                                                uint32_t strided_data, uint32_t vertex_count,
                                                                uint32_t indices, uint32_t index_count, uint32_t flags)
{
    return d3d7_device_draw_indexed_primitive_strided(((d3d_device3_t *)self)->inner7, prim_type, fvf,
                                                       (void *)(uintptr_t)strided_data, vertex_count,
                                                       (void *)(uintptr_t)indices, index_count, flags);
}
/* Real signature (D3DPRIMITIVETYPE, IDirect3DVertexBuffer*, start_vertex,
   count, flags) -- vb is THIS shim's own d3d_vertexbuffer3_t tear-off
   (see its type comment above), unwrapped to its real inner vb7_t before
   reaching d3d7's already-real vertex-buffer draw path. Was a no-op
   (`ok5`) before this -- see [[project_swep1rcr_port_scoping]]'s "no 3D
   in gameplay" investigation. */
static uint32_t WINAPI d3d_device3_DrawPrimitiveVB(void *self, uint32_t prim_type, uint32_t vb,
                                                    uint32_t start_vertex, uint32_t count, uint32_t flags)
{
    d3d_vertexbuffer3_t *v = (d3d_vertexbuffer3_t *)(uintptr_t)vb;
    if (v == NULL) return DDERR_INVALIDPARAMS;
    return d3d7_device_draw_primitive_vb(((d3d_device3_t *)self)->inner7, prim_type, v->inner_vb,
                                          start_vertex, count, flags);
}
/* Real D3D3 signature (D3DPRIMITIVETYPE, IDirect3DVertexBuffer*, WORD
   *indices, index_count, flags) -- genuinely narrower than D3D7's own
   DrawIndexedPrimitiveVB (no separate start_vertex/vertex_count, see
   d3d7.c's own d3d_device7_DrawIndexedPrimitiveVB3 comment for why this
   needed its own bridge function rather than reusing the 7-arg one). */
static uint32_t WINAPI d3d_device3_DrawIndexedPrimitiveVB(void *self, uint32_t prim_type, uint32_t vb,
                                                           uint32_t indices, uint32_t index_count, uint32_t flags)
{
    d3d_vertexbuffer3_t *v = (d3d_vertexbuffer3_t *)(uintptr_t)vb;
    if (v == NULL) return DDERR_INVALIDPARAMS;
    return d3d7_device_draw_indexed_primitive_vb(((d3d_device3_t *)self)->inner7, prim_type, v->inner_vb,
                                                  (void *)(uintptr_t)indices, index_count, flags);
}

/* ret[] is a per-sphere DWORD OUTPUT array sized by sphere_count -- left
   uninitialized, a game reading it back to decide frustum culling could
   behave erratically (randomly skip/draw objects based on stack
   garbage); zeroing it (matching D3DCLIPPED/"not visible"... actually
   0 == D3DSTATUS_DEFAULT-ish "fully visible" is the conservative choice
   here, consistent with this shim drawing everything unconditionally
   rather than culling anything). */
static uint32_t WINAPI d3d_device3_ComputeSphereVisibility(void *self, void *centers, void *radii,
                                                            uint32_t count, uint32_t flags, uint32_t *ret)
{
    (void)self; (void)centers; (void)radii; (void)flags;
    if (ret != NULL && count > 0) memset(ret, 0, (size_t)count * sizeof(uint32_t));
    return DD_OK;
}
static uint32_t WINAPI d3d_device3_ValidateDevice(void *self, uint32_t *out_passes)
{ return d3d7_device_validate_device(((d3d_device3_t *)self)->inner7, out_passes); }

static const d3d_device3_vtbl_t g_d3d_device3_vtbl = {
    .QueryInterface = (comfn2_t)d3d_device3_QueryInterface,
    .AddRef = d3d_device3_AddRef,
    .Release = d3d_device3_Release,
    .GetCaps = (comfn2_t)d3d_device3_GetCaps,
    .GetStats = fail1,
    .AddViewport = log_device3_AddViewport,
    .DeleteViewport = log_device3_DeleteViewport,
    .NextViewport = (comfn3_t)d3d_device3_NextViewport,
    .EnumTextureFormats = (comfn2_t)d3d_device3_EnumTextureFormats,
    .BeginScene = d3d_device3_BeginScene,
    .EndScene = d3d_device3_EndScene,
    .GetDirect3D = (comfn1_t)d3d_device3_GetDirect3D,
    .SetCurrentViewport = (comfn1_t)d3d_device3_SetCurrentViewport,
    .GetCurrentViewport = (comfn1_t)d3d_device3_GetCurrentViewport,
    .SetRenderTarget = (comfn2_t)d3d_device3_SetRenderTarget,
    .GetRenderTarget = (comfn1_t)d3d_device3_GetRenderTarget,
    .Begin = log_device3_Begin,
    .BeginIndexed = log_device3_BeginIndexed,
    .Vertex = log_device3_Vertex,
    .Index = log_device3_Index,
    .End = log_device3_End,
    .GetRenderState = (comfn2_t)d3d_device3_GetRenderState,
    .SetRenderState = d3d_device3_SetRenderState,
    .GetLightState = (comfn2_t)d3d_device3_GetLightState,
    .SetLightState = d3d_device3_SetLightState,
    .SetTransform = (comfn2_t)d3d_device3_SetTransform,
    .GetTransform = (comfn2_t)d3d_device3_GetTransform,
    .MultiplyTransform = (comfn2_t)d3d_device3_MultiplyTransform,
    .DrawPrimitive = d3d_device3_DrawPrimitive,
    .DrawIndexedPrimitive = d3d_device3_DrawIndexedPrimitive,
    .SetClipStatus = log_device3_SetClipStatus,
    .GetClipStatus = log_device3_GetClipStatus,
    .DrawPrimitiveStrided = d3d_device3_DrawPrimitiveStrided,
    .DrawIndexedPrimitiveStrided = d3d_device3_DrawIndexedPrimitiveStrided,
    .DrawPrimitiveVB = (comfn5_t)d3d_device3_DrawPrimitiveVB,
    .DrawIndexedPrimitiveVB = (comfn5_t)d3d_device3_DrawIndexedPrimitiveVB,
    .ComputeSphereVisibility = (comfn5_t)d3d_device3_ComputeSphereVisibility,
    .GetTexture = (comfn2_t)d3d_device3_GetTexture,
    .SetTexture = (comfn2_t)d3d_device3_SetTexture,
    .GetTextureStageState = (comfn3_t)d3d_device3_GetTextureStageState,
    .SetTextureStageState = d3d_device3_SetTextureStageState,
    .ValidateDevice = (comfn1_t)d3d_device3_ValidateDevice,
};

/* IDirect3D3::CreateDevice -- the one method in this whole legacy block
   that actually needs to succeed with a REAL, usable object: this is
   the call SWEP1RCR's own 3D init treats as fatal if it fails, and
   everything upstream of it (DirectDrawCreate/QueryInterface chains) only
   exists to reach this point. Takes a reference on the initial render-
   target surface, mirroring d3d7_CreateDevice's own established pattern
   in d3d7.c. */
static uint32_t WINAPI d3d3_CreateDevice(void *self, guid_t *rclsid, surface7_t *surface, void **out_device, void *outer)
{
    (void)rclsid; (void)outer;
    if (out_device == NULL) return DDERR_INVALIDPARAMS;
    d3d_device3_t *dev = calloc(1, sizeof(*dev));
    dev->lpVtbl = &g_d3d_device3_vtbl;
    atomic_init(&dev->refcount, 1u);
    d3d123_t *parent = (d3d123_t *)self;
    d3d123_AddRef(parent);
    dev->d3d_parent = parent;
    /* Real rendering delegate -- see d3d7.h's own header comment for why
       this is the actual point of this whole legacy block: D3D3 and D3D7
       share the same fixed-function pipeline underneath, just reached
       through a different, older COM surface. */
    dev->inner7 = d3d7_device_create_standalone();
    if (surface != NULL) {
        surface7_AddRef(surface);
        dev->render_target = surface;
        d3d7_device_set_render_target(dev->inner7, surface, 0);
    }
    *out_device = dev;
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] IDirect3D3::CreateDevice: d3d=%p surface=%p -> device=%p\n",
                self, (void *)surface, (void *)dev);
    }
    return DD_OK;
}

static const d3d123_vtbl_t g_d3d1_vtbl = {
    .QueryInterface = (comfn2_t)d3d123_QueryInterface,
    .AddRef = d3d123_AddRef,
    .Release = d3d123_Release,
    .EnumDevices = (comfn2_t)d3d123_EnumDevices,
    .CreateLight = fail2,
    .CreateMaterial = fail2,
    .CreateViewport = fail2,
    .FindDevice = fail2,
    .CreateDevice = fail4_CreateDevice_v1, /* real IDirect3D (v1) has no CreateDevice at all -- unreachable from a real v1 caller regardless */
};

static const d3d123_vtbl_t g_d3d2_vtbl = {
    .QueryInterface = (comfn2_t)d3d123_QueryInterface,
    .AddRef = d3d123_AddRef,
    .Release = d3d123_Release,
    .EnumDevices = (comfn2_t)d3d123_EnumDevices,
    .CreateLight = fail2,
    .CreateMaterial = fail2,
    .CreateViewport = fail2,
    .FindDevice = fail2,
    .CreateDevice = (comfn4_t)fail3, /* real v2 CreateDevice is 3-arg -- see this section's header comment for why Device2 isn't built out */
};

static const guid_t IID_IDirect3DVertexBuffer_ =
    { 0x7a503555, 0x4a83, 0x11d1, { 0xa5, 0xdb, 0x00, 0xa0, 0xc9, 0x03, 0x67, 0xf8 } };

static uint32_t WINAPI d3d_vertexbuffer3_QueryInterface(void *self, guid_t *riid, void **out)
{
    d3d_vertexbuffer3_t *vb = (d3d_vertexbuffer3_t *)self;
    if (out == NULL) return DDERR_INVALIDPARAMS;
    if (riid == NULL || guid_equal(riid, &IID_IDirect3DVertexBuffer_)) {
        atomic_fetch_add(&vb->refcount, 1u);
        *out = self;
        return DD_OK;
    }
    *out = NULL;
    return DDERR_NOINTERFACE;
}
static uint32_t WINAPI d3d_vertexbuffer3_AddRef(void *self)
{ return atomic_fetch_add(&((d3d_vertexbuffer3_t *)self)->refcount, 1u) + 1u; }
static uint32_t WINAPI d3d_vertexbuffer3_Release(void *self)
{
    d3d_vertexbuffer3_t *vb = (d3d_vertexbuffer3_t *)self;
    if (atomic_load(&vb->refcount) == 0) {
        fprintf(stderr, "[ddraw] BUG: Release called on d3d_vertexbuffer3 %p with refcount already 0 (over-release)\n", self);
        return 0;
    }
    uint32_t rc = atomic_fetch_sub(&vb->refcount, 1u) - 1u;
    if (rc == 0) {
        d3d7_vb_release(vb->inner_vb);
        free(vb);
    }
    return rc;
}
static uint32_t WINAPI d3d_vertexbuffer3_Lock(void *self, uint32_t flags, void **out_data, uint32_t *out_size)
{ return d3d7_vb_lock(((d3d_vertexbuffer3_t *)self)->inner_vb, flags, out_data, out_size); }
static uint32_t WINAPI d3d_vertexbuffer3_Unlock(void *self)
{ return d3d7_vb_unlock(((d3d_vertexbuffer3_t *)self)->inner_vb); }
/* Real semantics: transform+light this buffer's vertices into src_buffer's
   own slot in some OTHER vertex buffer for later reuse -- meaningless in
   this pipeline (transforms/lights per-draw already), matching the exact
   same no-op rationale as d3d7.c's own vb7_ProcessVertices. */
static uint32_t WINAPI d3d_vertexbuffer3_ProcessVertices(void *self, uint32_t vertex_op, uint32_t dst_idx, uint32_t count,
                                                           uint32_t src_buffer, uint32_t src_idx, uint32_t device, uint32_t flags)
{
    (void)self; (void)vertex_op; (void)dst_idx; (void)count; (void)src_buffer; (void)src_idx; (void)device; (void)flags;
    return DD_OK;
}
/* Real D3DVERTEXBUFFERDESC is a small, fixed 16-byte struct with no
   version variation between D3D3/D3D7 (confirmed against a real MinGW
   header) -- an unconditional write, same as d3d7.c's own vb7_
   GetVertexBufferDesc this delegates to. */
static uint32_t WINAPI d3d_vertexbuffer3_GetVertexBufferDesc(void *self, uint32_t out_desc)
{
    d3d_vertexbuffer3_t *vb = (d3d_vertexbuffer3_t *)self;
    if (out_desc == 0) return DDERR_INVALIDPARAMS;
    uint32_t caps = 0, fvf = 0, num_vertices = 0;
    uint32_t hr = d3d7_vb_get_desc(vb->inner_vb, &caps, &fvf, &num_vertices);
    if (hr == DD_OK) {
        uint32_t *p = (uint32_t *)(uintptr_t)out_desc;
        p[0] = 16u; /* dwSize */
        p[1] = caps;
        p[2] = fvf;
        p[3] = num_vertices;
    }
    return hr;
}
static uint32_t WINAPI d3d_vertexbuffer3_Optimize(void *self, uint32_t device, uint32_t flags)
{ (void)self; (void)device; (void)flags; return DD_OK; }

static const d3d_vertexbuffer3_vtbl_t g_d3d_vertexbuffer3_vtbl = {
    .QueryInterface = (comfn2_t)d3d_vertexbuffer3_QueryInterface,
    .AddRef = d3d_vertexbuffer3_AddRef,
    .Release = d3d_vertexbuffer3_Release,
    .Lock = (comfn3_t)d3d_vertexbuffer3_Lock,
    .Unlock = d3d_vertexbuffer3_Unlock,
    .ProcessVertices = (comfn7_t)d3d_vertexbuffer3_ProcessVertices,
    .GetVertexBufferDesc = d3d_vertexbuffer3_GetVertexBufferDesc,
    .Optimize = d3d_vertexbuffer3_Optimize,
};

static void *d3d_vertexbuffer3_create(uint32_t caps, uint32_t fvf, uint32_t num_vertices)
{
    d3d_vertexbuffer3_t *vb = calloc(1, sizeof(*vb));
    vb->lpVtbl = &g_d3d_vertexbuffer3_vtbl;
    atomic_init(&vb->refcount, 1u);
    vb->inner_vb = d3d7_vb_create(caps, fvf, num_vertices);
    return vb;
}

/* Real signature (D3DVERTEXBUFFERDESC *desc, IDirect3DVertexBuffer **buffer,
   DWORD flags, IUnknown *outer) -- 4 real args, verified against a real
   MinGW d3d.h (D3D3's own CreateVertexBuffer has one more param than
   D3D7's -- confirmed genuinely different, not a transcription slip).
   Was a hard fail4 before this -- see [[project_swep1rcr_port_scoping]]'s
   "no 3D in gameplay" investigation: CreateVertexBuffer failing outright
   meant any game code building static world geometry through a vertex
   buffer (as opposed to the plain/Strided DrawPrimitive* paths, both
   already real) had nothing to draw from at all. desc is read directly
   off the game's own raw struct (dwSize/dwCaps/dwFVF/dwNumVertices, all
   plain DWORDs at fixed offsets) rather than sharing d3d7.c's internal
   wire-struct type across the ddraw.c/d3d7.c link-cycle boundary, same
   convention as this file's other raw-pointer desc reads. */
static uint32_t WINAPI d3d3_CreateVertexBuffer(void *self, uint32_t desc, uint32_t out_buffer, uint32_t flags, uint32_t outer)
{
    (void)self; (void)flags; (void)outer;
    if (desc == 0 || out_buffer == 0) return DDERR_INVALIDPARAMS;
    const uint32_t *d = (const uint32_t *)(uintptr_t)desc;
    uint32_t caps = d[1], fvf = d[2], num_vertices = d[3];
    *(void **)(uintptr_t)out_buffer = d3d_vertexbuffer3_create(caps, fvf, num_vertices);
    if (ddraw_trace_enabled()) {
        fprintf(stderr, "[ddraw-trace] Device3::CreateVertexBuffer: vb=%p caps=0x%08x fvf=0x%08x num_vertices=%u\n",
                *(void **)(uintptr_t)out_buffer, caps, fvf, num_vertices);
    }
    return DD_OK;
}

/* CreateLight/CreateMaterial/CreateViewport: real signature is (self,
   void **out, IUnknown *outer) -- 2 real args, matches comfn2_t. Only
   wired on the v3 vtable, not v1/v2: real IDirect3D(2)::CreateMaterial/
   CreateViewport return the OLDER, differently-shaped IDirect3DMaterial
   (v1)/IDirect3DViewport (v1) objects (not the v3 ones this shim
   actually builds), and this game only ever reaches these through
   IDirect3D3 (confirmed via a real trace log, same interface its
   CreateDevice call succeeds on) -- same "don't hand back a wrong-
   version-shaped object" discipline already applied to Device2 above. */
static uint32_t WINAPI d3d3_CreateLight(void *self, void **out, void *outer)
{ (void)self; (void)outer; if (out == NULL) return DDERR_INVALIDPARAMS; *out = d3d_light_create(); return DD_OK; }
static uint32_t WINAPI d3d3_CreateMaterial(void *self, void **out, void *outer)
{ (void)self; (void)outer; if (out == NULL) return DDERR_INVALIDPARAMS; *out = d3d_material3_create(); return DD_OK; }
static uint32_t WINAPI d3d3_CreateViewport(void *self, void **out, void *outer)
{ (void)self; (void)outer; if (out == NULL) return DDERR_INVALIDPARAMS; *out = d3d_viewport3_create(); return DD_OK; }

static const d3d123_vtbl_t g_d3d3_vtbl = {
    .QueryInterface = (comfn2_t)d3d123_QueryInterface,
    .AddRef = d3d123_AddRef,
    .Release = d3d123_Release,
    .EnumDevices = (comfn2_t)d3d123_EnumDevices,
    .CreateLight = (comfn2_t)d3d3_CreateLight,
    .CreateMaterial = (comfn2_t)d3d3_CreateMaterial,
    .CreateViewport = (comfn2_t)d3d3_CreateViewport,
    .FindDevice = fail2,
    .CreateDevice = (comfn4_t)d3d3_CreateDevice,
    .CreateVertexBuffer = (comfn4_t)d3d3_CreateVertexBuffer,
    .EnumZBufferFormats = (comfn3_t)d3d123_EnumZBufferFormats,
    .EvictManagedTextures = log_EvictManagedTextures,
};

static void *d3d1_create(void)
{
    d3d123_t *d3d = calloc(1, sizeof(*d3d));
    d3d->lpVtbl = &g_d3d1_vtbl;
    atomic_init(&d3d->refcount, 1u);
    return d3d;
}
static void *d3d2_create(void)
{
    d3d123_t *d3d = calloc(1, sizeof(*d3d));
    d3d->lpVtbl = &g_d3d2_vtbl;
    atomic_init(&d3d->refcount, 1u);
    return d3d;
}
static void *d3d3_create(void)
{
    d3d123_t *d3d = calloc(1, sizeof(*d3d));
    d3d->lpVtbl = &g_d3d3_vtbl;
    atomic_init(&d3d->refcount, 1u);
    return d3d;
}

/* ===================== PE-imported entry points ===================== */

static uint32_t WINAPI stub_DirectDrawCreateEx(guid_t *driver_guid, void **out_ddraw, guid_t *iid, void *outer)
{
    (void)driver_guid; (void)outer;
    if (out_ddraw == NULL) return DDERR_INVALIDPARAMS;
    if (!guid_equal(iid, &IID_IDirectDraw7)) {
        *out_ddraw = NULL;
        return DDERR_NOINTERFACE;
    }
    d3d7_set_surface_interop(&g_d3d7_interop);
    ddraw_core_t *core = calloc(1, sizeof(*core));
    atomic_init(&core->refcount, 0u); /* bumped to 1 by ddraw_tearoff_create below */
    *out_ddraw = ddraw_tearoff_create(core, &g_ddraw7_vtbl);
    return DD_OK;
}

/* Real DirectDrawCreate (unlike CreateEx) has no IID param at all -- it
   always hands back a plain IDirectDraw (v1), which a DX6-era game
   (SWEP1RCR, see [[project_swep1rcr_port_scoping]]) then QueryInterfaces
   up to whichever real version (2/4) plus IDirect3D/2/3 it actually
   wants. Same underlying ddraw_core_t as CreateEx would allocate, just
   starting out with a v1 tear-off instead of v7's. */
static uint32_t WINAPI stub_DirectDrawCreate(guid_t *driver_guid, void **out_ddraw, void *outer)
{
    (void)driver_guid; (void)outer;
    if (out_ddraw == NULL) return DDERR_INVALIDPARAMS;
    d3d7_set_surface_interop(&g_d3d7_interop);
    ddraw_core_t *core = calloc(1, sizeof(*core));
    atomic_init(&core->refcount, 0u); /* bumped to 1 by ddraw_tearoff_create below */
    *out_ddraw = ddraw_tearoff_create(core, &g_ddraw1_vtbl);
    return DD_OK;
}

static uint32_t WINAPI stub_DirectDrawEnumerateA(void *cb_, void *ctx)
{
    typedef uint32_t (WINAPI *cb_t)(const guid_t *, char *, char *, void *);
    if (cb_ == NULL) return DD_OK;
    /* GUID NULL == the primary display device, standard DirectDraw
       enumeration convention -- one entry is enough, this game only
       ever runs against the primary display. */
    int cont_enum = ((cb_t)cb_)(NULL, "DirectDraw HAL", "display", ctx);
    if (cont_enum) {
        ((cb_t)cb_)(&IID_D3DDEVICE_D3DUID, "DirectDraw HAL", "display", ctx);
    }
    return DD_OK;
}

/* DDENUM_* flags (dwFlags) -- accepted but unused: this loader only ever
   has ONE real display device (no secondary/detached/non-display
   devices exist to filter in or out), so every flag combination reports
   the exact same single primary device -- matches real DirectDraw
   behavior on a real single-monitor machine regardless of which flags
   are passed. */
#define DDENUM_ATTACHEDSECONDARYDEVICES 0x00000001u
#define DDENUM_DETACHEDSECONDARYDEVICES 0x00000002u
#define DDENUM_NONDISPLAYDEVICES        0x00000004u

/* Same enumeration as DirectDrawEnumerateA (above) -- the only real
   difference in the Ex variant is the callback's own signature gaining
   a trailing HMONITOR parameter (LPDDENUMCALLBACKEXA takes 5 args, not
   4). No real multi-monitor support exists anywhere in this loader, so
   a fixed non-NULL placeholder handle is passed -- real callers that
   just log/store it (the common case) work fine; nothing here has ever
   been shown to actually call GetMonitorInfo or similar on it. */
static uint32_t WINAPI stub_DirectDrawEnumerateExA(void *cb_, void *ctx, uint32_t dwFlags)
{
    typedef uint32_t (WINAPI *cb_ex_t)(const guid_t *, char *, char *, void *, void *);
    (void)dwFlags;
    if (cb_ == NULL) return DD_OK;
    void *fake_hmonitor = (void *)(uintptr_t)1;
    int cont_enum = ((cb_ex_t)cb_)(NULL, "DirectDraw HAL", "display", ctx, fake_hmonitor);
    if (cont_enum) {
        ((cb_ex_t)cb_)(&IID_D3DDEVICE_D3DUID, "DirectDraw HAL", "display", ctx, fake_hmonitor);
    }
    return DD_OK;
}

shim_fn_t shim_ddraw_lookup(const char *name, uint32_t ordinal)
{
    (void)ordinal;
    if (name == NULL) return NULL;
    if (strcmp(name, "DirectDrawCreateEx") == 0) return (shim_fn_t)stub_DirectDrawCreateEx;
    if (strcmp(name, "DirectDrawCreate") == 0) return (shim_fn_t)stub_DirectDrawCreate;
    if (strcmp(name, "DirectDrawEnumerateA") == 0) return (shim_fn_t)stub_DirectDrawEnumerateA;
    if (strcmp(name, "DirectDrawEnumerateExA") == 0) return (shim_fn_t)stub_DirectDrawEnumerateExA;
    return NULL;
}
