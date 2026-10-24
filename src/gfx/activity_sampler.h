#ifndef LONGROAD2_ACTIVITY_SAMPLER_H
#define LONGROAD2_ACTIVITY_SAMPLER_H

/* Cooperative statistical sampler -- see [[project_cooperative_activity_sampler]].
 *
 * box86 does not deliver SIGPROF into guest (x86) code, so a real
 * interrupt-driven sampling profiler (SIGPROF + setitimer(ITIMER_PROF),
 * the userspace equivalent of `perf record`, since box86's own ARM-side
 * PMU access can't be meaningfully attributed back to emulated x86 code
 * either) isn't available here. This is the fallback: a real second
 * thread (pthread_create + nanosleep, both already proven working under
 * box86 throughout this project's own perf-trace work) wakes up on a
 * fixed interval and reads a single shared "what is the render thread
 * doing right now" tag, rather than being asynchronously interrupted
 * into it. No signal dependency at all.
 *
 * The tag is a plain store (activity_sampler_enter/_leave), not a
 * function call -- writing a global doesn't cross any box86 call
 * boundary, so tagging every existing perf-trace checkpoint this project
 * already has (state setters, draw dispatch, cmdbuf sort, the 4 replay
 * phases, gpu_finish, blt, present, msgpump) costs essentially nothing
 * extra, and reuses call sites already proven correct rather than
 * hunting for new ones.
 *
 * ACTIVITY_GAME (the default, whenever nothing else claims it) is the
 * actually new information: every existing counter on the perf-trace
 * line measures OUR OWN code's time precisely; none of them can say
 * anything about what happens in the gaps. This is the first mechanism
 * that samples those gaps directly instead of inferring them as
 * `unaccounted = real_wall - total`. */

typedef enum {
    ACTIVITY_GAME = 0, /* default: no tagged shim call currently running on this thread */
    ACTIVITY_STATE_CALLS,     /* the 9 D3D7 fixed-function state-setter vtable methods */
    ACTIVITY_DRAW_DISPATCH,   /* d3d_draw_common/_strided's record-time path */
    ACTIVITY_CMDBUF_SORT,     /* cmdbuf_build_order (native quicksort + zone scan) */
    ACTIVITY_CMDBUF_PREPARE,  /* draw_*_execute_now: d3d_draw_prepare */
    ACTIVITY_CMDBUF_UPLOAD,   /* draw_*_execute_now: vertex/index buffer upload (+ strided repack) */
    ACTIVITY_CMDBUF_ATTRIB,   /* draw_*_execute_now: vertex-attrib setup */
    ACTIVITY_CMDBUF_DISPATCH, /* draw_*_execute_now: the real glDraw*() call + cleanup */
    ACTIVITY_GPU_FINISH,      /* the glFinish() wait in d3d7_perf_report_for_frame */
    ACTIVITY_BLT,             /* ddraw.c Blt/BltFast CPU-side copy work */
    ACTIVITY_PRESENT,         /* ddraw.c present/present_gpu work */
    ACTIVITY_MSGPUMP,         /* platform.c GetMessage's own wait loop */
    /* d3d_draw_prepare's own 5 sub-steps -- a real capture found
       cmdbuf_prepare (the coarse tag above, still set briefly on entry)
       roughly tied with cmdbuf_dispatch as cmdbuf_flush's two biggest
       replay-loop costs (~14.5%/~13.1% of all render-frame samples),
       and a near-95% uniform-cache MISS rate in the same capture (most
       draws pay a real uniform re-upload, not a skip) -- these narrow
       down which of the 5 actually dominates instead of guessing.
       See [[project_cooperative_activity_sampler]]. */
    ACTIVITY_PREPARE_ENSURE_RT,      /* ensure_rendering_target: FBO/render-target binding */
    ACTIVITY_PREPARE_ENSURE_SHADER,  /* ensure_shader_program: shader variant lookup */
    ACTIVITY_PREPARE_RENDER_STATE,   /* apply_render_state + gl_state_use_program */
    ACTIVITY_PREPARE_UNIFORM_UPLOAD, /* the need_upload-gated transform/lighting/alpha/fog/clip uniform block */
    ACTIVITY_PREPARE_BIND_TEXTURE,   /* bind_texture_stages -- unconditional every draw */
    /* kernel32.c's HeapAlloc/HeapFree/HeapReAlloc (real malloc/free/realloc
       underneath) -- a reference trace of this game's real Win32 call
       volume (docs/api-surface/calls_by_frequency.txt) shows ~3.5 million
       heap alloc/free calls in one session, currently invisible inside
       ACTIVITY_GAME. Not about lock contention (already ruled out via
       cs_enter/wfso, see project_kernel32_sync_perf_investigation) -- this
       measures the raw per-call cost of crossing box86's x86->ARM libc-
       substitution boundary at that call volume. */
    ACTIVITY_HEAP,
    ACTIVITY_COUNT
} activity_id_t;

/* No-op unless LONGROAD2_ACTIVITY_SAMPLE=1 -- call once, early (same
   single-early-init-call convention as signal_handler_install()). Spawns
   a real, named ("activity_sampler") background thread; safe to call
   even when never followed by any activity_sampler_enter/_leave call. */
void activity_sampler_start(void);

/* Both cheap no-ops when the sampler isn't running (a single cached
   int check, no atomic touched) -- safe to call unconditionally from
   every tagged checkpoint regardless of whether LONGROAD2_ACTIVITY_SAMPLE
   is set. */
void activity_sampler_enter(activity_id_t id);
void activity_sampler_leave(void); /* sets back to ACTIVITY_GAME */

/* Reads+resets the accumulated per-activity sample counts. Safe to call
   from a different thread than the one calling enter/leave (relaxed
   atomics throughout -- this is a diagnostic tool, not a correctness-
   critical path, so the small read/reset race window is acceptable).
   counts_out must have ACTIVITY_COUNT elements. Leaves *counts_out
   zeroed and *out_total_samples 0 when the sampler was never started. */
void activity_sampler_report_and_reset(unsigned long *counts_out, unsigned long *out_total_samples);

#endif
