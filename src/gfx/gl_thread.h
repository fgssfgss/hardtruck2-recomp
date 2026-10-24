#ifndef LONGROAD2_GL_THREAD_H
#define LONGROAD2_GL_THREAD_H

#include <pthread.h>

/*
 * Real dedicated GL-issuer thread -- see [[project_gl_issuer_thread]].
 *
 * A real GL context is current on at most one thread at a time. This game
 * (Porsche.exe/NFS5, confirmed live) spawns its own real OS thread for
 * D3D7/DirectDraw calls, separate from whichever thread first called
 * render_init() -- and render_init() only ever makes the context current on
 * ITS OWN calling thread (see render.c's g_gl_context / the one SDL_GL_
 * CreateContext call). Nothing else in this codebase used to call
 * SDL_GL_MakeCurrent() again, so every real GL call issued from the game's
 * other thread ran with no context current at all -- confirmed live via
 * gl_target_ensure's own WRONG/NO GL CONTEXT CURRENT diagnostic (render.c)
 * actually firing.
 *
 * The fix is a single dedicated thread that permanently owns the context:
 * gl_thread_init() spawns it once (called from render_init(), after context
 * creation succeeds); its entry function calls SDL_GL_MakeCurrent() exactly
 * once at startup and never again. From that point on, every real GL call
 * anywhere in this codebase must go through gl_thread_call() instead of
 * running inline -- see gl_thread_call's own comment for how callers already
 * on the GL thread avoid a redundant hop.
 *
 * An earlier attempt at this bug used a recursive mutex that migrated the
 * context onto whichever thread called it (render_gl_lock()/unlock()) --
 * rejected in favor of this real dedicated-thread design.
 */

/* Spawns the dedicated GL thread. window/context are passed as void* (not
   SDL_Window* / SDL_GLContext) so this header doesn't need to pull in SDL.h
   -- gl_thread.c casts them back internally. The calling thread must NOT touch
   the GL context again after this returns; the new thread calls
   SDL_GL_MakeCurrent() on it once, at startup, before servicing its first
   request. Safe to call again after a prior gl_thread_shutdown() (a fresh
   render_init()/render_shutdown() cycle, e.g. in tests). */
void gl_thread_init(void *window, void *context);

/* Signals the GL thread to exit once its current/pending work (if any) is
   done, and joins it. Callers that still need to run real GL teardown
   (e.g. SDL_GL_DeleteContext) must do so via gl_thread_call() BEFORE calling
   this -- once the thread has exited, gl_thread_call() falls back to
   running inline on the caller's own thread (see its own comment), which is
   no longer safe for real GL work. */
void gl_thread_shutdown(void);

/* Runs fn(arg) on the dedicated GL thread and blocks the caller until it
   has actually finished -- a synchronous cross-thread RPC, not a fire-and-
   forget post. Any return value fn needs to hand back should be written
   into a field of *arg (safe to read once this call returns, since it's
   synchronous and the caller's own stack frame -- where arg usually lives
   -- is still alive the whole time it's blocked).

   If called from a thread OTHER than the GL thread, this blocks on a real
   mutex+condvar handoff. If called FROM the GL thread itself, fn(arg) runs
   immediately, inline, with no queueing -- required because GL-thread-
   resident work legitimately calls back into other wrapped functions (e.g.
   d3d7.c's draw replay reaching ddraw.c's ddraw_gl_bind_render_target
   through the registered d3d7_surface_interop_t callback, which itself
   calls into render.c's gl_target_* functions) and a naive version would
   deadlock waiting on itself -- the same insight that forced the earlier,
   reverted mutex attempt to be recursive, now applied to message-passing.

   Also falls back to running fn(arg) directly, inline, if the GL thread was
   never started (gl_thread_init() not yet called) or has already been shut
   down (gl_thread_shutdown() already ran) -- matches this codebase's
   existing single-threaded behavior for any caller outside the render_init/
   render_shutdown-bracketed lifetime (e.g. a test that calls a gl_target_*
   function directly without ever calling render_init()). */
void gl_thread_call(void (*fn)(void *arg), void *arg);

/* Fire-and-forget sibling of gl_thread_call() -- submits fn(arg) to the
   dedicated GL thread and returns immediately, WITHOUT waiting for it to
   run. This is what makes real CPU/GPU overlap possible: the caller (the
   game thread, at EndScene) can go on to record the next scene's draws
   while the GL thread is still replaying the previous one -- see
   [[project_gl_issuer_thread]]'s Phase 2 design.

   Two hard requirements on fn/arg, different from gl_thread_call's:
   1. arg MUST be heap-allocated, owned by fn, and freed by fn itself
      before it returns (a stack-allocated arg, safe for the synchronous
      gl_thread_call, is NOT safe here -- the poster's own stack frame may
      already be gone by the time fn actually runs).
   2. fn must not read or write any real, live, concurrently-mutable state
      (e.g. a device's own dev->state/render_target/textures, which the
      calling thread may keep mutating for the NEXT scene right after this
      call returns) -- it may only touch data that arg exclusively owns,
      or a stable, GL-thread-exclusive shadow (see cmdbuf_flush's own
      d3d_device7_t *replay_shadow for the concrete pattern this backs).

   Same same-thread bypass and not-yet-started/already-shut-down fallback
   as gl_thread_call (both run fn(arg) inline in that case -- always safe
   for post too, since there's no "later" to separate from "now"). Also
   provides real depth-2 backpressure: see gl_thread.c's own comment on
   where this blocks. */
void gl_thread_post(void (*fn)(void *arg), void *arg);

/* Blocks the caller until every job posted (gl_thread_post) or called
   (gl_thread_call) so far has fully finished running -- a real drain
   barrier, not a poll. Needed because gl_thread_post's whole point is to
   let EndScene return before its replay is done (see its own comment);
   anything that needs a DETERMINISTIC, already-replayed result right
   after EndScene -- a test checking a draw counter or reading back
   pixels without an intervening real GL-thread dispatch of its own, or
   any other code with the same need -- must call this first. Anything
   that already goes through gl_thread_call for its own read (e.g.
   surface Lock()'s GPU->CPU sync) gets this for free from gl_thread_
   call's own FIFO ordering and does NOT need to call this separately. */
void gl_thread_wait_idle(void);

/* Test-only: the dedicated GL thread's own pthread_t, valid once
   gl_thread_init() has returned. Lets a test's own dispatched callback
   assert pthread_equal(pthread_self(), gl_thread_get_id_for_test()) to
   prove real cross-thread dispatch actually happened, rather than a
   same-thread bypass or a no-op stand-in. */
pthread_t gl_thread_get_id_for_test(void);

#endif /* LONGROAD2_GL_THREAD_H */
