#include "d3d7_shader.h"

#include "gl_state_cache.h"
#include "gl_thread.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Fixed-function-to-GLSL rendering pipeline: vertex shader source, the
 * fragment-shader building blocks, and the per-render-state-combination
 * compiled-program (variant) cache. Split out of d3d7.c specifically to
 * give the shader subsystem its own boundary to grow in -- see
 * [[project_shader_variant_system]] for the full history of why a
 * variant cache replaced the original single uber-shader (a live
 * SWEP1RCR/RK3326 capture showed d3d7_gpu_finish dominated by the real
 * fragment shader's own branch-chain cost, not CPU dispatch, texture
 * fetch, or program switching), and for the per-combiner-op codegen
 * work this boundary exists to host next (resolving each stage's real
 * D3DTOP/D3DTA combination into straight-line GLSL on the CPU, instead
 * of uploading it as a uniform the shader branches on at runtime).
 *
 * One fixed vertex shader (k_vs_src) drives every draw regardless of
 * variant -- only the fragment side's GLSL structure changes per
 * shader_key_t (see compute_shader_key below). d3d7.c is still where
 * every OTHER draw-call concern lives (render-state application,
 * uniform VALUE uploads, texture-stage binding) -- this file only owns
 * turning device state into a compiled program and activating it.
 */

/* Two-level indirection needed so STRINGIFY(GL_MAX_LIGHTS) stringifies
   the expanded value "4", not the literal text "GL_MAX_LIGHTS" --
   standard C preprocessor idiom, used only to keep the GLSL source
   string's array size in sync with the C-side cap in d3d7_shader.h. */
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

shader_locs_t g_locs;
/* See this global's own doc comment in d3d7_shader.h. */
GLuint g_prog = 0;

/* Vertex shader, split into composable pieces (mirrors how the
   fragment shader is already split into k_fs_header/k_fs_main_start/
   footers) -- see build_vertex_shader_source below. u_is_rhw is gone:
   which of the RHW/XYZ bodies compiles in is now a C-side decision
   (shader_key_t.is_rhw), not a runtime GLSL branch.

   The varying INTERFACE itself is now also compile-time-specialized,
   not just the code bodies -- a real gap found auditing a live shader
   dump ([[project_shader_variant_grooming_followon]]): every declared
   `varying` costs real per-vertex write + per-fragment interpolation
   bandwidth on a tile-based GPU regardless of whether the fragment
   shader actually reads it, and v_world_pos/v_world_normal in
   particular were declared varyings in EVERY variant that ever existed
   while never being read by any fragment-side codegen path at all
   (lighting fully resolves in the vertex shader into v_diffuse --
   see k_vs_main_xyz_lighting -- so nothing downstream ever needed
   them as an inter-stage value). v_texcoord0/1, v_fog_dist, and
   v_clipdist_a/b ARE genuinely read sometimes, gated on the same
   shader_key_t axes (stage_count/fog_enabled/clip_plane_mask) that
   already gate the matching fragment-side code -- see
   build_vertex_shader_source/build_fragment_shader_source below, which
   must keep both sides' declarations in exact sync (GLSL ES 100
   requires linked stages to agree on varying declarations). */
static const char *k_vs_header_base =
    "#version 100\n"
    "attribute vec4 a_pos;\n"      /* xyz always; w = 1 for XYZ, or RHW for XYZRHW */
    "attribute vec3 a_normal;\n"
    "attribute vec4 a_diffuse;\n"  /* raw D3DCOLOR bytes: memory order B,G,R,A */
    "attribute vec2 a_texcoord0;\n"
    "attribute vec2 a_texcoord1;\n" /* second UV set -- only present for fvf tex_count>=2 */
    "uniform mat4 u_world;\n"
    "uniform mat4 u_view;\n"
    "uniform mat4 u_proj;\n"
    "uniform vec4 u_vp_px;\n"      /* x, y, width, height -- D3D viewport, pixels, Y-down */
    "uniform float u_point_size;\n" /* only visible when the draw's topology is GL_POINTS */
    "\n"
    "varying vec4 v_diffuse;\n";

/* Only declared/interpolated when shader_key_t.clip_plane_mask != 0
   (implies clipping_enabled, and at least one real plane active --
   tighter than gating on clipping_enabled alone, which could be set
   with zero planes active). Packed as 2 vec3 rather than a 6-element
   array to avoid varying arrays entirely (same GLSL ES 100 portability
   caution already applied to texcoords/sampler stages elsewhere in
   this file) -- .x/.y/.z of each hold clip planes 0-2 and 3-5
   respectively. */
static const char *k_vs_header_clip =
    "uniform vec4 u_clip_planes[6];\n" /* D3DRENDERSTATE_CLIPPING/CLIPPLANEENABLE -- world-space plane equations, ax+by+cz+d */
    "varying vec3 v_clipdist_a;\n"
    "varying vec3 v_clipdist_b;\n";

/* Only declared/interpolated when shader_key_t.stage_count > 0 -- no
   active texture stage means no fragment-side code ever reads either
   texcoord set. */
static const char *k_vs_header_texcoord =
    "varying vec2 v_texcoord0;\n"
    "varying vec2 v_texcoord1;\n";

/* Only declared/interpolated when shader_key_t.fog_enabled -- see
   k_fs_footer's own fog blend, the only fragment-side reader. */
static const char *k_vs_header_fog =
    "varying float v_fog_dist;\n"; /* view-space distance from eye, for fragment-side fog (see k_fs_footer) */

/* ----- fixed-function lighting (D3DRENDERSTATE_LIGHTING) -- emitted
   only when shader_key_t.lighting_enabled is set (see
   build_vertex_shader_source). Computed per-vertex (D3DSHADE_GOURAUD,
   this pipeline's only supported shade mode) in VIEW space, matching
   D3D7's own vertex T&L stage -- light position/direction are pre-
   transformed into view space on the CPU (d3d_draw_common) before
   upload, exactly like the vertex position itself is transformed by
   u_view here. u_lighting_enabled/u_specular_enabled are gone -- both
   are now compile-time key axes (whether this whole piece, and its own
   specular sub-block, exist at all) rather than runtime-uniform-gated
   dead code every unlit/non-specular draw used to carry regardless. */
static const char *k_vs_lighting_decls =
    "#define D3DLIGHT_POINT 1.0\n"
    "#define D3DLIGHT_SPOT 2.0\n"
    "#define D3DLIGHT_DIRECTIONAL 3.0\n"
    "struct D3DLight {\n"
    "    vec3 position;\n"
    "    vec3 direction;\n"
    "    vec4 diffuse;\n"
    "    vec4 specular;\n"
    "    vec4 ambient;\n"
    "    float range;\n"
    "    vec3 atten;\n"          /* atten0, atten1, atten2 */
    "    float falloff;\n"
    "    vec2 cone;\n"           /* cos(theta/2), cos(phi/2) -- precomputed on the CPU */
    "    float type;\n"
    "};\n"
    "uniform D3DLight u_lights[" STRINGIFY(GL_MAX_LIGHTS) "];\n"
    "uniform int u_light_count;\n"
    "uniform bool u_local_viewer;\n"
    "uniform bool u_normalize_normals;\n"
    "uniform vec4 u_global_ambient;\n"
    "uniform vec4 u_mat_diffuse, u_mat_ambient, u_mat_specular, u_mat_emissive;\n"
    "uniform float u_mat_power;\n"
    "\n";

/* computeLighting's own start through the always-present part of its
   per-light loop body (diffuse/ambient accumulation, run regardless of
   specular_enabled). */
static const char *k_vs_lighting_fn_start =
    "vec4 computeLighting(vec3 view_pos, vec3 view_normal, out vec4 out_specular) {\n"
    "    vec3 n = u_normalize_normals ? normalize(view_normal) : view_normal;\n"
    "    vec4 ambientSum = vec4(0.0);\n"
    "    vec4 diffuseSum = vec4(0.0);\n"
    "    vec4 specularSum = vec4(0.0);\n"
    "    for (int i = 0; i < " STRINGIFY(GL_MAX_LIGHTS) "; i++) {\n"
    "        if (i >= u_light_count) break;\n"
    "        D3DLight lt = u_lights[i];\n"
    "        vec3 hitDir;\n"
    "        float atten = 1.0;\n"
    "        if (lt.type == D3DLIGHT_DIRECTIONAL) {\n"
    "            hitDir = -lt.direction;\n"
    "        } else {\n"
    "            vec3 delta = lt.position - view_pos;\n"
    "            float dist = length(delta);\n"
    "            hitDir = dist > 0.0 ? delta / dist : vec3(0.0, 0.0, 1.0);\n"
    "            float a = lt.atten.x + lt.atten.y * dist + lt.atten.z * dist * dist;\n"
    "            atten = a > 0.0 ? 1.0 / a : 1.0;\n"
    "            if (dist > lt.range) atten = 0.0;\n"
    "            if (lt.type == D3DLIGHT_SPOT) {\n"
    "                float rho = dot(-hitDir, lt.direction);\n"
    "                float spot = clamp((rho - lt.cone.y) / max(lt.cone.x - lt.cone.y, 0.0001), 0.0, 1.0);\n"
    "                atten *= pow(spot, max(lt.falloff, 0.0001));\n"
    "            }\n"
    "        }\n"
    "        float diffuseFactor = max(dot(n, hitDir), 0.0);\n"
    "        ambientSum += lt.ambient * atten;\n"
    "        diffuseSum += lt.diffuse * diffuseFactor * atten;\n";

/* Emitted (still inside the per-light loop, right after
   k_vs_lighting_fn_start) only when shader_key_t.specular_enabled is
   set -- was the "if (u_specular_enabled) { ... }" block's body, now
   selected by the C-side key instead of a runtime GLSL condition (same
   pattern as is_rhw's own branch retirement). */
static const char *k_vs_lighting_specular =
    "        vec3 eyeDir = u_local_viewer ? normalize(-view_pos) : vec3(0.0, 0.0, -1.0);\n"
    "        vec3 mid = normalize(hitDir + eyeDir);\n"
    "        float specFactor = max(dot(n, mid), 0.0);\n"
    "        if (diffuseFactor > 0.0 && specFactor > 0.0) {\n"
    "            specularSum += lt.specular * pow(specFactor, max(u_mat_power, 1.0)) * atten;\n"
    "        }\n";

/* Closes the per-light loop and computeLighting itself -- always
   present when lighting_enabled, regardless of specular_enabled
   (specularSum simply stays vec4(0.0) throughout when the specular
   piece above was omitted, giving out_specular=vec4(0.0) here exactly
   matching the old runtime-false behavior, at the cost of one harmless
   multiply-by-zero -- not worth its own key axis, see this file's
   guiding rule on branch/loop-nested blocks vs. cheap fixed expressions). */
static const char *k_vs_lighting_fn_end =
    "    }\n"
    "    vec4 result = u_mat_emissive + u_mat_ambient * u_global_ambient +\n"
    "                  u_mat_ambient * ambientSum + u_mat_diffuse * diffuseSum;\n"
    "    result.a = u_mat_diffuse.a;\n"
    "    out_specular = clamp(u_mat_specular * specularSum, 0.0, 1.0);\n"
    "    return clamp(result, 0.0, 1.0);\n"
    "}\n"
    "\n";

/* Always emitted, before either main-body variant below. */
static const char *k_vs_main_open =
    "void main() {\n"
    "    v_diffuse = a_diffuse.zyxw;\n"; /* D3DCOLOR memory order -> RGBA; overwritten below if lit */

/* Only emitted when shader_key_t.stage_count > 0 -- see
   k_vs_header_texcoord's own comment. D3D's V=0 always means the bound
   texture's own first row. This used to need a compensating flip here
   because gl_target_upload_rgba row-flipped pixel data on its way into
   GL -- now that upload is a direct, unflipped glTexSubImage2D (see
   render.c, paired with the vertex shader's own clip-space Y flip and
   the unflipped viewport), the game's own V maps directly to GL's T
   with no adjustment. */
static const char *k_vs_texcoord_write =
    "    v_texcoord0 = a_texcoord0;\n"
    "    v_texcoord1 = a_texcoord1;\n";

/* Emitted instead of k_vs_main_xyz when shader_key_t.is_rhw is set --
   was the "if (u_is_rhw) { ... }" branch's body, now selected by the
   C-side key instead of a runtime GLSL condition. */
static const char *k_vs_main_rhw =
    "    float ndc_x = (a_pos.x - u_vp_px.x) / u_vp_px.z * 2.0 - 1.0;\n"
    "    float ndc_y = (a_pos.y - u_vp_px.y) / u_vp_px.w * 2.0 - 1.0;\n" /* TESTED explicit-flip variant live: made no difference to the black-screen symptom (full-frame glReadPixels still 0% nonblack), reverted. Real explanation for why this formula (structurally identical to ndc_x, no flip) is correct still not confirmed either way -- see this project's own memory file for the open question. Genuinely unrelated to the still-open black-screen investigation as far as testing has shown. */
    "    float ndc_z = a_pos.z * 2.0 - 1.0;\n" /* D3D depth [0,1] -> GL NDC [-1,1] */
    /* Real bug fix (SWEP1RCR.EXE, see [[project_swep1rcr_port_scoping]]'s
       "textures warping under the camera" investigation): a_pos.w carries
       the vertex's real RHW value (glVertexAttribPointer already binds 4
       components for is_rhw -- confirmed correct), but gl_Position.w used
       to be hardcoded to 1.0, discarding it. Real D3D XYZRHW vertices are
       ALREADY screen-space for POSITION, but rhw (=1/originalClipW) still
       drives PERSPECTIVE-CORRECT interpolation of every other varying
       (texcoord, diffuse, ...) during rasterization -- exactly what GL's
       own automatic divide-by-gl_Position.w does, but only if w is
       something other than a constant 1.0 for every vertex. With w
       hardcoded, every RHW draw silently fell back to affine (linear,
       non-perspective-corrected) interpolation -- invisible on small,
       roughly-fronto-parallel content (this shim's own HUD icons, the
       first RHW content ever exercised here) but exactly the classic
       "warping/swimming" texture artifact on any large surface viewed at
       a steep angle or close to the camera (confirmed live: SWEP1RCR's
       entire 3D world, not just its HUD, renders through this same RHW
       path -- see this same investigation's earlier update). Reconstruct
       the real clip-space w (=1/rhw) and pre-multiply x/y/z by it so
       GL's own divide-by-w recovers the IDENTICAL ndc_x/y/z as before
       (position unchanged) while every other varying now gets the same
       real perspective weighting a hardware XYZRHW pipeline would apply. */
    "    float rhw = a_pos.w;\n"
    "    float clip_w = rhw > 0.0 ? (1.0 / rhw) : 1.0;\n"
    "    gl_Position = vec4(ndc_x * clip_w, ndc_y * clip_w, ndc_z * clip_w, clip_w);\n";
    /* v_world_pos/v_world_normal writes used to live here unconditionally
       (matching the interface every OTHER variant declared) -- deleted
       outright, not just gated: nothing anywhere ever reads either one
       as a fragment-side varying (see k_vs_header_base's own comment),
       so there was never a real reason for either to exist as
       inter-stage data in the first place, RHW or not. */

/* Only emitted when shader_key_t.fog_enabled (k_vs_header_fog declared
   v_fog_dist at all). Pre-transformed vertices bypass the whole T&L
   pipeline in real D3D7, lighting included -- v_diffuse above (the
   vertex's own color) is already the final answer for this branch. Fog
   isn't meaningful for pre-transformed content either way -- 0 distance
   reads as "no fog" for any sane fog range. */
static const char *k_vs_main_rhw_fog =
    "    v_fog_dist = 0.0;\n";

/* Only emitted when shader_key_t.clip_plane_mask != 0 (k_vs_header_clip
   declared v_clipdist_a/b at all). Real D3D7 user clip planes don't
   apply to pre-transformed (screen-space) vertices either -- "always
   inside" sentinel. */
static const char *k_vs_main_rhw_clip =
    "    v_clipdist_a = vec3(1.0);\n"
    "    v_clipdist_b = vec3(1.0);\n";

/* Emitted instead of k_vs_main_rhw when shader_key_t.is_rhw is clear --
   was the "if (u_is_rhw) { ... } else { ... }" branch's else-body.
   Lighting's own call site is now split out into k_vs_main_xyz_lighting
   below (emitted only when shader_key_t.lighting_enabled is set),
   rather than gated by a runtime "if (u_lighting_enabled)" here. */
static const char *k_vs_main_xyz =
    "    vec4 world_pos = u_world * vec4(a_pos.xyz, 1.0);\n"
    "    gl_Position = u_proj * u_view * world_pos;\n"
    /* D3D's projection matrix assumes clip-space z in [0,w] (maps to
       D3D's own [0,1] NDC depth); GL expects [-w,w] (NDC [-1,1]). The
       game's own projection matrix is unmodified, so convert here:
       ndc'.z = 2*ndc.z - 1, done pre-divide as z' = 2z - w. */
    "    gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;\n"
    "    gl_Position.y = -gl_Position.y;\n";
    /* world_pos.xyz itself (formerly also copied out to a v_world_pos
       varying nothing downstream ever read -- see k_vs_header_base's own
       comment) already covers what the clip-plane block below needs;
       view_pos (needed by fog and/or lighting) is a separate, further-
       gated piece since plenty of variants need neither. */

/* Only emitted when fog_enabled or lighting_enabled -- both read
   view_pos (fog directly below, lighting via k_vs_main_xyz_lighting's
   own computeLighting call), but plenty of variants (untextured/
   unlit/unfogged, or lit-but-unfogged, etc.) need neither. */
static const char *k_vs_main_xyz_viewpos =
    "    vec3 view_pos = (u_view * world_pos).xyz;\n";

/* Only emitted when shader_key_t.fog_enabled. D3D7's default (non-range)
   fog is depth-based, not true distance -- eye-space Z magnitude, not
   length(view_pos) -- matching the common fixed-function default
   (D3DRENDERSTATE_RANGEFOGENABLE, a true-distance mode, isn't
   implemented -- no evidence this game uses it). Computed once per
   vertex and interpolated; the actual EXP/EXP2/LINEAR curve is
   evaluated per-fragment in k_fs_footer (real per-pixel accuracy for
   the curve itself, only the input distance is a per-vertex
   approximation -- fine for reasonably tessellated geometry, and
   consistent with this pipeline's other per-vertex-computed,
   per-fragment-interpolated quantities). */
static const char *k_vs_main_xyz_fog =
    "    v_fog_dist = abs(view_pos.z);\n";

/* Only emitted when shader_key_t.clip_plane_mask != 0. World space,
   matching this file's own general convention that everything not
   explicitly stated otherwise (lighting distance/attenuation, etc.) is
   computed against the un-viewed world position where D3D7's own docs
   are ambiguous/driver-dependent on exactly which space user clip
   planes live in -- no evidence this game uses clip planes at all, so
   this is a reasonable default rather than a confirmed-correct choice. */
static const char *k_vs_main_xyz_clip =
    "    v_clipdist_a = vec3(\n"
    "        dot(u_clip_planes[0].xyz, world_pos.xyz) + u_clip_planes[0].w,\n"
    "        dot(u_clip_planes[1].xyz, world_pos.xyz) + u_clip_planes[1].w,\n"
    "        dot(u_clip_planes[2].xyz, world_pos.xyz) + u_clip_planes[2].w);\n"
    "    v_clipdist_b = vec3(\n"
    "        dot(u_clip_planes[3].xyz, world_pos.xyz) + u_clip_planes[3].w,\n"
    "        dot(u_clip_planes[4].xyz, world_pos.xyz) + u_clip_planes[4].w,\n"
    "        dot(u_clip_planes[5].xyz, world_pos.xyz) + u_clip_planes[5].w);\n";

/* Emitted right after k_vs_main_xyz(_viewpos), only when shader_key_t.
   lighting_enabled is set -- was the "if (u_lighting_enabled) { ... }"
   block's body, now selected by the C-side key instead of a runtime
   GLSL condition (view_pos is still in scope here, declared earlier in
   k_vs_main_xyz_viewpos within the same unbraced main() body).
   world_normal used to be computed unconditionally into a v_world_normal
   VARYING regardless of whether lighting was even on (see
   k_vs_header_base's own comment) -- a real, separate waste beyond just
   the varying itself: since it was written to an inter-stage varying,
   the vertex-shader compiler couldn't prove (compiling this stage alone,
   before the fragment shader is even linked) that the mat3(u_world)*
   a_normal multiply was dead, so it almost certainly ran every vertex
   regardless of lighting_enabled. Now a plain local, computed only
   here -- gone entirely from every unlit variant. */
static const char *k_vs_main_xyz_lighting =
    "    vec3 world_normal = mat3(u_world) * a_normal;\n"
    "    vec3 view_normal = mat3(u_view) * world_normal;\n"
    "    vec4 specular;\n"
    "    vec4 lit = computeLighting(view_pos, view_normal, specular);\n"
    "    v_diffuse = vec4(lit.rgb + specular.rgb, lit.a);\n";

/* Always emitted, after whichever main-body variant was chosen. */
static const char *k_vs_main_close =
    "    gl_PointSize = u_point_size;\n"
    "}\n";

/* ----- texture-stage-state combiner -----
   Every real D3DTOP_/D3DTA_ combiner op (D3D7's complete D3DTEXTUREOP
   set, 1-24, no MULTIPLYADD/LERP/temp register -- D3D8+ additions) is
   now resolved at shader-BUILD time by emit_arg_expr/emit_op_formula
   (see build_fragment_shader_source/emit_stage_glsl below) into
   straight-line GLSL baked directly into each stage's own block -- the
   readArg()/calcOp() runtime-dispatch functions and the GLSL-side
   D3DTOP_/D3DTA_ #defines they used are gone; no generated shader
   references them anymore (see [[project_shader_variant_system]]).
   D3DTA_SPECULAR is approximated as v_diffuse (this pipeline folds the
   lit specular contribution additively into v_diffuse in the vertex
   shader -- see computeLighting's caller -- rather than carrying a
   second interpolated color through to here; referencing specular
   explicitly as its own combiner argument is a rare usage this
   simplification doesn't handle exactly, unlike the default "add
   specular at the end" behavior which it does). D3DTOP_BUMPENVMAP/
   BUMPENVMAPLUMINANCE fall through to the same SELECTARG1 default as an
   unrecognized op in emit_op_formula: real bump-mapped texture-
   coordinate offsetting for the *next* stage isn't implemented (a real,
   rare feature this scope cut leaves out), but the color result for
   the bump stage itself matches real D3D's own fallback for it. */
/* v_world_pos/v_world_normal are gone entirely (not just gated) -- see
   k_vs_header_base's own comment: no fragment-side codegen path (here or
   anywhere else in this file) has ever read either one, RHW or XYZ, lit
   or unlit. v_texcoord0/1, v_fog_dist, v_clipdist_a/b remain, but only
   declared when the matching shader_key_t axis actually needs them --
   must track build_vertex_shader_source's own gating exactly (GLSL ES
   100 requires linked stages to agree on varying declarations). */
static const char *k_fs_header_base =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec4 v_diffuse;\n";

/* Only declared when shader_key_t.stage_count > 0 -- see
   k_vs_header_texcoord's own comment. */
static const char *k_fs_header_texcoord =
    "varying vec2 v_texcoord0;\n"
    "varying vec2 v_texcoord1;\n";

/* Only declared when shader_key_t.fog_enabled -- see k_fs_footer's own
   fog blend, the only reader. */
static const char *k_fs_header_fog =
    "varying float v_fog_dist;\n";

/* Only declared when shader_key_t.clip_plane_mask != 0 -- see
   k_clip_plane_check, the only reader. */
static const char *k_fs_header_clip =
    "varying vec3 v_clipdist_a;\n"
    "varying vec3 v_clipdist_b;\n";

/* Uniforms stay unconditionally declared regardless of key -- unlike
   varyings, an unused uniform costs no per-fragment interpolation
   bandwidth, so there's no equivalent win to chase here (u_stages[8]'s
   own possible shrink-to-stage_count is a separate, lower-confidence
   idea flagged in [[project_shader_variant_grooming_followon]], not
   done here -- it would need auditing the C-side upload indexing in
   bind_texture_stages first). */
static const char *k_fs_header_uniforms =
    "\n"
    "uniform float u_alpha_ref;\n"  /* normalized to [0,1] on upload -- real D3DRENDERSTATE_ALPHAREF is a 0-255 byte */
    "\n"
    "uniform vec4 u_fog_color;\n"
    "uniform float u_fog_start;\n"
    "uniform float u_fog_end;\n"
    "uniform float u_fog_density;\n"
    "\n"
    "uniform vec4 u_texture_factor;\n"
    "\n"
    "struct TexStage {\n"
    "    float hasTexture;\n"
    "    float texCoordIndex;\n" /* D3DTSS_TEXCOORDINDEX, low bits only -- see bind_texture_stages' own comment on why the TCI_* generation-mode bits are masked off */
    "    float hasColorKey;\n" /* D3DRENDERSTATE_COLORKEYENABLE -- this stage's bound texture has a real SetColorKey set */
    "    vec3 colorKeyLow;\n"
    "    vec3 colorKeyHigh;\n"
    "};\n"
    "uniform TexStage u_stages[" STRINGIFY(GL_MAX_TEX_STAGES) "];\n"
    "\n";

/* One instance per stage, emitted right after k_fs_header -- still
   before "void main()" opens in k_fs_main_start below, since uniforms
   must be declared at global scope -- see k_fs_header's comment for why
   these are separate scalar samplers, not an array. */
static const char *k_fs_sampler_template =
    "uniform sampler2D u_tex%d;\n";

static const char *k_fs_main_start =
    "void main() {\n"
    "    vec4 current = v_diffuse;\n";

/* Per-stage combiner code is no longer instantiated from a fixed GLSL
   template with runtime dispatch (colorOp/argN selectors used to be
   read from uniforms and dispatched via readArg/calcOp's ~5-way/~15-way
   GLSL if-chains, evaluated on every fragment) -- see emit_stage_glsl
   and its neighboring codegen functions below, right before
   build_fragment_shader_source. The CPU already knows every stage's
   real colorOp/colorArg1/colorArg2/alphaOp/alphaArg1/alphaArg2 at
   variant-build time (it's right there in shader_key_t), so the
   dispatch now happens ONCE per distinct combination, in C, producing
   straight-line GLSL with the resolved formula baked in -- not
   re-decided by every GPU thread on every fragment. See
   [[project_shader_variant_system]]: this follows directly from a real
   RK3326 capture showing CHEAP_SHADER=2 (real texture fetch, zero
   combiner branches) running at a solid 60fps while the full branchy
   uber-shader ran under 30fps -- the branch chains, not the texture
   fetch, are what a weak mobile GPU pays for. Texture sampling and the
   colorkey discard stay runtime-uniform-driven exactly as before
   (hasTexture/hasColorKey aren't part of the key) -- neither was ever
   shown to be the expensive part, and colorkey specifically touches a
   history of subtle bugs (native565 epsilon, colorkey-vs-alpha
   interaction) not worth adding compile-time-specialization risk to
   for an unproven benefit. */

/* Per-plane clip discard checks -- unconditional now (no more
   `u_clip_enabledN &&` prefix; whether a plane's check is even emitted
   at all is now the compile-time signal, driven by
   shader_key_t.clip_plane_mask -- see build_fragment_shader_source).
   D3DRENDERSTATE_CLIPPING/CLIPPLANEENABLE: discard-based, same
   reasoning as not using gl_ClipDistance/GL_CLIP_DISTANCE0+ -- that
   needs GL_EXT_clip_cull_distance, a real extension-availability
   concern for this project's ARM deployment target (not core GLES
   3.2), whereas a fragment discard against an interpolated per-vertex
   plane distance works everywhere. */
static const char *k_clip_plane_check[6] = {
    "    if (v_clipdist_a.x < 0.0) discard;\n",
    "    if (v_clipdist_a.y < 0.0) discard;\n",
    "    if (v_clipdist_a.z < 0.0) discard;\n",
    "    if (v_clipdist_b.x < 0.0) discard;\n",
    "    if (v_clipdist_b.y < 0.0) discard;\n",
    "    if (v_clipdist_b.z < 0.0) discard;\n",
};

/* Resolves ONE D3DCMPFUNC into a self-contained GLSL boolean expression
   -- mirrors the old runtime u_alpha_func if-chain's exact per-branch
   comparisons (not a new derivation, just relocated from a GLSL
   if-chain, resolved by a C switch at shader-build time instead of a
   uniform read per fragment -- same technique as emit_op_formula). */
static const char *alpha_func_expr(uint32_t func)
{
    switch (func) {
        case D3DCMP_NEVER:        return "false";
        case D3DCMP_LESS:         return "current.a < u_alpha_ref";
        case D3DCMP_EQUAL:        return "current.a == u_alpha_ref";
        case D3DCMP_LESSEQUAL:    return "current.a <= u_alpha_ref";
        case D3DCMP_GREATER:      return "current.a > u_alpha_ref";
        case D3DCMP_NOTEQUAL:     return "current.a != u_alpha_ref";
        case D3DCMP_GREATEREQUAL: return "current.a >= u_alpha_ref";
        default:                   return "true"; /* ALWAYS / unrecognized */
    }
}

/* Resolves ONE D3DFOGMODE into a self-contained GLSL float expression
   for the (pre-clamp) fog blend factor -- same reasoning/technique as
   alpha_func_expr above, mirrors the old runtime u_fog_mode if-chain's
   exact per-branch formulas. */
static const char *fog_factor_expr(uint32_t mode)
{
    switch (mode) {
        case D3DFOG_EXP:    return "exp(-u_fog_density * v_fog_dist)";
        case D3DFOG_EXP2:   return "exp(-(u_fog_density * v_fog_dist) * (u_fog_density * v_fog_dist))";
        case D3DFOG_LINEAR: return "(u_fog_end - v_fog_dist) / max(u_fog_end - u_fog_start, 0.0001)";
        default:              return "1.0"; /* NONE / unrecognized -- leaves color untouched, matches the old runtime else-branch exactly */
    }
}

static const char *k_fs_footer_end =
    "    gl_FragColor = current;\n"
    "}\n";

/* LONGROAD2_CHEAP_SHADER=1|2|3|4: diagnostic-only, four levels,
   predating (and the direct motivation for) both the shader-variant
   system and the per-stage codegen (emit_stage_glsl et al, above) -- at
   the time =1/=2 were added, the real fragment shader was ONE branchy
   uber-shader: up to GL_MAX_TEX_STAGES iterations of texture sampling +
   two ~15-way calcOp branches + readArg's own branches each, plus
   clip-plane discards, alpha-test discard, and a fog blend, ALL
   evaluated per fragment regardless of how simple a given draw actually
   needed it to be. =1 (k_fs_cheap, below) skips every bit of that --
   paired with k_vs_cheap (a minimal vertex shader declaring the same
   varyings, needed since there's no longer a single shared k_vs_src to
   link against post-vertex-shader-specialization), the body is just
   `gl_FragColor = v_diffuse`. Confirmed live
   (SWEP1RCR/RK3326): d3d7_gpu_finish collapsed from ~65-67ms to ~16ms,
   proving the cost IS fragment-shader-bound, not CPU-side driver/call
   overhead. =2 (k_fs_texonly, below) narrows down WHERE inside the
   shader that cost lives: real stage-0 texture2D fetch + one multiply,
   but none of calcOp/readArg's branch chains, clipping, alpha test, or
   fog. If =2 comes back close to =1's ~16ms, the branchy combiner logic
   is the real cost (Mali-G31-class GPUs are known to handle dynamic
   branching poorly); if =2 is still close to the ~65-67ms baseline,
   raw texture-fetch/fill-rate is the cost instead, and the fix needs to
   target pixel count or texture bandwidth, not shader branch logic.

   =3/=4 answer a DIFFERENT, later question (see
   [[project_shader_dead_varying_and_alu_elimination]]): after the
   combiner-op elimination work, emit_stage_glsl's real per-stage code
   still carries two runtime-uniform-gated `if`s that were deliberately
   left OUT of shader_key_t -- `if (hasTexture>0.5) texVal=texture2D(...)`
   and the hasColorKey discard block -- because unlike everything else
   in the key, both depend on the bound TEXTURE OBJECT (SetTexture/
   SetColorKey), not device render state, and neither currently bumps
   state_generation. Whether either branch is worth the real engineering
   cost of changing that was never actually measured, only inferred from
   =1/=2 (which have NEITHER branch at all, so they can't isolate
   "branch cost" from "fetch cost"). =3 (k_fs_texonly_gated) reads the
   SAME real per-draw hasTexture uniform through the SAME branch shape
   as production, sampling nothing else -- if timing lands close to =2,
   the branch itself (a uniform, not per-fragment-varying, condition) is
   cheap on this hardware and folding hasTexture into the key isn't
   worth pursuing. =4 (k_fs_texonly_colorkey) adds the real colorkey
   discard block on top of =3, reading the SAME real hasColorKey/
   colorKeyLow/colorKeyHigh uniforms production does -- this ALSO
   exercises a genuine, otherwise-untested real-driver question: GLSL
   spec says `&&` short-circuits, but whether THIS Mali driver actually
   skips the all(greaterThanEqual(...))/all(lessThanEqual(...)) work
   when hasColorKey is false, or evaluates it anyway, is exactly the
   kind of thing that varies by driver in practice. Run against a real
   scene that actually uses colorkeyed textures for a meaningful =4
   signal -- one with hasColorKey always false everywhere only measures
   the short-circuit (or lack of it), not the discard's own real cost.

   Visibly WRONG output (no textures/lighting/fog/clipping/alpha-test at
   =1; no combiner/clip/alpha-test/fog at =2/=3/=4; no colorkey cutout at
   =3) is the point at every level; none are ever meant to be used
   outside a targeted A/B timing comparison. */
/* Minimal vertex shader for the LONGROAD2_CHEAP_SHADER diagnostic path
   (both =1 and =2) -- declares every varying k_fs_cheap/k_fs_texonly
   expect, but only actually computes the ones they read (v_diffuse,
   v_texcoord0/1); the rest are dummy-initialized purely so the varying
   interface between the two linked shaders matches. Plain non-RHW
   transform only -- diagnostic-only, never meant to render correctly
   (see this comment block's own opening paragraph). */
static const char *k_vs_cheap =
    "#version 100\n"
    "attribute vec4 a_pos;\n"
    "attribute vec4 a_diffuse;\n"
    "attribute vec2 a_texcoord0;\n"
    "attribute vec2 a_texcoord1;\n"
    "uniform mat4 u_world;\n"
    "uniform mat4 u_view;\n"
    "uniform mat4 u_proj;\n"
    "varying vec4 v_diffuse;\n"
    "varying vec3 v_world_pos;\n"
    "varying vec3 v_world_normal;\n"
    "varying vec2 v_texcoord0;\n"
    "varying vec2 v_texcoord1;\n"
    "varying float v_fog_dist;\n"
    "varying vec3 v_clipdist_a;\n"
    "varying vec3 v_clipdist_b;\n"
    "void main() {\n"
    "    v_diffuse = a_diffuse.zyxw;\n"
    "    v_texcoord0 = a_texcoord0;\n"
    "    v_texcoord1 = a_texcoord1;\n"
    "    v_world_pos = vec3(0.0);\n"
    "    v_world_normal = vec3(0.0);\n"
    "    v_fog_dist = 0.0;\n"
    "    v_clipdist_a = vec3(1.0);\n"
    "    v_clipdist_b = vec3(1.0);\n"
    "    gl_Position = u_proj * u_view * u_world * vec4(a_pos.xyz, 1.0);\n"
    "}\n";

static const char *k_fs_cheap =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec4 v_diffuse;\n"
    "varying vec3 v_world_pos;\n"
    "varying vec3 v_world_normal;\n"
    "varying vec2 v_texcoord0;\n"
    "varying vec2 v_texcoord1;\n"
    "varying float v_fog_dist;\n"
    "varying vec3 v_clipdist_a;\n"
    "varying vec3 v_clipdist_b;\n"
    "void main() {\n"
    "    gl_FragColor = v_diffuse;\n"
    "}\n";

static const char *k_fs_texonly =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec4 v_diffuse;\n"
    "varying vec3 v_world_pos;\n"
    "varying vec3 v_world_normal;\n"
    "varying vec2 v_texcoord0;\n"
    "varying vec2 v_texcoord1;\n"
    "varying float v_fog_dist;\n"
    "varying vec3 v_clipdist_a;\n"
    "varying vec3 v_clipdist_b;\n"
    "uniform sampler2D u_tex0;\n"
    "void main() {\n"
    "    gl_FragColor = v_diffuse * texture2D(u_tex0, v_texcoord0);\n"
    "}\n";

/* Declares the real TexStage struct/u_stages[8] uniform (byte-identical
   to k_fs_header_uniforms) so this reads the SAME real per-draw
   hasTexture value production does, through the SAME branch shape --
   see this file's own CHEAP_SHADER comment block above for what =3
   isolates and why. */
static const char *k_fs_texonly_gated =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec4 v_diffuse;\n"
    "varying vec3 v_world_pos;\n"
    "varying vec3 v_world_normal;\n"
    "varying vec2 v_texcoord0;\n"
    "varying vec2 v_texcoord1;\n"
    "varying float v_fog_dist;\n"
    "varying vec3 v_clipdist_a;\n"
    "varying vec3 v_clipdist_b;\n"
    "struct TexStage {\n"
    "    float hasTexture;\n"
    "    float texCoordIndex;\n"
    "    float hasColorKey;\n"
    "    vec3 colorKeyLow;\n"
    "    vec3 colorKeyHigh;\n"
    "};\n"
    "uniform TexStage u_stages[" STRINGIFY(GL_MAX_TEX_STAGES) "];\n"
    "uniform sampler2D u_tex0;\n"
    "void main() {\n"
    "    vec4 texVal = vec4(1.0);\n"
    "    if (u_stages[0].hasTexture > 0.5) texVal = texture2D(u_tex0, v_texcoord0);\n"
    "    gl_FragColor = v_diffuse * texVal;\n"
    "}\n";

/* =3 plus the real colorkey discard block, byte-identical to
   emit_stage_glsl's own -- see this file's own CHEAP_SHADER comment
   block above for what =4 isolates and why (including the real-driver
   short-circuit question this specifically exercises). */
static const char *k_fs_texonly_colorkey =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec4 v_diffuse;\n"
    "varying vec3 v_world_pos;\n"
    "varying vec3 v_world_normal;\n"
    "varying vec2 v_texcoord0;\n"
    "varying vec2 v_texcoord1;\n"
    "varying float v_fog_dist;\n"
    "varying vec3 v_clipdist_a;\n"
    "varying vec3 v_clipdist_b;\n"
    "struct TexStage {\n"
    "    float hasTexture;\n"
    "    float texCoordIndex;\n"
    "    float hasColorKey;\n"
    "    vec3 colorKeyLow;\n"
    "    vec3 colorKeyHigh;\n"
    "};\n"
    "uniform TexStage u_stages[" STRINGIFY(GL_MAX_TEX_STAGES) "];\n"
    "uniform sampler2D u_tex0;\n"
    "void main() {\n"
    "    vec4 texVal = vec4(1.0);\n"
    "    if (u_stages[0].hasTexture > 0.5) texVal = texture2D(u_tex0, v_texcoord0);\n"
    "    if (u_stages[0].hasTexture > 0.5 && u_stages[0].hasColorKey > 0.5 &&\n"
    "        all(greaterThanEqual(texVal.rgb, u_stages[0].colorKeyLow - vec3(0.0032))) &&\n"
    "        all(lessThanEqual(texVal.rgb, u_stages[0].colorKeyHigh + vec3(0.0032)))) {\n"
    "        discard;\n"
    "    }\n"
    "    gl_FragColor = v_diffuse * texVal;\n"
    "}\n";

static int d3d7_cheap_shader_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *v = getenv("LONGROAD2_CHEAP_SHADER");
        mode = (v != NULL) ? atoi(v) : 0;
    }
    return mode;
}

static GLuint d3d7_compile_shader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        GLsizei log_len = 0;
        glGetShaderInfoLog(sh, sizeof(log), &log_len, log);
        /* An empty info log alongside a real compile failure is a real,
           if unusual, driver response (seen live on a real device) --
           glGetError() at least distinguishes "the driver genuinely
           rejected this GLSL with nothing to say" from "something
           upstream (a bad GLuint, a lost context) made even issuing the
           compile itself invalid". */
        GLenum err = glGetError();
        fprintf(stderr, "[d3d7] shader compile failed (type=0x%04x, log_len=%d, glGetError=0x%04x): "
                        "%s\n", type, (int)log_len, err, log_len > 0 ? log : "(empty info log)");
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* One active stage's full combiner IDENTITY -- everything that decides
   which GLSL CODE gets generated for it (see emit_stage_glsl below),
   as opposed to a uniform VALUE consumed by code already present
   regardless (texCoordIndex, hasTexture, hasColorKey, colorKeyLow/High,
   u_texture_factor's actual color -- all stay runtime-uniform-driven,
   see emit_stage_glsl's own comment on why). Raw D3DTA_* byte values
   (selector in the low nibble, D3DTA_COMPLEMENT/ALPHAREPLICATE as
   separate OR'd-in bits) straight from SetTextureStageState -- decoded
   at codegen time (emit_arg_expr), not here. */
typedef struct {
    uint32_t color_op;
    uint32_t color_arg1;
    uint32_t color_arg2;
    uint32_t alpha_op;
    uint32_t alpha_arg1;
    uint32_t alpha_arg2;
} stage_key_t;

/* Which shader VARIANT a draw needs -- deliberately covers only the
   render-state axes that change shader STRUCTURE (which GLSL code paths
   exist at all), not ones that only change a uniform VALUE consumed by
   code already present in every variant that compiles it in at all
   (e.g. u_alpha_func's specific comparison). stage_count is the count
   of LEADING active stages, stopping at the first D3DTOP_DISABLE --
   matching real D3D7 semantics (and bind_texture_stages' own
   "stageActive" chain, now retired -- see emit_stage_glsl): once one
   stage disables, every later stage is unreachable regardless of its
   own state, so compiling combiner code for it would be dead code no
   draw could ever reach. Only `stages[0, stage_count)` are meaningful;
   the rest of the array is never read. */
typedef struct {
    /* vertex-shader-structural -- see build_vertex_shader_source */
    int is_rhw;
    int lighting_enabled;  /* forced 0 when is_rhw -- real D3D7 RHW vertices bypass the whole T&L pipeline, lighting included */
    int specular_enabled;  /* forced 0 when !lighting_enabled */
    /* fragment-shader-structural */
    int stage_count;
    stage_key_t stages[MAX_TEX_STAGES];
    int fog_enabled;
    int alpha_test_enabled;
    int clipping_enabled;
    /* Resolved VALUE of each *_enabled axis above -- forced to a
       canonical sentinel (0 / D3DFOG_NONE) whenever its own enable is
       off, so two device states that differ only in an irrelevant
       leftover value (e.g. a stale ALPHAFUNC while ALPHATESTENABLE=0)
       don't spuriously build two functionally-identical variants. Same
       pattern as the `if (!enabled) return;` early-outs already in
       upload_alpha_test_uniforms/upload_fog_uniforms/
       upload_clip_plane_uniforms (d3d7.c). */
    uint32_t alpha_func;      /* D3DCMPFUNC */
    uint32_t fog_mode;        /* D3DFOGMODE, post FOGTABLEMODE/FOGVERTEXMODE precedence */
    uint32_t clip_plane_mask; /* low 6 bits of D3DRENDERSTATE_CLIPPLANEENABLE */
} shader_key_t;

/* is_rhw is per-draw-call data derived from the vertex FVF (decode_fvf's
   own is_rhw field), not part of the persistent dev->state -- it's
   already threaded through d3d_draw_prepare as a plain parameter today,
   just not yet into this function. Passed in rather than read from dev
   for the same reason d3d_draw_prepare itself receives it as a
   parameter: nothing in d3d_state_t records it. */
static shader_key_t compute_shader_key(const d3d_device7_t *dev, int is_rhw)
{
    shader_key_t key;
    key.is_rhw = is_rhw != 0;
    key.lighting_enabled = !key.is_rhw && (dev->state.render_state[D3DRENDERSTATE_LIGHTING] != 0);
    key.specular_enabled = key.lighting_enabled && (dev->state.render_state[D3DRENDERSTATE_SPECULARENABLE] != 0);
    key.stage_count = 0;
    for (int i = 0; i < GL_MAX_TEX_STAGES; i++) {
        const uint32_t *tss = dev->state.tss_state[i];
        if (tss[D3DTSS_COLOROP] == D3DTOP_DISABLE) break;
        stage_key_t *sk = &key.stages[key.stage_count];
        sk->color_op = tss[D3DTSS_COLOROP];
        sk->color_arg1 = tss[D3DTSS_COLORARG1];
        sk->color_arg2 = tss[D3DTSS_COLORARG2];
        sk->alpha_op = tss[D3DTSS_ALPHAOP];
        sk->alpha_arg1 = tss[D3DTSS_ALPHAARG1];
        sk->alpha_arg2 = tss[D3DTSS_ALPHAARG2];
        key.stage_count++;
    }
    const uint32_t *rs = dev->state.render_state;
    key.fog_enabled = rs[D3DRENDERSTATE_FOGENABLE] != 0;
    key.alpha_test_enabled = rs[D3DRENDERSTATE_ALPHATESTENABLE] != 0;
    key.clipping_enabled = rs[D3DRENDERSTATE_CLIPPING] != 0;

    key.alpha_func = key.alpha_test_enabled ? rs[D3DRENDERSTATE_ALPHAFUNC] : 0u;

    /* Exact same table-mode-takes-precedence-over-vertex-mode logic as
       upload_fog_uniforms (d3d7.c) -- reused, not re-derived. */
    uint32_t fog_mode = rs[D3DRENDERSTATE_FOGTABLEMODE];
    if (fog_mode == D3DFOG_NONE) fog_mode = rs[D3DRENDERSTATE_FOGVERTEXMODE];
    key.fog_mode = key.fog_enabled ? fog_mode : D3DFOG_NONE;

    key.clip_plane_mask = key.clipping_enabled ? (rs[D3DRENDERSTATE_CLIPPLANEENABLE] & 0x3Fu) : 0u;

    return key;
}

static int stage_key_equal(const stage_key_t *a, const stage_key_t *b)
{
    return a->color_op == b->color_op &&
           a->color_arg1 == b->color_arg1 && a->color_arg2 == b->color_arg2 &&
           a->alpha_op == b->alpha_op &&
           a->alpha_arg1 == b->alpha_arg1 && a->alpha_arg2 == b->alpha_arg2;
}

static int shader_key_equal(const shader_key_t *a, const shader_key_t *b)
{
    if (a->is_rhw != b->is_rhw ||
        a->lighting_enabled != b->lighting_enabled ||
        a->specular_enabled != b->specular_enabled ||
        a->stage_count != b->stage_count ||
        a->fog_enabled != b->fog_enabled ||
        a->alpha_test_enabled != b->alpha_test_enabled ||
        a->clipping_enabled != b->clipping_enabled ||
        a->alpha_func != b->alpha_func ||
        a->fog_mode != b->fog_mode ||
        a->clip_plane_mask != b->clip_plane_mask) {
        return 0;
    }
    for (int i = 0; i < a->stage_count; i++) {
        if (!stage_key_equal(&a->stages[i], &b->stages[i])) return 0;
    }
    return 1;
}

/* One compiled GL program per distinct shader_key_t, built lazily the
   first time a draw needs a key not seen before (same lazy-build
   rationale render.c's ensure_present_resources already uses -- a
   device that never draws shouldn't pay for GL program objects), then
   cached forever (real D3D7 games cycle through a small, bounded set of
   render-state combinations, not an unbounded one -- see
   [[project_cheap_shader_experiment]] for the measurement that
   motivated compiling per-combination programs at all). locs holds this
   variant's OWN attrib/uniform locations -- these are NOT guaranteed to
   match another variant's locations even for identically-named
   attributes/uniforms in a byte-identical vertex shader, since each
   variant is a SEPARATE linked program object. */
typedef struct {
    shader_key_t key;
    GLuint prog;
    shader_locs_t locs;
} shader_variant_t;

static shader_variant_t *g_variants = NULL;
static int g_variant_count = 0;
static int g_variant_capacity = 0;

static shader_variant_t *find_shader_variant(const shader_key_t *key)
{
    for (int i = 0; i < g_variant_count; i++) {
        if (shader_key_equal(&g_variants[i].key, key)) return &g_variants[i];
    }
    return NULL;
}

/* Appends printf-style text to out (bounded by out_size), returning the
   new logical position -- same overflow-safe clamping convention every
   snprintf call in this file already follows by hand (see
   build_fragment_shader_source's own comment for why the clamp
   matters), factored into one helper here since the per-stage codegen
   below calls this many times per stage rather than once per fixed
   template string. */
static size_t emit_fmt(char *out, size_t out_size, size_t pos, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + pos, pos < out_size ? out_size - pos : 0, fmt, ap);
    va_end(ap);
    if (n > 0) pos += (size_t)n;
    if (pos > out_size) pos = out_size;
    return pos;
}

/* Resolves ONE D3DTA_* combiner argument (selector + D3DTA_COMPLEMENT/
   ALPHAREPLICATE modifier bits) into a self-contained GLSL expression
   string, given everything about it is now compile-time-known --
   mirrors readArg's exact runtime formula branch-for-branch (not a new
   derivation, the same already-shipped math just relocated from a GLSL
   if-chain to a C switch), with ONE additional resolution readArg
   itself couldn't do: readArg's D3DTA_CURRENT case is
   `mix(current, current*texVal, premodulate)`, where premodulate used
   to be a RUNTIME variable updated at the end of the PRECEDING stage
   (`premodulate = (colorOp == D3DTOP_PREMODULATE) ? 1.0 : 0.0`) -- but
   that preceding stage's colorOp is ALSO in the key now, so the caller
   (emit_stage_glsl) can pass whether premodulate is 1 or 0 as a plain
   compile-time C bool, letting this resolve straight to `current` or
   `(current * texVal)` with no runtime mix() at all. GLSL's own mix(x,
   y,0.0)==x / mix(x,y,1.0)==y exactly (IEEE754 x*1+y*0 / x*0+y*1), so
   this is bit-identical to the old runtime formula, not an
   approximation. */
static void emit_arg_expr(char *buf, size_t buf_size, uint32_t arg_packed, int premodulate_is_one)
{
    uint32_t selector = arg_packed & D3DTA_SELECTMASK;
    int complement = (arg_packed & D3DTA_COMPLEMENT) != 0;
    int alpha_replicate = (arg_packed & D3DTA_ALPHAREPLICATE) != 0;
    const char *base;
    switch (selector) {
        case D3DTA_CURRENT:  base = premodulate_is_one ? "(current * texVal)" : "current"; break;
        case D3DTA_TEXTURE:  base = "texVal"; break;
        case D3DTA_TFACTOR:  base = "u_texture_factor"; break;
        case D3DTA_SPECULAR: base = "v_diffuse"; break;
        default:              base = "v_diffuse"; break; /* D3DTA_DIFFUSE */
    }
    if (complement && alpha_replicate) {
        snprintf(buf, buf_size, "(vec4(1.0) - %s).aaaa", base);
    } else if (complement) {
        snprintf(buf, buf_size, "(vec4(1.0) - %s)", base);
    } else if (alpha_replicate) {
        snprintf(buf, buf_size, "%s.aaaa", base);
    } else {
        snprintf(buf, buf_size, "%s", base);
    }
}

/* Resolves ONE D3DTOP_* combiner op into a self-contained GLSL vec4
   expression, given two already-resolved argument expressions (see
   emit_arg_expr) -- mirrors calcOp's exact runtime formula branch-for-
   branch, same reasoning as emit_arg_expr above. diffuseA/textureA/
   tfactorA/currentA are always the same fixed sub-expressions
   (v_diffuse.a/texVal.a/u_texture_factor.a/current.a), inlined directly
   rather than passed as parameters -- calcOp took them as parameters
   only because it was ONE shared runtime function serving every op;
   here each op gets its own generated statement, so there's no shared
   function body that needs them threaded through. arg1/arg2 are
   emitted into the formula textually, not evaluated into a temp first
   -- both are pure (variable references or a `1.0 - x` wrap, never a
   function call with side effects), so a formula like ADDSMOOTH that
   references arg1 or arg2 twice just re-reads the same value, not a
   double side effect. */
static void emit_op_formula(char *buf, size_t buf_size, uint32_t op, const char *arg1, const char *arg2)
{
    switch (op) {
        case D3DTOP_SELECTARG2:
            snprintf(buf, buf_size, "(%s)", arg2);
            break;
        case D3DTOP_MODULATE:
            snprintf(buf, buf_size, "clamp((%s) * (%s), 0.0, 1.0)", arg1, arg2);
            break;
        case D3DTOP_MODULATE2X:
            snprintf(buf, buf_size, "clamp((%s) * (%s) * 2.0, 0.0, 1.0)", arg1, arg2);
            break;
        case D3DTOP_MODULATE4X:
            snprintf(buf, buf_size, "clamp((%s) * (%s) * 4.0, 0.0, 1.0)", arg1, arg2);
            break;
        case D3DTOP_ADD:
            snprintf(buf, buf_size, "clamp((%s) + (%s), 0.0, 1.0)", arg1, arg2);
            break;
        case D3DTOP_ADDSIGNED:
            snprintf(buf, buf_size, "clamp((%s) + ((%s) - 0.5), 0.0, 1.0)", arg1, arg2);
            break;
        case D3DTOP_ADDSIGNED2X:
            snprintf(buf, buf_size, "clamp(((%s) + ((%s) - 0.5)) * 2.0, 0.0, 1.0)", arg1, arg2);
            break;
        case D3DTOP_SUBTRACT:
            snprintf(buf, buf_size, "clamp((%s) - (%s), 0.0, 1.0)", arg1, arg2);
            break;
        case D3DTOP_ADDSMOOTH:
            snprintf(buf, buf_size, "clamp((%s) + (%s) - (%s) * (%s), 0.0, 1.0)", arg1, arg2, arg1, arg2);
            break;
        case D3DTOP_BLENDDIFFUSEALPHA:
            snprintf(buf, buf_size, "mix(%s, %s, v_diffuse.a)", arg2, arg1);
            break;
        case D3DTOP_BLENDTEXTUREALPHA:
            snprintf(buf, buf_size, "mix(%s, %s, texVal.a)", arg2, arg1);
            break;
        case D3DTOP_BLENDFACTORALPHA:
            snprintf(buf, buf_size, "mix(%s, %s, u_texture_factor.a)", arg2, arg1);
            break;
        case D3DTOP_BLENDTEXTUREALPHAPM:
            snprintf(buf, buf_size, "clamp((%s) + (%s) * (1.0 - texVal.a), 0.0, 1.0)", arg1, arg2);
            break;
        case D3DTOP_BLENDCURRENTALPHA:
            snprintf(buf, buf_size, "mix(%s, %s, current.a)", arg2, arg1);
            break;
        case D3DTOP_MODULATEALPHA_ADDCOLOR:
            snprintf(buf, buf_size, "clamp((%s).a * (%s) + (%s), 0.0, 1.0)", arg1, arg2, arg1);
            break;
        case D3DTOP_MODULATECOLOR_ADDALPHA:
            snprintf(buf, buf_size, "clamp((%s) * (%s) + (%s).a, 0.0, 1.0)", arg1, arg2, arg1);
            break;
        case D3DTOP_MODULATEINVALPHA_ADDCOLOR:
            snprintf(buf, buf_size, "clamp((1.0 - (%s).a) * (%s) + (%s), 0.0, 1.0)", arg1, arg2, arg1);
            break;
        case D3DTOP_MODULATEINVCOLOR_ADDALPHA:
            snprintf(buf, buf_size, "clamp((1.0 - (%s)) * (%s) + (%s).a, 0.0, 1.0)", arg1, arg2, arg1);
            break;
        case D3DTOP_DOTPRODUCT3:
            snprintf(buf, buf_size,
                     "vec4(clamp(dot((%s).rgb - 0.5, (%s).rgb - 0.5) * 4.0, 0.0, 1.0))", arg1, arg2);
            break;
        default: /* SELECTARG1 / PREMODULATE / BUMPENVMAP[LUMINANCE] / unrecognized -- matches calcOp's own fallback */
            snprintf(buf, buf_size, "(%s)", arg1);
            break;
    }
}

/* Generates ONE active stage's full block: texture sample + colorkey
   discard (unchanged from before, still runtime-uniform-driven -- see
   the comment above this function's neighbors), then the color (and,
   if this stage's alphaOp isn't DISABLE, alpha) combine using the fully
   resolved formulas above. newColor/newAlpha are computed from the
   PRE-stage `current` (matching the original template's `next`
   indirection) BEFORE either is written back to `current` -- required
   because D3DTOP_BLENDCURRENTALPHA's formula reads current.a, and
   alphaArg1/2 can reference D3DTA_CURRENT too; writing current.rgb
   first would corrupt what those reads see. Wrapped in a bare `{ }`
   block (not `if (stageActive)` -- compile-time guaranteed active by
   construction, see shader_key_t's own comment) purely for GLSL
   variable scoping, so texVal/stageUv can reuse the same names across
   every stage without colliding. Returns the caller's premodulate
   state for the NEXT stage (see emit_arg_expr's own comment). */
/* Test-only counter: how many times emit_stage_glsl's own duplicate-ALU
   merge (below) actually fired -- a pixel-correctness check alone can't
   distinguish "merged" from "not merged" here (the merge only fires
   when the two resolved formula strings are ALREADY byte-identical, so
   by construction it's semantically a no-op either way; a strcmp bug
   letting it fire on non-identical formulas is the only way it could
   ever be wrong, not a math error) -- same call/skip-counting idiom as
   the shader-variant lookup cache's own test hooks. */
static long g_stage_merge_count = 0;
/* See d3d7.c's own d3d7_perf_*_for_test comment -- same "GL-thread-
   exclusive counter, EndScene's real replay is now async" reasoning
   applies to every test counter in this file too. */
long d3d7_shader_get_stage_merge_count_for_test(void) { gl_thread_wait_idle(); return g_stage_merge_count; }
void d3d7_shader_reset_stage_merge_count_for_test(void) { gl_thread_wait_idle(); g_stage_merge_count = 0; }

static size_t emit_stage_glsl(char *out, size_t out_size, size_t pos, int stage_index,
                               const stage_key_t *sk, int premodulate_in_is_one)
{
    pos = emit_fmt(out, out_size, pos,
        "    {\n"
        "        vec2 stageUv = (u_stages[%d].texCoordIndex < 0.5) ? v_texcoord0 : v_texcoord1;\n"
        "        vec4 texVal = vec4(1.0);\n"
        "        if (u_stages[%d].hasTexture > 0.5) texVal = texture2D(u_tex%d, stageUv);\n"
        "        if (u_stages[%d].hasTexture > 0.5 && u_stages[%d].hasColorKey > 0.5 &&\n"
        "            all(greaterThanEqual(texVal.rgb, u_stages[%d].colorKeyLow - vec3(0.0032))) &&\n"
        "            all(lessThanEqual(texVal.rgb, u_stages[%d].colorKeyHigh + vec3(0.0032)))) {\n"
        "            discard;\n"
        "        }\n",
        stage_index, stage_index, stage_index, stage_index, stage_index, stage_index, stage_index);

    char arg1c[64], arg2c[64], color_formula[512];
    emit_arg_expr(arg1c, sizeof(arg1c), sk->color_arg1, premodulate_in_is_one);
    emit_arg_expr(arg2c, sizeof(arg2c), sk->color_arg2, premodulate_in_is_one);
    emit_op_formula(color_formula, sizeof(color_formula), sk->color_op, arg1c, arg2c);

    /* Real, live-confirmed bug: a stage whose op/args reference D3DTA_TEXTURE
       (resolves to the GLSL local `texVal`, see emit_arg_expr) but has NO
       texture actually bound this draw (u_stages[N].hasTexture <= 0.5,
       confirmed real via a live capture: this game explicitly calls
       SetTexture(stage, NULL) while leaving that stage's own COLOROP/
       ALPHAOP configured as if a texture WERE coming, e.g. SELECTARG1
       with ColorArg1=D3DTA_TEXTURE) used to run this stage's formula
       anyway, using texVal's own "no texture" default (vec4(1.0), opaque
       white) -- silently REPLACING whatever the PREVIOUS stage had
       already computed (a real, correctly textured color) with solid
       white. Real D3D7 treats a stage referencing an unbound texture as a
       no-op (same as COLOROP/ALPHAOP == DISABLE), not as "substitute
       white" -- confirmed the actual, live root cause of a "second
       texture stage is configured but never actually used" report
       rendering as a flat white rectangle instead of the real, first-
       stage-textured image. Gating the whole update (not just the
       texture2D sample, which was ALREADY correctly guarded) behind the
       same hasTexture check this stage's own texVal already uses makes an
       unbound-texture stage a genuine pass-through, leaving `current`
       exactly as the previous stage left it. Only applied when this
       stage's own args actually need a texture -- a stage combining
       DIFFUSE/CURRENT/TFACTOR only must keep running unconditionally,
       it was never texture-dependent to begin with. */
    int needs_texture =
        (sk->color_arg1 & D3DTA_SELECTMASK) == D3DTA_TEXTURE ||
        (sk->color_arg2 & D3DTA_SELECTMASK) == D3DTA_TEXTURE ||
        (sk->alpha_op != D3DTOP_DISABLE &&
         ((sk->alpha_arg1 & D3DTA_SELECTMASK) == D3DTA_TEXTURE ||
          (sk->alpha_arg2 & D3DTA_SELECTMASK) == D3DTA_TEXTURE));
    if (needs_texture) {
        pos = emit_fmt(out, out_size, pos, "        if (u_stages[%d].hasTexture > 0.5) {\n", stage_index);
    }

    /* Real, common duplicate-ALU case found auditing a live shader dump
       ([[project_shader_variant_grooming_followon]]): plenty of real D3D
       content sets the SAME op/args for both COLOROP and ALPHAOP (e.g.
       MODULATE/MODULATE with identical TEXTURE*DIFFUSE args on both --
       confirmed live in 2 of 4 real textured variants captured), which
       used to emit the textually IDENTICAL formula twice -- once for
       newColor's .rgb, once for newAlpha's .a -- a real, avoidable extra
       multiply+clamp per fragment for every draw using that combination.
       Detected by comparing the two RESOLVED formula strings (not just
       op/arg equality -- a textual compare is what actually determines
       whether the emitted GLSL is identical either way, and is exactly
       as cheap to check). */
    if (sk->alpha_op != D3DTOP_DISABLE) {
        char arg1a[64], arg2a[64], alpha_formula[512];
        emit_arg_expr(arg1a, sizeof(arg1a), sk->alpha_arg1, premodulate_in_is_one);
        emit_arg_expr(arg2a, sizeof(arg2a), sk->alpha_arg2, premodulate_in_is_one);
        emit_op_formula(alpha_formula, sizeof(alpha_formula), sk->alpha_op, arg1a, arg2a);

        if (strcmp(color_formula, alpha_formula) == 0) {
            g_stage_merge_count++;
            pos = emit_fmt(out, out_size, pos, "        current = (%s);\n", color_formula);
            if (needs_texture) pos = emit_fmt(out, out_size, pos, "        }\n");
            return emit_fmt(out, out_size, pos, "    }\n");
        }

        pos = emit_fmt(out, out_size, pos, "        vec3 newColor = (%s).rgb;\n", color_formula);
        pos = emit_fmt(out, out_size, pos, "        float newAlpha = (%s).a;\n", alpha_formula);
        pos = emit_fmt(out, out_size, pos, "        current.rgb = newColor;\n");
        pos = emit_fmt(out, out_size, pos, "        current.a = newAlpha;\n");
        if (needs_texture) pos = emit_fmt(out, out_size, pos, "        }\n");
        return emit_fmt(out, out_size, pos, "    }\n");
    }

    pos = emit_fmt(out, out_size, pos, "        vec3 newColor = (%s).rgb;\n", color_formula);
    pos = emit_fmt(out, out_size, pos, "        current.rgb = newColor;\n");
    if (needs_texture) pos = emit_fmt(out, out_size, pos, "        }\n");
    return emit_fmt(out, out_size, pos, "    }\n");
}

/* Builds the final vertex shader source for ONE variant: k_vs_header_base
   (attributes/uniforms/v_diffuse, always) plus k_vs_header_clip/
   _texcoord/_fog, each only when the matching shader_key_t axis needs
   it (must track build_fragment_shader_source's own gating exactly --
   GLSL ES 100 requires linked stages to agree on varying declarations),
   the lighting declarations + computeLighting() (only when
   key->lighting_enabled, with its own specular sub-block only when
   key->specular_enabled too -- real D3D7 RHW vertices bypass lighting
   entirely, so lighting_enabled is already forced 0 whenever is_rhw is
   set, see compute_shader_key), then k_vs_main_open (+ k_vs_texcoord_
   write when needed), then k_vs_main_rhw (+ its own gated fog/clip
   pieces) or (k_vs_main_xyz + gated viewpos/fog/clip pieces + optionally
   k_vs_main_xyz_lighting) depending on key->is_rhw/lighting_enabled,
   then k_vs_main_close. Same emit_fmt+clamp idiom as
   build_fragment_shader_source. */
static void build_vertex_shader_source(char *out, size_t out_size, const shader_key_t *key)
{
    int need_texcoords = key->stage_count > 0;
    int need_clip = key->clip_plane_mask != 0;
    int need_viewpos = key->fog_enabled || key->lighting_enabled;

    size_t pos = 0;
    pos = emit_fmt(out, out_size, pos, "%s", k_vs_header_base);
    if (need_clip) pos = emit_fmt(out, out_size, pos, "%s", k_vs_header_clip);
    if (need_texcoords) pos = emit_fmt(out, out_size, pos, "%s", k_vs_header_texcoord);
    if (key->fog_enabled) pos = emit_fmt(out, out_size, pos, "%s", k_vs_header_fog);
    pos = emit_fmt(out, out_size, pos, "\n");
    if (key->lighting_enabled) {
        pos = emit_fmt(out, out_size, pos, "%s", k_vs_lighting_decls);
        pos = emit_fmt(out, out_size, pos, "%s", k_vs_lighting_fn_start);
        if (key->specular_enabled) {
            pos = emit_fmt(out, out_size, pos, "%s", k_vs_lighting_specular);
        }
        pos = emit_fmt(out, out_size, pos, "%s", k_vs_lighting_fn_end);
    }
    pos = emit_fmt(out, out_size, pos, "%s", k_vs_main_open);
    if (need_texcoords) pos = emit_fmt(out, out_size, pos, "%s", k_vs_texcoord_write);
    if (key->is_rhw) {
        pos = emit_fmt(out, out_size, pos, "%s", k_vs_main_rhw);
        if (key->fog_enabled) pos = emit_fmt(out, out_size, pos, "%s", k_vs_main_rhw_fog);
        if (need_clip) pos = emit_fmt(out, out_size, pos, "%s", k_vs_main_rhw_clip);
    } else {
        pos = emit_fmt(out, out_size, pos, "%s", k_vs_main_xyz);
        if (need_viewpos) pos = emit_fmt(out, out_size, pos, "%s", k_vs_main_xyz_viewpos);
        if (key->fog_enabled) pos = emit_fmt(out, out_size, pos, "%s", k_vs_main_xyz_fog);
        if (need_clip) pos = emit_fmt(out, out_size, pos, "%s", k_vs_main_xyz_clip);
        if (key->lighting_enabled) {
            pos = emit_fmt(out, out_size, pos, "%s", k_vs_main_xyz_lighting);
        }
    }
    emit_fmt(out, out_size, pos, "%s", k_vs_main_close);
}

/* Builds the final fragment shader source for ONE variant: k_fs_header_base
   plus k_fs_header_texcoord/_fog/_clip (only when the matching
   shader_key_t axis needs it -- must track build_vertex_shader_source's
   own gating exactly) then k_fs_header_uniforms, one sampler uniform per
   active stage (see k_fs_header_uniforms' own comment for why each
   stage needs its own literal sampler rather than an indexed array),
   k_fs_main_start, then emit_stage_glsl once per active stage (see
   shader_key_t's own comment on why stage_count stops at the first
   DISABLE), tracking premodulate purely as a C int between iterations
   (no runtime premodulate variable exists anymore -- see emit_arg_expr),
   then the key-selected footer pieces. Runs once per NEW variant (result
   only used to compile a shader immediately, not retained). */
static void build_fragment_shader_source(char *out, size_t out_size, const shader_key_t *key)
{
    /* snprintf returns the length it WOULD have written, which can
       exceed the remaining space if a single call's own text is longer
       than what's left -- without this clamp, pos could end up >
       out_size, and the next call's "out_size - pos" (both size_t)
       would underflow to a huge value. Not reachable with the actual
       fixed buffer size the one caller uses (comfortably larger than
       the real total), but cheap to make impossible outright rather
       than rely on that margin never changing. */
    size_t pos = 0;
    int n = snprintf(out + pos, out_size - pos, "%s", k_fs_header_base);
    if (n > 0) pos += (size_t)n;
    if (pos > out_size) pos = out_size;
    if (key->stage_count > 0) {
        n = snprintf(out + pos, out_size - pos, "%s", k_fs_header_texcoord);
        if (n > 0) pos += (size_t)n;
        if (pos > out_size) pos = out_size;
    }
    if (key->fog_enabled) {
        n = snprintf(out + pos, out_size - pos, "%s", k_fs_header_fog);
        if (n > 0) pos += (size_t)n;
        if (pos > out_size) pos = out_size;
    }
    if (key->clip_plane_mask != 0) {
        n = snprintf(out + pos, out_size - pos, "%s", k_fs_header_clip);
        if (n > 0) pos += (size_t)n;
        if (pos > out_size) pos = out_size;
    }
    n = snprintf(out + pos, out_size - pos, "%s", k_fs_header_uniforms);
    if (n > 0) pos += (size_t)n;
    if (pos > out_size) pos = out_size;
    for (int i = 0; i < key->stage_count && pos < out_size; i++) {
        n = snprintf(out + pos, out_size - pos, k_fs_sampler_template, i);
        if (n > 0) pos += (size_t)n;
        if (pos > out_size) pos = out_size;
    }
    n = snprintf(out + pos, out_size - pos, "%s", k_fs_main_start);
    if (n > 0) pos += (size_t)n;
    if (pos > out_size) pos = out_size;

    int premodulate_is_one = 0;
    for (int i = 0; i < key->stage_count && pos < out_size; i++) {
        pos = emit_stage_glsl(out, out_size, pos, i, &key->stages[i], premodulate_is_one);
        premodulate_is_one = (key->stages[i].color_op == D3DTOP_PREMODULATE) ? 1 : 0;
    }

    if (key->clipping_enabled) {
        for (int i = 0; i < 6 && pos < out_size; i++) {
            if (key->clip_plane_mask & (1u << i)) {
                pos = emit_fmt(out, out_size, pos, "%s", k_clip_plane_check[i]);
            }
        }
    }
    if (key->alpha_test_enabled && pos < out_size) {
        pos = emit_fmt(out, out_size, pos, "    if (!(%s)) discard;\n", alpha_func_expr(key->alpha_func));
    }
    if (key->fog_enabled && pos < out_size) {
        pos = emit_fmt(out, out_size, pos,
            "    current.rgb = mix(u_fog_color.rgb, current.rgb, clamp(%s, 0.0, 1.0));\n",
            fog_factor_expr(key->fog_mode));
    }
    if (pos < out_size) snprintf(out + pos, out_size - pos, "%s", k_fs_footer_end);
}

/* Shared by both build_shader_variant and ensure_cheap_shader_program:
   compiles+links vs_source+fs_source (both now vary per variant -- the
   vertex side no longer has a single shared k_vs_src, see
   build_vertex_shader_source), and on success populates g_locs (via the
   g_attr_X/g_unif_X macros) with THIS program's own attrib/uniform
   locations. Returns 0 on any compile/link failure, the linked program
   otherwise. Caller owns copying g_locs out before it's overwritten by
   the next call (variants do so into their own stored shader_locs_t;
   the cheap-shader path never needs to, since it never switches away). */
static GLuint compile_link_and_locate(const char *vs_source, const char *fs_source)
{
    GLuint vs = d3d7_compile_shader(GL_VERTEX_SHADER, vs_source);
    GLuint fs = d3d7_compile_shader(GL_FRAGMENT_SHADER, fs_source);
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
        fprintf(stderr, "[d3d7] shader link failed: %s\n", log);
        glDeleteProgram(prog);
        return 0;
    }

    g_attr_pos = glGetAttribLocation(prog, "a_pos");
    g_attr_normal = glGetAttribLocation(prog, "a_normal");
    g_attr_diffuse = glGetAttribLocation(prog, "a_diffuse");
    g_attr_texcoord0 = glGetAttribLocation(prog, "a_texcoord0");
    g_attr_texcoord1 = glGetAttribLocation(prog, "a_texcoord1");
    g_unif_world = glGetUniformLocation(prog, "u_world");
    g_unif_view = glGetUniformLocation(prog, "u_view");
    g_unif_proj = glGetUniformLocation(prog, "u_proj");
    g_unif_vp_px = glGetUniformLocation(prog, "u_vp_px");
    g_unif_point_size = glGetUniformLocation(prog, "u_point_size");

    g_unif_local_viewer = glGetUniformLocation(prog, "u_local_viewer");
    g_unif_normalize_normals = glGetUniformLocation(prog, "u_normalize_normals");
    g_unif_global_ambient = glGetUniformLocation(prog, "u_global_ambient");
    g_unif_light_count = glGetUniformLocation(prog, "u_light_count");
    g_unif_mat_diffuse = glGetUniformLocation(prog, "u_mat_diffuse");
    g_unif_mat_ambient = glGetUniformLocation(prog, "u_mat_ambient");
    g_unif_mat_specular = glGetUniformLocation(prog, "u_mat_specular");
    g_unif_mat_emissive = glGetUniformLocation(prog, "u_mat_emissive");
    g_unif_mat_power = glGetUniformLocation(prog, "u_mat_power");

    g_unif_alpha_ref = glGetUniformLocation(prog, "u_alpha_ref");
    g_unif_fog_color = glGetUniformLocation(prog, "u_fog_color");
    g_unif_fog_start = glGetUniformLocation(prog, "u_fog_start");
    g_unif_fog_end = glGetUniformLocation(prog, "u_fog_end");
    g_unif_fog_density = glGetUniformLocation(prog, "u_fog_density");

    g_unif_clip_planes = glGetUniformLocation(prog, "u_clip_planes");

    for (int i = 0; i < GL_MAX_LIGHTS; i++) {
        char name[64];
        light_uniform_locs_t *l = &g_unif_lights[i];
        snprintf(name, sizeof(name), "u_lights[%d].position", i);  l->position  = glGetUniformLocation(prog, name);
        snprintf(name, sizeof(name), "u_lights[%d].direction", i); l->direction = glGetUniformLocation(prog, name);
        snprintf(name, sizeof(name), "u_lights[%d].diffuse", i);   l->diffuse   = glGetUniformLocation(prog, name);
        snprintf(name, sizeof(name), "u_lights[%d].specular", i);  l->specular  = glGetUniformLocation(prog, name);
        snprintf(name, sizeof(name), "u_lights[%d].ambient", i);   l->ambient   = glGetUniformLocation(prog, name);
        snprintf(name, sizeof(name), "u_lights[%d].range", i);     l->range     = glGetUniformLocation(prog, name);
        snprintf(name, sizeof(name), "u_lights[%d].atten", i);     l->atten     = glGetUniformLocation(prog, name);
        snprintf(name, sizeof(name), "u_lights[%d].falloff", i);   l->falloff   = glGetUniformLocation(prog, name);
        snprintf(name, sizeof(name), "u_lights[%d].cone", i);      l->cone      = glGetUniformLocation(prog, name);
        snprintf(name, sizeof(name), "u_lights[%d].type", i);      l->type      = glGetUniformLocation(prog, name);
    }

    g_unif_texture_factor = glGetUniformLocation(prog, "u_texture_factor");
    for (int i = 0; i < GL_MAX_TEX_STAGES; i++) {
        char name[64];
        tex_stage_uniform_locs_t *s = &g_unif_stages[i];
        snprintf(name, sizeof(name), "u_stages[%d].hasTexture", i);  s->hasTexture  = glGetUniformLocation(prog, name);
        snprintf(name, sizeof(name), "u_stages[%d].texCoordIndex", i); s->texCoordIndex = glGetUniformLocation(prog, name);
        snprintf(name, sizeof(name), "u_stages[%d].hasColorKey", i); s->hasColorKey = glGetUniformLocation(prog, name);
        snprintf(name, sizeof(name), "u_stages[%d].colorKeyLow", i); s->colorKeyLow = glGetUniformLocation(prog, name);
        snprintf(name, sizeof(name), "u_stages[%d].colorKeyHigh", i); s->colorKeyHigh = glGetUniformLocation(prog, name);
        snprintf(name, sizeof(name), "u_tex%d", i);
        g_unif_tex[i] = glGetUniformLocation(prog, name);
    }

    /* Fixed sampler-to-texture-unit mapping (u_texN always samples GL
       texture unit N) -- set once here, not per draw call, since it
       never changes for this program's lifetime. */
    gl_state_use_program(prog);
    for (int i = 0; i < GL_MAX_TEX_STAGES; i++) glUniform1i(g_unif_tex[i], i);

    return prog;
}

/* FNV-1a over explicit shader_key_t FIELDS, not raw struct bytes -- a
   plain memcmp/hash of the struct would pick up compiler-inserted
   padding bytes between fields, which C leaves uninitialized (this
   struct is stack-allocated in compute_shader_key with no
   `= {0}` -- confirmed by reading it), so two logically-identical keys
   built on different runs (or even the same run, different call sites)
   could hash differently for reasons that have nothing to do with the
   actual render state. Only mixes stages[0, stage_count) -- the rest of
   the array is documented as never meaningfully read (see
   shader_key_t's own comment), so mixing it in would be the same
   padding-style nondeterminism risk. Debug-naming only, not a
   correctness-critical hash (find_shader_variant never uses this) --
   collisions just mean two dumped files share a name and one overwrites
   the other, acceptable for a diagnostic feature. */
static uint32_t shader_key_hash(const shader_key_t *key)
{
    uint32_t h = 2166136261u;
#define MIX(v) do { h ^= (uint32_t)(v); h *= 16777619u; } while (0)
    MIX(key->is_rhw);
    MIX(key->lighting_enabled);
    MIX(key->specular_enabled);
    MIX(key->stage_count);
    for (int i = 0; i < key->stage_count; i++) {
        const stage_key_t *sk = &key->stages[i];
        MIX(sk->color_op); MIX(sk->color_arg1); MIX(sk->color_arg2);
        MIX(sk->alpha_op); MIX(sk->alpha_arg1); MIX(sk->alpha_arg2);
    }
    MIX(key->fog_enabled);
    MIX(key->alpha_test_enabled);
    MIX(key->clipping_enabled);
    MIX(key->alpha_func);
    MIX(key->fog_mode);
    MIX(key->clip_plane_mask);
#undef MIX
    return h;
}

/* LONGROAD2_SHADER_DUMP_DIR: writes every newly-built variant's real
   GLSL source to <dir>/<hash>.vert and <dir>/<hash>.frag -- same
   env-var-supplies-an-existing-directory convention as render.c's own
   present_dump_frame_if_enabled (LONGROAD2_FRAME_DUMP_DIR): no mkdir
   here, fopen() failing (directory doesn't exist) just silently skips
   the dump rather than erroring. Debug/analysis aid only -- lets a
   specific variant's actual compiled GLSL be inspected or diffed across
   a session without attaching a GPU debugger, e.g. to confirm a given
   combiner-op combination resolved to the expected formula. */
static void dump_variant_if_enabled(const shader_key_t *key, const char *vs_source, const char *fs_source)
{
    static int checked = 0;
    static const char *dump_dir = NULL;
    if (!checked) {
        dump_dir = getenv("LONGROAD2_SHADER_DUMP_DIR");
        checked = 1;
    }
    if (dump_dir == NULL) return;

    uint32_t id = shader_key_hash(key) & 0xFFFFFFu;
    char path[512];
    snprintf(path, sizeof(path), "%s/%06X.vert", dump_dir, id);
    FILE *f = fopen(path, "wb");
    if (f != NULL) { fputs(vs_source, f); fclose(f); }
    snprintf(path, sizeof(path), "%s/%06X.frag", dump_dir, id);
    f = fopen(path, "wb");
    if (f != NULL) {
        fputs(fs_source, f);
        fclose(f);
        fprintf(stderr, "[d3d7] dumped shader variant %06X to %s\n", id, dump_dir);
    }
}

static shader_variant_t *build_shader_variant(const shader_key_t *key)
{
    static char vs_source[16384];
    static char fs_source[32768];
    build_vertex_shader_source(vs_source, sizeof(vs_source), key);
    build_fragment_shader_source(fs_source, sizeof(fs_source), key);

    /* Dump BEFORE compiling, not after: a failed compile is exactly the
       case LONGROAD2_SHADER_DUMP_DIR is most useful for (the real GLSL
       source behind a "[d3d7] shader compile failed" log line), and the
       old post-success-only placement meant that source was never
       written anywhere -- a real diagnostic gap, not intentional. */
    dump_variant_if_enabled(key, vs_source, fs_source);

    GLuint prog = compile_link_and_locate(vs_source, fs_source);
    if (prog == 0) return NULL;

    if (g_variant_count == g_variant_capacity) {
        int new_cap = (g_variant_capacity == 0) ? 4 : g_variant_capacity * 2;
        shader_variant_t *grown = (shader_variant_t *)realloc(g_variants, (size_t)new_cap * sizeof(*g_variants));
        if (grown == NULL) {
            glDeleteProgram(prog);
            return NULL;
        }
        g_variants = grown;
        g_variant_capacity = new_cap;
    }

    shader_variant_t *variant = &g_variants[g_variant_count++];
    variant->key = *key;
    variant->prog = prog;
    variant->locs = g_locs; /* compile_link_and_locate just populated g_locs for THIS program */
    return variant;
}

/* Skips repeating compute_shader_key + find_shader_variant's own O(n)
   scan when nothing that could change the result has changed since the
   last call -- this function now runs unconditionally on EVERY draw
   (d3d_draw_prepare calls it before checking anything else), and most
   consecutive draws share identical render state. Mirrors d3d7.c's own
   g_draw_uniform_cache invariant one call site later: state_generation
   bumps on every SetRenderState/SetTransform/SetTextureStageState call
   (confirmed by direct audit -- the only three setters compute_shader_key
   reads from), so an unchanged (device, state_generation, is_rhw) triple
   guarantees compute_shader_key would resolve to the identical key
   without needing to actually recompute or look it up. Global, not
   per-device, same reasoning as g_draw_uniform_cache: only one GL
   program is ever active regardless of which d3d_device7_t issued a
   call, so cached_device guards against two different devices
   coincidentally reaching the same generation number.

   Stores an INDEX into g_variants, not a shader_variant_t*: g_variants
   is realloc()-grown (build_shader_variant), so a raw pointer held
   across calls could dangle the next time ANY new variant is built for
   a completely different key/device and happens to trigger a grow --
   the exact realloc-invalidation shape already caught by ASAN once
   before in this codebase (see
   [[project_cmdbuf_record_state_snapshot_dedup]]). An index stays valid
   forever: g_variants only ever grows by appending, entries are never
   removed or reordered. */
static struct {
    int valid;
    d3d_device7_t *cached_device;
    uint64_t state_generation;
    int is_rhw;
    int variant_index;
} g_last_variant_cache;

/* Test-only call/skip counters -- same "count total attempts vs. how many
   were actual skips" idiom as gl_state_cache.c's own vertex-attrib
   counters, exposed here (not d3d7.c) since this is where the cache they
   observe actually lives; d3d7.h re-declares these two so tests (which
   only see the public d3d7 module boundary, not this private header) can
   reach them -- same cross-file pattern d3d7_perf_get_gl_draws_for_test
   already establishes. */
static long g_variant_lookup_calls = 0;
static long g_variant_lookup_skips = 0;

long d3d7_shader_get_variant_lookup_calls_for_test(void) { gl_thread_wait_idle(); return g_variant_lookup_calls; }
long d3d7_shader_get_variant_lookup_skips_for_test(void) { gl_thread_wait_idle(); return g_variant_lookup_skips; }
void d3d7_shader_reset_variant_lookup_counters_for_test(void)
{
    gl_thread_wait_idle();
    g_variant_lookup_calls = 0;
    g_variant_lookup_skips = 0;
}

static int ensure_shader_variant(d3d_device7_t *dev, int is_rhw)
{
    g_variant_lookup_calls++;
    if (g_last_variant_cache.valid &&
        g_last_variant_cache.cached_device == dev &&
        g_last_variant_cache.state_generation == dev->state_generation &&
        g_last_variant_cache.is_rhw == is_rhw) {
        g_variant_lookup_skips++;
        shader_variant_t *variant = &g_variants[g_last_variant_cache.variant_index];
        g_prog = variant->prog;
        g_locs = variant->locs;
        return 1;
    }

    shader_key_t key = compute_shader_key(dev, is_rhw);
    shader_variant_t *variant = find_shader_variant(&key);
    if (variant == NULL) {
        double t0 = d3d7_perf_now_ms();
        variant = build_shader_variant(&key);
        double compile_ms = d3d7_perf_now_ms() - t0;
        if (variant == NULL) {
            g_last_variant_cache.valid = 0;
            return 0;
        }
        fprintf(stderr, "[d3d7] cold shader variant miss: id=%06X compile=%.1fms "
                        "(variant #%d, mid-gameplay stall if this isn't startup)\n",
                        shader_key_hash(&key) & 0xFFFFFFu, compile_ms, g_variant_count);
    }
    g_prog = variant->prog;
    g_locs = variant->locs;

    g_last_variant_cache.valid = 1;
    g_last_variant_cache.cached_device = dev;
    g_last_variant_cache.state_generation = dev->state_generation;
    g_last_variant_cache.is_rhw = is_rhw;
    g_last_variant_cache.variant_index = (int)(variant - g_variants);
    return 1;
}

void d3d7_shader_variant_cache_reset(void)
{
    g_last_variant_cache.valid = 0;
}

/* LONGROAD2_CHEAP_SHADER=1|2|3|4 diagnostic path -- deliberately bypasses
   the variant system entirely (a single ad hoc program, exactly like
   before this refactor) since it exists only for A/B timing comparisons
   against the real shader, never to be combined with per-key variants.
   See k_fs_cheap/k_fs_texonly's own comment above for what each level
   does and why. */
static int ensure_cheap_shader_program(int cheap_mode)
{
    static int built_mode = 0;
    if (built_mode == cheap_mode) return 1;

    const char *fs_to_compile;
    switch (cheap_mode) {
        case 1:
            fs_to_compile = k_fs_cheap;
            fprintf(stderr, "[d3d7] LONGROAD2_CHEAP_SHADER=1: using the trivial diagnostic fragment "
                            "shader -- output will be visibly WRONG (flat vertex color only), this is "
                            "for A/B timing comparisons only\n");
            break;
        case 3:
            fs_to_compile = k_fs_texonly_gated;
            fprintf(stderr, "[d3d7] LONGROAD2_CHEAP_SHADER=3: using the branch-gated texture-only "
                            "diagnostic fragment shader (real per-draw hasTexture uniform through the "
                            "same branch shape production uses, no combiner/colorkey/clip/alpha-test/"
                            "fog) -- output will be visibly WRONG, this is for A/B timing comparisons "
                            "only\n");
            break;
        case 4:
            fs_to_compile = k_fs_texonly_colorkey;
            fprintf(stderr, "[d3d7] LONGROAD2_CHEAP_SHADER=4: using the branch-gated texture+colorkey "
                            "diagnostic fragment shader (real per-draw hasTexture/hasColorKey uniforms, "
                            "same discard block production uses, no combiner/clip/alpha-test/fog) -- "
                            "output will be visibly WRONG, this is for A/B timing comparisons only\n");
            break;
        case 2:
        default:
            fs_to_compile = k_fs_texonly;
            fprintf(stderr, "[d3d7] LONGROAD2_CHEAP_SHADER=2: using the texture-only diagnostic "
                            "fragment shader (real stage-0 texture fetch, no combiner/clip/alpha-test/"
                            "fog) -- output will be visibly WRONG, this is for A/B timing comparisons "
                            "only\n");
            break;
    }

    GLuint prog = compile_link_and_locate(k_vs_cheap, fs_to_compile);
    if (prog == 0) return 0;

    g_prog = prog;
    built_mode = cheap_mode;
    return 1;
}

int ensure_shader_program(d3d_device7_t *dev, int is_rhw)
{
    int cheap_mode = d3d7_cheap_shader_mode();
    if (cheap_mode != 0) return ensure_cheap_shader_program(cheap_mode);
    return ensure_shader_variant(dev, is_rhw);
}
