#include "gfx/graphics_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe::gfx {
namespace {

typedef GraphicsSettings GS;

const char* const kPresets[] = { "low", "medium", "high", "ultra", "custom", nullptr };
const char* const kLevels[] = { "auto", "fl0", "fl1", "gpu", nullptr };
const char* const kShadowModes[] = { "off", "single", "cascades", nullptr };
const char* const kRes[] = { "512", "1024", "2048", "4096", nullptr };
const char* const kAo[] = { "off", "ssao", nullptr };
const char* const kAa[] = { "off", "fxaa", nullptr };
const char* const kBloom[] = { "off", "simple", "chain", nullptr };
const char* const kGi[] = { "hemisphere", "probes", nullptr };
const char* const kGiDensity[] = { "coarse", "normal", "fine", nullptr };
const char* const kGiUpdate[] = { "slow", "normal", "fast", nullptr };

const int kResValues[4] = { 512, 1024, 2048, 4096 };

int res_index(int v) {
    int best = 0;
    for (int i = 0; i < 4; i++) if (kResValues[i] <= v) best = i;
    return best;
}

// One property: the getters / setters work on an integer (Bool, Integer, Choice) or a float (Number).
struct Prop {
    GraphicsProperty info;
    int (*geti)(const GS&);
    void (*seti)(GS&, int);
    float (*getf)(const GS&);
    void (*setf)(GS&, float);
};

#define IP(NAME, KIND, MIN, MAX, CH, HELP, GET, SET) { { NAME, KIND, MIN, MAX, CH, HELP }, GET, SET, nullptr, nullptr }
#define FP(NAME, MIN, MAX, HELP, FIELD) { { NAME, GraphicsPropKind::Number, MIN, MAX, nullptr, HELP }, nullptr, nullptr, \
    [](const GS& s) { return s.FIELD; }, [](GS& s, float v) { s.FIELD = v; } }
#define BP(NAME, HELP, FIELD) IP(NAME, GraphicsPropKind::Bool, 0, 1, nullptr, HELP, \
    [](const GS& s) { return s.FIELD ? 1 : 0; }, [](GS& s, int v) { s.FIELD = v != 0; })

const Prop kProps[] = {
    IP("preset", GraphicsPropKind::Choice, 0, 0, kPresets, "Applies a set of the options below (custom: they were changed)",
       [](const GS& s) { return (int)s.preset; }, [](GS& s, int v) { if (v < 4) s.apply_preset((GraphicsPreset)v); else s.preset = GraphicsPreset::Custom; }),
    IP("feature_level", GraphicsPropKind::Choice, 0, 0, kLevels, "auto picks from the device; fl0 = base tier, fl1 = FL1-ready, gpu = GPU tier",
       [](const GS& s) { return (int)s.feature_level; }, [](GS& s, int v) { s.feature_level = (FeatureLevelMode)v; }),
    FP("render_scale", 0.25f, 1.0f, "Internal resolution, upscaled when shown", render_scale),
    BP("dynamic_resolution", "Move the render scale to hold target_frame_ms", dynamic_resolution),
    FP("target_frame_ms", 8.0f, 100.0f, "Frame time dynamic resolution aims for", target_frame_ms),
    FP("min_render_scale", 0.25f, 1.0f, "Lowest scale dynamic resolution may use", min_render_scale),
    IP("shadows", GraphicsPropKind::Choice, 0, 0, kShadowModes, "Sun shadows: off, one map, or cascades (GPU tier)",
       [](const GS& s) { return !s.shadows ? 0 : (s.shadow_cascades <= 1 ? 1 : 2); },
       [](GS& s, int v) { s.shadows = v != 0; if (v == 1) s.shadow_cascades = 1; else if (v == 2 && s.shadow_cascades < 2) s.shadow_cascades = 3; }),
    IP("shadow_cascades", GraphicsPropKind::Integer, 2, 3, nullptr, "Cascades when shadows = cascades",
       [](const GS& s) { return s.shadow_cascades < 2 ? 2 : s.shadow_cascades; }, [](GS& s, int v) { if (s.shadow_cascades >= 2) s.shadow_cascades = v; }),
    IP("shadow_resolution", GraphicsPropKind::Choice, 0, 0, kRes, "Texels of the sun shadow map (a side)",
       [](const GS& s) { return res_index(s.shadow_resolution); }, [](GS& s, int v) { s.shadow_resolution = kResValues[v]; }),
    FP("shadow_distance", 1.0f, 2000.0f, "Meters from the camera that get sun shadows", shadow_distance),
    BP("soft_shadows", "PCF filtering", soft_shadows),
    BP("cascade_cache", "Far cascades keep their fit for a few frames", cascade_cache),
    BP("local_shadows", "Point and spot light shadows", local_shadows),
    IP("local_shadow_lights", GraphicsPropKind::Integer, 1, 4, nullptr, "Lights with shadows at a time",
       [](const GS& s) { return s.local_shadow_lights; }, [](GS& s, int v) { s.local_shadow_lights = v; }),
    IP("local_shadow_resolution", GraphicsPropKind::Choice, 0, 0, kRes, "Texels of the point / spot shadow atlas (a side)",
       [](const GS& s) { return res_index(s.local_shadow_resolution); }, [](GS& s, int v) { s.local_shadow_resolution = kResValues[v]; }),
    IP("ao", GraphicsPropKind::Choice, 0, 0, kAo, "Ambient occlusion: off or screen space (SSAO)",
       [](const GS& s) { return s.ambient_occlusion ? 1 : 0; }, [](GS& s, int v) { s.ambient_occlusion = v != 0; }),
    FP("ao_radius", 0.05f, 5.0f, "AO radius in meters", ao_radius),
    FP("ao_strength", 0.0f, 2.0f, "AO strength", ao_strength),
    IP("aa", GraphicsPropKind::Choice, 0, 0, kAa, "Anti-aliasing: off or FXAA",
       [](const GS& s) { return s.antialiasing ? 1 : 0; }, [](GS& s, int v) { s.antialiasing = v != 0; }),
    IP("bloom", GraphicsPropKind::Choice, 0, 0, kBloom, "Bloom: off, the simple 4 level glow, or the 5 level chain",
       [](const GS& s) { return !s.bloom ? 0 : (s.bloom_mode == BloomMode::Simple ? 1 : 2); },
       [](GS& s, int v) { s.bloom = v != 0; if (v == 1) s.bloom_mode = BloomMode::Simple; else if (v == 2) s.bloom_mode = BloomMode::Chain; }),
    IP("gi", GraphicsPropKind::Choice, 0, 0, kGi, "Ambient light: the hemisphere colours, or probe GI (voxel bounce light, a few CPU ms)",
       [](const GS& s) { return (int)s.gi_mode; }, [](GS& s, int v) { s.gi_mode = (GiMode)v; }),
    IP("gi_density", GraphicsPropKind::Choice, 0, 0, kGiDensity, "Probe spacing: 2 / 1 / 0.5 m",
       [](const GS& s) { return (int)s.gi_density; }, [](GS& s, int v) { s.gi_density = (GiDensity)v; }),
    IP("gi_update", GraphicsPropKind::Choice, 0, 0, kGiUpdate, "Share of the probes refreshed a frame: 1/32, 1/16, 1/8",
       [](const GS& s) { return (int)s.gi_update; }, [](GS& s, int v) { s.gi_update = (GiUpdate)v; }),
    BP("pbr", "GGX shading, else Blinn-Phong", pbr),
    BP("particles", "Particles on or off", fx_particles),
    FP("particle_density", 0.1f, 1.0f, "Share of every emitter's rate and max particles", fx_particle_density),
    BP("particle_depth_test", "Meshes hide particles and text (off is cheaper)", fx_depth_test),
    BP("sprites", "Billboard and Text 3D modules", fx_sprites),
    BP("audio_reverb", "Audio Reverb Zones", fx_reverb),
    BP("allow_gpu_effects_on_cpu", "Run the GPU tier on the CPU backend (slow)", allow_gpu_effects_on_cpu),
};
constexpr int kPropCount = (int)(sizeof(kProps) / sizeof(kProps[0]));

int choice_count(const char* const* names) {
    int n = 0;
    while (names[n]) n++;
    return n;
}

bool parse_bool(const char* v, int* out) {
    if (!strcmp(v, "true") || !strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "yes")) { *out = 1; return true; }
    if (!strcmp(v, "false") || !strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "no")) { *out = 0; return true; }
    return false;
}

void fail(char* err, int cap, const char* fmt, const char* a, const char* b = "") {
    if (err && cap > 0) snprintf(err, (size_t)cap, fmt, a, b);
}

void allowed_list(const Prop& p, char* out, int cap) {
    out[0] = 0;
    switch (p.info.kind) {
        case GraphicsPropKind::Bool: snprintf(out, (size_t)cap, "true, false"); break;
        case GraphicsPropKind::Choice: {
            size_t n = 0;
            for (int i = 0; p.info.choices[i] && n + 2 < (size_t)cap; i++)
                n += (size_t)snprintf(out + n, (size_t)cap - n, "%s%s", i ? ", " : "", p.info.choices[i]);
            break;
        }
        default: snprintf(out, (size_t)cap, "%g to %g", p.info.min, p.info.max); break;
    }
}

}  // namespace

int graphics_property_count() { return kPropCount; }
const GraphicsProperty& graphics_property(int i) { return kProps[i < 0 ? 0 : (i >= kPropCount ? kPropCount - 1 : i)].info; }

int graphics_property_find(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < kPropCount; i++) if (!strcmp(kProps[i].info.name, name)) return i;
    return -1;
}

bool graphics_get_text(const GraphicsSettings& s, const char* name, char* out, int cap) {
    int i = graphics_property_find(name);
    if (i < 0 || cap < 1) return false;
    const Prop& p = kProps[i];
    switch (p.info.kind) {
        case GraphicsPropKind::Bool: snprintf(out, (size_t)cap, "%s", p.geti(s) ? "true" : "false"); break;
        case GraphicsPropKind::Integer: snprintf(out, (size_t)cap, "%d", p.geti(s)); break;
        case GraphicsPropKind::Number: snprintf(out, (size_t)cap, "%g", (double)p.getf(s)); break;
        case GraphicsPropKind::Choice: snprintf(out, (size_t)cap, "%s", p.info.choices[p.geti(s)]); break;
    }
    return true;
}

bool graphics_set_text(GraphicsSettings* s, const char* name, const char* value, char* err, int err_cap) {
    int i = graphics_property_find(name);
    if (i < 0) { fail(err, err_cap, "unknown graphics setting '%s'", name ? name : "(null)"); return false; }
    const Prop& p = kProps[i];
    char allowed[160];
    allowed_list(p, allowed, sizeof allowed);
    if (!value) value = "";
    bool is_preset = i == 0;
    switch (p.info.kind) {
        case GraphicsPropKind::Bool: {
            int b;
            if (!parse_bool(value, &b)) break;
            p.seti(*s, b);
            if (!is_preset) s->preset = GraphicsPreset::Custom;
            return true;
        }
        case GraphicsPropKind::Choice: {
            int n = choice_count(p.info.choices), k = -1;
            for (int c = 0; c < n; c++) if (!strcmp(p.info.choices[c], value)) k = c;
            if (k < 0) break;
            p.seti(*s, k);
            if (!is_preset) s->preset = GraphicsPreset::Custom;
            return true;
        }
        case GraphicsPropKind::Integer:
        case GraphicsPropKind::Number: {
            char* end = nullptr;
            double v = strtod(value, &end);
            if (!value[0] || !end || *end) break;
            if (v < (double)p.info.min || v > (double)p.info.max) break;
            if (p.info.kind == GraphicsPropKind::Integer) {
                if (v != (double)(int)v) break;
                p.seti(*s, (int)v);
            } else {
                p.setf(*s, (float)v);
            }
            s->preset = GraphicsPreset::Custom;
            return true;
        }
    }
    if (err && err_cap > 0) snprintf(err, (size_t)err_cap, "invalid value '%s' for graphics setting '%s' (allowed: %s)", value, name, allowed);
    return false;
}

bool graphics_apply_preset_name(GraphicsSettings* s, const char* name) {
    if (!name) return false;
    for (int i = 0; i < 4; i++)
        if (!strcmp(kPresets[i], name)) { s->apply_preset((GraphicsPreset)i); return true; }
    return false;
}

size_t graphics_settings_to_text(const GraphicsSettings& s, char* out, size_t cap) {
    size_t n = 0;
    char line[96];
    auto put = [&](const char* text) {
        size_t l = strlen(text);
        if (cap && n < cap) {
            size_t room = cap - n - 1;
            memcpy(out + n, text, l < room ? l : room);
        }
        n += l;
    };
    put("farcade-graphics 1\n");
    for (int i = 0; i < kPropCount; i++) {
        char v[40];
        graphics_get_text(s, kProps[i].info.name, v, sizeof v);
        snprintf(line, sizeof line, "%s %s\n", kProps[i].info.name, v);
        put(line);
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return n;
}

int graphics_settings_from_text(GraphicsSettings* s, const char* text, size_t size) {
    if (!text || !s) return -1;
    *s = GraphicsSettings();
    int ignored = 0;
    int preset_label = -1, applied = 0;
    size_t i = 0;
    while (i < size && text[i]) {
        size_t e = i;
        while (e < size && text[e] && text[e] != '\n') e++;
        char line[160];
        size_t l = e - i < sizeof line - 1 ? e - i : sizeof line - 1;
        memcpy(line, text + i, l);
        line[l] = 0;
        i = e < size && text[e] ? e + 1 : e;
        // trim, skip empty lines, comments and the header
        char* p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        char* q = p + strlen(p);
        while (q > p && (q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\r')) *--q = 0;
        if (!*p || *p == '#' || !strncmp(p, "farcade-graphics", 16)) continue;
        char* sp = p;
        while (*sp && *sp != ' ' && *sp != '\t') sp++;
        if (!*sp) { ignored++; continue; }
        *sp++ = 0;
        while (*sp == ' ' || *sp == '\t') sp++;
        if (graphics_property_find(p) < 0 || !graphics_set_text(s, p, sp)) { ignored++; continue; }
        applied++;
        if (!strcmp(p, "preset")) preset_label = (int)s->preset;
    }
    // the label survives only when nothing differs from the preset it names
    if (preset_label >= 0 && preset_label < 4) {
        GraphicsSettings ref = *s;
        ref.apply_preset((GraphicsPreset)preset_label);
        ref.preset = s->preset;
        char a[1024], b[1024];
        graphics_settings_to_text(*s, a, sizeof a);
        graphics_settings_to_text(ref, b, sizeof b);
        s->preset = strcmp(a, b) == 0 ? (GraphicsPreset)preset_label : GraphicsPreset::Custom;
    } else if (preset_label < 0 && applied > 0) {
        s->preset = GraphicsPreset::Custom;     // no preset line: the values stand on their own
    }
    return ignored;
}

char* graphics_settings_to_text(const GraphicsSettings& s, size_t* size) {
    size_t need = graphics_settings_to_text(s, nullptr, 0);
    char* out = (char*)malloc(need + 1);
    if (!out) { if (size) *size = 0; return nullptr; }
    graphics_settings_to_text(s, out, need + 1);
    if (size) *size = need;
    return out;
}

bool graphics_settings_from_text(const char* text, size_t size, GraphicsSettings* out) {
    return graphics_settings_from_text(out, text, size) == 0;
}

}  // namespace fe::gfx
