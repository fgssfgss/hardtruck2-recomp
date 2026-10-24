#ifndef LONGROAD2_GL_STATE_CACHE_H
#define LONGROAD2_GL_STATE_CACHE_H

#include <GLES2/gl2.h>

/* Cache-aware wrappers around the GL state d3d7.c's draw path and
   render.c's present/GPU-Blt path both mutate, interleaved within a
   single frame. Must be shared, not scoped to one file: bind_texture_
   stages (d3d7.c) already documents why its own LOCAL texture-bind
   cache was unsafe (blind to render.c mutating the same units) -- every
   call site that touches these categories has to go through here for
   "already current, skip it" to actually be true.

   glTexParameteri (per-texture-OBJECT, not per-unit -- a shared object
   can be reparameterized elsewhere between calls) is covered too, via
   gl_state_set_tex_parameters below, keyed by the real GL texture id
   rather than by unit -- see its own comment for why bind_texture_stages
   (d3d7.c) needed this specifically. FBO binding has its own
   pre-existing cache (bind_fbo in render.c), the one thing that's
   genuinely single-owner. */

void gl_state_reset(void);

void gl_state_use_program(GLuint program);
void gl_state_bind_texture_2d(GLenum unit, GLuint texture); /* unit: GL_TEXTURE0 + n; combines glActiveTexture+glBindTexture */
/* Real-unbinds `texture` from every unit the cache thinks holds it.
   Needed before rendering into a texture that may still be bound as a
   sampler from an earlier draw -- a real GL feedback loop, confirmed
   live to produce GL_INVALID_OPERATION. */
void gl_state_unbind_texture_everywhere(GLuint texture);

void gl_state_set_blend_enabled(int enabled);
void gl_state_set_blend_func(GLenum src, GLenum dst);

void gl_state_set_depth_test_enabled(int enabled);
void gl_state_set_depth_func(GLenum func);
void gl_state_set_depth_mask(GLboolean mask);

void gl_state_set_cull_enabled(int enabled);
void gl_state_set_cull_face(GLenum mode);
void gl_state_set_front_face(GLenum mode);

void gl_state_set_stencil_test_enabled(int enabled); /* d3d7.c sets STENCILENABLE; render.c force-disables it before a Blt/present -- same cross-module reasoning as everything else here */
void gl_state_set_stencil_func(GLenum func, GLint ref, GLuint mask);   /* glStencilFunc -- bundled into one valid flag, matches gl_state_set_blend_func's own bundling of src+dst */
void gl_state_set_stencil_op(GLenum fail, GLenum zfail, GLenum pass); /* glStencilOp -- bundled into one valid flag */
void gl_state_set_stencil_write_mask(GLuint mask); /* glStencilMask -- named _write_mask (not _mask) so it can't be confused with glStencilFunc's own "mask" (compare) parameter above */

void gl_state_set_viewport(GLint x, GLint y, GLsizei w, GLsizei h);
void gl_state_set_depth_range(GLfloat near_val, GLfloat far_val); /* glDepthRangef */

void gl_state_set_scissor_enabled(int enabled);
void gl_state_set_scissor(GLint x, GLint y, GLsizei w, GLsizei h);

void gl_state_set_dither_enabled(int enabled); /* glEnable/glDisable(GL_DITHER) */

/* Cache-aware glTexParameteri x4 (wrap S/T + min/mag filter), keyed by
   the real GL texture object id -- NOT by texture unit, since wrap/
   filter is per-object state (see this file's own top comment). Every
   real call site sets all 4 together, so they're bundled into one call,
   matching gl_state_set_blend_func's own bundling convention.

   Found via a real capture (LONGROAD2_ACTIVITY_SAMPLE=1,
   [[project_cooperative_activity_sampler]]): bind_texture_stages
   (d3d7.c) paid these 4 calls unconditionally on every draw specifically
   because a shared texture object can be reparameterized behind its
   back by render_present_gl_target or gl_target_blit (render.c) --
   5.6% of ALL render-frame activity samples in that capture, the single
   biggest piece of d3d_draw_prepare, for a hazard that in practice only
   fires once per Flip (or once per Blt) at most. This cache lets
   bind_texture_stages skip the common case (nothing changed) while
   still correctly detecting the rare case (render.c changed it) --
   render.c's own call sites must ALSO route through this function for
   that detection to actually work; a caller that bypasses it and calls
   raw glTexParameteri directly reintroduces the exact stale-cache bug
   this exists to prevent.

   texture==0 is a no-op (nothing to parameterize, matches every real
   call site's own `if (tex != 0)` guard). Fixed-size direct cache with
   round-robin eviction on a full-and-not-found miss -- safe either way
   (eviction can only cause an extra real GL call later, never a wrong
   skip), and the real number of distinct textures these call sites
   touch per frame is small in practice. */
#define GL_STATE_TEX_PARAM_CACHE_SIZE 64
/* GLES 2.0 without OES_texture_npot (hardtruck2-recomp): a texture whose
   size is not a power of two is incomplete -- samples black -- unless it
   clamps. render.c reports each texture's size and whether NPOT wrapping
   works; gl_state_set_tex_parameters below then quietly clamps such a
   texture instead of setting GL_REPEAT on it. */
void gl_state_set_npot_repeat_ok(int ok);
void gl_state_note_tex_size(GLuint texture, int w, int h);
void gl_state_set_tex_parameters(GLuint texture, GLint wrap_s, GLint wrap_t,

                                  GLint min_filter, GLint mag_filter);
long gl_state_get_tex_param_calls_for_test(void);
long gl_state_get_tex_param_skips_for_test(void);
void gl_state_reset_tex_param_counters_for_test(void);

/* GLES3-guaranteed minimum for GL_MAX_VERTEX_ATTRIBS -- real usage here is
   <=5 named locations in d3d7.c's own shader variants, <=2 in render.c's
   three present/Blt programs. Generous headroom, not a tight bound. */
#define GL_STATE_MAX_VERTEX_ATTRIBS 16

/* Cache-aware glEnableVertexAttribArray(index) + glVertexAttribPointer(...).
   `buffer` is the GL_ARRAY_BUFFER name the caller has ALREADY bound (this
   does NOT call glBindBuffer itself -- every real call site already has an
   unavoidable prior bind via glBufferData/glBufferSubData). Required, not
   optional, for correctness: it's what lets this cache tell apart two
   callers (d3d7.c vs render.c) that legitimately reuse the same numeric
   attribute index for unrelated data -- see this file's own top comment on
   why nothing here can be scoped to one module. `index` values outside
   [0, GL_STATE_MAX_VERTEX_ATTRIBS) -- including the (GLuint)-1 a caller
   gets back from glGetAttribLocation when an attribute is declared but
   never actually read in that particular shader variant (a real, common
   case: every RHW vertex-shader variant never reads a_normal, and
   LONGROAD2_CHEAP_SHADER's diagnostic vertex shader doesn't declare it at
   all) -- pass through uncached, same as gl_state_bind_texture_2d's own
   out-of-range-unit handling. */
void gl_state_set_vertex_attrib_array(GLuint index, GLint size, GLenum type,
                                       GLboolean normalized, GLsizei stride,
                                       GLuint buffer, const void *offset);
/* Cache-aware glDisableVertexAttribArray(index). Same out-of-range passthrough. */
void gl_state_set_vertex_attrib_disabled(GLuint index);

/* Test-only observability, mirroring d3d7_perf_get_gl_draws_for_test's own
   established pattern (d3d7.h) -- must live here, not in d3d7.c, since this
   cache is the one thing that can also observe render.c's own calls. */
long gl_state_get_vertex_attrib_calls_for_test(void);
long gl_state_get_vertex_attrib_skips_for_test(void);
void gl_state_reset_vertex_attrib_counters_for_test(void);

#endif /* LONGROAD2_GL_STATE_CACHE_H */
