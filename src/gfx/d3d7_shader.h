#ifndef LONGROAD2_D3D7_SHADER_H
#define LONGROAD2_D3D7_SHADER_H

/*
 * Private to the d3d7 module (like d3d7_internal.h) -- the fixed-
 * function-to-GLSL rendering pipeline: vertex/fragment shader source,
 * the per-render-state-combination compiled-program (variant) cache,
 * and the attrib/uniform location bookkeeping every draw call reads.
 * Split out of d3d7.c into its own file specifically so the shader
 * subsystem has a clear boundary to grow in -- see
 * [[project_shader_variant_system]] for why a variant cache replaced
 * the original single uber-shader, and the ongoing per-combiner-op
 * codegen work this boundary exists to host next. d3d7.c is still
 * where every OTHER draw-call concern lives (render state application,
 * uniform value uploads, texture-stage binding) -- this header only
 * exposes what d3d7.c needs to drive that: the shared location struct
 * (so its existing upload call sites keep compiling unchanged, see the
 * macro block below) and the one entry point, ensure_shader_program.
 */

#include "d3d7_internal.h"
#include <GLES2/gl2.h>

/* Real per-draw cap on simultaneously active lights this shader
   supports -- generous for this game's era (typically 1-3 active
   lights per scene); raise if real-game testing shows it's not enough. */
#define GL_MAX_LIGHTS 4
#define GL_MAX_TEX_STAGES MAX_TEX_STAGES /* 8, already used for state tracking */

typedef struct {
    GLint position, direction, diffuse, specular, ambient;
    GLint range, atten, falloff, cone, type;
} light_uniform_locs_t;
typedef struct {
    GLint hasTexture, texCoordIndex;
    GLint hasColorKey, colorKeyLow, colorKeyHigh;
} tex_stage_uniform_locs_t;

/* Every attrib/uniform location the pipeline resolves via
   glGetAttribLocation/glGetUniformLocation, in one struct -- each
   compiled shader_variant_t (d3d7_shader.c) holds its own copy, since
   locations aren't guaranteed to match across separate linked program
   objects even for identically-named attributes/uniforms. g_locs is
   always "whichever variant is currently active"'s own copy. Exposed
   here (not kept private to d3d7_shader.c) purely so d3d7.c's existing
   upload_lighting_uniforms/upload_alpha_test_uniforms/upload_fog_
   uniforms/upload_clip_plane_uniforms/bind_texture_stages/
   d3d_draw_prepare -- which reference these locations by their original
   g_attr_X/g_unif_X names throughout -- keep compiling completely
   unchanged; the #define block below redirects each name to
   g_locs.field so no call site needed to be touched when this struct
   was introduced. */
typedef struct {
    GLint attr_pos, attr_normal, attr_diffuse, attr_texcoord0, attr_texcoord1;
    GLint unif_world, unif_view, unif_proj;
    GLint unif_vp_px;
    GLint unif_point_size;
    GLint unif_local_viewer, unif_normalize_normals;
    GLint unif_global_ambient, unif_light_count;
    GLint unif_mat_diffuse, unif_mat_ambient;
    GLint unif_mat_specular, unif_mat_emissive, unif_mat_power;
    GLint unif_alpha_ref;
    GLint unif_fog_color;
    GLint unif_fog_start, unif_fog_end, unif_fog_density;
    GLint unif_clip_planes;
    light_uniform_locs_t unif_lights[GL_MAX_LIGHTS];
    GLint unif_texture_factor;
    GLint unif_tex[MAX_TEX_STAGES]; /* u_tex0..u_tex7 -- see fragment shader header comment */
    tex_stage_uniform_locs_t unif_stages[MAX_TEX_STAGES];
} shader_locs_t;
extern shader_locs_t g_locs;

/* Whichever shader program is currently active -- either the found/
   built shader_variant_t's own prog (normal path, see
   ensure_shader_variant in d3d7_shader.c) or the single ad hoc
   diagnostic program (LONGROAD2_CHEAP_SHADER path). Read directly by
   d3d7.c's d3d_draw_prepare (gl_state_use_program(g_prog)). */
extern GLuint g_prog;

#define g_attr_pos g_locs.attr_pos
#define g_attr_normal g_locs.attr_normal
#define g_attr_diffuse g_locs.attr_diffuse
#define g_attr_texcoord0 g_locs.attr_texcoord0
#define g_attr_texcoord1 g_locs.attr_texcoord1
#define g_unif_world g_locs.unif_world
#define g_unif_view g_locs.unif_view
#define g_unif_proj g_locs.unif_proj
#define g_unif_vp_px g_locs.unif_vp_px
#define g_unif_point_size g_locs.unif_point_size
#define g_unif_local_viewer g_locs.unif_local_viewer
#define g_unif_normalize_normals g_locs.unif_normalize_normals
#define g_unif_global_ambient g_locs.unif_global_ambient
#define g_unif_light_count g_locs.unif_light_count
#define g_unif_mat_diffuse g_locs.unif_mat_diffuse
#define g_unif_mat_ambient g_locs.unif_mat_ambient
#define g_unif_mat_specular g_locs.unif_mat_specular
#define g_unif_mat_emissive g_locs.unif_mat_emissive
#define g_unif_mat_power g_locs.unif_mat_power
#define g_unif_alpha_ref g_locs.unif_alpha_ref
#define g_unif_fog_color g_locs.unif_fog_color
#define g_unif_fog_start g_locs.unif_fog_start
#define g_unif_fog_end g_locs.unif_fog_end
#define g_unif_fog_density g_locs.unif_fog_density
#define g_unif_clip_planes g_locs.unif_clip_planes
#define g_unif_lights g_locs.unif_lights
#define g_unif_texture_factor g_locs.unif_texture_factor
#define g_unif_tex g_locs.unif_tex
#define g_unif_stages g_locs.unif_stages

/* Called once per draw (d3d_draw_prepare, d3d7.c) -- finds or lazily
   builds the shader variant dev's CURRENT render state (plus is_rhw,
   per-draw-call data derived from the vertex FVF, not part of dev's own
   persistent state) needs, or, under LONGROAD2_CHEAP_SHADER, the single
   diagnostic program -- and leaves g_prog/g_locs pointing at it. Returns
   0 if the required program failed to compile/link. */
int ensure_shader_program(d3d_device7_t *dev, int is_rhw);

/* Invalidates ensure_shader_variant's own single-entry "last resolved
   variant" cache (d3d7_shader.c) -- must be called anywhere a
   d3d_device7_t's address could be reused by a NEW device (a freed
   device's memory handed back out by a later calloc), same
   device-identity-spoofing risk d3d7_draw_uniform_cache_reset's own
   comment documents and already guards against at both
   d3d_device7_Release and every test-suite device-group boundary --
   folded into that same function/call sites rather than requiring a
   second, easy-to-forget reset call everywhere. */
void d3d7_shader_variant_cache_reset(void);

#endif /* LONGROAD2_D3D7_SHADER_H */
