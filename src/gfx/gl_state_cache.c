#include "gl_state_cache.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define GL_STATE_MAX_TEX_UNITS 8 /* matches d3d7.c's own MAX_TEX_STAGES; render.c never uses more than 2 */

typedef struct {
    int valid;
    int enabled;
    GLint size;
    GLenum type;
    GLboolean normalized;
    GLsizei stride;
    GLuint buffer;
    const void *offset;
} vertex_attrib_slot_t;

typedef struct {
    int valid;
    GLuint texture;
    GLint wrap_s, wrap_t, min_filter, mag_filter;
} tex_param_entry_t;

static struct {
    GLuint program;

    GLuint bound_tex[GL_STATE_MAX_TEX_UNITS];
    int bound_tex_valid[GL_STATE_MAX_TEX_UNITS];

    int blend_enabled; /* -1: unknown */
    GLenum blend_src, blend_dst;
    int blend_func_valid;

    int depth_test_enabled;
    GLenum depth_func;
    int depth_func_valid;
    GLboolean depth_mask; /* 0xFF: unknown -- GL_TRUE/GL_FALSE never equal it */

    int cull_enabled;
    GLenum cull_face;
    int cull_face_valid;
    GLenum front_face;
    int front_face_valid;

    int stencil_enabled;
    GLenum stencil_func;
    GLint stencil_ref;
    GLuint stencil_func_mask;
    int stencil_func_valid;
    GLenum stencil_fail, stencil_zfail, stencil_pass;
    int stencil_op_valid;
    GLuint stencil_write_mask;
    int stencil_write_mask_valid;

    int viewport_valid;
    GLint viewport_x, viewport_y;
    GLsizei viewport_w, viewport_h;
    int depth_range_valid;
    GLfloat depth_range_near, depth_range_far;

    int scissor_enabled;
    int scissor_valid;
    GLint scissor_x, scissor_y;
    GLsizei scissor_w, scissor_h;

    int dither_enabled;

    vertex_attrib_slot_t vertex_attrib[GL_STATE_MAX_VERTEX_ATTRIBS];

    tex_param_entry_t tex_param[GL_STATE_TEX_PARAM_CACHE_SIZE];
    int tex_param_next_evict; /* round-robin index, used only when the cache is full AND the texture isn't already in it */
} g_state;

/* Test-only, separate from g_state proper (which gl_state_reset() clears)
   -- counters must survive a gl_state_reset() so a test can reset+drive a
   scene and still read back what happened, matching
   d3d7_perf_report_for_frame's own "read, then the CALLER decides whether
   to reset" convention rather than auto-clearing on every state reset. */
static long g_vertex_attrib_calls;
static long g_vertex_attrib_skips;

void gl_state_reset(void)
{
    g_state.program = (GLuint)-1;
    for (int i = 0; i < GL_STATE_MAX_TEX_UNITS; i++) g_state.bound_tex_valid[i] = 0;
    g_state.blend_enabled = -1;
    g_state.blend_func_valid = 0;
    g_state.depth_test_enabled = -1;
    g_state.depth_func_valid = 0;
    g_state.depth_mask = 0xFFu;
    g_state.cull_enabled = -1;
    g_state.cull_face_valid = 0;
    g_state.front_face_valid = 0;
    g_state.stencil_enabled = -1;
    g_state.stencil_func_valid = 0;
    g_state.stencil_op_valid = 0;
    g_state.stencil_write_mask_valid = 0;
    g_state.viewport_valid = 0;
    g_state.depth_range_valid = 0;
    g_state.scissor_enabled = -1;
    g_state.scissor_valid = 0;
    g_state.dither_enabled = -1;
    for (int i = 0; i < GL_STATE_MAX_VERTEX_ATTRIBS; i++) g_state.vertex_attrib[i].valid = 0;
    for (int i = 0; i < GL_STATE_TEX_PARAM_CACHE_SIZE; i++) g_state.tex_param[i].valid = 0;
    g_state.tex_param_next_evict = 0;
}

void gl_state_use_program(GLuint program)
{
    if (g_state.program == program) return;
    glUseProgram(program);
    g_state.program = program;
}

void gl_state_bind_texture_2d(GLenum unit, GLuint texture)
{
    unsigned idx = (unsigned)(unit - GL_TEXTURE0);
    if (idx >= GL_STATE_MAX_TEX_UNITS) { /* out-of-range unit: always safe to just pass through uncached */
        glActiveTexture(unit);
        glBindTexture(GL_TEXTURE_2D, texture);
        return;
    }
    if (g_state.bound_tex_valid[idx] && g_state.bound_tex[idx] == texture) return;
    glActiveTexture(unit);
    glBindTexture(GL_TEXTURE_2D, texture);
    g_state.bound_tex[idx] = texture;
    g_state.bound_tex_valid[idx] = 1;
}

void gl_state_unbind_texture_everywhere(GLuint texture)
{
    if (texture == 0) return;
    for (unsigned i = 0; i < GL_STATE_MAX_TEX_UNITS; i++) {
        if (g_state.bound_tex_valid[i] && g_state.bound_tex[i] == texture) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, 0);
            g_state.bound_tex[i] = 0;
        }
    }
}

void gl_state_set_blend_enabled(int enabled)
{
    enabled = enabled != 0;
    if (g_state.blend_enabled == enabled) return;
    if (enabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    g_state.blend_enabled = enabled;
}

void gl_state_set_blend_func(GLenum src, GLenum dst)
{
    if (g_state.blend_func_valid && g_state.blend_src == src && g_state.blend_dst == dst) return;
    glBlendFunc(src, dst);
    g_state.blend_src = src;
    g_state.blend_dst = dst;
    g_state.blend_func_valid = 1;
}

void gl_state_set_depth_test_enabled(int enabled)
{
    enabled = enabled != 0;
    if (g_state.depth_test_enabled == enabled) return;
    if (enabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    g_state.depth_test_enabled = enabled;
}

void gl_state_set_depth_func(GLenum func)
{
    if (g_state.depth_func_valid && g_state.depth_func == func) return;
    glDepthFunc(func);
    g_state.depth_func = func;
    g_state.depth_func_valid = 1;
}

void gl_state_set_depth_mask(GLboolean mask)
{
    if (g_state.depth_mask == mask) return;
    glDepthMask(mask);
    g_state.depth_mask = mask;
}

void gl_state_set_cull_enabled(int enabled)
{
    enabled = enabled != 0;
    if (g_state.cull_enabled == enabled) return;
    if (enabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    g_state.cull_enabled = enabled;
}

void gl_state_set_cull_face(GLenum mode)
{
    if (g_state.cull_face_valid && g_state.cull_face == mode) return;
    glCullFace(mode);
    g_state.cull_face = mode;
    g_state.cull_face_valid = 1;
}

void gl_state_set_front_face(GLenum mode)
{
    if (g_state.front_face_valid && g_state.front_face == mode) return;
    glFrontFace(mode);
    g_state.front_face = mode;
    g_state.front_face_valid = 1;
}

void gl_state_set_stencil_test_enabled(int enabled)
{
    enabled = enabled != 0;
    if (g_state.stencil_enabled == enabled) return;
    if (enabled) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    g_state.stencil_enabled = enabled;
}

void gl_state_set_stencil_func(GLenum func, GLint ref, GLuint mask)
{
    if (g_state.stencil_func_valid && g_state.stencil_func == func &&
        g_state.stencil_ref == ref && g_state.stencil_func_mask == mask) return;
    glStencilFunc(func, ref, mask);
    g_state.stencil_func = func;
    g_state.stencil_ref = ref;
    g_state.stencil_func_mask = mask;
    g_state.stencil_func_valid = 1;
}

void gl_state_set_stencil_op(GLenum fail, GLenum zfail, GLenum pass)
{
    if (g_state.stencil_op_valid && g_state.stencil_fail == fail &&
        g_state.stencil_zfail == zfail && g_state.stencil_pass == pass) return;
    glStencilOp(fail, zfail, pass);
    g_state.stencil_fail = fail;
    g_state.stencil_zfail = zfail;
    g_state.stencil_pass = pass;
    g_state.stencil_op_valid = 1;
}

void gl_state_set_stencil_write_mask(GLuint mask)
{
    if (g_state.stencil_write_mask_valid && g_state.stencil_write_mask == mask) return;
    glStencilMask(mask);
    g_state.stencil_write_mask = mask;
    g_state.stencil_write_mask_valid = 1;
}

void gl_state_set_viewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (g_state.viewport_valid && g_state.viewport_x == x && g_state.viewport_y == y &&
        g_state.viewport_w == w && g_state.viewport_h == h) return;
    glViewport(x, y, w, h);
    g_state.viewport_x = x; g_state.viewport_y = y; g_state.viewport_w = w; g_state.viewport_h = h;
    g_state.viewport_valid = 1;
}

void gl_state_set_depth_range(GLfloat near_val, GLfloat far_val)
{
    if (g_state.depth_range_valid && g_state.depth_range_near == near_val &&
        g_state.depth_range_far == far_val) return;
    glDepthRangef(near_val, far_val);
    g_state.depth_range_near = near_val;
    g_state.depth_range_far = far_val;
    g_state.depth_range_valid = 1;
}

void gl_state_set_scissor_enabled(int enabled)
{
    enabled = enabled != 0;
    if (g_state.scissor_enabled == enabled) return;
    if (enabled) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    g_state.scissor_enabled = enabled;
}

void gl_state_set_scissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (g_state.scissor_valid && g_state.scissor_x == x && g_state.scissor_y == y &&
        g_state.scissor_w == w && g_state.scissor_h == h) return;
    glScissor(x, y, w, h);
    g_state.scissor_x = x; g_state.scissor_y = y; g_state.scissor_w = w; g_state.scissor_h = h;
    g_state.scissor_valid = 1;
}

void gl_state_set_dither_enabled(int enabled)
{
    enabled = enabled != 0;
    if (g_state.dither_enabled == enabled) return;
    if (enabled) glEnable(GL_DITHER); else glDisable(GL_DITHER);
    g_state.dither_enabled = enabled;
}

void gl_state_set_vertex_attrib_array(GLuint index, GLint size, GLenum type,
                                       GLboolean normalized, GLsizei stride,
                                       GLuint buffer, const void *offset)
{
    if (index >= GL_STATE_MAX_VERTEX_ATTRIBS) { /* out-of-range index (includes (GLuint)-1 from glGetAttribLocation): always safe to just pass through uncached, same as gl_state_bind_texture_2d's own unit passthrough */
        glEnableVertexAttribArray(index);
        glVertexAttribPointer(index, size, type, normalized, stride, offset);
        return;
    }
    assert(index < GL_STATE_MAX_VERTEX_ATTRIBS); /* redundant with the out-of-range check above by construction -- belt-and-suspenders against a future edit accidentally weakening that check while leaving this indexing in place */
    g_vertex_attrib_calls++;
    vertex_attrib_slot_t *slot = &g_state.vertex_attrib[index];
    if (slot->valid && slot->enabled && slot->size == size && slot->type == type &&
        slot->normalized == normalized && slot->stride == stride &&
        slot->buffer == buffer && slot->offset == offset) {
        g_vertex_attrib_skips++;
        return;
    }
    glEnableVertexAttribArray(index);
    glVertexAttribPointer(index, size, type, normalized, stride, offset);
    slot->enabled = 1;
    slot->size = size;
    slot->type = type;
    slot->normalized = normalized;
    slot->stride = stride;
    slot->buffer = buffer;
    slot->offset = offset;
    slot->valid = 1;
}

void gl_state_set_vertex_attrib_disabled(GLuint index)
{
    if (index >= GL_STATE_MAX_VERTEX_ATTRIBS) { /* out-of-range index: pass through uncached */
        glDisableVertexAttribArray(index);
        return;
    }
    assert(index < GL_STATE_MAX_VERTEX_ATTRIBS); /* same belt-and-suspenders as gl_state_set_vertex_attrib_array's own */
    g_vertex_attrib_calls++;
    vertex_attrib_slot_t *slot = &g_state.vertex_attrib[index];
    if (slot->valid && !slot->enabled) {
        g_vertex_attrib_skips++;
        return;
    }
    glDisableVertexAttribArray(index);
    slot->enabled = 0;
    slot->valid = 1;
}

long gl_state_get_vertex_attrib_calls_for_test(void) { return g_vertex_attrib_calls; }
long gl_state_get_vertex_attrib_skips_for_test(void) { return g_vertex_attrib_skips; }
void gl_state_reset_vertex_attrib_counters_for_test(void)
{
    g_vertex_attrib_calls = 0;
    g_vertex_attrib_skips = 0;
}

/* Test-only, same "survives gl_state_reset()" reasoning as
   g_vertex_attrib_calls/_skips above. */
static long g_tex_param_calls;
static long g_tex_param_skips;

static int g_npot_repeat_ok = 1;
static uint8_t *g_tex_npot;       /* by texture name: 1 = not a power of two */
static size_t g_tex_npot_n;

void gl_state_set_npot_repeat_ok(int ok) { g_npot_repeat_ok = ok; }

void gl_state_note_tex_size(GLuint texture, int w, int h)
{
    if (texture == 0) return;
    if (texture >= g_tex_npot_n) {
        size_t n = g_tex_npot_n ? g_tex_npot_n : 256;
        while (n <= texture) n *= 2;
        uint8_t *p = realloc(g_tex_npot, n);
        if (p == NULL) return;
        memset(p + g_tex_npot_n, 0, n - g_tex_npot_n);
        g_tex_npot = p, g_tex_npot_n = n;
    }
    g_tex_npot[texture] = (w & (w - 1)) != 0 || (h & (h - 1)) != 0;
}

void gl_state_set_tex_parameters(GLuint texture, GLint wrap_s, GLint wrap_t,
                                  GLint min_filter, GLint mag_filter)
{
    if (texture == 0) return;
    g_tex_param_calls++;
    if (!g_npot_repeat_ok && texture < g_tex_npot_n && g_tex_npot[texture]) {
        wrap_s = GL_CLAMP_TO_EDGE;
        wrap_t = GL_CLAMP_TO_EDGE;
    }


    int slot = -1;
    for (int i = 0; i < GL_STATE_TEX_PARAM_CACHE_SIZE; i++) {
        tex_param_entry_t *e = &g_state.tex_param[i];
        if (e->valid && e->texture == texture) {
            if (e->wrap_s == wrap_s && e->wrap_t == wrap_t &&
                e->min_filter == min_filter && e->mag_filter == mag_filter) {
                g_tex_param_skips++;
                return;
            }
            slot = i; /* same texture, different params -- update this exact slot, not a new one */
            break;
        }
        if (slot < 0 && !e->valid) slot = i; /* remember the first free slot, keep scanning for an exact match */
    }
    if (slot < 0) {
        slot = g_state.tex_param_next_evict;
        g_state.tex_param_next_evict = (g_state.tex_param_next_evict + 1) % GL_STATE_TEX_PARAM_CACHE_SIZE;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);

    tex_param_entry_t *e = &g_state.tex_param[slot];
    e->texture = texture;
    e->wrap_s = wrap_s;
    e->wrap_t = wrap_t;
    e->min_filter = min_filter;
    e->mag_filter = mag_filter;
    e->valid = 1;
}

long gl_state_get_tex_param_calls_for_test(void) { return g_tex_param_calls; }
long gl_state_get_tex_param_skips_for_test(void) { return g_tex_param_skips; }
void gl_state_reset_tex_param_counters_for_test(void)
{
    g_tex_param_calls = 0;
    g_tex_param_skips = 0;
}
