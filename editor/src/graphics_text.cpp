#include "graphics_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe {

char* graphics_settings_to_text(const gfx::GraphicsSettings& g, size_t* size) {
    char* out = (char*)malloc(1024);
    if (!out) return nullptr;
    size_t n = (size_t)snprintf(out, 1024,
        "preset %d\nrender_scale %g\nshadows %s\nshadow_resolution %d\nshadow_distance %g\nsoft_shadows %s\n"
        "local_shadows %s\nlocal_shadow_lights %d\nlocal_shadow_resolution %d\nambient_occlusion %s\nao_radius %g\n"
        "ao_strength %g\nbloom %s\nantialiasing %s\npbr %s\nallow_gpu_effects_on_cpu %s\n",
        (int)g.preset, g.render_scale, g.shadows ? "true" : "false", g.shadow_resolution, g.shadow_distance,
        g.soft_shadows ? "true" : "false", g.local_shadows ? "true" : "false", g.local_shadow_lights, g.local_shadow_resolution,
        g.ambient_occlusion ? "true" : "false", g.ao_radius, g.ao_strength, g.bloom ? "true" : "false",
        g.antialiasing ? "true" : "false", g.pbr ? "true" : "false", g.allow_gpu_effects_on_cpu ? "true" : "false");
    if (size) *size = n;
    return out;
}

bool graphics_settings_from_text(const char* text, size_t size, gfx::GraphicsSettings* g) {
    char* buf = (char*)malloc(size + 1);
    if (!buf) return false;
    memcpy(buf, text, size);
    buf[size] = 0;
    bool any = false;
    char* s = buf;
    while (s && *s) {
        char* nl = strchr(s, '\n');
        if (nl) *nl = 0;
        char* line = s;
        s = nl ? nl + 1 : nullptr;
        while (*line == ' ' || *line == '\t') line++;
        char* sp = strpbrk(line, " \t");
        if (!sp || *line == '#') continue;
        *sp++ = 0;
        while (*sp == ' ' || *sp == '\t') sp++;
        size_t vl = strlen(sp);
        while (vl && (sp[vl - 1] == '\r' || sp[vl - 1] == ' ')) sp[--vl] = 0;
        const char* k = line;
        bool b = !strcmp(sp, "true") || !strcmp(sp, "1");
        float f = (float)atof(sp);
        int i = atoi(sp);
        bool ok = true;
        if (!strcmp(k, "preset")) { if (i >= 0 && i <= (int)gfx::GraphicsPreset::Custom) g->preset = (gfx::GraphicsPreset)i; else ok = false; }
        else if (!strcmp(k, "render_scale")) g->render_scale = f < 0.25f ? 0.25f : (f > 1.0f ? 1.0f : f);
        else if (!strcmp(k, "shadows")) g->shadows = b;
        else if (!strcmp(k, "shadow_resolution")) g->shadow_resolution = i < 512 ? 512 : (i > 4096 ? 4096 : i);
        else if (!strcmp(k, "shadow_distance")) g->shadow_distance = f;
        else if (!strcmp(k, "soft_shadows")) g->soft_shadows = b;
        else if (!strcmp(k, "local_shadows")) g->local_shadows = b;
        else if (!strcmp(k, "local_shadow_lights")) g->local_shadow_lights = i < 1 ? 1 : (i > 4 ? 4 : i);
        else if (!strcmp(k, "local_shadow_resolution")) g->local_shadow_resolution = i < 512 ? 512 : (i > 4096 ? 4096 : i);
        else if (!strcmp(k, "ambient_occlusion")) g->ambient_occlusion = b;
        else if (!strcmp(k, "ao_radius")) g->ao_radius = f;
        else if (!strcmp(k, "ao_strength")) g->ao_strength = f;
        else if (!strcmp(k, "bloom")) g->bloom = b;
        else if (!strcmp(k, "antialiasing")) g->antialiasing = b;
        else if (!strcmp(k, "pbr")) g->pbr = b;
        else if (!strcmp(k, "allow_gpu_effects_on_cpu")) g->allow_gpu_effects_on_cpu = b;
        else ok = false;
        any |= ok;
    }
    free(buf);
    return any;
}

}  // namespace fe
