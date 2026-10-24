#include "render.h"

#include "gl_state_cache.h"
#include "gl_thread.h"
#include <GLES2/gl2.h>

#include <SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window *g_window = NULL;

/* --- GLES 2.0 support (hardtruck2-recomp) ---
   LongRoad2 required GLES 3.1+. Every shader here is already GLSL ES 1.00,
   and the GLES3-only API it used came down to five things, each with a
   GLES2 path picked once at render_init from the context's version and
   extensions (gles_detect_caps):

     GL_UNPACK_ROW_LENGTH         ES3, or EXT_unpack_subimage; else repack rows
     GL_R8/GL_RED (palette index) GL_LUMINANCE everywhere: legal in ES2 and ES3,
                                  and the shader's .r reads the same byte
     GL_RGBA8 / GL_RGB565 storage unsized GL_RGBA / GL_RGB everywhere
     GL_DEPTH24_STENCIL8          ES3, or OES_packed_depth_stencil; else a
                                  16-bit depth-only renderbuffer
     GL_REPEAT on NPOT textures   ES3, or OES_texture_npot; else clamped
                                  (gl_state_cache.c), or ES2 samples black */
static int g_gles_major = 3;
static int g_has_unpack_subimage = 1;
static int g_has_packed_depth_stencil = 1;
#ifndef GL_UNPACK_ROW_LENGTH_EXT
#define GL_UNPACK_ROW_LENGTH_EXT 0x0CF2
#endif
#ifndef GL_DEPTH24_STENCIL8_OES
#define GL_DEPTH24_STENCIL8_OES 0x88F0
#endif

static int gl_has_ext(const char *exts, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = exts; p != NULL && (p = strstr(p, name)) != NULL; p += n) {
        if ((p == exts || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return 1;
    }
    return 0;
}

static void gles_detect_caps(void)
{
    const char *ver = (const char *)glGetString(GL_VERSION);
    const char *exts = (const char *)glGetString(GL_EXTENSIONS);
    int major = 0, minor = 0;
    if (ver == NULL || sscanf(ver, "OpenGL ES %d.%d", &major, &minor) < 1) major = 2;
    const char *force = getenv("HT2_GLES");
    if (force != NULL && atoi(force) == 2) {
        /* Drivers may answer a 2.0 request with a newer, compatible context
           (Mesa gives 3.2): behave as a bare GLES 2.0 driver anyway, no
           extensions, so every fallback path below is the one taken. */
        major = 2, minor = 0;
        exts = "";
    }

    g_gles_major = major;
    g_has_unpack_subimage = major >= 3 || gl_has_ext(exts, "GL_EXT_unpack_subimage");
    g_has_packed_depth_stencil = major >= 3 || gl_has_ext(exts, "GL_OES_packed_depth_stencil");
    gl_state_set_npot_repeat_ok(major >= 3 || gl_has_ext(exts, "GL_OES_texture_npot"));
    fprintf(stderr, "render_init: GLES %d.%d: unpack_subimage=%d packed_depth_stencil=%d npot_repeat=%d\n",
            major, minor, g_has_unpack_subimage, g_has_packed_depth_stencil,
            major >= 3 || gl_has_ext(exts, "GL_OES_texture_npot"));
}

/* glTexImage2D/glTexSubImage2D of a w x h image whose rows are
   row_stride_px apart (>= w). With GL_UNPACK_ROW_LENGTH unavailable (plain
   ES2) a padded image is repacked into a tight copy first. */
static void upload_strided(int sub, GLint internal_fmt, int w, int h, GLenum fmt, GLenum type,
                           int bytes_pp, const void *pixels, int row_stride_px)
{
    const void *src = pixels;
    void *tight = NULL;
    int padded = row_stride_px != 0 && row_stride_px != w;
    if (padded && g_has_unpack_subimage) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, row_stride_px);
    } else if (padded && pixels != NULL) {
        tight = malloc((size_t)w * (size_t)h * (size_t)bytes_pp);
        if (tight != NULL) {
            for (int y = 0; y < h; y++)
                memcpy((uint8_t *)tight + (size_t)y * w * bytes_pp,
                       (const uint8_t *)pixels + (size_t)y * row_stride_px * bytes_pp, (size_t)w * bytes_pp);
            src = tight;
        }
    }
    if (bytes_pp != 4) glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (sub) glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, type, src);
    else glTexImage2D(GL_TEXTURE_2D, 0, internal_fmt, w, h, 0, fmt, type, src);
    if (bytes_pp != 4) glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (padded && g_has_unpack_subimage) glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
    free(tight);
}
static SDL_GLContext g_gl_context = NULL;

/* Every glBindFramebuffer call in this whole codebase goes through this
   one function (confirmed: no other file ever calls glBindFramebuffer
   directly) -- skips the real GL call entirely when the requested target
   is already bound. Found to matter live: a real RK3326/Mali-G31/box86
   LONGROAD2_PERF_TRACE capture showed ~50-70ms/frame dominated by
   per-GL-call overhead, and gl_target_blit specifically was rebinding
   away from and back to the SAME destination FBO on every single one of
   ~46-108 consecutive Blt calls per frame that all composite onto that
   one target (minimap icons) -- on a tile-based GPU like Mali, switching
   framebuffers isn't free bookkeeping, it can force flushing/reloading
   on-chip tile memory, so doing that needlessly dozens of times a frame
   for calls that never actually needed to leave the target is a real,
   avoidable cost. -1 (not 0) is the initial "unknown" sentinel rather
   than assuming the real default-framebuffer binding (0) is already
   current -- 0 IS a valid real target here (the window's own
   framebuffer), so it must not be silently assumed true before this
   process has ever actually bound anything. */
static GLuint g_current_fbo = (GLuint)-1;
/* Test-only counters: how many bind_fbo() calls actually issued a real
   glBindFramebuffer vs. were skipped as already-current -- same call/
   skip-counting idiom used throughout this codebase's other caches
   (gl_state_cache.c's vertex-attrib counters, d3d7_shader.c's variant-
   lookup counters). Needed to prove gl_target_blit/gl_target_clear_rect's
   own trailing bind_fbo(0) removal (below) actually eliminates the
   redundant rebind for consecutive same-destination Blts, the real
   pattern already documented in ddraw.c's own comments (up to ~48 Blt
   calls in one frame, all targeting the same destination surface) --
   see [[project_ddraw_blt_fbo_bind_elimination]]. */
static long g_fbo_bind_calls = 0;
static long g_fbo_bind_skips = 0;
/* See d3d7.c's own d3d7_perf_*_for_test comment -- same "GL-thread-
   exclusive counter, EndScene's real replay is now async" reasoning. */
long render_get_fbo_bind_calls_for_test(void) { gl_thread_wait_idle(); return g_fbo_bind_calls; }
long render_get_fbo_bind_skips_for_test(void) { gl_thread_wait_idle(); return g_fbo_bind_skips; }
void render_reset_fbo_bind_counters_for_test(void)
{
    gl_thread_wait_idle();
    g_fbo_bind_calls = 0;
    g_fbo_bind_skips = 0;
}

static void bind_fbo(GLuint fbo)
{
    g_fbo_bind_calls++;
    if (fbo == g_current_fbo) { g_fbo_bind_skips++; return; }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    g_current_fbo = fbo;
}

/* LONGROAD2_TARGET_RESOLUTION locks the window to one fixed size,
   period -- when set, that's the ONLY resolution anywhere in this
   process: the real window, and everything reported back to the game
   (GetClientRect, mouse coordinates) all agree on it, and the game's own
   requested modes (SetDisplayMode, CreateWindow dims) are ignored
   outright rather than tracked separately. Previously this tracked two
   different sizes (a pinned physical window vs. a "logical" size that
   followed whatever the game last requested) specifically so the game's
   own internal coordinate math stayed self-consistent with what
   GetClientRect/mouse events reported -- but that mismatch between what
   the window really was and what got reported as its size is exactly
   the kind of thing a real hardware/driver DLL (dx7z.dll, once the real
   guest DLL loader could actually load and run one) can trip over: a
   live SIGSEGV inside it followed `render_init: 640x480 physical window
   (logical 1920x1080)`, a 3x mismatch far larger than this scheme was
   ever exercised with before. One resolution, always agreeing with
   itself, removes that whole class of mismatch. The present pipeline
   needs no other changes: every present path already stretches a full
   -1..1 NDC quad to whatever the real window's current size is (see
   present_reset_state_and_clear), so the game's own DirectDraw surfaces
   (created at whatever size IT wants, independent of this) still get
   scaled to fit for free. */
static int g_target_w = 0, g_target_h = 0; /* 0,0 = unset -- today's behavior */
static int g_logical_w = 0, g_logical_h = 0;

static void ensure_target_resolution_parsed(void)
{
    static int parsed = 0;
    if (parsed) return;
    parsed = 1;
    const char *spec = getenv("LONGROAD2_TARGET_RESOLUTION");
    if (spec != NULL) {
        int w = 0, h = 0;
        if (sscanf(spec, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            g_target_w = w;
            g_target_h = h;
        } else {
            fprintf(stderr, "render: LONGROAD2_TARGET_RESOLUTION=\"%s\" -- expected WxH, ignoring\n", spec);
        }
    }
}

/* LONGROAD2_FPS_LIMIT=<N>: caps presented frames to N per second, padding
   each present up to a minimum frame time instead of relying on the
   display's own vsync cadence. This game (see render_init's own swap-
   interval comment below) has no frame limiter of its own and relies
   entirely on Flip()/SDL_GL_SwapWindow blocking for its own pacing -- so
   making this call block longer directly throttles the game's own
   per-second call rate into EndScene/DrawPrimitive/etc, which is what
   actually reduces CPU/GPU load on weak target hardware, not just hiding
   presented frames. Deliberately NOT done via SDL_GL_SetSwapInterval(2):
   many embedded GLES/EGL drivers silently clamp any interval>1 down to 1,
   which would appear to work in testing but silently do nothing on the
   actual target hardware -- padding here works regardless of driver
   quirks or the display's real native refresh rate. */
static int g_fps_limit_checked = 0;
static Uint32 g_fps_limit_frame_ms = 0; /* 0 = disabled */
static Uint32 g_fps_limit_last_swap_ticks = 0;
static int g_fps_limit_has_last = 0;

static void frame_pace_wait(void)
{
    if (!g_fps_limit_checked) {
        g_fps_limit_checked = 1;
        const char *v = getenv("LONGROAD2_FPS_LIMIT");
        int fps = (v != NULL) ? atoi(v) : 0;
        g_fps_limit_frame_ms = (fps > 0) ? (Uint32)(1000 / fps) : 0;
    }
    if (g_fps_limit_frame_ms == 0) return;

    if (g_fps_limit_has_last) {
        Uint32 now = SDL_GetTicks();
        Uint32 elapsed = now - g_fps_limit_last_swap_ticks; /* unsigned -- wraps safely across SDL_GetTicks()'s ~49-day rollover */
        if (elapsed < g_fps_limit_frame_ms) SDL_Delay(g_fps_limit_frame_ms - elapsed);
    }
    g_fps_limit_last_swap_ticks = SDL_GetTicks();
    g_fps_limit_has_last = 1;
}

/* Xlib is not thread-safe by default -- concurrent calls into it from more
   than one thread are undefined behavior unless XInitThreads() was called
   before the FIRST Xlib call the process ever makes (here, SDL_Init's own
   X11 connection). This project now has real, permanent concurrent X11/GLX
   traffic from two different threads for the rest of the process's life --
   the dedicated GL thread (gl_thread.c) creates/destroys the GLX context
   and issues every real GL call, while whichever thread called render_init
   owns the SDL window itself (resize/title/show, and DestroyWindow at
   render_shutdown) -- so this is real, correct hygiene for that, not
   defensive filler. Investigated as a candidate fix for a real hang seen
   on this project's own X11 dev environment (game_loop_smoke_test's
   process never exiting at teardown) -- ruled out as THAT bug's actual
   cause (the same hang was isolated, via a clean `git stash` back to the
   single-threaded pre-gl_thread commit, to be a pre-existing X11/Mesa
   dev-sandbox flakiness unrelated to this project's own code -- see
   [[project_gl_issuer_thread]]), but kept anyway since it's independently
   correct regardless. Never a concern on the real target (an ARM SBC
   running kmsdrm, no X11 involved at all, see this file's own header
   comment) -- dlopen'd at runtime rather than linked, so a kmsdrm-only
   build/deployment with no libX11 installed at all still works, this
   simply becomes a harmless no-op there. */
static void init_x11_threading_if_available(void)
{
    void *libx11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_GLOBAL);
    if (libx11 == NULL) return;
    int (*init_threads)(void) = (int (*)(void))dlsym(libx11, "XInitThreads");
    if (init_threads != NULL) init_threads();
}

int render_init(int width, int height, const char *title)
{
    init_x11_threading_if_available();
    ensure_target_resolution_parsed();
    int phys_w = g_target_w != 0 ? g_target_w : width;
    int phys_h = g_target_h != 0 ? g_target_h : height;
    g_logical_w = phys_w;
    g_logical_h = phys_h;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "render_init: SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    /* GLES 3.2 preferred, 3.1 minimum -- everything this codebase actually
       uses (every shader is "#version 100"/GLSL ES 1.00, and the C-side GL
       calls are plain GLES3.0-core: glTexImage2D, glFramebufferTexture2D,
       GL_DEPTH24_STENCIL8/GL_RGBA8 renderbuffers, VBOs, standard draw
       calls -- nothing geometry/tessellation-shader/compute-shader/SSBO-
       shaped that would actually need 3.1 or 3.2 specifically) runs fine
       on either. Found live on a real X11/Mesa/Panfrost board that only
       advertises GLES 3.1: unconditionally requesting minor version 2
       left no EGLConfig for SDL to match against the X11 window's visual
       at all, so SDL_CreateWindow itself failed with EGL_BAD_MATCH before
       ever reaching SDL_GL_CreateContext. Retrying the WHOLE window+
       context creation (not just the context) at 3.1 is what actually
       matters here -- under X11/EGL, SDL picks the window's visual to
       match the requested GL attributes as part of SDL_CreateWindow
       itself, so a stale 3.2-seeking window can't be salvaged by just
       asking for a lower-version context on top of it afterward. */
    /* hardtruck2-recomp: then 3.0 and 2.0 -- see the GLES 2.0 note at the
       top of this file. HT2_GLES=2 starts at 2.0, to test that path on a
       desktop that would otherwise always hand out 3.x. */
    static const int k_gles_versions[][2] = { { 3, 2 }, { 3, 1 }, { 3, 0 }, { 2, 0 } };
    const size_t n_versions = sizeof(k_gles_versions) / sizeof(k_gles_versions[0]);
    const char *force = getenv("HT2_GLES");
    size_t first = (force != NULL && atoi(force) == 2) ? n_versions - 1 : 0;
    const char *last_error = "";
    for (size_t i = first; i < n_versions; i++) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, k_gles_versions[i][0]);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, k_gles_versions[i][1]);

        g_window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     phys_w, phys_h, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
        if (g_window == NULL) {
            last_error = SDL_GetError();
            continue;
        }

        g_gl_context = SDL_GL_CreateContext(g_window);
        if (g_gl_context == NULL) {
            last_error = SDL_GetError();
            SDL_DestroyWindow(g_window);
            g_window = NULL;
            continue;
        }

        if (i != first) {
            fprintf(stderr, "render_init: GLES %d.%d unavailable, using GLES %d.%d instead\n",
                    k_gles_versions[first][0], k_gles_versions[first][1],
                    k_gles_versions[i][0], k_gles_versions[i][1]);
        }
        break;
    }
    if (g_window == NULL || g_gl_context == NULL) {
        fprintf(stderr, "render_init: SDL_CreateWindow/SDL_GL_CreateContext failed at every "
                        "attempted GLES version: %s\n", last_error);
        SDL_Quit();
        return -1;
    }

    if (g_target_w != 0) {
        fprintf(stderr, "render_init: %dx%d window (LONGROAD2_TARGET_RESOLUTION, game requested %dx%d "
                        "-- ignored) via SDL video driver \"%s\"\n",
                phys_w, phys_h, width, height, SDL_GetCurrentVideoDriver());
    } else {
        fprintf(stderr, "render_init: %dx%d window via SDL video driver \"%s\"\n",
                phys_w, phys_h, SDL_GetCurrentVideoDriver());
    }
    fprintf(stderr, "render_init: GL_VERSION=%s\n", (const char *)glGetString(GL_VERSION));
    fprintf(stderr, "render_init: GL_RENDERER=%s\n", (const char *)glGetString(GL_RENDERER));
    gles_detect_caps();


    /* A fresh GL context starts at the spec-defined default state (no
       program, unit 0 active with nothing bound, blend/depth-test/cull/
       stencil/scissor all off, depth mask GL_TRUE) -- the state cache's
       own "unknown" sentinels already default-initialize to that, but
       reset explicitly here anyway so a future second render_init call
       (context recreation) can't inherit stale beliefs from a torn-down
       context whose real GL state no longer exists. */
    gl_state_reset();

    /* --- Frame pacing: two independent, composable knobs ---
       Real DirectDraw's Flip() on real hardware genuinely blocks until
       the next vertical blank -- this game (like most of its era) has no
       explicit frame limiter of its own and relies entirely on that
       blocking behavior for pacing, the same way its own cursor-
       visibility logic (SetCursor(NULL) during gameplay vs. a real
       HCURSOR in menus -- see king.exe.c) assumes "one visible frame per
       vsync tick" ordering. Without any swap interval set here, this
       renderer's own Flip()/SDL_GL_SwapWindow was returning immediately,
       running the whole game loop as fast as the CPU/GPU allow --
       hundreds or thousands of "frames" for every one actually worth
       presenting.

       Knob 1, swap interval (this block): controls whether
       SDL_GL_SwapWindow itself blocks for vblank. Default tries adaptive
       vsync first (falls back to a normal blocking swap on a missed
       frame instead of stalling an extra tick), then plain vsync, so at
       least one of them sticks on whatever's actually available.
       LONGROAD2_NO_VSYNC forces interval 0 (swap returns immediately,
       uncapped framerate) -- originally added as a diagnostic to isolate
       whether present_gpu_ms's 45-56ms/call cost on a real RK3326/Mali-
       G31 device was genuine vblank waiting or the swap mechanism itself
       being slow; that question is now RESOLVED (it was genuine GPU-
       bound render time, not swap overhead -- see
       [[project_present_swap_overhead_investigation]]), but the flag
       stays as a permanent tool: useful for uncapped-throughput
       benchmarking, or working around a driver whose vsync is itself
       broken/janky.

       Knob 2, LONGROAD2_FPS_LIMIT (frame_pace_wait() above, called from
       all 3 render_present_* functions just before their own
       SDL_GL_SwapWindow): an independent SOFTWARE pacer, orthogonal to
       swap interval -- see its own comment above for why it sleeps
       instead of using SDL_GL_SetSwapInterval(2)+. Where swap interval
       only blocks the swap call itself, frame_pace_wait() sleeps BEFORE
       it, throttling the game's own per-second call rate into
       EndScene/DrawPrimitive/etc -- actually reducing CPU/GPU load, not
       just capping how often a frame gets displayed. The two knobs
       compose freely: e.g. NO_VSYNC=1 with FPS_LIMIT=30 uncaps the swap
       call but still caps real work to 30Hz via the software pacer. */
    if (getenv("LONGROAD2_NO_VSYNC") != NULL) {
        SDL_GL_SetSwapInterval(0);
    } else if (SDL_GL_SetSwapInterval(-1) != 0) {
        SDL_GL_SetSwapInterval(1);
    }
    const char *fps_limit_env = getenv("LONGROAD2_FPS_LIMIT");
    if (fps_limit_env != NULL && atoi(fps_limit_env) > 0) {
        fprintf(stderr, "render_init: swap interval=%d, fps limit=%d\n",
                SDL_GL_GetSwapInterval(), atoi(fps_limit_env));
    } else {
        fprintf(stderr, "render_init: swap interval=%d, fps limit=off\n", SDL_GL_GetSwapInterval());
    }

    /* SDL_GL_CreateContext above implicitly made g_gl_context current on
       THIS thread (the one calling render_init()) -- real GLX (and
       presumably EGL) refuses to bind a context to a second thread while
       it's still bound to a first one (confirmed live: SDL_GL_MakeCurrent
       from the new GL thread below failed with a real "BadAccess (attempt
       to access private resource denied)" X error before this release was
       added). Release it here, on this thread, before the dedicated GL
       thread ever tries to claim it. */
    SDL_GL_MakeCurrent(g_window, NULL);

    /* Real dedicated GL-issuer thread -- see gl_thread.h's own comment.
       Spawned here, once, right after context creation succeeds (and
       released above): from this point on, THIS thread (whatever called
       render_init()) no longer owns the context either -- every real GL
       call in this codebase, including ones issued from this same thread,
       must go through gl_thread_call() (wrapped transparently by every
       gl_target_* / render_present_* / render_clear_and_swap public
       function below). */
    gl_thread_init(g_window, g_gl_context);

    return 0;
}

int render_is_initialized(void)
{
    return g_window != NULL;
}

void render_resize(int width, int height)
{
    /* A pinned target resolution ignores the game's requested size
       outright -- see g_target_w/h's own comment: one resolution,
       always, everywhere. */
    if (g_target_w != 0) return;
    g_logical_w = width;
    g_logical_h = height;
    if (g_window != NULL) SDL_SetWindowSize(g_window, width, height);
}

void render_set_title(const char *title)
{
    if (g_window != NULL) SDL_SetWindowTitle(g_window, title);
}

void render_show(int show)
{
    if (g_window == NULL) return;
    if (show) SDL_ShowWindow(g_window);
    else SDL_HideWindow(g_window);
}

/* The window's size -- always the real window's actual pixel size (this
   is what GetClientRect/GetWindowRect, platform.c, report back to the
   game). See g_target_w/h's own comment: there is only one resolution. */
void render_get_size(int *out_w, int *out_h)
{
    if (g_window != NULL) { *out_w = g_logical_w; *out_h = g_logical_h; }
    else { *out_w = 0; *out_h = 0; }
}

void render_get_target_resolution(int *out_w, int *out_h)
{
    ensure_target_resolution_parsed();
    *out_w = g_target_w;
    *out_h = g_target_h;
}

void render_get_screen_pos(int *out_x, int *out_y)
{
    if (g_window != NULL) SDL_GetWindowPosition(g_window, out_x, out_y);
    else { *out_x = 0; *out_y = 0; }
}

static void present_trace(const char *kind, int win_w, int win_h);

static void render_clear_and_swap_impl(float r, float g, float b)
{

    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    present_trace("clear", g_logical_w, g_logical_h);
    SDL_GL_SwapWindow(g_window);

}

/* --- Present pipeline: one texture + a fullscreen quad. GLSL ES 100 (not
   300 es) is used deliberately -- it's accepted by both GLES2 and our
   GLES 3.2 context, and client-side glVertexAttribPointer arrays (used
   below instead of a VBO) are valid in GLES at any version, unlike
   desktop core-profile GL. Keeps this to one code path with no version
   branching. */
static GLuint g_present_prog = 0;
static GLuint g_present_tex = 0;
static GLint g_present_attr_pos = -1;
static GLint g_present_unif_tex = -1;
static int g_present_tex_w = 0;
static int g_present_tex_h = 0;

static const char *k_present_vs =
    "#version 100\n"
    "attribute vec2 a_pos;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = vec2(a_pos.x * 0.5 + 0.5, 0.5 - a_pos.y * 0.5);\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char *k_present_fs =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_tex, v_uv);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "render: shader compile failed: %s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* Shared static fullscreen -1..1 NDC quad position data -- every present
   path below (RGBA, palette-lookup, GPU-resident-direct) AND
   gl_target_blit's own position attribute all draw this EXACT same quad
   (only gl_target_blit's UV coordinates vary per call, see
   g_blit_uv_vbo further down). Every one of these draws used to point
   glVertexAttribPointer directly at a local C array (a client-side
   vertex array, never a bound GL_ARRAY_BUFFER) -- confirmed live via
   LONGROAD2_PERF_TRACE on a real RK3326/Mali-G31/box86 deployment: a
   single present draw call cost ~56ms, and 46 small (40K total px)
   minimap Blt draw calls cost ~68ms combined (~1.5ms EACH) -- both wildly
   disproportionate to the actual vertex/pixel counts involved (4
   vertices; a few thousand pixels), and both scaling with DRAW CALL
   COUNT rather than data size, the signature of a driver paying a fixed
   per-call validation/upload cost for client-side attribute pointers
   rather than sourcing from a real GPU buffer -- a well-known slow path
   on mobile GPU drivers (Mali's Bifrost/Valhall GLES driver among them),
   and one box86's own x86->ARM call translation layer only makes more
   expensive per hit. A real VBO is the standard fix: uploaded once here,
   reused by every draw below via glBindBuffer + an offset instead of a
   raw pointer. */
static GLuint g_quad_pos_vbo = 0;

static int ensure_quad_pos_vbo(void)
{
    if (g_quad_pos_vbo != 0) return 1;
    static const GLfloat quad[] = { -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f,  1.0f, 1.0f };
    glGenBuffers(1, &g_quad_pos_vbo);
    if (g_quad_pos_vbo == 0) return 0;
    glBindBuffer(GL_ARRAY_BUFFER, g_quad_pos_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return 1;
}

/* Lazily builds the shader program + texture object on first present --
   deferred rather than done in render_init() so a window that's created
   but never used for DirectDraw output (unlikely here, but cheap to keep
   correct) doesn't pay for GL resources it never needs. */
static int ensure_present_resources(void)
{
    if (!ensure_quad_pos_vbo()) return 0;
    if (g_present_prog != 0) return 1;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, k_present_vs);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, k_present_fs);
    if (vs == 0 || fs == 0) {
        if (vs != 0) glDeleteShader(vs);
        if (fs != 0) glDeleteShader(fs);
        return 0;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "render: shader link failed: %s\n", log);
        glDeleteProgram(prog);
        return 0;
    }

    g_present_prog = prog;
    g_present_attr_pos = glGetAttribLocation(prog, "a_pos");
    g_present_unif_tex = glGetUniformLocation(prog, "u_tex");

    glGenTextures(1, &g_present_tex);
    gl_state_bind_texture_2d(GL_TEXTURE0, g_present_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    return 1;
}

/* --- Palette-lookup present pipeline: same fullscreen-quad shape as the
   plain RGBA one above, but the fragment shader resolves an 8-bit
   palette index to a color itself (a classic palette-texture lookup)
   instead of a CPU loop doing it per pixel before upload. Worth having
   as its own separate program/resources, not a variant of the RGBA one
   above, specifically so the existing, already-proven RGBA path stays
   completely untouched -- this is purely additive. idx*255.0 recovers
   the raw byte from the normalized-unsigned-byte sample the same way
   any 8-bit texture read does; floor(...+0.5) snaps it back to an exact
   integer since that round trip isn't guaranteed exact in floating
   point (256 possible input bytes, well within mediump's guaranteed
   exact-integer range per the GLSL ES spec, so no precision loss beyond
   that single rounding step). GL_NEAREST on both textures is required,
   not just a minor-quality choice -- indices and palette entries are
   discrete lookups, not continuous values; any interpolation between
   adjacent texels would blend unrelated palette entries together. */
static GLuint g_paletted_prog = 0;
static GLuint g_paletted_index_tex = 0;
static GLuint g_paletted_palette_tex = 0;
static GLint g_paletted_attr_pos = -1;
static GLint g_paletted_unif_index_tex = -1;
static GLint g_paletted_unif_palette_tex = -1;
static int g_paletted_tex_w = 0;
static int g_paletted_tex_h = 0;

static const char *k_paletted_fs =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_indexTex;\n"
    "uniform sampler2D u_paletteTex;\n"
    "void main() {\n"
    "    float idx = floor(texture2D(u_indexTex, v_uv).r * 255.0 + 0.5);\n"
    "    float paletteU = (idx + 0.5) / 256.0;\n"
    "    gl_FragColor = texture2D(u_paletteTex, vec2(paletteU, 0.5));\n"
    "}\n";

static int ensure_paletted_present_resources(void)
{
    if (!ensure_quad_pos_vbo()) return 0;
    if (g_paletted_prog != 0) return 1;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, k_present_vs); /* same fullscreen-quad vertex shader as the RGBA path */
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, k_paletted_fs);
    if (vs == 0 || fs == 0) {
        if (vs != 0) glDeleteShader(vs);
        if (fs != 0) glDeleteShader(fs);
        return 0;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "render: paletted shader link failed: %s\n", log);
        glDeleteProgram(prog);
        return 0;
    }

    g_paletted_prog = prog;
    g_paletted_attr_pos = glGetAttribLocation(prog, "a_pos");
    g_paletted_unif_index_tex = glGetUniformLocation(prog, "u_indexTex");
    g_paletted_unif_palette_tex = glGetUniformLocation(prog, "u_paletteTex");

    glGenTextures(1, &g_paletted_index_tex);
    gl_state_bind_texture_2d(GL_TEXTURE0, g_paletted_index_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &g_paletted_palette_tex);
    gl_state_bind_texture_2d(GL_TEXTURE0, g_paletted_palette_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    return 1;
}

/* Shared by render_present_rgba and render_present_paletted: every GL
   state reset below was individually earned by a real, live-reproduced
   bug this project chased down (see each reset's own inline history) --
   duplicating this block into two copies would risk the two silently
   drifting apart over time, so both present paths call the exact same
   function instead. Binds the default framebuffer, resets every
   process-global GL toggle a D3D7 draw could have left on, sets the
   viewport to the window's current size, and clears to black. Returns
   the window size via out params since both callers need it afterward
   (viewport math, frame-dump readback). */
/* Shared by every draw-then-composite path below (present, and now
   gl_target_blit): resets the process-global GL enable/disable toggles
   a preceding D3D7 draw could have left on. Each toggle here was
   individually earned by a real, live-reproduced bug this project
   chased down, not written defensively up front -- see
   present_reset_state_and_clear's git history / the project's own
   memory notes for the scissor/stencil leak investigations. Deliberately
   does NOT touch GL_SCISSOR_TEST or the scissor rect, or framebuffer
   binding/viewport: those differ per caller (present wants scissor off
   and the default framebuffer at full window size; a GPU blit wants
   scissor ON and narrowed to the destination rect, in an arbitrary
   FBO) -- each caller sets those explicitly right after calling this. */
static void gl_reset_defensive_toggles(void)
{
    /* d3d7.c's apply_render_state() enables GL_DEPTH_TEST and
       GL_CULL_FACE (real D3D7 defaults -- ZENABLE and CULLMODE=CCW are
       both on by default) and leaves them enabled after its last draw
       call -- and now GL_BLEND too (D3DRENDERSTATE_ALPHABLENDENABLE),
       which a plain Blt must not inherit: real DirectDraw Blt semantics
       are an opaque replace (except where colorkey-discarded), not a
       blend against whatever the destination already holds. */
    gl_state_set_depth_test_enabled(0);
    gl_state_set_cull_enabled(0);
    gl_state_set_blend_enabled(0);

    /* Same reasoning again, for GL_STENCIL_TEST: d3d7.c's apply_render_
       state() now really enables it (D3DRENDERSTATE_STENCILENABLE)
       instead of being a permanent no-op, and leaves it enabled after
       its last draw call, exactly like depth/cull above. Unlike a
       shader's own per-program state (e.g. the clip-plane discard
       logic, which can't leak here since these paths use a completely
       different, separate GL program), enable/disable flags are GL
       context-global and DO survive a glUseProgram switch -- so a game
       whose last D3D7 draw of a frame happens to leave stencil testing
       on (against whatever stencil bits the target framebuffer happens
       to have, likely a different stencil buffer entirely from the one
       the D3D7 render target's own FBO has) could silently reject some
       or all of this draw's fragments too, exactly the "half the
       objects vanish" shape of bug the earlier scissor leak was. */
    gl_state_set_stencil_test_enabled(0);
}

static void present_reset_state_and_clear(int *out_win_w, int *out_win_h)
{
    /* hardtruck2-recomp: forget every cached GL state first. Without this,
       about every other in-race present drew black (measured with
       HT2_PRESENT_TRACE: 963 of 2069; with the reset: 9, at mode switches).
       Something leaves GL state the cache no longer matches; no raw GL
       state call outside gl_state_cache.c was found, so the culprit is
       still open. HT2_PRESENT_RESET=0 turns this off to chase it. */
    static int reset = -1;
    if (reset < 0) reset = !(getenv("HT2_PRESENT_RESET") && !strcmp(getenv("HT2_PRESENT_RESET"), "0"));

    if (reset) { gl_state_reset(); g_current_fbo = (GLuint)-1; }

    /* Defensive: a d3d7 3D render (gl_target_bind) or an interop readback
       (gl_target_read_rgba) may have left a non-default framebuffer
       bound. This function always draws to the real window surface, so
       it can't rely on any other module's binding discipline. */
    bind_fbo(0);
    gl_reset_defensive_toggles();

    /* d3d7.c's apply_device_viewport() enables GL_SCISSOR_TEST and
       narrows glScissor to the D3D7 device's own viewport (this game's
       3D world area, deliberately excluding the DirectDraw-owned HUD
       strip) on every Clear()/DrawPrimitive*, and nothing ever disables
       it afterward -- it's process-global GL state, not scoped to any
       one framebuffer. Without this, once a single D3D7 draw has ever
       happened, both the glClear below and this function's own
       fullscreen quad draw stay silently clipped to that same leftover
       sub-rectangle forever, so anything outside it (the HUD strip, or
       the whole window before the first D3D7 draw sets a "normal" rect)
       never gets touched by this present again -- it just keeps showing
       whichever stale pixels were already in that part of the
       swapchain's current backbuffer. */
    gl_state_set_scissor_enabled(0);

    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(g_window, &win_w, &win_h);
    gl_state_set_viewport(0, 0, win_w, win_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    *out_win_w = win_w;
    *out_win_h = win_h;
}

/* Shared by render_present_rgba and render_present_paletted, called
   after the fullscreen quad is actually drawn. Opt-in (via
   LONGROAD2_FRAME_DUMP_DIR) frame capture: dumps actually-GL-drawn
   frames to disk to verify the whole pipeline end to end, independent
   of any external screenshot tool/compositor quirks.
   LONGROAD2_FRAME_DUMP_EVERY (default 1) samples every Nth presented
   frame instead of just the first few, so a long-running session can
   still capture what's on screen well after startup/loading (e.g. once
   real gameplay is reached) without writing every single frame to disk.
   Capped at 2000 dumped frames total either way. The frame-index
   counter is shared across both present paths -- a real DirectDraw
   session only ever uses one or the other for its primary surface at a
   time in practice, and keeping one counter means dumped filenames stay
   contiguous either way, matching what every existing test already
   assumes. */
/* HT2_PRESENT_TRACE=1 (hardtruck2-recomp diagnostic): one line per presented
 * frame -- which present path drew it, how bright it is (a 4x4 grid of
 * pixels read back before the swap), and the ms since the previous present.
 * Finds which path hands the window a black frame. */
static void present_trace(const char *kind, int win_w, int win_h)
{
    static int on = -1;
    static long n;
    static uint32_t last;
    if (on < 0) on = getenv("HT2_PRESENT_TRACE") != NULL;
    if (!on || win_w <= 0 || win_h <= 0) return;
    unsigned sum = 0;
    for (int gy = 0; gy < 4; gy++)
        for (int gx = 0; gx < 4; gx++) {
            uint8_t px[4] = { 0 };
            glReadPixels(win_w * (2 * gx + 1) / 8, win_h * (2 * gy + 1) / 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            sum += (unsigned)px[0] + px[1] + px[2];
        }
    uint32_t now = SDL_GetTicks();
    fprintf(stderr, "[present] %ld %s mean=%u dt=%u\n", n++, kind, sum / 48, now - last);
    last = now;
}

static void present_dump_frame_if_enabled(int win_w, int win_h)
{

    static long s_frame_index = -2;
    static long s_dump_written = 0;
    static long s_dump_every = 1;
    static const char *s_dump_dir = NULL;
    if (s_frame_index == -2) {
        s_dump_dir = getenv("LONGROAD2_FRAME_DUMP_DIR");
        const char *every = getenv("LONGROAD2_FRAME_DUMP_EVERY");
        if (every != NULL && atol(every) > 0) s_dump_every = atol(every);
        s_frame_index = 0;
    }
    if (s_dump_dir != NULL && s_dump_written < 2000 && (s_frame_index % s_dump_every) == 0) {
        uint8_t *buf = malloc((size_t)win_w * win_h * 4);
        if (buf != NULL) {
            glReadPixels(0, 0, win_w, win_h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
            char path[512];
            snprintf(path, sizeof(path), "%s/gl_frame_%ld.ppm", s_dump_dir, s_frame_index);
            FILE *f = fopen(path, "wb");
            if (f != NULL) {
                fprintf(stderr, "render: dumping frame %ld to %s\n", s_frame_index, path);
                fprintf(f, "P6\n%d %d\n255\n", win_w, win_h);
                for (int y = win_h - 1; y >= 0; y--) { /* GL rows are bottom-up */
                    for (int x = 0; x < win_w; x++) {
                        uint8_t *p = buf + (size_t)(y * win_w + x) * 4;
                        fputc(p[0], f);
                        fputc(p[1], f);
                        fputc(p[2], f);
                    }
                }
                fclose(f);
            }
            free(buf);
        }
        s_dump_written++;
    }
    s_frame_index++;
}

static void render_present_rgba_impl(const uint8_t *rgba, int width, int height, int row_stride_px)
{
    if (g_window == NULL || rgba == NULL || width <= 0 || height <= 0) return;
    if (!ensure_present_resources()) return;

    int win_w = 0, win_h = 0;
    present_reset_state_and_clear(&win_w, &win_h);

    gl_state_use_program(g_present_prog);
    gl_state_bind_texture_2d(GL_TEXTURE0, g_present_tex);

    if (width != g_present_tex_w || height != g_present_tex_h) {
        upload_strided(0, GL_RGBA, width, height, GL_RGBA, GL_UNSIGNED_BYTE, 4, rgba, row_stride_px);
        g_present_tex_w = width;
        g_present_tex_h = height;
    } else {
        upload_strided(1, GL_RGBA, width, height, GL_RGBA, GL_UNSIGNED_BYTE, 4, rgba, row_stride_px);
    }

    glUniform1i(g_present_unif_tex, 0);

    GLuint attr = (GLuint)g_present_attr_pos;
    glBindBuffer(GL_ARRAY_BUFFER, g_quad_pos_vbo);
    gl_state_set_vertex_attrib_array(attr, 2, GL_FLOAT, GL_FALSE, 0, g_quad_pos_vbo, (const void *)0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl_state_set_vertex_attrib_disabled(attr);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    present_trace("rgba", win_w, win_h);
    present_dump_frame_if_enabled(win_w, win_h);
    frame_pace_wait();
    SDL_GL_SwapWindow(g_window);
}

static void render_present_gl_target_impl(gl_target_t *t)
{
    if (g_window == NULL || t == NULL || t->tex == 0 || t->w <= 0 || t->h <= 0) return;
    if (!ensure_present_resources()) return;

    int win_w = 0, win_h = 0;
    present_reset_state_and_clear(&win_w, &win_h);

    gl_state_use_program(g_present_prog);
    gl_state_bind_texture_2d(GL_TEXTURE0, t->tex); /* t's OWN texture, no upload -- the whole point of this path */
    /* gl_target_ensure's own default filter/wrap (GL_LINEAR, GL_REPEAT --
       right for a D3D7 texture-stage sampler, see its own comment) isn't
       what a fullscreen present quad wants: g_present_tex above always
       uses GL_NEAREST (matches the source pixel data 1:1 outside of the
       stretch-to-window-size scaling, no softening) and GL_CLAMP_TO_EDGE
       (no wraparound bleed at the stretched edges). Force both here, same
       as gl_target_blit already does for its own source texture, so this
       path's visible output actually matches render_present_rgba's.
       Through gl_state_set_tex_parameters (shared cross-module cache,
       see gl_state_cache.h's own comment) so bind_texture_stages (d3d7.c)
       can correctly detect this texture was reparameterized behind its
       back the next time it's sampled as a D3D7 stage texture, instead
       of a raw glTexParameteri call it has no way to observe. */
    gl_state_set_tex_parameters(t->tex, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_NEAREST, GL_NEAREST);
    glUniform1i(g_present_unif_tex, 0);

    GLuint attr = (GLuint)g_present_attr_pos;
    glBindBuffer(GL_ARRAY_BUFFER, g_quad_pos_vbo);
    gl_state_set_vertex_attrib_array(attr, 2, GL_FLOAT, GL_FALSE, 0, g_quad_pos_vbo, (const void *)0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl_state_set_vertex_attrib_disabled(attr);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    /* TEMP DIAGNOSTIC (opt-in via LONGROAD2_DDRAW_TRACE): investigating a
       real, live black-screen report -- reads back a small patch of the
       real default-framebuffer content right after this present's own
       draw, before the swap, to tell "the source texture/geometry itself
       is black" apart from "the frame is fine but something after this
       point (compositor, swap) isn't showing it." Remove once resolved. */
    if (getenv("LONGROAD2_DDRAW_TRACE") != NULL && win_w > 0 && win_h > 0) {
        uint8_t *full = malloc((size_t)win_w * (size_t)win_h * 4);
        if (full != NULL) {
            glReadPixels(0, 0, win_w, win_h, GL_RGBA, GL_UNSIGNED_BYTE, full);
            long nonblack_rgb = 0;
            uint32_t max_r = 0, max_g = 0, max_b = 0;
            long total_px = (long)win_w * win_h;
            for (long i = 0; i < total_px; i++) {
                uint8_t r = full[i * 4 + 0], g = full[i * 4 + 1], b = full[i * 4 + 2];
                if (r != 0 || g != 0 || b != 0) nonblack_rgb++;
                if (r > max_r) max_r = r;
                if (g > max_g) max_g = g;
                if (b > max_b) max_b = b;
            }
            fprintf(stderr, "render: present_gl_target tex=%u src=%dx%d -> window=%dx%d FULL SCAN: "
                            "nonblack_pixels=%ld/%ld (%.2f%%) max_rgb=(%u,%u,%u)\n",
                    t->tex, t->w, t->h, win_w, win_h, nonblack_rgb, total_px,
                    100.0 * (double)nonblack_rgb / (double)total_px, max_r, max_g, max_b);
            free(full);
        }
    }

    present_trace("gl_target", win_w, win_h);
    present_dump_frame_if_enabled(win_w, win_h);
    frame_pace_wait();
    SDL_GL_SwapWindow(g_window);
}

static void render_present_paletted_impl(const uint8_t *indices, int width, int height, int row_stride_px,
                              const uint8_t *palette_rgba256)
{
    if (g_window == NULL || indices == NULL || palette_rgba256 == NULL || width <= 0 || height <= 0) return;
    if (!ensure_paletted_present_resources()) return;

    int win_w = 0, win_h = 0;
    present_reset_state_and_clear(&win_w, &win_h);

    gl_state_use_program(g_paletted_prog);

    gl_state_bind_texture_2d(GL_TEXTURE1, g_paletted_palette_tex);
    /* 1KB, trivial to just always re-upload rather than dirty-track --
       one 256x1 texel transfer is noise next to the index texture (up
       to width*height bytes) or the fullscreen draw itself. */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, palette_rgba256);

    gl_state_bind_texture_2d(GL_TEXTURE0, g_paletted_index_tex);
    /* GL_LUMINANCE, not GL_R8/GL_RED: legal on ES2 and ES3 alike, and the
       palette shader's texture2D(...).r reads the same index byte. */
    if (width != g_paletted_tex_w || height != g_paletted_tex_h) {
        upload_strided(0, GL_LUMINANCE, width, height, GL_LUMINANCE, GL_UNSIGNED_BYTE, 1, indices, row_stride_px);
        g_paletted_tex_w = width;
        g_paletted_tex_h = height;
    } else {
        upload_strided(1, GL_LUMINANCE, width, height, GL_LUMINANCE, GL_UNSIGNED_BYTE, 1, indices, row_stride_px);
    }

    glUniform1i(g_paletted_unif_index_tex, 0);
    glUniform1i(g_paletted_unif_palette_tex, 1);

    GLuint attr = (GLuint)g_paletted_attr_pos;
    glBindBuffer(GL_ARRAY_BUFFER, g_quad_pos_vbo);
    gl_state_set_vertex_attrib_array(attr, 2, GL_FLOAT, GL_FALSE, 0, g_quad_pos_vbo, (const void *)0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl_state_set_vertex_attrib_disabled(attr);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    /* Real, live-reproduced bug (see [[project_ddraw_blt_fbo_bind_elimination]]-
       style investigations): GL_TEXTURE1 is this present path's own
       borrowed unit (g_paletted_palette_tex, above), never touched by any
       other module -- but d3d7.c's bind_texture_stages (own comment:
       "stages 1-7 are permanently DISABLE'd for the entire session")
       stops examining a texture-stage unit entirely, forever, the first
       time it observes that stage already DISABLE'd, specifically to
       avoid ~10-12 redundant GL calls per stage per draw. A game that
       never uses texture stage 1+ (confirmed real for this present path's
       own game_loop_smoke_test regression: a plain untextured 3D quad)
       means unit 1 is NEVER rebound again after the very first draw --
       so if this function is ever called from something other than a
       Flip-driven present (a Blt or Lock/Unlock write straight to the
       primary surface, both real, live-confirmed call patterns), the
       very next D3D7 draw silently renders wrong/missing content, with
       nothing else in the whole pipeline ever revisiting unit 1 to
       notice or fix it. Cleaning up the ONE unit this function borrows
       removes the leak at its source rather than requiring d3d7.c's own
       permanently-disabled-stage fast path to lose the optimization it
       was written for. */
    gl_state_unbind_texture_everywhere(g_paletted_palette_tex);

    present_trace("paletted", win_w, win_h);
    present_dump_frame_if_enabled(win_w, win_h);
    frame_pace_wait();
    SDL_GL_SwapWindow(g_window);
}

int render_should_quit(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) return 1;
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) return 1;
    }
    return 0;
}

/* --- gl_target_t: FBO/texture resource management, see render.h --- */

static void gl_target_ensure_impl(gl_target_t *t, int w, int h, int as_render_target)
{
    if (t == NULL || w <= 0 || h <= 0) return;
    if (t->tex != 0 && t->w == w && t->h == h && !t->native565 && (!as_render_target || t->fbo != 0)) return;

    /* TEMP DIAGNOSTIC: a live report showed glGenTextures/glGenFramebuffers/
       glGenRenderbuffers all returning 0 (tex=0 depth_rb=0 fbo=0 in the
       framebuffer-incomplete log below) with glGetError()==GL_NO_ERROR --
       real GL never returns object name 0 from a successful Gen call, and
       a genuinely failed Gen call normally DOES set a real GL error. That
       combination is consistent with "no GL context is actually current
       on this thread right now" (SDL_GL_MakeCurrent never called, or a
       context got torn down) more than a normal driver-level failure.
       This confirms or rules that out directly rather than inferring it
       secondhand from Gen-call return values. */
    if (SDL_GL_GetCurrentContext() != g_gl_context) {
        fprintf(stderr, "gl_target_ensure: WRONG/NO GL CONTEXT CURRENT on this thread "
                        "(SDL_GL_GetCurrentContext()=%p, expected g_gl_context=%p)\n",
                (void *)SDL_GL_GetCurrentContext(), (void *)g_gl_context);
    }

    if (t->tex == 0 || t->w != w || t->h != h || t->native565) {
        if (t->tex == 0) glGenTextures(1, &t->tex);
        gl_state_bind_texture_2d(GL_TEXTURE0, t->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        gl_state_note_tex_size(t->tex, w, h);
        /* GL_REPEAT (not CLAMP_TO_EDGE) -- D3DTADDRESS_WRAP is D3D's own
           default addressing mode, and this gl_target_t is also used as
           a texture-stage sampler source (d3d7.c), where tiled textures
           (road surfaces, etc.) rely on wrapping. d3d7.c overrides this
           per draw call from the real D3DTSS_ADDRESSU/V state anyway;
           this is just the initial value. Through gl_state_set_tex_
           parameters (shared cache, see gl_state_cache.h) so that
           override is actually detected as a real change rather than a
           stale "unchanged" skip. */
        gl_state_set_tex_parameters(t->tex, GL_REPEAT, GL_REPEAT, GL_LINEAR, GL_LINEAR);
        /* Deliberately NOT unbinding unit 0 here. Tried once (to close a
           theoretical feedback-loop window between this bind and the
           FBO attachment below), and it looked right against one
           synthetic test -- but broke real gameplay (visible flickering,
           confirmed live) and was reverted. Whatever real-game case that
           unbind was fighting isn't understood yet; don't re-add it
           without root-causing that first. */
        t->native565 = 0;
    }

    if (as_render_target) {
        if (t->fbo == 0) glGenFramebuffers(1, &t->fbo);
        if (t->depth_rb == 0) glGenRenderbuffers(1, &t->depth_rb);

        glBindRenderbuffer(GL_RENDERBUFFER, t->depth_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, g_has_packed_depth_stencil ? GL_DEPTH24_STENCIL8_OES
                                                                          : GL_DEPTH_COMPONENT16, w, h);

        bind_fbo(t->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth_rb);
        if (g_has_packed_depth_stencil)
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->depth_rb);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            /* status==0 is NOT a real GL_FRAMEBUFFER_* value (those all
               start at 0x8CD5+) -- seen live on a real device, alongside
               shader compiles also failing with an empty info log in
               the same session. That combination doesn't match a normal
               "this specific FBO's attachments are wrong" failure;
               glGetError() here helps tell "real check, real
               incompleteness" apart from "the call itself couldn't
               execute" (bad texture/renderbuffer name, lost context, or
               an error already pending from something earlier in the
               same frame). */
            GLenum err = glGetError();
            fprintf(stderr, "gl_target_ensure: framebuffer incomplete (0x%04x, glGetError=0x%04x, "
                            "tex=%u depth_rb=%u fbo=%u %dx%d)\n",
                    status, err, t->tex, t->depth_rb, t->fbo, w, h);
        }
        bind_fbo(0);
    }

    t->w = w;
    t->h = h;
}

static void gl_target_bind_impl(gl_target_t *t)
{
    if (t == NULL || t->fbo == 0) return;
    /* t->tex may still be bound as a texture-stage sampler from an
       earlier, unrelated draw (see gl_state_unbind_texture_everywhere's
       own comment) -- about to become this FBO's color attachment target,
       so break that feedback loop for real before binding. */
    gl_state_unbind_texture_everywhere(t->tex);
    bind_fbo(t->fbo);
    gl_state_set_viewport(0, 0, t->w, t->h);
}

static void gl_target_read_rgba_impl(gl_target_t *t, uint8_t *out_rgba)
{
    if (t == NULL || t->fbo == 0 || out_rgba == NULL) return;

    bind_fbo(t->fbo);
    /* Unflipped: paired with the vertex shader's clip-space Y flip and
       the unflipped viewport in apply_device_viewport (d3d7.c). */
    glReadPixels(0, 0, t->w, t->h, GL_RGBA, GL_UNSIGNED_BYTE, out_rgba);
    bind_fbo(0);
}

static void gl_target_upload_rgba_impl(gl_target_t *t, const uint8_t *rgba, int w, int h)
{
    if (t == NULL || rgba == NULL || w <= 0 || h <= 0) return;
    gl_target_ensure(t, w, h, 0);
    if (t->tex == 0) return;
    /* Unflipped: paired with the vertex shader's clip-space Y flip and
       the unflipped viewport in apply_device_viewport (d3d7.c). */
    gl_state_bind_texture_2d(GL_TEXTURE0, t->tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

/* See render.h's own comment. GL_RGB (3-component, no alpha) sampled via
   texture2D() in GLSL ES always yields alpha=1.0 automatically -- the
   exact same "opaque" result every other alpha-less D3D pixel format
   (RGB888, XRGB8888, XRGB1555, ...) already produces via the CPU
   unpack_rgba path elsewhere in this codebase, so this needs no shader
   changes to look identical to that path's output. */
static int gl_target_ensure_native565_impl(gl_target_t *t, int w, int h)
{
    if (t == NULL || w <= 0 || h <= 0 || t->fbo != 0) return 0;
    if (t->tex != 0 && t->w == w && t->h == h && t->native565) return 1;

    if (t->tex == 0) glGenTextures(1, &t->tex);
    gl_state_bind_texture_2d(GL_TEXTURE0, t->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
    gl_state_note_tex_size(t->tex, w, h);
    /* Through gl_state_set_tex_parameters -- see gl_target_ensure's own
       identical conversion just above for why. */
    gl_state_set_tex_parameters(t->tex, GL_REPEAT, GL_REPEAT, GL_LINEAR, GL_LINEAR);

    t->w = w;
    t->h = h;
    t->native565 = 1;
    return 1;
}

static void gl_target_upload_native565_impl(gl_target_t *t, const uint8_t *rgb565, int w, int h)
{
    if (t == NULL || rgb565 == NULL || t->tex == 0) return;
    gl_state_bind_texture_2d(GL_TEXTURE0, t->tex);
    /* RGB565 is 2 bytes/pixel, but GL's default GL_UNPACK_ALIGNMENT is 4
       -- it assumes every row is padded out to a 4-byte boundary. This
       buffer is tightly packed instead (row stride == w*2 exactly, no
       padding -- see ddraw.c's own CreateSurface: pitch = width * bpp,
       always). For an ODD width, w*2 isn't a multiple of 4, so with the
       default alignment the driver would read 2 phantom padding bytes
       at the end of every row that aren't actually there, silently
       shifting every row after the first by a growing offset -- a real,
       reproducible source of row-shifted/"warped" textures for any
       odd-width RGB565 texture (confirmed: this codebase's own 4x4/
       square test textures happen to always be a multiple of 4 bytes
       wide regardless of alignment, so this went uncaught until a real
       odd-width case was tried). 1 is correct for any row width
       regardless of bytes-per-pixel; restored to GL's own default right
       after so nothing else touches a changed global default. */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, rgb565);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

/* --- GPU-resident Blt: draws src's texture into dst's FBO at a specific
   destination rect, with optional colorkey discard, without either side
   ever touching CPU memory -- used by ddraw.c's Blt (see surface7_Blt)
   to composite DirectDraw content on the GPU instead of a CPU pixel-copy
   loop, which is what let glReadPixels stop being called on every Blt
   (expensive on tile-based GPUs -- see [[project_ddraw_present_performance_fix]]
   and the plan this was implemented from).

   Positioning deliberately reuses the exact, already-proven convention
   d3d7.c's apply_device_viewport uses for XYZRHW/screen-space content,
   rather than deriving a new one: an unflipped glViewport/glScissor
   (dst_y measured top-down, exactly as DirectDraw's own rects are, no
   extra negation) narrowed to the destination rect, combined with a
   fixed, always-identical (-1..1) NDC quad -- the viewport transform
   alone maps that quad onto the requested sub-rect, so no per-blit NDC
   math is needed. Source UV addressing is separately just fractional
   texture-region addressing (src_rect/src dimensions) -- independent of
   the window-space question, since texture sampling uses its own UV
   convention (u=0/v=0 is simply the first stored texel, which is
   DirectDraw's own top-left row given gl_target_upload_rgba's own
   unflipped upload). This project's most notorious recurring bug class
   is exactly this kind of coordinate mismatch -- reusing a mapping
   that's already live-tested beats re-deriving one from scratch. */
static GLuint g_blit_prog = 0;
static GLint g_blit_attr_pos = -1;
static GLint g_blit_attr_uv = -1;
static GLint g_blit_unif_tex = -1;
static GLint g_blit_unif_use_ck = -1;
static GLint g_blit_unif_ck_low = -1;
static GLint g_blit_unif_ck_high = -1;

static const char *k_blit_vs =
    "#version 100\n"
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char *k_blit_fs =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform bool u_use_colorkey;\n"
    "uniform vec3 u_ck_low;\n"
    "uniform vec3 u_ck_high;\n"
    "void main() {\n"
    "    vec4 c = texture2D(u_tex, v_uv);\n"
    /* Same hard-cutout-discard technique already proven in d3d7.c's
       D3DRENDERSTATE_COLORKEYENABLE shader (k_fs_stage_template) -- real
       DirectDraw color-key Blt is a binary keep/reject per source texel,
       not a soft alpha blend. Same +-0.0032 epsilon and same reason:
       see k_fs_stage_template's own comment -- a native GL_RGB565
       sample's per-channel normalization (v/(2^N-1)) can disagree with
       u_ck_low/high's CPU-side bit-replicated-then-/255 computation by
       up to ~0.0028, enough to break an exact single-color key match
       entirely for a colorkeyed source Blt from a native565 surface. */
    "    if (u_use_colorkey && all(greaterThanEqual(c.rgb, u_ck_low - vec3(0.0032))) &&\n"
    "        all(lessThanEqual(c.rgb, u_ck_high + vec3(0.0032)))) {\n"
    "        discard;\n"
    "    }\n"
    "    gl_FragColor = c;\n"
    "}\n";

/* gl_target_blit's own per-call UV data (the position attribute reuses
   g_quad_pos_vbo above -- only the source sub-rect's UVs actually change
   between calls). A small (4 vertices x vec2 = 32 bytes) dynamic buffer,
   re-filled via glBufferSubData right before every draw -- same reasoning
   as g_quad_pos_vbo's own comment: avoiding a client-side pointer here is
   the point, not the data size (which was always trivial). */
static GLuint g_blit_uv_vbo = 0;

static int ensure_blit_resources(void)
{
    if (!ensure_quad_pos_vbo()) return 0;
    if (g_blit_uv_vbo == 0) {
        glGenBuffers(1, &g_blit_uv_vbo);
        if (g_blit_uv_vbo == 0) return 0;
        glBindBuffer(GL_ARRAY_BUFFER, g_blit_uv_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 8, NULL, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    if (g_blit_prog != 0) return 1;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, k_blit_vs);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, k_blit_fs);
    if (vs == 0 || fs == 0) {
        if (vs != 0) glDeleteShader(vs);
        if (fs != 0) glDeleteShader(fs);
        return 0;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "render: blit shader link failed: %s\n", log);
        glDeleteProgram(prog);
        return 0;
    }

    g_blit_prog = prog;
    g_blit_attr_pos = glGetAttribLocation(prog, "a_pos");
    g_blit_attr_uv = glGetAttribLocation(prog, "a_uv");
    g_blit_unif_tex = glGetUniformLocation(prog, "u_tex");
    g_blit_unif_use_ck = glGetUniformLocation(prog, "u_use_colorkey");
    g_blit_unif_ck_low = glGetUniformLocation(prog, "u_ck_low");
    g_blit_unif_ck_high = glGetUniformLocation(prog, "u_ck_high");
    return 1;
}

static int gl_target_blit_impl(gl_target_t *dst, int dst_x, int dst_y, int dst_w, int dst_h,
                    gl_target_t *src, int src_x, int src_y, int src_w, int src_h,
                    int use_colorkey, const uint8_t ck_low[3], const uint8_t ck_high[3])
{
    if (dst == NULL || src == NULL || dst->fbo == 0 || src->tex == 0) return 0;
    if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return 0;
    if (!ensure_blit_resources()) return 0;

    bind_fbo(dst->fbo);
    gl_reset_defensive_toggles();

    /* Enabled and narrowed, unlike present's (which disables it) -- a
       Blt must only affect dst_rect, matching both real Blt semantics
       and the CPU path's own rect clamping. glScissor, not just
       glViewport, is what's spec-guaranteed to actually clip the draw
       rather than just transform it (same reasoning already documented
       on apply_device_viewport in d3d7.c). */
    gl_state_set_scissor_enabled(1);
    gl_state_set_viewport(dst_x, dst_y, dst_w, dst_h);
    gl_state_set_scissor(dst_x, dst_y, dst_w, dst_h);

    gl_state_use_program(g_blit_prog);

    gl_state_bind_texture_2d(GL_TEXTURE0, src->tex);
    /* GL_NEAREST is required, not a quality choice -- the existing CPU
       Blt path stretches with integer nearest-neighbor sampling (no
       interpolation), and sharp sprite/colorkey edges must not be
       blended with neighboring texels. gl_target_ensure's default for a
       plain (non-render-target) texture is GL_LINEAR, and this texture
       object may also be sampled elsewhere (a D3D7 texture stage) with
       a different filter request -- reset explicitly every call rather
       than assume. Through gl_state_set_tex_parameters (shared cache,
       see gl_state_cache.h) rather than a raw glTexParameteri call, so
       d3d7.c's bind_texture_stages can actually detect "this got
       reparameterized since I last sampled it" instead of a texture-
       stage cache there wrongly thinking "unchanged" and keeping
       whatever filter this function last set. */
    gl_state_set_tex_parameters(src->tex, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_NEAREST, GL_NEAREST);
    glUniform1i(g_blit_unif_tex, 0);

    glUniform1i(g_blit_unif_use_ck, use_colorkey);
    if (use_colorkey) {
        float low3[3] = { ck_low[0] / 255.0f, ck_low[1] / 255.0f, ck_low[2] / 255.0f };
        float high3[3] = { ck_high[0] / 255.0f, ck_high[1] / 255.0f, ck_high[2] / 255.0f };
        glUniform3fv(g_blit_unif_ck_low, 1, low3);
        glUniform3fv(g_blit_unif_ck_high, 1, high3);
    }

    float u0 = (float)src_x / (float)src->w, u1 = (float)(src_x + src_w) / (float)src->w;
    float v0 = (float)src_y / (float)src->h, v1 = (float)(src_y + src_h) / (float)src->h;
    const GLfloat uv[] = { u0, v0,  u1, v0,  u0, v1,  u1, v1 };

    GLuint attr_pos = (GLuint)g_blit_attr_pos, attr_uv = (GLuint)g_blit_attr_uv;
    glBindBuffer(GL_ARRAY_BUFFER, g_quad_pos_vbo);
    gl_state_set_vertex_attrib_array(attr_pos, 2, GL_FLOAT, GL_FALSE, 0, g_quad_pos_vbo, (const void *)0);
    glBindBuffer(GL_ARRAY_BUFFER, g_blit_uv_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(uv), uv);
    gl_state_set_vertex_attrib_array(attr_uv, 2, GL_FLOAT, GL_FALSE, 0, g_blit_uv_vbo, (const void *)0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl_state_set_vertex_attrib_disabled(attr_pos);
    gl_state_set_vertex_attrib_disabled(attr_uv);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    /* No trailing bind_fbo(0) -- real, measured bug
       ([[project_ddraw_blt_fbo_bind_elimination]]): the documented real
       usage pattern for this function is MANY consecutive Blts to the
       SAME destination surface (ddraw.c's own comment: up to ~48 in one
       frame, one layered icon per Blt -- a real king.exe capture showed
       47-143 Blt calls/frame). Unconditionally rebinding FBO 0 here
       defeated bind_fbo's own cache for every single one of those calls:
       call N ends on FBO 0, so call N+1's own bind_fbo(dst->fbo) can
       never see a cache hit even when dst is identical to last time,
       forcing 2 real glBindFramebuffer calls per Blt instead of (after
       the first) zero. Every real caller that actually needs FBO 0 bound
       (present_reset_state_and_clear, gl_target_bind, gl_target_read_
       rgba) already binds it explicitly before touching it -- see each
       of their own comments -- so nothing downstream relies on this
       function leaving FBO 0 behind. */
    /* TEMP DIAGNOSTIC (opt-in via LONGROAD2_DDRAW_TRACE): investigating a
       real, live "renders as a solid white rectangle instead of the real
       texture image" report -- dst->fbo is still bound right here (no
       trailing unbind, see comment above), so a real glReadPixels tells
       us directly whether this GPU blit actually wrote real, varied pixel
       content or left the destination blank/white. Remove once resolved. */
    if (getenv("LONGROAD2_DDRAW_TRACE") != NULL && dst_w <= 256 && dst_h <= 256) {
        uint8_t *buf = malloc((size_t)dst_w * dst_h * 4);
        if (buf != NULL) {
            glReadPixels(dst_x, dst_y, dst_w, dst_h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
            long total = (long)dst_w * dst_h;
            long nonwhite = 0;
            for (long i = 0; i < total; i++) {
                if (buf[i * 4 + 0] != 255 || buf[i * 4 + 1] != 255 || buf[i * 4 + 2] != 255) nonwhite++;
            }
            fprintf(stderr, "[render] gl_target_blit: dst=%p %dx%d src=%p srctex=%u nonwhite_pixels=%ld/%ld "
                            "first_rgba=(%u,%u,%u,%u)\n",
                    (void *)dst, dst_w, dst_h, (void *)src, src->tex, nonwhite, total,
                    buf[0], buf[1], buf[2], buf[3]);
            free(buf);
        }
    }
    return 1;
}

static int gl_target_clear_rect_impl(gl_target_t *dst, int dst_x, int dst_y, int dst_w, int dst_h,
                          const uint8_t rgba[4])
{
    if (dst == NULL || dst->fbo == 0) return 0;
    if (dst_w <= 0 || dst_h <= 0) return 0;

    bind_fbo(dst->fbo);
    gl_reset_defensive_toggles();

    /* Enabled and narrowed, same convention gl_target_blit uses -- only
       dst_rect should be affected, not the whole surface. */
    gl_state_set_scissor_enabled(1);
    gl_state_set_viewport(dst_x, dst_y, dst_w, dst_h);
    gl_state_set_scissor(dst_x, dst_y, dst_w, dst_h);

    glClearColor(rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f, rgba[3] / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* No trailing bind_fbo(0) -- same reasoning as gl_target_blit's own
       identical removal just above (colorfill Blts hit this exact same
       consecutive-same-destination pattern too). */
    return 1;
}

static void gl_target_destroy_impl(gl_target_t *t)
{
    if (t == NULL) return;
    if (t->tex != 0) glDeleteTextures(1, &t->tex);
    if (t->fbo != 0) {
        /* Deleting the currently-bound FBO makes GL itself silently
           revert the real binding to 0 (spec behavior) -- bind_fbo's
           cache has no way to observe that side effect, so force it back
           to the "unknown" sentinel here whenever this is the one it
           thinks is bound, guaranteeing the next bind_fbo() call (to
           anything, not just 0) issues a real glBindFramebuffer instead
           of trusting a now-stale cached value. */
        if (t->fbo == g_current_fbo) g_current_fbo = (GLuint)-1;
        glDeleteFramebuffers(1, &t->fbo);
    }
    if (t->depth_rb != 0) glDeleteRenderbuffers(1, &t->depth_rb);
    memset(t, 0, sizeof(*t));
}

/* --- GL-thread dispatch wrappers ------------------------------------
   Every function above that touches real GL state has its real body
   renamed to a static _impl sibling; these thin wrappers are the ONLY
   ones exported (see render.h) and the only ones any other module ever
   calls. Each packs its arguments into a small on-stack struct and runs
   its _impl via gl_thread_call() -- see gl_thread.h's own comment for
   why a call already running ON the GL thread (e.g. gl_target_upload_
   rgba_impl's own nested call to gl_target_ensure below) bypasses the
   real cross-thread hop for free instead of deadlocking on itself.

   ddraw.c never calls a raw GL function directly anywhere (confirmed:
   every real GL touch point in that file goes through one of these) --
   so wrapping this one layer boundary is sufficient to make the whole
   DirectDraw Blt/Lock/Flip/present path thread-safe with no changes to
   ddraw.c at all. d3d7.c's own real GL work (shader compile, uniform
   upload, draw dispatch -- reached via clear_execute_now/draw_flat_
   execute_now/draw_strided_execute_now/cmdbuf_flush, and the
   independent d3d7_perf_report_for_frame) is wrapped the same way, at
   its own layer, in d3d7.c/d3d7_cmdbuf.c. */

typedef struct { float r, g, b; } render_clear_and_swap_args_t;
static void render_clear_and_swap_trampoline(void *arg)
{
    render_clear_and_swap_args_t *a = (render_clear_and_swap_args_t *)arg;
    render_clear_and_swap_impl(a->r, a->g, a->b);
}
void render_clear_and_swap(float r, float g, float b)
{
    render_clear_and_swap_args_t a = { r, g, b };
    gl_thread_call(render_clear_and_swap_trampoline, &a);
}

typedef struct { const uint8_t *rgba; int width, height, row_stride_px; } render_present_rgba_args_t;
static void render_present_rgba_trampoline(void *arg)
{
    render_present_rgba_args_t *a = (render_present_rgba_args_t *)arg;
    render_present_rgba_impl(a->rgba, a->width, a->height, a->row_stride_px);
}
void render_present_rgba(const uint8_t *rgba, int width, int height, int row_stride_px)
{
    render_present_rgba_args_t a = { rgba, width, height, row_stride_px };
    gl_thread_call(render_present_rgba_trampoline, &a);
}

typedef struct { gl_target_t *t; } render_present_gl_target_args_t;
static void render_present_gl_target_trampoline(void *arg)
{
    render_present_gl_target_args_t *a = (render_present_gl_target_args_t *)arg;
    render_present_gl_target_impl(a->t);
}
void render_present_gl_target(gl_target_t *t)
{
    render_present_gl_target_args_t a = { t };
    gl_thread_call(render_present_gl_target_trampoline, &a);
}

typedef struct {
    const uint8_t *indices; int width, height, row_stride_px;
    const uint8_t *palette_rgba256;
} render_present_paletted_args_t;
static void render_present_paletted_trampoline(void *arg)
{
    render_present_paletted_args_t *a = (render_present_paletted_args_t *)arg;
    render_present_paletted_impl(a->indices, a->width, a->height, a->row_stride_px, a->palette_rgba256);
}
void render_present_paletted(const uint8_t *indices, int width, int height, int row_stride_px,
                              const uint8_t *palette_rgba256)
{
    render_present_paletted_args_t a = { indices, width, height, row_stride_px, palette_rgba256 };
    gl_thread_call(render_present_paletted_trampoline, &a);
}

typedef struct { gl_target_t *t; int w, h, as_render_target; } gl_target_ensure_args_t;
static void gl_target_ensure_trampoline(void *arg)
{
    gl_target_ensure_args_t *a = (gl_target_ensure_args_t *)arg;
    gl_target_ensure_impl(a->t, a->w, a->h, a->as_render_target);
}
void gl_target_ensure(gl_target_t *t, int w, int h, int as_render_target)
{
    gl_target_ensure_args_t a = { t, w, h, as_render_target };
    gl_thread_call(gl_target_ensure_trampoline, &a);
}

typedef struct { gl_target_t *t; } gl_target_bind_args_t;
static void gl_target_bind_trampoline(void *arg)
{
    gl_target_bind_args_t *a = (gl_target_bind_args_t *)arg;
    gl_target_bind_impl(a->t);
}
void gl_target_bind(gl_target_t *t)
{
    gl_target_bind_args_t a = { t };
    gl_thread_call(gl_target_bind_trampoline, &a);
}

typedef struct { gl_target_t *t; uint8_t *out_rgba; } gl_target_read_rgba_args_t;
static void gl_target_read_rgba_trampoline(void *arg)
{
    gl_target_read_rgba_args_t *a = (gl_target_read_rgba_args_t *)arg;
    gl_target_read_rgba_impl(a->t, a->out_rgba);
}
void gl_target_read_rgba(gl_target_t *t, uint8_t *out_rgba)
{
    gl_target_read_rgba_args_t a = { t, out_rgba };
    gl_thread_call(gl_target_read_rgba_trampoline, &a);
}

typedef struct { gl_target_t *t; const uint8_t *rgba; int w, h; } gl_target_upload_rgba_args_t;
static void gl_target_upload_rgba_trampoline(void *arg)
{
    gl_target_upload_rgba_args_t *a = (gl_target_upload_rgba_args_t *)arg;
    gl_target_upload_rgba_impl(a->t, a->rgba, a->w, a->h);
}
void gl_target_upload_rgba(gl_target_t *t, const uint8_t *rgba, int w, int h)
{
    gl_target_upload_rgba_args_t a = { t, rgba, w, h };
    gl_thread_call(gl_target_upload_rgba_trampoline, &a);
}

typedef struct { gl_target_t *t; int w, h; int result; } gl_target_ensure_native565_args_t;
static void gl_target_ensure_native565_trampoline(void *arg)
{
    gl_target_ensure_native565_args_t *a = (gl_target_ensure_native565_args_t *)arg;
    a->result = gl_target_ensure_native565_impl(a->t, a->w, a->h);
}
int gl_target_ensure_native565(gl_target_t *t, int w, int h)
{
    gl_target_ensure_native565_args_t a = { t, w, h, 0 };
    gl_thread_call(gl_target_ensure_native565_trampoline, &a);
    return a.result;
}

typedef struct { gl_target_t *t; const uint8_t *rgb565; int w, h; } gl_target_upload_native565_args_t;
static void gl_target_upload_native565_trampoline(void *arg)
{
    gl_target_upload_native565_args_t *a = (gl_target_upload_native565_args_t *)arg;
    gl_target_upload_native565_impl(a->t, a->rgb565, a->w, a->h);
}
void gl_target_upload_native565(gl_target_t *t, const uint8_t *rgb565, int w, int h)
{
    gl_target_upload_native565_args_t a = { t, rgb565, w, h };
    gl_thread_call(gl_target_upload_native565_trampoline, &a);
}

typedef struct {
    gl_target_t *dst; int dst_x, dst_y, dst_w, dst_h;
    gl_target_t *src; int src_x, src_y, src_w, src_h;
    int use_colorkey; const uint8_t *ck_low; const uint8_t *ck_high;
    int result;
} gl_target_blit_args_t;
static void gl_target_blit_trampoline(void *arg)
{
    gl_target_blit_args_t *a = (gl_target_blit_args_t *)arg;
    a->result = gl_target_blit_impl(a->dst, a->dst_x, a->dst_y, a->dst_w, a->dst_h,
                                     a->src, a->src_x, a->src_y, a->src_w, a->src_h,
                                     a->use_colorkey, a->ck_low, a->ck_high);
}
int gl_target_blit(gl_target_t *dst, int dst_x, int dst_y, int dst_w, int dst_h,
                    gl_target_t *src, int src_x, int src_y, int src_w, int src_h,
                    int use_colorkey, const uint8_t ck_low[3], const uint8_t ck_high[3])
{
    gl_target_blit_args_t a = { dst, dst_x, dst_y, dst_w, dst_h,
                                 src, src_x, src_y, src_w, src_h,
                                 use_colorkey, ck_low, ck_high, 0 };
    gl_thread_call(gl_target_blit_trampoline, &a);
    return a.result;
}

typedef struct {
    gl_target_t *dst; int dst_x, dst_y, dst_w, dst_h; const uint8_t *rgba;
    int result;
} gl_target_clear_rect_args_t;
static void gl_target_clear_rect_trampoline(void *arg)
{
    gl_target_clear_rect_args_t *a = (gl_target_clear_rect_args_t *)arg;
    a->result = gl_target_clear_rect_impl(a->dst, a->dst_x, a->dst_y, a->dst_w, a->dst_h, a->rgba);
}
int gl_target_clear_rect(gl_target_t *dst, int dst_x, int dst_y, int dst_w, int dst_h,
                          const uint8_t rgba[4])
{
    gl_target_clear_rect_args_t a = { dst, dst_x, dst_y, dst_w, dst_h, rgba, 0 };
    gl_thread_call(gl_target_clear_rect_trampoline, &a);
    return a.result;
}

typedef struct { gl_target_t *t; } gl_target_destroy_args_t;
static void gl_target_destroy_trampoline(void *arg)
{
    gl_target_destroy_args_t *a = (gl_target_destroy_args_t *)arg;
    gl_target_destroy_impl(a->t);
}
void gl_target_destroy(gl_target_t *t)
{
    gl_target_destroy_args_t a = { t };
    gl_thread_call(gl_target_destroy_trampoline, &a);
}

static void delete_gl_context_trampoline(void *arg)
{
    (void)arg;
    /* Real fix, not defensive filler: waits for every real GPU command
       this process ever submitted to actually finish before tearing down
       the context that owns them. */
    glFinish();
    SDL_GL_DeleteContext(g_gl_context);
}

void render_shutdown(void)
{
    if (g_gl_context != NULL) {
        /* Real GL teardown -- must still run ON the GL thread (context is
           only ever current there now), so route it through gl_thread_call
           BEFORE gl_thread_shutdown() stops the thread and gl_thread_call's
           own inline fallback kicks in. */
        gl_thread_call(delete_gl_context_trampoline, NULL);
        gl_thread_shutdown();
        g_gl_context = NULL;
    }
    if (g_window != NULL) {
        SDL_DestroyWindow(g_window);
        g_window = NULL;
    }
    SDL_Quit();
}
