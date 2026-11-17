// The host FGPU device: SYS_GPU for the PC, so that the SDK's farcadegl.h runs
// unmodified on the host build (engine/gfx/host/farcadegl_host.h routes
// mos_syscall1(SYS_GPU, ...) here).
//
// This is the kernel's command processor (Minimal-OS src/modules/GPU/fgpu.c)
// without the kernel: the same operations, limits, result codes and command
// semantics, drawing with the vendored CPU backend (external/fgpu: the
// rasterizer and the shader machine the kernel runs). Differences, all
// because there is one program and no screen:
//   - single threaded, one owner: no locks, no quotas per process (the
//     kernel's per-program quotas are kept), no asynchronous submits
//     (SYS_GPU_WAIT succeeds at once, nothing is ever queued),
//   - surfaces and buffers live in plain memory (never VRAM),
//   - SYS_GPU_PRESENT validates like the kernel and then does nothing (there
//     is no screen or window to show it on; the frame stays in the surface
//     and READBACK returns it),
//   - no hardware backend; shaders run on the CPU shader machine with the x86-64
//     JIT like the kernel's (fe_fgpu_host_set_jit(0) selects the interpreter).
#include "fgpu_host.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "x86_64/fgpu_raster.h"
#include "x86_64/fgpu_shader.h"
#include "x86_64/fgpu_jit_x64.h"    // the shader JIT, as in the kernel (mk/gfx.mk builds it)

// Result codes of the SDK's syscall.h (its "syscall.h" is shadowed by glibc's
// on the C include path, and only these few are needed).
#define SYS_SUCCESS 0
#define SYS_ERR_NOTFOUND ((uint64_t)-3)
#define SYS_ERR_INVAL    ((uint64_t)-4)
#define SYS_ERR_NOMEM    ((uint64_t)-8)

/* ------------------------------------------------------ object tables
 *
 * One table per object type. A handle is (generation << 20) | (type << 18) |
 * (slot + 1): looking one up is O(1) and a stale handle (a freed and reused
 * slot) does not match, exactly as in the kernel.
 */
enum { OBJ_SURFACE = 1, OBJ_BUFFER = 2, OBJ_PROGRAM = 3, OBJ_TYPES = 4 };

#define FGPU_TABLE_MAX (1u << 16)
static const uint32_t k_quota[OBJ_TYPES] = { 0, 1024, 4096, 1024 };

typedef struct {
    void* obj;
    uint16_t gen;
} slot_t;

typedef struct {
    slot_t* slots;
    uint32_t cap, used, hint;
} table_t;

typedef struct {
    uint32_t id;
    uint32_t width, height, format, flags;
    uint32_t pitch;             // in pixels / floats
    uint32_t mip_count;         // levels of a FGPU_SURFACE_MIPMAPS chain, else 1
    void* mem;
} surface_t;

typedef struct {
    uint32_t id;
    uint64_t size;
    void* mem;
} buffer_t;

typedef struct {
    uint32_t id;
    uint32_t* blob;             // `program` points into it
    fgpu_program_t program;
} program_t;

static table_t g_tables[OBJ_TYPES];
static fgpu_pass_t* g_pass;
static uint32_t g_submits, g_presents;
static uint64_t g_max_command_bytes;

static uint32_t table_insert(uint32_t type, void* obj) {
    table_t* t = &g_tables[type];
    for (;;) {
        if (t->used < t->cap) {
            for (uint32_t n = 0; n < t->cap; n++) {
                uint32_t i = (t->hint + n) % t->cap;
                slot_t* sl = &t->slots[i];
                if (sl->obj) continue;
                sl->obj = obj;
                sl->gen = (uint16_t)((sl->gen + 1) & 0x7FF);
                if (!sl->gen) sl->gen = 1;
                t->used++;
                t->hint = i + 1;
                return ((uint32_t)sl->gen << 20) | (type << 18) | (i + 1);
            }
        }
        if (t->cap >= FGPU_TABLE_MAX) return 0;
        uint32_t ncap = t->cap ? t->cap * 2 : 64;
        slot_t* ns = (slot_t*)calloc(ncap, sizeof(*ns));
        if (!ns) return 0;
        if (t->cap) memcpy(ns, t->slots, (size_t)t->cap * sizeof(*ns));
        free(t->slots);
        t->slots = ns;
        t->cap = ncap;
    }
}

static bool handle_split(uint32_t type, uint32_t h, uint32_t* slot, uint16_t* gen) {
    if (!h || ((h >> 18) & 3u) != type || !(h & 0x3FFFFu)) return false;
    *slot = (h & 0x3FFFFu) - 1;
    *gen = (uint16_t)(h >> 20);
    return true;
}

static void* table_get(uint32_t type, uint32_t h) {
    uint32_t i;
    uint16_t gen;
    if (!handle_split(type, h, &i, &gen)) return NULL;
    table_t* t = &g_tables[type];
    if (i < t->cap && t->slots[i].obj && t->slots[i].gen == gen) return t->slots[i].obj;
    return NULL;
}

static void table_remove(uint32_t type, uint32_t h) {
    uint32_t i;
    uint16_t gen;
    if (!handle_split(type, h, &i, &gen)) return;
    table_t* t = &g_tables[type];
    if (i < t->cap && t->slots[i].obj && t->slots[i].gen == gen) {
        t->slots[i].obj = NULL;
        t->used--;
    }
}

static surface_t* find_surface(uint32_t id) { return (surface_t*)table_get(OBJ_SURFACE, id); }
static buffer_t* find_buffer(uint32_t id) { return (buffer_t*)table_get(OBJ_BUFFER, id); }
static program_t* find_program(uint32_t id) { return (program_t*)table_get(OBJ_PROGRAM, id); }

static void surface_free(surface_t* s) { free(s->mem); free(s); }
static void buffer_free(buffer_t* b) { free(b->mem); free(b); }
static void program_free(program_t* p) {
    fgpu_program_release(&p->program);      // the JIT code
    free(p->blob);
    free(p);
}

/* ------------------------------------------------------------- passes
 *
 * CPU draws are recorded in a pass and rendered at flush: at the end of every
 * SUBMIT and at the start of every other operation (the kernel's flush
 * points), so nothing is ever pending between calls.
 */
static uint32_t pass_flush(void) {
    return g_pass ? fgpu_pass_flush(g_pass) : 0;
}

/* ------------------------------------------------------------ commands */

// State of one SYS_GPU_SUBMIT; every command buffer starts from the defaults.
typedef struct {
    surface_t* color;
    surface_t* depth;
    fgpu_pipeline_t pipe;
    fgpu_layout_t layout;
    float constants[FGPU_SHADER_MAX_UNIFORMS][4];
    surface_t* textures[FGPU_TEXTURE_UNITS];
    uint32_t triangles;
} exec_state_t;

static bool make_target(exec_state_t* st, fgpu_target_t* t) {
    memset(t, 0, sizeof(*t));
    if (!st->color && !st->depth) return false;
    uint32_t w = st->color ? st->color->width : st->depth->width;
    uint32_t h = st->color ? st->color->height : st->depth->height;
    if (st->depth && st->color &&
        (st->depth->width < w || st->depth->height < h)) return false;
    t->width = w;
    t->height = h;
    if (st->color) { t->color = (uint32_t*)st->color->mem; t->color_pitch = st->color->pitch; }
    if (st->depth) { t->depth = (float*)st->depth->mem; t->depth_pitch = st->depth->pitch; }
    return true;
}

static uint64_t execute(exec_state_t* st, const uint32_t* cmd, uint32_t words) {
    uint32_t pos = 0;
    while (pos < words) {
        uint32_t header = cmd[pos];
        uint32_t op = header >> 16;
        uint32_t len = header & 0xFFFF;
        if (len == 0 || pos + len > words) return SYS_ERR_INVAL;
        const uint32_t* arg = cmd + pos + 1;
        uint32_t n = len - 1;
        switch (op) {
            case FGPU_OP_BIND_TARGET: {
                if (n < 2) return SYS_ERR_INVAL;
                surface_t* c = arg[0] ? find_surface(arg[0]) : NULL;
                surface_t* d = arg[1] ? find_surface(arg[1]) : NULL;
                if ((arg[0] && (!c || c->format != FGPU_FORMAT_XRGB8888)) ||
                    (arg[1] && (!d || d->format != FGPU_FORMAT_DEPTH32F)))
                    return SYS_ERR_NOTFOUND;
                st->color = c;
                st->depth = d;
                surface_t* any = c ? c : d;
                if (any) {
                    st->pipe.vp_x = 0; st->pipe.vp_y = 0;
                    st->pipe.vp_w = (int32_t)any->width;
                    st->pipe.vp_h = (int32_t)any->height;
                }
                break;
            }
            case FGPU_OP_VIEWPORT:
                if (n < 4) return SYS_ERR_INVAL;
                st->pipe.vp_x = (int32_t)arg[0];
                st->pipe.vp_y = (int32_t)arg[1];
                st->pipe.vp_w = (int32_t)arg[2];
                st->pipe.vp_h = (int32_t)arg[3];
                if (st->pipe.vp_w < 0 || st->pipe.vp_h < 0 ||
                    st->pipe.vp_w > 8192 || st->pipe.vp_h > 8192) return SYS_ERR_INVAL;
                break;
            case FGPU_OP_CLEAR: {
                if (n < 3) return SYS_ERR_INVAL;
                fgpu_target_t t;
                if (!make_target(st, &t)) return SYS_ERR_INVAL;
                float depth;
                memcpy(&depth, &arg[2], sizeof(depth));
                if (st->pipe.scissor) {
                    if (g_pass) fgpu_pass_clear_rect(g_pass, &t, arg[0], arg[1], depth, st->pipe.sc_x,
                                                     st->pipe.sc_y, st->pipe.sc_w, st->pipe.sc_h);
                    else fgpu_raster_clear_rect(&t, arg[0], arg[1], depth, st->pipe.sc_x, st->pipe.sc_y,
                                                st->pipe.sc_w, st->pipe.sc_h);
                    break;
                }
                if (g_pass) fgpu_pass_clear(g_pass, &t, arg[0], arg[1], depth);
                else fgpu_raster_clear(&t, arg[0], arg[1], depth);
                break;
            }
            case FGPU_OP_SET_STATE:
                if (n < 1) return SYS_ERR_INVAL;
                st->pipe.state = arg[0];
                break;
            case FGPU_OP_SET_PROGRAM:
                if (n < 1 || arg[0] < FGPU_PROGRAM_COLOR || arg[0] > FGPU_PROGRAM_PHONG)
                    return SYS_ERR_INVAL;
                st->pipe.program = arg[0];
                break;
            case FGPU_OP_SET_UNIFORMS:
                if (n * 4 < sizeof(fgpu_uniforms_t)) return SYS_ERR_INVAL;
                memcpy(&st->pipe.u, arg, sizeof(fgpu_uniforms_t));
                break;
            case FGPU_OP_SET_SHADER: {
                if (n < 1) return SYS_ERR_INVAL;
                if (!arg[0]) { st->pipe.shader = NULL; break; }
                program_t* sp = find_program(arg[0]);
                if (!sp) return SYS_ERR_NOTFOUND;
                st->pipe.shader = &sp->program;
                break;
            }
            case FGPU_OP_SET_CONSTANTS: {
                if (n < 2) return SYS_ERR_INVAL;
                uint32_t first = arg[0], count = arg[1];
                if (first > FGPU_SHADER_MAX_UNIFORMS || count > FGPU_SHADER_MAX_UNIFORMS - first ||
                    n - 2 < count * 4) return SYS_ERR_INVAL;
                memcpy(st->constants[first], arg + 2, (size_t)count * 16);
                break;
            }
            case FGPU_OP_SET_VERTEX_LAYOUT: {
                if (n < 2 || arg[1] > FGPU_MAX_ATTRIBS || n - 2 < arg[1]) return SYS_ERR_INVAL;
                fgpu_layout_t l;
                memset(&l, 0, sizeof(l));
                l.stride = arg[0];
                l.count = arg[1];
                for (uint32_t i = 0; i < l.count; i++) l.attrib[i] = arg[2 + i];
                if (!fgpu_layout_valid(&l)) return SYS_ERR_INVAL;
                st->layout = l;
                break;
            }
            case FGPU_OP_BIND_TEXTURE: {
                if (n < 3 || arg[0] >= FGPU_TEXTURE_UNITS) return SYS_ERR_INVAL;
                fgpu_texture_t* tex = &st->pipe.textures[arg[0]];
                memset(tex, 0, sizeof(*tex));
                st->textures[arg[0]] = NULL;
                if (!arg[1]) break;
                surface_t* s = find_surface(arg[1]);
                if (!s || s->format != FGPU_FORMAT_XRGB8888) return SYS_ERR_NOTFOUND;
                st->textures[arg[0]] = s;
                tex->pixels = (const uint32_t*)s->mem;
                tex->width = s->width;
                tex->height = s->height;
                tex->pitch = s->pitch;
                tex->flags = arg[2] & FGPU_TEXTURE_FLAGS_KNOWN;
                if (s->mip_count > 1) {
                    tex->mip_count = fgpu_mip_layout(s->width, s->height, s->pitch, tex->mip_offset, NULL);
                } else {
                    tex->flags &= ~(uint32_t)(FGPU_TEXTURE_MIP_NEAREST | FGPU_TEXTURE_MIP_LINEAR);
                }
                // Optional ABI 4 words: lod bias, min lod, max lod (floats).
                float extra[3] = { 0.0f, 0.0f, 0.0f };
                for (uint32_t i = 0; i < 3 && 3 + i < n; i++) memcpy(&extra[i], &arg[3 + i], sizeof(float));
                if (!(extra[0] > -64.0f && extra[0] < 64.0f)) extra[0] = 0.0f;
                if (!(extra[1] > 0.0f && extra[1] < 64.0f)) extra[1] = 0.0f;
                if (!(extra[2] > 0.0f && extra[2] < 64.0f)) extra[2] = 0.0f;
                tex->lod_bias = extra[0];
                tex->min_lod = extra[1];
                tex->max_lod = extra[2];
                break;
            }
            case FGPU_OP_SCISSOR: {
                if (n < 5) return SYS_ERR_INVAL;
                int32_t sx = (int32_t)arg[1], sy = (int32_t)arg[2], sw = (int32_t)arg[3], sh = (int32_t)arg[4];
                if (sw < 0 || sh < 0) return SYS_ERR_INVAL;
                st->pipe.scissor = arg[0] ? 1u : 0u;
                st->pipe.sc_x = sx;
                st->pipe.sc_y = sy;
                st->pipe.sc_w = sw;
                st->pipe.sc_h = sh;
                break;
            }
            case FGPU_OP_GENERATE_MIPMAPS: {
                if (n < 1) return SYS_ERR_INVAL;
                surface_t* s = find_surface(arg[0]);
                if (!s || s->format != FGPU_FORMAT_XRGB8888) return SYS_ERR_NOTFOUND;
                if (s->mip_count <= 1) return SYS_ERR_INVAL;
                st->triangles += pass_flush();      // draws recorded so far (maybe into it) land first
                fgpu_mip_generate((uint32_t*)s->mem, s->width, s->height, s->pitch);
                break;
            }
            case FGPU_OP_BIND_STORAGE_BUFFER: {
                if (n < 2 || arg[0] >= FGPU_MAX_STORAGE_BUFFERS) return SYS_ERR_INVAL;
                fgpu_storage_buffer_t* buffer = &st->pipe.storage_buffers[arg[0]];
                memset(buffer, 0, sizeof(*buffer));
                if (!arg[1]) break;
                buffer_t* b = find_buffer(arg[1]);
                if (!b) return SYS_ERR_NOTFOUND;
                if ((b->size & 15u) || b->size / 16u > UINT32_MAX) return SYS_ERR_INVAL;
                buffer->data = (const float (*)[4])b->mem;
                buffer->vec4_count = (uint32_t)(b->size / 16u);
                break;
            }
            case FGPU_OP_DRAW: {
                if (n * 4 < sizeof(fgpu_draw_t)) return SYS_ERR_INVAL;
                fgpu_draw_t d;
                memcpy(&d, arg, sizeof(d));
                fgpu_layout_t def;
                const fgpu_layout_t* layout;
                if (d.vertex_format == FGPU_VERTEX_P3N3C4) {
                    fgpu_layout_default(&def);
                    layout = &def;
                } else if (d.vertex_format == FGPU_VERTEX_LAYOUT) {
                    layout = &st->layout;
                } else {
                    return SYS_ERR_INVAL;
                }
                buffer_t* vb = find_buffer(d.vertex_buffer);
                if (!vb) return SYS_ERR_NOTFOUND;
                uint64_t stride = (uint64_t)layout->stride * sizeof(float);
                uint64_t total = vb->size / stride;
                if (d.first_vertex > total) return SYS_ERR_INVAL;
                const float* verts = (const float*)vb->mem + (uint64_t)d.first_vertex * layout->stride;
                uint32_t avail = (uint32_t)(total - d.first_vertex);
                fgpu_target_t t;
                if (!make_target(st, &t)) return SYS_ERR_INVAL;
                if (!st->pipe.program) st->pipe.program = FGPU_PROGRAM_COLOR;
                buffer_t* ib = NULL;
                if (d.index_buffer) {
                    ib = find_buffer(d.index_buffer);
                    if (!ib) return SYS_ERR_NOTFOUND;
                    uint64_t icount = ib->size / 4;
                    if (d.first_index > icount || d.index_count > icount - d.first_index)
                        return SYS_ERR_INVAL;
                }
                const uint32_t* idx = ib ? (const uint32_t*)ib->mem + d.first_index : NULL;
                uint32_t count = ib ? d.index_count : (d.vertex_count < avail ? d.vertex_count : avail);
                // Pipeline state, uniforms and constants are snapshotted by the
                // pass; buffers, textures and programs stay alive until the flush.
                if (g_pass) fgpu_pass_draw_layout(g_pass, &t, &st->pipe, layout, verts, avail, idx, count, 0);
                else st->triangles += fgpu_raster_draw_layout(&t, &st->pipe, layout, verts, avail, idx, count);
                break;
            }
            default:
                return SYS_ERR_INVAL;
        }
        pos += len;
    }
    return SYS_SUCCESS;
}

static uint64_t run_commands(const uint32_t* cmd, uint32_t words, uint32_t* triangles) {
    *triangles = 0;
    exec_state_t* st = (exec_state_t*)calloc(1, sizeof(exec_state_t));
    if (!st) return SYS_ERR_NOMEM;
    st->pipe.program = FGPU_PROGRAM_COLOR;
    st->pipe.constants = (const float (*)[4])st->constants;
    st->pipe.sc_w = st->pipe.sc_h = 0x7FFFFFFF;     // scissor off; the rectangle covers any target
    fgpu_layout_default(&st->layout);
    uint64_t result = execute(st, cmd, words);
    st->triangles += pass_flush();      // also when the buffer failed half way
    *triangles = st->triangles;
    free(st);
    return result;
}

/* ------------------------------------------------------------- syscalls */

static uint64_t op_info(syscall_gpu_request_t* r) {
    syscall_gpu_info_t* out = (syscall_gpu_info_t*)r->out;
    // Programs built before ABI 4 pass the size without the limits block.
    if (!out || (out->size != sizeof(*out) && out->size != SYS_GPU_INFO_SIZE_BASE)) return SYS_ERR_INVAL;
    uint32_t want = out->size;
    syscall_gpu_info_t info;
    memset(&info, 0, sizeof(info));
    info.size = want;
    info.version = SYS_GPU_ABI_VERSION;
    strcpy(info.backend, "cpu (FGPU software, host)");
    strcpy(info.name, "Farcade Engine host GPU");
    strcpy(info.driver, "fgpu-host");
    // No async submits and no screen: shaders and the ABI 4 CPU-backend
    // features (scissor, mipmaps, 16 texture units) are offered.
    info.caps = FGPU_CAP_SHADERS | FGPU_CAP_SCISSOR | FGPU_CAP_MIPMAPS | FGPU_CAP_TEXTURE_UNITS16;
    info.max_texture_units = FGPU_TEXTURE_UNITS;
    info.max_mip_levels = FGPU_MAX_MIP_LEVELS;
    info.max_varyings = FGPU_SHADER_MAX_VARYINGS;
    info.max_uniform_vec4 = FGPU_SHADER_MAX_UNIFORMS;
    info.max_storage_buffers = FGPU_MAX_STORAGE_BUFFERS;
    info.max_surface_size = FGPU_MAX_SURFACE_SIZE;
    info.max_buffer_bytes = FGPU_MAX_BUFFER_BYTES;
    memcpy(out, &info, want);
    return SYS_SUCCESS;
}

static uint64_t op_create_surface(syscall_gpu_request_t* r) {
    if (!r->width || !r->height || r->width > FGPU_MAX_SURFACE_SIZE ||
        r->height > FGPU_MAX_SURFACE_SIZE ||
        (r->format != FGPU_FORMAT_XRGB8888 && r->format != FGPU_FORMAT_DEPTH32F))
        return SYS_ERR_INVAL;
    // A scanout surface has to match the screen: there is none here.
    if (r->flags & FGPU_SURFACE_SCANOUT) return SYS_ERR_INVAL;
    // ABI 4: a mip chain after level 0 (color surfaces only).
    uint64_t texels = (uint64_t)r->width * r->height;
    uint32_t mip_count = 1;
    if (r->version >= 4 && (r->flags & FGPU_SURFACE_MIPMAPS)) {
        if (r->format != FGPU_FORMAT_XRGB8888) return SYS_ERR_INVAL;
        mip_count = fgpu_mip_layout(r->width, r->height, r->width, NULL, &texels);
    }
    surface_t* s = (surface_t*)calloc(1, sizeof(*s));
    if (!s) return SYS_ERR_NOMEM;
    s->mem = calloc((size_t)texels, 4);
    if (!s->mem) { free(s); return SYS_ERR_NOMEM; }
    s->mip_count = mip_count;
    s->width = r->width;
    s->height = r->height;
    s->format = r->format;
    s->flags = r->flags;
    s->pitch = r->width;
    s->id = table_insert(OBJ_SURFACE, s);
    if (!s->id) { surface_free(s); return SYS_ERR_NOMEM; }
    r->handle = s->id;
    return SYS_SUCCESS;
}

static uint64_t op_create_buffer(syscall_gpu_request_t* r) {
    if (!r->bytes || r->bytes > FGPU_MAX_BUFFER_BYTES) return SYS_ERR_INVAL;
    buffer_t* b = (buffer_t*)calloc(1, sizeof(*b));
    if (!b) return SYS_ERR_NOMEM;
    b->mem = calloc(1, (size_t)r->bytes);
    if (!b->mem) { free(b); return SYS_ERR_NOMEM; }
    b->size = r->bytes;
    b->id = table_insert(OBJ_BUFFER, b);
    if (!b->id) { buffer_free(b); return SYS_ERR_NOMEM; }
    r->handle = b->id;
    return SYS_SUCCESS;
}

static uint64_t op_create_program(syscall_gpu_request_t* r) {
    if (!r->data || r->bytes < sizeof(fgpu_shader_header_t) || r->bytes > FGPU_MAX_PROGRAM_BYTES ||
        (r->bytes & 3))
        return SYS_ERR_INVAL;
    // Check our own copy, so the program cannot change afterwards.
    uint32_t* blob = (uint32_t*)malloc((size_t)r->bytes);
    if (!blob) return SYS_ERR_NOMEM;
    memcpy(blob, r->data, (size_t)r->bytes);
    program_t* sp = (program_t*)calloc(1, sizeof(*sp));
    if (!sp) { free(blob); return SYS_ERR_NOMEM; }
    const char* why = "";
    if (!fgpu_program_check(blob, (uint32_t)r->bytes, &sp->program, &why)) {
        free(blob);
        free(sp);
        return SYS_ERR_INVAL;
    }
    fgpu_program_jit(&sp->program);         // compile now; falls back to the interpreter silently
    sp->blob = blob;
    sp->id = table_insert(OBJ_PROGRAM, sp);
    if (!sp->id) { program_free(sp); return SYS_ERR_NOMEM; }
    r->handle = sp->id;
    return SYS_SUCCESS;
}

static uint64_t op_write_surface(syscall_gpu_request_t* r) {
    surface_t* s = find_surface(r->handle);
    if (!s) return SYS_ERR_NOTFOUND;
    // ABI 4: `offset` is the mip level (0 = the surface itself).
    uint32_t level = 0;
    if (r->version >= 4) {
        if (r->offset >= s->mip_count) return SYS_ERR_INVAL;
        level = (uint32_t)r->offset;
    }
    uint32_t width = s->width, height = s->height, pitch = s->pitch;
    uint8_t* dst = (uint8_t*)s->mem;
    if (level) {
        uint32_t offsets[FGPU_MAX_MIP_LEVELS];
        fgpu_mip_layout(s->width, s->height, s->pitch, offsets, NULL);
        width = s->width >> level; if (!width) width = 1;
        height = s->height >> level; if (!height) height = 1;
        pitch = width;
        dst += (size_t)offsets[level] * 4;
    }
    uint64_t row = (uint64_t)width * 4;
    if (!r->data || r->pitch < row) return SYS_ERR_INVAL;
    for (uint32_t y = 0; y < height; y++) {
        memcpy(dst + (size_t)y * pitch * 4, (const uint8_t*)r->data + (size_t)y * r->pitch, (size_t)row);
    }
    return SYS_SUCCESS;
}

// The kernel's checks of a present; then nothing to show it on.
static uint64_t op_present(syscall_gpu_request_t* r) {
    surface_t* s = find_surface(r->handle);
    if (!s || s->format != FGPU_FORMAT_XRGB8888) return SYS_ERR_NOTFOUND;
    if (r->version < 3) r->flags &= FGPU_PRESENT_FLIP;
    const uint32_t scale_bits = r->flags & (FGPU_PRESENT_SCALE2 | FGPU_PRESENT_SCALE3);
    if ((r->flags & ~FGPU_PRESENT_KNOWN) ||
        scale_bits == (FGPU_PRESENT_SCALE2 | FGPU_PRESENT_SCALE3) ||
        (scale_bits && (r->window_id || (r->flags & FGPU_PRESENT_FLIP))))
        return SYS_ERR_INVAL;
    if ((r->flags & FGPU_PRESENT_PACED) && r->out) *(uint64_t*)r->out = 0;
    g_presents++;
    return SYS_SUCCESS;
}

uint64_t fe_fgpu_host_syscall(syscall_gpu_request_t* r) {
    if (!r || r->size != sizeof(*r) ||
        r->version < SYS_GPU_ABI_VERSION_MIN || r->version > SYS_GPU_ABI_VERSION)
        return SYS_ERR_INVAL;
    if (r->op == SYS_GPU_INFO) return op_info(r);
    if (!g_pass) g_pass = fgpu_pass_create();     // NULL: draws run immediately

    // Everything but SUBMIT reads, writes or frees what pending draws point
    // at: nothing is pending between calls, this keeps that true.
    if (r->op != SYS_GPU_SUBMIT) pass_flush();

    uint64_t result = SYS_ERR_INVAL;
    switch (r->op) {
        case SYS_GPU_CREATE_SURFACE:
            if (g_tables[OBJ_SURFACE].used >= k_quota[OBJ_SURFACE]) { result = SYS_ERR_NOMEM; break; }
            result = op_create_surface(r);
            break;
        case SYS_GPU_DESTROY_SURFACE: {
            surface_t* s = find_surface(r->handle);
            if (!s) { result = SYS_ERR_NOTFOUND; break; }
            table_remove(OBJ_SURFACE, s->id);
            surface_free(s);
            result = SYS_SUCCESS;
            break;
        }
        case SYS_GPU_CREATE_BUFFER:
            if (g_tables[OBJ_BUFFER].used >= k_quota[OBJ_BUFFER]) { result = SYS_ERR_NOMEM; break; }
            result = op_create_buffer(r);
            break;
        case SYS_GPU_UPLOAD_BUFFER: {
            buffer_t* b = find_buffer(r->handle);
            if (!b) { result = SYS_ERR_NOTFOUND; break; }
            if (!r->data || r->offset > b->size || r->bytes > b->size - r->offset) break;
            memcpy((uint8_t*)b->mem + r->offset, r->data, (size_t)r->bytes);
            result = SYS_SUCCESS;
            break;
        }
        case SYS_GPU_DESTROY_BUFFER: {
            buffer_t* b = find_buffer(r->handle);
            if (!b) { result = SYS_ERR_NOTFOUND; break; }
            table_remove(OBJ_BUFFER, b->id);
            buffer_free(b);
            result = SYS_SUCCESS;
            break;
        }
        case SYS_GPU_SUBMIT: {
            // FGPU_SUBMIT_ASYNC is not offered (FGPU_CAP_ASYNC is not set):
            // every submit runs at once and `handle` gets the triangle count.
            if (!r->data || r->bytes < 4 || r->bytes > FGPU_MAX_COMMAND_BYTES || (r->bytes & 3)) break;
            uint32_t tris = 0;
            result = run_commands((const uint32_t*)r->data, (uint32_t)(r->bytes / 4), &tris);
            r->handle = tris;
            if (result == SYS_SUCCESS) {
                g_submits++;
                if (r->bytes > g_max_command_bytes) g_max_command_bytes = r->bytes;
            }
            break;
        }
        case SYS_GPU_READBACK: {
            surface_t* s = find_surface(r->handle);
            if (!s) { result = SYS_ERR_NOTFOUND; break; }
            uint64_t row = (uint64_t)s->width * 4;
            if (!r->out || r->pitch < row) break;
            for (uint32_t y = 0; y < s->height; y++) {
                memcpy((uint8_t*)r->out + (size_t)y * r->pitch,
                       (const uint8_t*)s->mem + (size_t)y * s->pitch * 4, (size_t)row);
            }
            result = SYS_SUCCESS;
            break;
        }
        case SYS_GPU_PRESENT:
            result = op_present(r);
            break;
        case SYS_GPU_SET_PACE: {
            uint64_t ns = r->bytes ? r->bytes : 16666667ull;
            if (ns < FGPU_PACE_MIN_NS || ns > FGPU_PACE_MAX_NS) { result = SYS_ERR_INVAL; break; }
            result = SYS_SUCCESS;
            break;
        }
        case SYS_GPU_RELEASE_DISPLAY:
            result = SYS_SUCCESS;
            break;
        case SYS_GPU_CREATE_PROGRAM:
            if (g_tables[OBJ_PROGRAM].used >= k_quota[OBJ_PROGRAM]) { result = SYS_ERR_NOMEM; break; }
            result = op_create_program(r);
            break;
        case SYS_GPU_DESTROY_PROGRAM: {
            program_t* sp = find_program(r->handle);
            if (!sp) { result = SYS_ERR_NOTFOUND; break; }
            table_remove(OBJ_PROGRAM, sp->id);
            program_free(sp);
            result = SYS_SUCCESS;
            break;
        }
        case SYS_GPU_WRITE_SURFACE:
            result = op_write_surface(r);
            break;
        case SYS_GPU_WAIT:
            // Nothing is ever queued: every submit has finished.
            r->handle = 0;
            result = SYS_SUCCESS;
            break;
        default:
            break;
    }
    return result;
}

void fe_fgpu_host_set_jit(int enabled) {
    fgpu_jit_set_mode(enabled ? FGPU_JIT_AUTO : FGPU_JIT_OFF);
}

void fe_fgpu_host_reset(void) {
    pass_flush();
    for (uint32_t type = OBJ_SURFACE; type < OBJ_TYPES; type++) {
        table_t* t = &g_tables[type];
        for (uint32_t i = 0; i < t->cap; i++) {
            void* o = t->slots[i].obj;
            if (!o) continue;
            if (type == OBJ_SURFACE) surface_free((surface_t*)o);
            else if (type == OBJ_BUFFER) buffer_free((buffer_t*)o);
            else program_free((program_t*)o);
            t->slots[i].obj = NULL;
        }
        t->used = 0;
    }
    g_submits = g_presents = 0;
    g_max_command_bytes = 0;
}

void fe_fgpu_host_stats(fe_fgpu_host_stats_t* out) {
    out->surfaces = g_tables[OBJ_SURFACE].used;
    out->buffers = g_tables[OBJ_BUFFER].used;
    out->programs = g_tables[OBJ_PROGRAM].used;
    out->submits = g_submits;
    out->presents = g_presents;
    out->max_command_bytes = g_max_command_bytes;
}
