/*
 * gfx_host.c - the window and input under the DirectDraw 7 / D3D7 bridge,
 * and the few LongRoad2 platform/kernel32 calls src/gfx/ makes.
 *
 * LongRoad2 opened its window when the game showed its main window; here
 * win32hle owns the Win32 windows (with no display of their own), and the
 * real SDL window is the one render_init makes for DirectDraw, opened on the
 * first SetDisplayMode. Its SDL events go to win32hle as input -- the same
 * way win32hle's own screen.c does for DirectDraw 1-3 games -- from the
 * pump hook, which runs whenever the game finds its message queue empty.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "gfx_host.h"
#include "../gfx/render.h"
#include "../gfx/platform.h"
#include "../gfx/kernel32.h"

/* the messages fed to win32hle (each win32hle file defines its own, too) */
#define WM_CLOSE       0x0010u
#define WM_MOUSEMOVE   0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP   0x0202u
#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP   0x0205u
#define WM_MBUTTONDOWN 0x0207u
#define WM_MBUTTONUP   0x0208u

static int g_mode_w = 640, g_mode_h = 480;


/* Window pixels -> display-mode pixels: render.c stretches the picture over
 * the whole window. */
static void to_game(int x, int y, int *gx, int *gy) {
    int ww = 0, wh = 0;
    render_get_size(&ww, &wh);
    *gx = ww > 0 ? x * g_mode_w / ww : x;
    *gy = wh > 0 ? y * g_mode_h / wh : y;
    if (*gx < 0) *gx = 0;
    if (*gy < 0) *gy = 0;
    if (*gx >= g_mode_w) *gx = g_mode_w - 1;
    if (*gy >= g_mode_h) *gy = g_mode_h - 1;
}

/* HT2_INPUT: scripted input for unattended runs, in display-mode pixels and
 * milliseconds since the window opened:
 *     HT2_INPUT="3000:click:90,395;9000:key:13;12000:move:400,300"
 * click = move there, left button down and up; key = a virtual key's down
 * and up (13 = VK_RETURN, 27 = VK_ESCAPE). Each step runs once, in order. */
static struct { uint32_t at; char what; int a, b; } g_script[64];
static int g_script_n, g_script_next;
static uint32_t g_t0;
static void script_load(void) {
    const char *s = getenv("HT2_INPUT");
    while (s && *s && g_script_n < 64) {
        unsigned at; char what[8]; int a = 0, b = 0, used = 0;
        if (sscanf(s, "%u:%7[a-z]:%d,%d%n", &at, what, &a, &b, &used) >= 3 ||
            sscanf(s, "%u:%7[a-z]:%d%n", &at, what, &a, &used) >= 3) {
            g_script[g_script_n].at = at, g_script[g_script_n].what = what[0];
            g_script[g_script_n].a = a, g_script[g_script_n].b = b;
            g_script_n++;
        }
        s = strchr(s, ';');
        if (s) s++;
    }
    if (g_script_n) fprintf(stderr, "[gfx] HT2_INPUT: %d scripted steps\n", g_script_n);
}
static void script_step(void) {
    uint32_t now = SDL_GetTicks() - g_t0;
    while (g_script_next < g_script_n && now >= g_script[g_script_next].at) {
        int a = g_script[g_script_next].a, b = g_script[g_script_next].b;
        switch (g_script[g_script_next].what) {
        case 'c': hle_input_mouse(WM_MOUSEMOVE, a, b); hle_input_mouse(WM_LBUTTONDOWN, a, b);
                  hle_input_mouse(WM_LBUTTONUP, a, b); break;
        case 'm': hle_input_mouse(WM_MOUSEMOVE, a, b); break;
        case 'k': hle_input_key(1, (uint32_t)a, 0); hle_input_key(0, (uint32_t)a, 0); break;
        }
        fprintf(stderr, "[gfx] HT2_INPUT step %d at %u ms\n", g_script_next, now);
        g_script_next++;
    }
}

static void pump(void) {
    SDL_Event e;
    script_step();
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: {
            uint32_t h = hle_first_hwnd();
            if (h) hle_post_message(h, WM_CLOSE, 0, 0); else hle_exit(0);
            break;
        }
        case SDL_KEYDOWN: case SDL_KEYUP: {
            int vk = hle_sdl_vk(e.key.keysym.scancode);
            if (vk) hle_input_key(e.type == SDL_KEYDOWN, (uint32_t)vk, hle_sdl_scan(e.key.keysym.scancode));
            break;
        }
        case SDL_MOUSEMOTION: {
            int gx, gy;
            to_game(e.motion.x, e.motion.y, &gx, &gy);
            hle_input_mouse(WM_MOUSEMOVE, gx, gy);
            break;
        }
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
            int gx, gy, down = e.type == SDL_MOUSEBUTTONDOWN;
            to_game(e.button.x, e.button.y, &gx, &gy);
            uint32_t m = e.button.button == SDL_BUTTON_RIGHT ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP)
                       : e.button.button == SDL_BUTTON_MIDDLE ? (down ? WM_MBUTTONDOWN : WM_MBUTTONUP)
                       : (down ? WM_LBUTTONDOWN : WM_LBUTTONUP);
            hle_input_mouse(m, gx, gy);
            break;
        }
        default: break;
        }
    }
}

void gfx_host_open(int w, int h) {
    if (w <= 0 || h <= 0) w = g_mode_w, h = g_mode_h;
    if (w != g_mode_w || h != g_mode_h || !render_is_initialized())
        fprintf(stderr, "[gfx] display mode %dx%d\n", w, h);

    g_mode_w = w, g_mode_h = h;
    hle_set_screen_size(w, h);
    if (render_is_initialized()) return;
    if (render_init(w, h, "Hard Truck 2") != 0) {
        fprintf(stderr, "[gfx] no GLES window: the game runs, but shows nothing\n");
        return;
    }
    SDL_ShowCursor(SDL_DISABLE);         /* the game draws its own */
    g_t0 = SDL_GetTicks();
    script_load();

    hle_set_pump_hook(pump);
}

/* ---- LongRoad2 platform.h / kernel32.h ---- */
void platform_get_screen_size(int *out_w, int *out_h) {
    *out_w = g_mode_w;
    *out_h = g_mode_h;
}
void platform_perf_report_for_frame(double *out_wait_ms, long *out_wait_calls) {
    *out_wait_ms = 0;
    *out_wait_calls = 0;
}
void kernel32_perf_report_for_frame(double *out_cs_wait_ms, long *out_cs_calls, long *out_cs_contended,
                                    double *out_wfso_wait_ms, long *out_wfso_calls, long *out_wfso_contended) {
    *out_cs_wait_ms = 0, *out_cs_calls = 0, *out_cs_contended = 0;
    *out_wfso_wait_ms = 0, *out_wfso_calls = 0, *out_wfso_contended = 0;
}
