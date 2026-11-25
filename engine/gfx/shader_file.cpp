#include "gfx/shader_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe::gfx {
namespace {

enum class Stage { None, Vertex, Fragment, Hook };

bool marker(const char* line, size_t size, const char* name) {
    while (size && (*line == ' ' || *line == '\t')) { line++; size--; }
    const char* tag = "// @";
    size_t tag_size = 4;
    if (size < tag_size || memcmp(line, tag, tag_size) != 0) return false;
    line += tag_size;
    size -= tag_size;
    size_t name_size = strlen(name);
    if (size < name_size || memcmp(line, name, name_size) != 0) return false;
    line += name_size;
    size -= name_size;
    while (size && (*line == ' ' || *line == '\t' || *line == '\r')) { line++; size--; }
    return size == 0;
}

bool append(char* out, size_t cap, size_t* used, const char* text, size_t count) {
    if (*used >= cap || count >= cap - *used) return false;
    memcpy(out + *used, text, count);
    *used += count;
    out[*used] = 0;
    return true;
}

void fail(char* error, size_t cap, const char* message) {
    if (error && cap) snprintf(error, cap, "%s", message);
}

}  // namespace

bool parse_shader_file(const char* source, size_t size,
                       char* vertex, size_t vertex_cap,
                       char* fragment, size_t fragment_cap,
                       char* error, size_t error_cap) {
    if (error && error_cap) error[0] = 0;
    if (vertex && vertex_cap) vertex[0] = 0;
    if (fragment && fragment_cap) fragment[0] = 0;
    if (!source || !vertex || !vertex_cap || !fragment || !fragment_cap) {
        fail(error, error_cap, "invalid shader output buffers");
        return false;
    }

    Stage stage = Stage::None;
    bool saw_vertex = false, saw_fragment = false;
    size_t vertex_size = 0, fragment_size = 0;
    for (size_t pos = 0; pos < size;) {
        size_t end = pos;
        while (end < size && source[end] != '\n') end++;
        size_t line_size = end - pos;
        if (line_size && source[pos + line_size - 1] == '\r') line_size--;
        if (marker(source + pos, line_size, "displace") || marker(source + pos, line_size, "surface")) {
            stage = Stage::Hook;                // the renderer's hooks (parse_shader_sections)
        } else if (marker(source + pos, line_size, "vertex")) {
            if (saw_vertex) { fail(error, error_cap, "duplicate vertex section"); return false; }
            saw_vertex = true;
            stage = Stage::Vertex;
        } else if (marker(source + pos, line_size, "fragment")) {
            if (saw_fragment) { fail(error, error_cap, "duplicate fragment section"); return false; }
            saw_fragment = true;
            stage = Stage::Fragment;
        } else if (stage == Stage::Vertex) {
            if (!append(vertex, vertex_cap, &vertex_size, source + pos, end - pos + (end < size ? 1 : 0))) {
                fail(error, error_cap, "vertex section is too large");
                return false;
            }
        } else if (stage == Stage::Fragment) {
            if (!append(fragment, fragment_cap, &fragment_size, source + pos, end - pos + (end < size ? 1 : 0))) {
                fail(error, error_cap, "fragment section is too large");
                return false;
            }
        } else if (stage == Stage::Hook) {
            // skipped
        } else if (line_size) {
            fail(error, error_cap, "shader source must start with // @vertex");
            return false;
        }
        pos = end < size ? end + 1 : size;
    }
    if (!saw_vertex || !vertex_size) { fail(error, error_cap, "missing or empty vertex section"); return false; }
    if (!saw_fragment || !fragment_size) { fail(error, error_cap, "missing or empty fragment section"); return false; }
    return true;
}


// ---- sections

namespace {

char* dup_range(const char* s, size_t n) {
    char* d = (char*)malloc(n + 1);
    if (!d) return nullptr;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

uint64_t fnv64(uint64_t h, const char* s) {
    for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    return h;
}

}  // namespace

bool parse_shader_sections(const char* source, size_t size, ShaderSections* out, char* error, size_t error_cap) {
    *out = ShaderSections();
    if (error && error_cap) error[0] = 0;
    if (!source) { fail(error, error_cap, "no source"); return false; }
    static const char* const names[4] = { "vertex", "fragment", "displace", "surface" };
    size_t start[4] = {}, stop[4] = {};
    bool seen[4] = {};
    int cur = -1;
    for (size_t pos = 0; pos < size;) {
        size_t end = pos;
        while (end < size && source[end] != '\n') end++;
        size_t line_size = end - pos;
        if (line_size && source[pos + line_size - 1] == '\r') line_size--;
        size_t next = end < size ? end + 1 : size;
        int m = -1;
        for (int i = 0; i < 4; i++) if (marker(source + pos, line_size, names[i])) m = i;
        if (m >= 0) {
            if (seen[m]) {
                char b[64];
                snprintf(b, sizeof b, "duplicate %s section", names[m]);
                fail(error, error_cap, b);
                return false;
            }
            if (cur >= 0) stop[cur] = pos;
            seen[m] = true;
            start[m] = next;
            cur = m;
        } else if (cur < 0 && line_size) {
            bool blank = true;
            for (size_t i = 0; i < line_size; i++) if (source[pos + i] != ' ' && source[pos + i] != '\t') blank = false;
            if (!blank) { fail(error, error_cap, "shader source must start with a // @vertex, @displace or @surface marker"); return false; }
        }
        pos = next;
    }
    if (cur >= 0) stop[cur] = size;
    if (seen[0] != seen[1]) { fail(error, error_cap, seen[0] ? "a // @vertex section needs a // @fragment section" : "a // @fragment section needs a // @vertex section"); return false; }
    if (!seen[0] && !seen[2] && !seen[3]) { fail(error, error_cap, "no // @vertex, @displace or @surface section"); return false; }
    char** dst[4] = { &out->vertex, &out->fragment, &out->displace, &out->surface };
    for (int i = 0; i < 4; i++) {
        if (!seen[i]) continue;
        *dst[i] = dup_range(source + start[i], stop[i] > start[i] ? stop[i] - start[i] : 0);
        if (!*dst[i]) { free_shader_sections(out); fail(error, error_cap, "out of memory"); return false; }
    }
    if (out->displace && !strstr(out->displace, "displace(")) { free_shader_sections(out); fail(error, error_cap, "the @displace section must define vec3 displace(vec3 p, vec3 n, vec2 uv)"); return false; }
    if (out->surface && !strstr(out->surface, "surface(")) { free_shader_sections(out); fail(error, error_cap, "the @surface section must define vec3 surface(vec3 albedo, vec2 uv)"); return false; }
    if (out->hooks()) {
        uint64_t h = fnv64(1469598103934665603ull, out->displace ? out->displace : "");
        h = fnv64(h ^ 0x5u, "|");
        h = fnv64(h, out->surface ? out->surface : "");
        out->hook_id = h | 1u;
    }
    return true;
}

void free_shader_sections(ShaderSections* s) {
    free(s->vertex); free(s->fragment); free(s->displace); free(s->surface);
    *s = ShaderSections();
}

// ---- splicing

namespace {

// A growable copy of a shader being rewritten, bounded by cap.
struct Buf {
    char* p;
    size_t cap, n = 0;
    bool ok = true;
    Buf(size_t c) : cap(c) { p = (char*)malloc(c); ok = p != nullptr; if (p) p[0] = 0; }
    ~Buf() { free(p); }
    void add(const char* s, size_t l) {
        if (!ok) return;
        if (n + l + 1 > cap) { ok = false; return; }
        memcpy(p + n, s, l);
        n += l;
        p[n] = 0;
    }
    void add(const char* s) { add(s, strlen(s)); }
};

// Whether `line` (without its newline, spaces trimmed) is one of the lines of `src`.
bool has_line(const char* src, const char* line, size_t n) {
    while (n && (*line == ' ' || *line == '\t')) { line++; n--; }
    while (n && (line[n - 1] == ' ' || line[n - 1] == '\t' || line[n - 1] == '\r')) n--;
    if (!n) return false;
    for (const char* p = src; *p;) {
        const char* e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        const char* a = p;
        size_t m = l;
        while (m && (*a == ' ' || *a == '\t')) { a++; m--; }
        while (m && (a[m - 1] == ' ' || a[m - 1] == '\t' || a[m - 1] == '\r')) m--;
        if (m == n && !memcmp(a, line, n)) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

// Appends a hook's text, leaving out the uniform / attribute / varying declarations `src` already has.
void add_hook(Buf& b, const char* src, const char* hook) {
    for (const char* p = hook; p && *p;) {
        const char* e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        const char* a = p;
        while (*a == ' ' || *a == '\t') a++;
        bool decl = !strncmp(a, "uniform ", 8) || !strncmp(a, "attribute ", 10) || !strncmp(a, "varying ", 8);
        if (!(decl && has_line(src, p, l))) { b.add(p, l); b.add("\n"); }
        if (!e) break;
        p = e + 1;
    }
}

// Copies `body` replacing every whole-word `from` with `to`.
void add_replaced(Buf& b, const char* body, const char* from, const char* to) {
    size_t fl = strlen(from);
    for (const char* p = body; *p;) {
        const char* q = strstr(p, from);
        if (!q) { b.add(p); break; }
        bool left = q == body || !(q[-1] == '_' || (q[-1] >= 'a' && q[-1] <= 'z') || (q[-1] >= 'A' && q[-1] <= 'Z') || (q[-1] >= '0' && q[-1] <= '9'));
        char r = q[fl];
        bool right = !(r == '_' || (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9'));
        b.add(p, (size_t)(q - p));
        b.add(left && right ? to : from);
        p = q + fl;
    }
}

bool finish(char* src, size_t cap, Buf& b) {
    if (!b.ok || b.n + 1 > cap) return false;
    memcpy(src, b.p, b.n + 1);
    return true;
}

const char* const kMainLine = "void main() {\n";

// Where `void main()` starts and where its body starts (after the `{` and the rest of that line's spaces and
// newline), or false.
bool find_main(char* src, char** decl, const char** body) {
    char* m = strstr(src, "void main()");
    if (!m) return false;
    const char* b = m + 11;
    while (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n') b++;
    if (*b != '{') return false;
    b++;
    while (*b == ' ' || *b == '\t' || *b == '\r') b++;
    if (*b == '\n') b++;
    *decl = m;
    *body = b;
    return true;
}

}  // namespace

bool hook_vertex(char* src, size_t cap, const char* displace, bool uv) {
    char* m = nullptr;
    const char* body = nullptr;
    if (!find_main(src, &m, &body)) return false;
    Buf b(cap);
    b.add(src, (size_t)(m - src));
    if (displace && !strstr(src, "attribute vec3 normal;")) b.add("attribute vec3 normal;\n");
    if ((displace || uv) && !strstr(src, "attribute vec2 texcoord;")) b.add("attribute vec2 texcoord;\n");
    if (uv && !strstr(src, "varying vec2 v_uv;")) b.add("varying vec2 v_uv;\n");
    if (displace) add_hook(b, src, displace);
    b.add(kMainLine);
    if (displace) b.add("    vec3 fe_p = displace(position, normal, texcoord);\n");
    if (uv && !strstr(body, "v_uv = texcoord;")) b.add("    v_uv = texcoord;\n");
    if (displace) {
        Buf tmp(cap);
        add_replaced(tmp, body, "vec4(position, 1.0)", "vec4(fe_p, 1.0)");
        if (!tmp.ok) return false;
        b.add(tmp.p, tmp.n);
    } else {
        b.add(body);
    }
    return finish(src, cap, b);
}

bool hook_fragment(char* src, size_t cap, const char* surface, bool tex) {
    char* m = nullptr;
    const char* body = nullptr;
    if (!find_main(src, &m, &body)) return false;
    static const char* const kBaseTex = "v_alb * srgb2lin(texture2D(u_texture, v_uv).rgb)";
    const bool base_tex = strstr(body, kBaseTex) != nullptr;       // the base tier's lit shader textures already
    Buf b(cap);
    b.add(src, (size_t)(m - src));
    if ((tex || base_tex) && !strstr(src, "uniform sampler2D u_texture;")) b.add("uniform sampler2D u_texture;\n");
    if ((tex || surface || base_tex) && !strstr(src, "varying vec2 v_uv;")) b.add("varying vec2 v_uv;\n");
    if (tex && !strstr(src, "vec3 srgb2lin(")) b.add("vec3 srgb2lin(vec3 c) {\n    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));\n}\n");
    if (surface) add_hook(b, src, surface);
    b.add(kMainLine);
    b.add("    vec3 fe_alb = v_alb");
    if (tex || base_tex) b.add(" * srgb2lin(texture2D(u_texture, v_uv).rgb)");
    b.add(";\n");
    if (surface) b.add("    fe_alb = surface(fe_alb, v_uv);\n");
    Buf tmp(cap);
    if (base_tex) {
        Buf t2(cap);
        add_replaced(t2, body, kBaseTex, "v_alb");
        if (!t2.ok) return false;
        add_replaced(tmp, t2.p, "v_alb", "fe_alb");
    } else {
        add_replaced(tmp, body, "v_alb", "fe_alb");
    }
    if (!tmp.ok) return false;
    b.add(tmp.p, tmp.n);
    return finish(src, cap, b);
}

bool hdr_wrap_fragment(char* src, size_t cap) {
    char* m = nullptr;
    const char* body = nullptr;
    if (!find_main(src, &m, &body)) return false;
    // main's closing brace: the brace that brings the depth back to zero.
    int depth = 1;
    const char* close = nullptr;
    for (const char* p = body; *p; p++) {
        if (*p == '{') depth++;
        else if (*p == '}' && --depth == 0) { close = p; break; }
    }
    if (!close) return false;
    Buf b(cap);
    b.add(src, (size_t)(m - src));
    b.add(kMainLine);
    b.add("    vec4 fe_out = vec4(0.0, 0.0, 0.0, 1.0);\n");
    char* inner = dup_range(body, (size_t)(close - body));
    if (!inner) return false;
    Buf tmp(cap);
    add_replaced(tmp, inner, "gl_FragColor", "fe_out");
    free(inner);
    if (!tmp.ok) return false;
    b.add(tmp.p, tmp.n);
    b.add("    vec3 fe_c = clamp(fe_out.rgb, 0.0, 1.0);\n"
          "    fe_c = fe_c * (fe_c * (fe_c * 0.305306011 + 0.682171111) + 0.012522878);\n"
          "    gl_FragColor = vec4(sqrt(clamp(fe_c, 0.0, 8.0) * 0.125), 1.0);\n");
    b.add(close);
    return finish(src, cap, b);
}

}  // namespace fe::gfx
