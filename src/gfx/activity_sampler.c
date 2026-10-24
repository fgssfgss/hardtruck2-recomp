#define _GNU_SOURCE /* pthread_setname_np -- must precede all system headers, same precedent as kernel32.c/audio_mixer.c's own top-of-file comment */

#include "activity_sampler.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

static int g_enabled = -1;
static int sampler_enabled(void)
{
    if (g_enabled < 0) {
        const char *v = getenv("LONGROAD2_ACTIVITY_SAMPLE");
        g_enabled = (v != NULL && v[0] != '0') ? 1 : 0;
    }
    return g_enabled;
}

/* relaxed throughout: this is a statistical diagnostic, not a
   correctness-critical path -- losing or double-counting an occasional
   sample to a torn read is an acceptable cost for never needing a real
   memory barrier on activity_sampler_enter/_leave's own hot call sites. */
static _Atomic int g_current_activity = ACTIVITY_GAME;
static _Atomic unsigned long g_counts[ACTIVITY_COUNT];
static _Atomic unsigned long g_total_samples;
static _Atomic int g_sampler_running;

void activity_sampler_enter(activity_id_t id)
{
    if (!sampler_enabled()) return;
    atomic_store_explicit(&g_current_activity, (int)id, memory_order_relaxed);
}

void activity_sampler_leave(void)
{
    if (!sampler_enabled()) return;
    atomic_store_explicit(&g_current_activity, ACTIVITY_GAME, memory_order_relaxed);
}

static void *sampler_thread_main(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "activity_sampler");

    long interval_us = 1000; /* 1ms default -- matches a typical `perf record -F 1000` cadence */
    const char *v = getenv("LONGROAD2_ACTIVITY_SAMPLE_US");
    if (v != NULL) {
        long parsed = atol(v);
        if (parsed > 0) interval_us = parsed;
    }
    struct timespec ts;
    ts.tv_sec = interval_us / 1000000;
    ts.tv_nsec = (interval_us % 1000000) * 1000L;

    atomic_store_explicit(&g_sampler_running, 1, memory_order_relaxed);
    for (;;) {
        nanosleep(&ts, NULL);
        int id = atomic_load_explicit(&g_current_activity, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_counts[id], 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_total_samples, 1, memory_order_relaxed);
    }
    return NULL; /* unreachable -- this thread runs for the process's whole life, same as audio_mixer's own SDL audio thread */
}

void activity_sampler_start(void)
{
    if (!sampler_enabled()) return;
    if (atomic_load_explicit(&g_sampler_running, memory_order_relaxed)) return; /* idempotent */

    pthread_t tid;
    if (pthread_create(&tid, NULL, sampler_thread_main, NULL) != 0) return;
    pthread_detach(tid); /* fire-and-forget, matches audio_mixer's own SDL-owned thread -- nothing ever joins this one */
}

void activity_sampler_report_and_reset(unsigned long *counts_out, unsigned long *out_total_samples)
{
    for (int i = 0; i < ACTIVITY_COUNT; i++) {
        counts_out[i] = atomic_exchange_explicit(&g_counts[i], 0, memory_order_relaxed);
    }
    *out_total_samples = atomic_exchange_explicit(&g_total_samples, 0, memory_order_relaxed);
}
