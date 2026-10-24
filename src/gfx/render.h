#ifndef LONGROAD2_RENDER_H
#define LONGROAD2_RENDER_H

/*
 * SDL2 window/GLES context setup. Milestone 2 preparation: just enough to
 * open a real window with a working GLES 3.2 context and prove it on a
 * normal desktop, before this gets wired into the CreateWindowEx shim
 * (src/shim/user32/) and the DirectDraw-surface compositing path
 * (src/shim/ddraw/) that will actually use it.
 *
 * Deliberately does NOT set SDL_VIDEODRIVER anywhere in code -- that's an
 * environment variable set at deployment time (kmsdrm on the ARM
 * console; unset here, so SDL picks whatever this desktop already uses --
 * x11/wayland). See AGENT.md.
 */

#include <stdint.h>

int render_init(int width, int height, const char *title);

/* True once render_init() has succeeded and render_shutdown() hasn't run
   yet -- lets platform_create_window() tell whether it's creating the
   real backing window for the first time or just resizing/retitling the
   one that already exists. */
int render_is_initialized(void);

void render_resize(int width, int height);
void render_set_title(const char *title);
void render_show(int show);

/* Current client size and the window's screen-space origin -- backs
   user32's GetClientRect/GetWindowRect/ClientToScreen/ScreenToClient for
   the one HWND that has a real SDL2 window behind it. This is always the
   real window's actual pixel size: LONGROAD2_TARGET_RESOLUTION (when
   set) locks it to one fixed size and the game's own requested modes
   (SetDisplayMode, CreateWindow dims) are ignored outright rather than
   tracked separately -- see render.c's own comment on g_target_w/h for
   why (a real dx7z.dll SIGSEGV, traced to exactly this kind of
   real-window-vs-reported-size mismatch). */
void render_get_size(int *out_w, int *out_h);
void render_get_screen_pos(int *out_x, int *out_y);

/* LONGROAD2_TARGET_RESOLUTION's own pinned size, available even before a
   window exists (unlike render_get_size, which needs g_window != NULL) --
   for callers that need to answer a "what's the current display mode"
   query (ddraw's own GetDisplayMode) consistently with the one true
   resolution this process ever reports, not the real host desktop size.
   Writes 0,0 to both if unset. */
void render_get_target_resolution(int *out_w, int *out_h);

/* Clears to the given color and swaps -- a visible "yes, this is alive"
   placeholder frame. Only used by tests/render_smoke_test.c now that
   src/shim/ddraw/ exists and drives real frames via render_present_rgba. */
void render_clear_and_swap(float r, float g, float b);

/* The actual DirectDraw-surface compositing path (src/shim/ddraw/): takes
   a tightly-packed top-down RGBA8888 CPU buffer (already converted from
   whatever native DDraw pixel format the surface is in -- this function
   knows nothing about DDraw), uploads it as a texture, draws it as a
   fullscreen quad stretched to the window's current size, and swaps.
   row_stride_px is the buffer's row stride in pixels (may exceed width if
   the caller didn't tightly pack it). No-op if no window exists yet. */
void render_present_rgba(const uint8_t *rgba, int width, int height, int row_stride_px);

/* Same job as render_present_rgba, but for an 8-bit palette-indexed CPU
   buffer instead of pre-converted RGBA8888 -- does the index->color
   resolution on the GPU (a classic palette-texture fragment shader
   lookup) instead of a per-pixel CPU loop. This game's own primary/3D
   render-target surface is confirmed real 8bpp paletted content, and
   this is the buffer that gets presented every displayed frame, so this
   is the one CPU-side conversion in the present path actually worth
   moving to the GPU (see ddraw.c's surface7_present, which picks this
   over render_present_rgba specifically for that format). indices is a
   tightly-packed-per-row (row_stride_px may exceed width) buffer of raw
   palette index bytes, one per pixel, same top-down row order
   render_present_rgba expects. palette_rgba256 is exactly 256*4 bytes,
   already RGBA8 (not the DirectDraw PALETTEENTRY layout -- alpha is
   meaningless there, callers must supply real alpha, normally 255). No-op
   if no window exists yet, same as render_present_rgba. */
void render_present_paletted(const uint8_t *indices, int width, int height, int row_stride_px,
                              const uint8_t *palette_rgba256);

/* Pumps the SDL event queue and reports whether the window was asked to
   close (X button or Escape). Real SDL-event -> Win32-message translation
   is src/platform/'s job, not this -- this is just enough for a smoke
   test to exit cleanly. Not used by the platform/user32 message pump
   path (src/platform/platform.c polls SDL events itself) -- only the
   standalone smoke test (tests/render_smoke_test.c) calls this. */
int render_should_quit(void);

void render_shutdown(void);

/*
 * GL-backed pixel storage for one DirectDraw surface used as a D3D7
 * render target or (later milestone) texture-stage sampler source.
 * Pure GL resource management -- no knowledge of DirectDraw/D3D7 concepts
 * (surfaces, dirty flags, CPU buffers); src/shim/ddraw/ddraw.c owns one
 * of these per surface7_t and is responsible for all cross-module
 * bookkeeping (see ddraw.c's gl_target field + surface_sync_t
 * sync_state, and d3d7.c's d3d7_surface_interop_t which is how d3d7.c
 * reaches this without shim_d3d7 linking shim_ddraw -- would create a link cycle,
 * since shim_ddraw already links shim_d3d7).
 */
typedef struct {
    unsigned tex;      /* GLuint, 0 if not yet created */
    unsigned fbo;      /* GLuint, 0 unless ensured as a render target */
    unsigned depth_rb; /* GLuint, paired 1:1 with fbo (depth+stencil) */
    int w, h;
    /* 1 if tex currently holds a plain (non-FBO) native GL_RGB565
       texture via gl_target_ensure_native565/gl_target_upload_native565,
       instead of gl_target_ensure's usual RGBA8 storage -- see those
       functions' own comments. Always forced back to 0 by a subsequent
       gl_target_ensure() call (RGBA8 realloc), so a target that's ever
       used as a render target or general RGBA8 texture safely reclaims
       proper storage regardless of this flag's prior state. */
    int native565;
} gl_target_t;

/* (Re)allocates tex (and, if as_render_target, fbo+depth_rb) to be w x h,
   only touching GL objects that are missing or the wrong size. Binds
   nothing -- call gl_target_bind() separately to render into it. */
void gl_target_ensure(gl_target_t *t, int w, int h, int as_render_target);

/* Binds t->fbo (must have been gl_target_ensure()'d with as_render_target)
   as the current framebuffer and sets the GL viewport to its full size. */
void gl_target_bind(gl_target_t *t);

/* Reads t->fbo's color attachment back into a tightly packed top-down
   (row 0 = top, matching DirectDraw's surface buffer convention)
   RGBA8888 CPU buffer of size t->w * t->h * 4. Restores the default
   framebuffer binding before returning. */
void gl_target_read_rgba(gl_target_t *t, uint8_t *out_rgba);

/* Ensures t->tex exists at w x h (as a plain texture, not a render
   target) and uploads a tightly packed top-down RGBA8888 CPU buffer
   into it. Unlike gl_target_read_rgba, no row-flip is needed here --
   glTexImage2D has no framebuffer-vs-window-space orientation quirk,
   it just stores the rows it's given in the order given, and texture
   sampling at v=0 fetches whichever row was given first. */
void gl_target_upload_rgba(gl_target_t *t, const uint8_t *rgba, int w, int h);

/* Ensures t->tex exists at w x h as a plain (non-FBO) NATIVE GL_RGB565
   texture -- no upload, mirrors gl_target_ensure's own ensure/upload
   split. Returns 0 (leaving t untouched) if t->fbo != 0: a target
   that's ever been used as a D3D7 render target must stay RGBA8 for
   FBO/blending/present correctness and is never downgraded to a
   format with no alpha channel, even if later used only as a plain
   texture. Returns 1 otherwise (freshly created, or already correct
   size/format -- a no-op in the latter case, like gl_target_ensure). */
int gl_target_ensure_native565(gl_target_t *t, int w, int h);

/* Uploads a tightly packed top-down RGB565 CPU buffer directly into a
   target already gl_target_ensure_native565()'d -- zero CPU-side
   format conversion, since DirectDraw's own RGB565 bit layout
   (R:11-15,G:5-10,B:0-4) is bit-for-bit identical to GL's own
   GL_UNSIGNED_SHORT_5_6_5 packing. No-op if t->tex == 0 (ensure wasn't
   called, or was refused). */
void gl_target_upload_native565(gl_target_t *t, const uint8_t *rgb565, int w, int h);

/* Draws src's texture into dst's FBO at the given destination rect
   (dst_x/dst_y/dst_w/dst_h, top-down DirectDraw-style pixel coordinates
   within dst's own w x h), sampling the given source sub-rect
   (src_x/src_y/src_w/src_h, same convention within src's own w x h),
   entirely GPU-side -- no CPU memory is read or written by this call.
   dst must already be gl_target_ensure()'d as a render target; src must
   already have a valid tex (gl_target_ensure()'d, plain or render
   target, either is fine -- only ->tex is read). If use_colorkey, any
   sampled source texel whose RGB falls within [ck_low, ck_high]
   (inclusive, 0-255 per channel) is discarded rather than written,
   matching real DirectDraw DDBLT_KEYSRC semantics -- a hard cutout, not
   a blend. Does NOT restore the default framebuffer binding before
   returning (real, measured fix -- see
   [[project_ddraw_blt_fbo_bind_elimination]]): the real usage pattern is
   many consecutive Blts to the same destination, and every caller that
   actually needs FBO 0 bound already binds it explicitly itself before
   touching it. Returns 0 (no-op) if dst/src aren't ready or any rect is
   empty -- callers should fall back to a CPU-side Blt in that case. */
int gl_target_blit(gl_target_t *dst, int dst_x, int dst_y, int dst_w, int dst_h,
                    gl_target_t *src, int src_x, int src_y, int src_w, int src_h,
                    int use_colorkey, const uint8_t ck_low[3], const uint8_t ck_high[3]);

/* Simpler sibling of gl_target_blit: fills a rect of dst's FBO with a
   flat color instead of sampling a source texture -- no shader/program
   needed, just a scissored glClear. dst_x/dst_y/dst_w/dst_h are the same
   top-down DirectDraw-style pixel-coordinate convention gl_target_blit
   uses. rgba is 4 bytes, 0-255 per channel. dst must already be
   gl_target_ensure()'d as a render target. Does NOT restore the default
   framebuffer binding before returning -- see gl_target_blit's own
   comment on why. Returns 0 (no-op) if dst isn't ready or the rect is
   empty -- callers should fall back to a CPU-side fill in that case. */
int gl_target_clear_rect(gl_target_t *dst, int dst_x, int dst_y, int dst_w, int dst_h,
                          const uint8_t rgba[4]);

/* bind_fbo() call/skip test hooks (render.c) -- same idiom used
   throughout this codebase's other caches. Proves gl_target_blit/
   gl_target_clear_rect's own removed trailing bind_fbo(0) (see their
   comments) actually eliminates the redundant rebind for consecutive
   same-destination Blts, not just "looks right by inspection." */
long render_get_fbo_bind_calls_for_test(void);
long render_get_fbo_bind_skips_for_test(void);
void render_reset_fbo_bind_counters_for_test(void);

/* Same visible effect as render_present_rgba, but samples t->tex directly
   as the present quad's source instead of uploading a CPU buffer into a
   separate texture first -- for a surface whose GPU-resident content is
   already known-current (ddraw.c's own surface_sync_t sync_state
   decides when that's true; this function itself has no opinion, it
   just draws whatever t->tex currently holds). t->tex already stores
   real RGBA colors regardless of the surface's nominal DirectDraw pixel
   format -- GPU rendering (a real D3D7 draw, or gl_target_blit) can only
   ever produce real colors, never literal palette index bytes, so this
   is correct to call even for a nominally palette-indexed surface as
   long as its ->gl is the authoritative side. Shares the exact same
   vertex/fragment shader as render_present_rgba (t->tex has the same
   row-0-is-DirectDraw's-top convention as a CPU-uploaded texture,
   whether it got there via gl_target_upload_rgba or a real GL draw --
   see gl_target_read_rgba's own doc comment and apply_device_viewport's
   unflipped convention -- so no new UV mapping is needed here). No-op
   if no window exists yet or t has no valid texture. */
void render_present_gl_target(gl_target_t *t);

void gl_target_destroy(gl_target_t *t);

#endif /* LONGROAD2_RENDER_H */
