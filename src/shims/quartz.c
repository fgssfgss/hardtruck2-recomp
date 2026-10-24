/*
 * quartz.c - CLSID_FilterGraph for king.exe's background music: the game's
 * DirectShow use is one music channel, "RenderFile(MUSIC\trackNNl.mp3) -> Run
 * -> poll GetEvent until EC_COMPLETE -> next track" (LongRoad2's
 * src/shim/misc/quartz.c found that, and this is its design on the COM bridge).
 *
 * One filter graph object, four interfaces (IGraphBuilder, IMediaControl,
 * IMediaEventEx, IBasicAudio for the music volume), one shared refcount: a
 * proxy per interface kind, all onto
 * the same host object. No filters are ever built; RenderFile opens the file
 * with libmpg123 and a decoder thread feeds the PCM to win32hle's host-stream
 * API (hle_audio_stream_*, the one Bink movies use) -- the DirectSound mixer
 * itself is not touched. EC_COMPLETE is reported once the track is decoded AND
 * played out, not before: the game advances its playlist on it.
 *
 * HT2_NO_MUSIC=1 loads nothing (RenderFile still succeeds, GetEvent then
 * completes at once), to rule the music out when chasing something.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mpg123.h>
#include "com_bridge.h"
#include "../gfx/gfx_mem.h"

#define S_OK        0u
#define E_NOTIMPL   0x80004001u
#define E_ABORT     0x80004004u
#define EC_COMPLETE 0x00000001u

typedef struct { uint32_t refcount; int running; } graph_t;

static const uint8_t CLSID_FilterGraph_[16] =    /* {E436EBB3-524F-11CE-9F53-0020AF0BA770} */
    { 0xB3,0xEB,0x36,0xE4, 0x4F,0x52, 0xCE,0x11, 0x9F,0x53,0x00,0x20,0xAF,0x0B,0xA7,0x70 };
/* {56a868xx-0ad4-11ce-b03a-0020af0ba770}: the DirectShow interface family, by Data1 */
static int iid_ds(uint32_t riid_va, uint32_t data1) {
    static const uint8_t tail[12] = { 0xd4,0x0a, 0xce,0x11, 0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70 };
    const uint8_t *g = hle_ptr(riid_va);
    return g && *(const uint32_t *)g == data1 && !memcmp(g + 4, tail, 12);
}
static int iid_unknown(uint32_t riid_va) {
    static const uint8_t unk[16] = { 0,0,0,0, 0,0, 0,0, 0xC0,0,0,0,0,0,0,0x46 };
    const uint8_t *g = hle_ptr(riid_va);
    return g && !memcmp(g, unk, 16);
}
/* the interface an IID names, or CK_NONE */
static com_kind_t kind_of_iid(uint32_t riid) {
    if (iid_unknown(riid) || iid_ds(riid, 0x56a8689f) || iid_ds(riid, 0x56a868a9)) return CK_QGRAPH;   /* IFilterGraph, IGraphBuilder */
    if (iid_ds(riid, 0x56a868b1)) return CK_QCONTROL;                                                   /* IMediaControl */
    if (iid_ds(riid, 0x56a868b6) || iid_ds(riid, 0x56a868c0)) return CK_QEVENT;                         /* IMediaEvent(Ex) */
    if (iid_ds(riid, 0x56a868b3)) return CK_QAUDIO;                                                     /* IBasicAudio */
    return CK_NONE;
}

/* ---------------------------------------------------------------- the music channel
 * Module-wide, as the game has one music channel whatever graph object it
 * reuses. The decoder thread owns the mpg123 handle once it is handed over;
 * g_mu guards the handover and the state flags. */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static int g_thread_up, g_inited;
static mpg123_handle *g_mh, *g_pending;
static int g_stream = -1, g_rate, g_channels;
static int g_have_pending, g_pending_rate, g_pending_channels;
static int g_playing;            /* IMediaControl Run, no Pause/Stop since */
static int g_decoded;            /* the decoder reached the end of the current track */
static int g_delivered;          /* EC_COMPLETE already handed out for it */
static int g_volume, g_balance;  /* IBasicAudio, hundredths of a dB: 0 is full, -10000 silent */
/* Playing: full gain. The game's own IBasicAudio volume is kept for
 * get_Volume but not applied -- at its default midivol it asks for -38 dB,
 * which on this mix is silence after the first note (heard). LongRoad2 never
 * applied it either. */
static float gain_now(void) {    /* caller holds g_mu */
    return g_playing ? 1.0f : 0.0f;
}

static void *decoder(void *arg) {
    (void)arg;
    static int16_t pcm[8192];
    for (;;) {
        pthread_mutex_lock(&g_mu);
        if (g_have_pending) {                            /* a new track: its own stream, at its own rate */
            if (g_mh) { mpg123_close(g_mh); mpg123_delete(g_mh); }
            if (g_stream >= 0) hle_audio_stream_close(g_stream);
            g_mh = g_pending, g_pending = NULL, g_have_pending = 0;
            g_rate = g_pending_rate, g_channels = g_pending_channels;
            g_stream = hle_audio_stream_open(g_rate, g_channels);
            hle_audio_stream_gain(g_stream, gain_now());
        }
        mpg123_handle *mh = g_mh;
        int want = mh && !g_decoded && g_playing && g_stream >= 0 &&
                   hle_audio_stream_queued(g_stream) < g_rate / 2;   /* keep about half a second queued */
        pthread_mutex_unlock(&g_mu);
        if (!want) { usleep(10000); continue; }
        size_t got = 0;
        int err = mpg123_read(mh, pcm, sizeof pcm, &got);
        pthread_mutex_lock(&g_mu);
        if (mh == g_mh) {                                /* not replaced meanwhile */
            if (got) hle_audio_stream_push(g_stream, pcm, (int)(got / (2u * (unsigned)g_channels)));
            if (err == MPG123_DONE || (err != MPG123_OK && err != MPG123_NEW_FORMAT && !got)) {
                g_decoded = 1;
                fprintf(stderr, "[quartz] track decoded\n");
            }
        }
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}

static int music_disabled(void) {
    static int off = -1;
    if (off < 0) off = getenv("HT2_NO_MUSIC") && strcmp(getenv("HT2_NO_MUSIC"), "0");
    return off;
}

/* RenderFile: open the track and hand it to the decoder. A file that does not
 * open leaves whatever plays alone; GetEvent then completes this "track" at
 * once, so the game moves on instead of waiting. */
static void music_load(uint32_t wide_va) {
    const uint16_t *w = hle_ptr(wide_va);
    char win[520], host[1024];
    size_t i = 0;
    for (; w && w[i] && i < sizeof win - 1; i++) win[i] = w[i] < 128 ? (char)w[i] : '_';
    win[i] = 0;
    pthread_mutex_lock(&g_mu);
    g_decoded = 0, g_delivered = 0;
    pthread_mutex_unlock(&g_mu);
    if (music_disabled()) return;
    if (!hle_host_path(win, host, sizeof host)) {
        fprintf(stderr, "[quartz] RenderFile \"%s\": not found\n", win);
        pthread_mutex_lock(&g_mu);
        g_decoded = 1;
        pthread_mutex_unlock(&g_mu);
        return;
    }
    if (!g_inited) mpg123_init(), g_inited = 1;
    int err = 0;
    mpg123_handle *mh = mpg123_new(NULL, &err);
    long rate = 0;
    int channels = 0, enc = 0;
    if (!mh || mpg123_open(mh, host) != MPG123_OK || mpg123_getformat(mh, &rate, &channels, &enc) != MPG123_OK) {
        fprintf(stderr, "[quartz] RenderFile \"%s\": mpg123 cannot read it (%s)\n", win,
                mh ? mpg123_strerror(mh) : mpg123_plain_strerror(err));
        if (mh) mpg123_delete(mh);
        pthread_mutex_lock(&g_mu);
        g_decoded = 1;
        pthread_mutex_unlock(&g_mu);
        return;
    }
    mpg123_format_none(mh);                              /* that rate and layout, 16-bit signed */
    mpg123_format(mh, rate, channels == 1 ? MPG123_MONO : MPG123_STEREO, MPG123_ENC_SIGNED_16);
    pthread_mutex_lock(&g_mu);
    if (g_have_pending) { mpg123_close(g_pending); mpg123_delete(g_pending); }
    g_pending = mh, g_have_pending = 1;
    g_pending_rate = (int)rate, g_pending_channels = channels == 1 ? 1 : 2;
    if (!g_thread_up) g_thread_up = pthread_create(&g_thread, NULL, decoder, NULL) == 0;
    pthread_mutex_unlock(&g_mu);
    fprintf(stderr, "[quartz] RenderFile \"%s\": %ld Hz, %d channel(s)\n", win, rate, channels);
}

static void set_playing(graph_t *g, int on) {
    g->running = on;
    pthread_mutex_lock(&g_mu);
    g_playing = on;
    if (g_stream >= 0) hle_audio_stream_gain(g_stream, gain_now());   /* stopped: what is queued drains silently */
    pthread_mutex_unlock(&g_mu);
}

/* ---------------------------------------------------------------- IUnknown, shared */
static void query(com_kind_t self_kind) {
    COM_SELF(graph_t, self_kind, 2);
    com_kind_t k = kind_of_iid(A32(1));
    if (k == CK_NONE) {
        const uint8_t *g = hle_ptr(A32(1));
        if (g) fprintf(stderr, "[quartz] QueryInterface for {%08x-%04x-%04x-...}: not offered\n",
                       *(const uint32_t *)g, *(const uint16_t *)(g + 4), *(const uint16_t *)(g + 6));
        com_out(A32(2), NULL, CK_NONE);
        RET(COM_E_NOINTERFACE, 3);
    }
    self->refcount++;
    com_out(A32(2), self, k);
    RET(S_OK, 3);
}
static uint32_t release(graph_t *g) {
    uint32_t rc = --g->refcount;
    if (rc == 0) { gfx_native_freed(g); free(g); }
    return rc;
}
#define UNKNOWN(kind, pfx) \
    static void pfx##_QueryInterface(void) { query(kind); } \
    static void pfx##_AddRef(void) { COM_SELF(graph_t, kind, 0); RET(++self->refcount, 1); } \
    static void pfx##_Release(void) { COM_SELF(graph_t, kind, 0); RET(release(self), 1); }
UNKNOWN(CK_QGRAPH, gb)
UNKNOWN(CK_QCONTROL, mc)
UNKNOWN(CK_QEVENT, me)
static void fail8(void) { RET(E_NOTIMPL, 9); }           /* IDispatch::Invoke: 8 arguments, more than com_fail7 */

/* ---------------------------------------------------------------- IGraphBuilder */
static void gb_RenderFile(void) { COM_SELF(graph_t, CK_QGRAPH, 2); music_load(A32(1)); RET(S_OK, 3); }   /* (file, playlist) */
static const win32hle_shim k_graph[] = {
    { "QueryInterface", gb_QueryInterface }, { "AddRef", gb_AddRef }, { "Release", gb_Release },
    { "AddFilter", com_fail2 }, { "RemoveFilter", com_fail1 }, { "EnumFilters", com_fail1 },
    { "FindFilterByName", com_fail2 }, { "ConnectDirect", com_fail3 }, { "Reconnect", com_fail1 },
    { "Disconnect", com_fail1 }, { "SetDefaultSyncSource", com_fail0 },
    { "Connect", com_fail2 }, { "Render", com_fail1 }, { "RenderFile", gb_RenderFile },
    { "AddSourceFilter", com_fail3 }, { "SetLogFile", com_fail1 }, { "Abort", com_fail0 },
    { "ShouldOperationContinue", com_fail0 },
};

/* ---------------------------------------------------------------- IMediaControl */
static void mc_Run(void)   { COM_SELF(graph_t, CK_QCONTROL, 0); set_playing(self, 1); RET(S_OK, 1); }
static void mc_Pause(void) { COM_SELF(graph_t, CK_QCONTROL, 0); set_playing(self, 0); RET(S_OK, 1); }
static void mc_Stop(void)  { COM_SELF(graph_t, CK_QCONTROL, 0); set_playing(self, 0); RET(S_OK, 1); }
static void mc_GetState(void) {                          /* (timeout, &state): State_Stopped 0, State_Running 2 */
    COM_SELF(graph_t, CK_QCONTROL, 2);
    uint32_t *st = COM_P(2);
    if (st) *st = self->running ? 2u : 0u;
    RET(S_OK, 3);
}
static void mc_RenderFile(void) { COM_SELF(graph_t, CK_QCONTROL, 1); music_load(A32(1)); RET(S_OK, 2); }   /* (BSTR) */
static void mc_StopWhenReady(void) { COM_SELF(graph_t, CK_QCONTROL, 0); RET(S_OK, 1); }
static const win32hle_shim k_control[] = {
    { "QueryInterface", mc_QueryInterface }, { "AddRef", mc_AddRef }, { "Release", mc_Release },
    { "GetTypeInfoCount", com_fail1 }, { "GetTypeInfo", com_fail3 }, { "GetIDsOfNames", com_fail5 },
    { "Invoke", fail8 },
    { "Run", mc_Run }, { "Pause", mc_Pause }, { "Stop", mc_Stop }, { "GetState", mc_GetState },
    { "RenderFile", mc_RenderFile }, { "AddSourceFilter", com_fail2 }, { "get_FilterCollection", com_fail1 },
    { "get_RegFilterCollection", com_fail1 }, { "StopWhenReady", mc_StopWhenReady },
};

/* ---------------------------------------------------------------- IMediaEventEx */
static void me_GetEventHandle(void) {                    /* polled, never waited on (LongRoad2's trace): no handle */
    COM_SELF(graph_t, CK_QEVENT, 1);
    uint32_t *h = COM_P(1);
    if (h) *h = 0;
    RET(S_OK, 2);
}
static void me_GetEvent(void) {                          /* (&code, &p1, &p2, timeout) */
    COM_SELF(graph_t, CK_QEVENT, 4);
    pthread_mutex_lock(&g_mu);
    int done = g_decoded && (g_stream < 0 || hle_audio_stream_queued(g_stream) == 0) && !g_have_pending;
    int give = self->running && done && !g_delivered;
    if (give) g_delivered = 1;
    pthread_mutex_unlock(&g_mu);
    if (!give) RET(E_ABORT, 5);                          /* nothing within the timeout */
    uint32_t *code = COM_P(1), *p1 = COM_P(2), *p2 = COM_P(3);
    if (code) *code = EC_COMPLETE;
    if (p1) *p1 = 0;
    if (p2) *p2 = 0;
    fprintf(stderr, "[quartz] EC_COMPLETE\n");
    RET(S_OK, 5);
}
static void me_WaitForCompletion(void) {                 /* (timeout, &code): not seen used; completes, as LongRoad2's */
    COM_SELF(graph_t, CK_QEVENT, 2);
    uint32_t *code = COM_P(2);
    pthread_mutex_lock(&g_mu);
    g_delivered = 1;
    pthread_mutex_unlock(&g_mu);
    if (code) *code = EC_COMPLETE;
    RET(S_OK, 3);
}
static void me_FreeEventParams(void) { COM_SELF(graph_t, CK_QEVENT, 3); RET(S_OK, 4); }
static const win32hle_shim k_event[] = {
    { "QueryInterface", me_QueryInterface }, { "AddRef", me_AddRef }, { "Release", me_Release },
    { "GetTypeInfoCount", com_fail1 }, { "GetTypeInfo", com_fail3 }, { "GetIDsOfNames", com_fail5 },
    { "Invoke", fail8 },
    { "GetEventHandle", me_GetEventHandle }, { "GetEvent", me_GetEvent },
    { "WaitForCompletion", me_WaitForCompletion }, { "CancelDefaultHandling", com_fail1 },
    { "RestoreDefaultHandling", com_fail1 }, { "FreeEventParams", me_FreeEventParams },
    { "SetNotifyWindow", com_fail3 }, { "SetNotifyFlags", com_fail1 }, { "GetNotifyFlags", com_fail1 },
};

/* ---------------------------------------------------------------- IBasicAudio
 * The music volume (the game's midivol) and balance: kept and answered, not
 * applied (gain_now says why). */
UNKNOWN(CK_QAUDIO, ba)
static void ba_put_Volume(void) {
    COM_SELF(graph_t, CK_QAUDIO, 1);
    int32_t v = (int32_t)A32(1);
    if (v > 0 || v < -10000) RET(0x80070057u, 2);         /* E_INVALIDARG */
    pthread_mutex_lock(&g_mu);
    g_volume = v;
    pthread_mutex_unlock(&g_mu);

    RET(S_OK, 2);
}
static void ba_get_Volume(void)  { COM_SELF(graph_t, CK_QAUDIO, 1); uint32_t *p = COM_P(1); if (p) *p = (uint32_t)g_volume; RET(S_OK, 2); }
static void ba_put_Balance(void) { COM_SELF(graph_t, CK_QAUDIO, 1); g_balance = (int32_t)A32(1); RET(S_OK, 2); }
static void ba_get_Balance(void) { COM_SELF(graph_t, CK_QAUDIO, 1); uint32_t *p = COM_P(1); if (p) *p = (uint32_t)g_balance; RET(S_OK, 2); }
static const win32hle_shim k_audio[] = {
    { "QueryInterface", ba_QueryInterface }, { "AddRef", ba_AddRef }, { "Release", ba_Release },
    { "GetTypeInfoCount", com_fail1 }, { "GetTypeInfo", com_fail3 }, { "GetIDsOfNames", com_fail5 },
    { "Invoke", fail8 },
    { "put_Volume", ba_put_Volume }, { "get_Volume", ba_get_Volume },
    { "put_Balance", ba_put_Balance }, { "get_Balance", ba_get_Balance },
};

static uint32_t create(const uint8_t *iid, uint32_t *out) {
    com_kind_t k = kind_of_iid(hle_va(iid));
    if (k == CK_NONE) { *out = 0; return COM_E_NOINTERFACE; }
    graph_t *g = calloc(1, sizeof *g);
    g->refcount = 1;
    *out = com_wrap(g, k);
    return S_OK;
}

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
void ht2_quartz_register(void) {
    com_define(CK_QGRAPH,   "IGraphBuilder", k_graph,   N(k_graph));
    com_define(CK_QCONTROL, "IMediaControl", k_control, N(k_control));
    com_define(CK_QEVENT,   "IMediaEventEx", k_event,   N(k_event));
    com_define(CK_QAUDIO,   "IBasicAudio",   k_audio,   N(k_audio));
    hle_com_register_class(CLSID_FilterGraph_, create);
}
_Static_assert(N(k_graph) == 3 + 8 + 7,         "IGraphBuilder: IUnknown + IFilterGraph + its own");
_Static_assert(N(k_control) == 3 + 4 + 9,       "IMediaControl: IUnknown + IDispatch + its own");
_Static_assert(N(k_event) == 3 + 4 + 6 + 3,     "IMediaEventEx: IUnknown + IDispatch + IMediaEvent + Ex");
_Static_assert(N(k_audio) == 3 + 4 + 4,         "IBasicAudio: IUnknown + IDispatch + its own");

