#define _GNU_SOURCE /* pthread_setname_np -- must precede all system headers, same precedent as kernel32.c/audio_mixer.c/activity_sampler.c's own top-of-file comment */

#include "gl_thread.h"

#include <SDL.h>
#include <stdio.h>

/* See gl_thread.h's own comment for the full design. This is a real
   pthread doing real synchronous C-call dispatch -- the same proven
   pattern this project already uses elsewhere for a dedicated background
   thread under box86 (platform/activity_sampler.c's sampler thread,
   kernel32.c's CreateThread itself), not a new box86 risk. */

static SDL_Window *g_window = NULL;
static SDL_GLContext g_context = NULL;
static pthread_t g_thread;
static volatile int g_started = 0;

/* Single-slot synchronous RPC. g_busy spans the WHOLE request/response
   cycle (submission through the submitter observing completion), so at
   most one request is ever in flight system-wide -- a second concurrent
   caller blocks on g_free_cond until the first is fully done, rather than
   needing its own slot. Three condvars, one per distinct wait reason
   (request posted / request done / slot free), rather than one shared
   condvar with a broadcast -- clearer, and correct even though a single
   condvar would also work here (at most one thread is ever waiting on
   "done" or "free" at a time, since g_busy already serializes submitters). */
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_request_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_done_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_free_cond = PTHREAD_COND_INITIALIZER;
static int g_busy = 0;
static int g_has_request = 0;
static int g_done = 0;
static int g_shutdown_requested = 0;
static void (*g_pending_fn)(void *arg) = NULL;
static void *g_pending_arg = NULL;

static void *gl_thread_main(void *unused)
{
    (void)unused;
    /* Real OS thread name, purely a debugging aid (visible in gdb/htop/
       `ps -T`/perf) -- same convention as this project's other dedicated
       threads (activity_sampler.c, audio_mixer.c, kernel32.c's own
       game-thread naming), including the same 15-char-plus-null Linux cap.
       Best-effort: a naming failure isn't worth failing the thread over. */
    pthread_setname_np(pthread_self(), "gl_thread");

    /* The one and only SDL_GL_MakeCurrent call for the entire remaining
       lifetime of this process -- see gl_thread.h's own comment. */
    if (SDL_GL_MakeCurrent(g_window, g_context) != 0) {
        fprintf(stderr, "gl_thread_main: SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
    }

    pthread_mutex_lock(&g_mutex);
    for (;;) {
        while (!g_has_request && !g_shutdown_requested) {
            pthread_cond_wait(&g_request_cond, &g_mutex);
        }
        if (g_shutdown_requested && !g_has_request) break;

        void (*fn)(void *arg) = g_pending_fn;
        void *arg = g_pending_arg;
        g_has_request = 0;
        pthread_mutex_unlock(&g_mutex);

        fn(arg);

        pthread_mutex_lock(&g_mutex);
        /* g_busy clears HERE, uniformly, regardless of whether the
           submitter was a blocking gl_thread_call() or a fire-and-forget
           gl_thread_post() -- see gl_thread_post's own comment for why
           this is what makes the single-slot FIFO give async posts real
           backpressure (a second post blocks here until the first one has
           actually finished running, not just been picked up). g_done is
           a separate flag: only a gl_thread_call() waiter ever looks at
           it, to know ITS OWN job specifically finished (e.g. to safely
           read a result out of its own stack-allocated args struct). */
        g_done = 1;
        g_busy = 0;
        pthread_cond_signal(&g_done_cond);
        pthread_cond_signal(&g_free_cond);
    }
    pthread_mutex_unlock(&g_mutex);
    return NULL;
}

/* Caller must already hold g_mutex and have already waited for !g_busy. */
static void gl_thread_submit_locked(void (*fn)(void *arg), void *arg)
{
    g_busy = 1;
    g_pending_fn = fn;
    g_pending_arg = arg;
    g_has_request = 1;
    g_done = 0;
    pthread_cond_signal(&g_request_cond);
}

void gl_thread_init(void *window, void *context)
{
    g_window = (SDL_Window *)window;
    g_context = (SDL_GLContext)context;
    g_shutdown_requested = 0;
    if (pthread_create(&g_thread, NULL, gl_thread_main, NULL) != 0) {
        fprintf(stderr, "gl_thread_init: pthread_create failed -- GL calls will run inline "
                        "on whichever thread issues them (pre-existing thread-affinity bug NOT fixed)\n");
        return;
    }
    g_started = 1;
}

void gl_thread_shutdown(void)
{
    if (!g_started) return;
    pthread_mutex_lock(&g_mutex);
    g_shutdown_requested = 1;
    pthread_cond_signal(&g_request_cond);
    pthread_mutex_unlock(&g_mutex);
    pthread_join(g_thread, NULL);
    g_started = 0;
}

void gl_thread_call(void (*fn)(void *arg), void *arg)
{
    if (!g_started || pthread_equal(pthread_self(), g_thread)) {
        fn(arg);
        return;
    }

    pthread_mutex_lock(&g_mutex);
    while (g_busy) pthread_cond_wait(&g_free_cond, &g_mutex);
    gl_thread_submit_locked(fn, arg);
    while (!g_done) pthread_cond_wait(&g_done_cond, &g_mutex);
    pthread_mutex_unlock(&g_mutex);
}

void gl_thread_post(void (*fn)(void *arg), void *arg)
{
    if (!g_started || pthread_equal(pthread_self(), g_thread)) {
        fn(arg);
        return;
    }

    pthread_mutex_lock(&g_mutex);
    /* This wait is the real backpressure: it blocks until the PREVIOUS
       occupant of the single slot has fully finished running (not just
       been picked up -- see g_thread_main's own comment on where g_busy
       clears), before this new job can even be submitted. Since jobs are
       strictly FIFO, by the time this call returns, every job posted
       before it (in particular the one whose slot this caller is about to
       start reusing/recording into, in cmdbuf_flush's double-buffered
       design) is guaranteed complete -- real depth-2 pipelining with no
       separate "wait for my slot" call needed. */
    while (g_busy) pthread_cond_wait(&g_free_cond, &g_mutex);
    gl_thread_submit_locked(fn, arg);
    pthread_mutex_unlock(&g_mutex); /* does NOT wait for g_done -- returns immediately once submitted */
}

void gl_thread_wait_idle(void)
{
    if (!g_started || pthread_equal(pthread_self(), g_thread)) return;
    pthread_mutex_lock(&g_mutex);
    while (g_busy) pthread_cond_wait(&g_free_cond, &g_mutex);
    pthread_mutex_unlock(&g_mutex);
}

pthread_t gl_thread_get_id_for_test(void)
{
    return g_thread;
}
