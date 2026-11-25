// .fshader files: a full shader (`// @vertex` and `// @fragment` sections) and/or hooks that the renderer
// splices into its own generated shaders (`// @displace` and `// @surface`), see docs/graphics.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fe::gfx {

// Reads a combined .fshader source with `// @vertex` and `// @fragment`
// section markers. The stage outputs are always NUL-terminated on success.
// Hook sections (`// @displace`, `// @surface`) are skipped.
bool parse_shader_file(const char* source, size_t size,
                       char* vertex, size_t vertex_cap,
                       char* fragment, size_t fragment_cap,
                       char* error, size_t error_cap);

// Every section of a .fshader. Each string is malloc'd (null when the section is absent); free with
// free_shader_sections. The hooks:
//   // @displace   declarations plus  vec3 displace(vec3 p, vec3 n, vec2 uv)  : moves a vertex (mesh space),
//                  run in every pass that draws the mesh (shadows, depth, normals, lighting) so they agree.
//                  The normal follows: an optional  vec3 displace_normal(vec3 p, vec3 n, vec2 uv)  returns it,
//                  else it is worked out from displace (finite differences along the surface)
//   // @surface    declarations plus  vec3 surface(vec3 albedo, vec2 uv)  : the linear albedo (material colour
//                  times the texture) in, the albedo to light out
// Either may use u_time (x seconds, y delta, z frame) and u_texture by declaring them.
struct ShaderSections {
    char* vertex = nullptr;
    char* fragment = nullptr;
    char* displace = nullptr;
    char* surface = nullptr;
    uint64_t hook_id = 0;           // hash of the displace and surface text (0 = no hooks)
    bool full() const { return vertex && fragment; }
    bool hooks() const { return displace || surface; }
};
// False (with a message) when the file has no usable section, a section twice, text before the first
// marker, or a full shader with only one of its two parts.
bool parse_shader_sections(const char* source, size_t size, ShaderSections* out, char* error, size_t error_cap);
void free_shader_sections(ShaderSections* s);

// Splicing into generated shaders (they must have a `void main() {` line). Both return false when the
// result does not fit in cap (the source is then unchanged).
//
// hook_vertex: declares what is missing (attributes normal and texcoord, the varying v_uv when `uv`), adds the
// displace hook's declarations (lines the shader already has are left out), computes
// `vec3 fe_p = displace(position, normal, texcoord);` first in main and uses it for every
// `vec4(position, 1.0)`; where the shader reads `vec4(normal, 0.0)` it reads the displaced normal fe_n
// (displace_normal, or finite differences of displace). `uv`: also writes v_uv = texcoord (for a texture
// or a surface hook).
bool hook_vertex(char* src, size_t cap, const char* displace, bool uv);
// hook_fragment: the albedo the shader reads from the varying v_alb becomes
// `fe_alb = surface(v_alb * texture, v_uv)` (the texture when `tex`, the call when `surface`). A shader that
// already multiplies v_alb by u_texture (the base tier's) keeps doing it once.
// `detail`: the albedo is also multiplied by a tiling detail texture (u_detail at v_uv * u_det.x, 0.5 grey is
// neutral, strength u_det.y), before the surface hook.
bool hook_fragment(char* src, size_t cap, const char* surface, bool tex, bool detail = false);
// Material features of a fragment shader: `cutout` discards where u_texture's alpha is below u_cut.x (leaf
// cards; the vertex shader needs v_uv: hook_vertex(..., uv = true)); `two_sided` turns the normal it reads
// towards the viewer (normalize(v_nrm) in shaders with u_cam and v_pos, the normal pass's view space
// normalize(v_n)), so back faces are lit from their side.
bool hook_material(char* src, size_t cap, bool cutout, bool two_sided);
// For a full custom fragment shader drawn into an HDR target: its gl_FragColor (a display colour) is kept
// in a local and written HDR encoded (sqrt(linear / 8)) at the end of main.
bool hdr_wrap_fragment(char* src, size_t cap);

}  // namespace fe::gfx
