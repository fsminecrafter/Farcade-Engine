#include "scene_io.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe {

namespace {

const char* const kObjectKinds[] = { "mesh", "empty", "camera", "skylight", "sky", "post_process" };
const char* const kMeshKinds[] = { "cube", "plane", "sphere", "cylinder", "model" };
const char* const kPropTypes[] = { "number", "text", "toggle", "list", "object", "file" };
static_assert(sizeof(kObjectKinds) / sizeof(kObjectKinds[0]) == (int)ObjectKind::Count, "object kinds");

int index_of(const char* const* names, int count, const char* s) {
    for (int i = 0; i < count; i++)
        if (strcmp(names[i], s) == 0) return i;
    return -1;
}

// ---------------------------------------------------------------- writer

struct Out {
    char* buf = nullptr;
    size_t len = 0, cap = 0;
    bool failed = false;

    void raw(const char* s, size_t n) {
        if (failed) return;
        if (len + n + 1 > cap) {
            size_t nc = cap ? cap * 2 : 4096;
            while (nc < len + n + 1) nc *= 2;
            char* nb = (char*)realloc(buf, nc);
            if (!nb) { failed = true; return; }
            buf = nb;
            cap = nc;
        }
        memcpy(buf + len, s, n);
        len += n;
        buf[len] = 0;
    }
    void f(const char* fmt, ...) {
        char tmp[512];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
        va_end(ap);
        if (n < 0) { failed = true; return; }
        raw(tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
    }
    // A quoted string: \" \\ \n escaped.
    void str(const char* s) {
        raw("\"", 1);
        for (; *s; s++) {
            if (*s == '"' || *s == '\\') { char e[2] = { '\\', *s }; raw(e, 2); }
            else if (*s == '\n') raw("\\n", 2);
            else raw(s, 1);
        }
        raw("\"", 1);
    }
    void num(float v) { f(" %.9g", (double)v); }
    void vec(const Vec3& v) { num(v.x); num(v.y); num(v.z); }
};

// `path` relative to the project when it lies inside it.
const char* relative(const char* path, const char* project_dir) {
    if (!project_dir || !*project_dir) return path;
    size_t n = strlen(project_dir);
    while (n && project_dir[n - 1] == '/') n--;
    if (n && strncmp(path, project_dir, n) == 0 && path[n] == '/') return path + n + 1;
    return path;
}

void write_fields(Out& o, const ModuleInfo& mi, const void* data, const char* indent) {
    const uint8_t* base = (const uint8_t*)data;
    for (int i = 0; i < mi.field_count; i++) {
        const FieldInfo& fi = mi.fields[i];
        const uint8_t* p = base + fi.offset;
        o.f("%s%s", indent, fi.name);
        switch (fi.type) {
        case FieldType::Float: { float v; memcpy(&v, p, 4); o.num(v); break; }
        case FieldType::Int: { int32_t v; memcpy(&v, p, 4); o.f(" %d", (int)v); break; }
        case FieldType::Bits: { uint32_t v; memcpy(&v, p, 4); o.f(" %u", (unsigned)v); break; }
        case FieldType::Bool: o.f(" %s", *(const bool*)p ? "true" : "false"); break;
        case FieldType::Enum: {
            int v = *p, n = 0;
            while (fi.enum_names && fi.enum_names[n]) n++;
            if (v < n) o.f(" %s", fi.enum_names[v]);
            else o.f(" %d", v);
            break;
        }
        case FieldType::Color:
        case FieldType::Vec3: { Vec3 v; memcpy(&v, p, sizeof v); o.vec(v); break; }
        case FieldType::Text:
        case FieldType::Path: o.raw(" ", 1); o.str((const char*)p); break;
        case FieldType::Object: { uint32_t v; memcpy(&v, p, 4); o.f(" %u", (unsigned)v); break; }
        }
        o.raw("\n", 1);
    }
}

// ---------------------------------------------------------------- reader

struct Line {
    char* tok[24];
    int n = 0;
};

// Splits one line into tokens (in place); quoted strings lose their quotes
// and escapes. '#' starts a comment outside quotes.
void split(char* s, Line* l) {
    l->n = 0;
    while (*s && l->n < 24) {
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        if (!*s || *s == '#') break;
        if (*s == '"') {
            char* w = ++s;
            l->tok[l->n++] = w;
            while (*s && *s != '"') {
                if (*s == '\\' && s[1]) { s++; *w++ = *s == 'n' ? '\n' : *s; s++; }
                else *w++ = *s++;
            }
            if (*s) s++;
            *w = 0;
        } else {
            l->tok[l->n++] = s;
            while (*s && *s != ' ' && *s != '\t' && *s != '\r') s++;
            if (*s) *s++ = 0;
        }
    }
}

bool parse_float(const char* s, float* out) {
    char* end = nullptr;
    double d = strtod(s, &end);
    if (!end || end == s || *end) return false;
    *out = (float)d;
    return true;
}

bool parse_int(const char* s, long* out) {
    char* end = nullptr;
    long v = strtol(s, &end, 10);
    if (!end || end == s || *end) return false;
    *out = v;
    return true;
}

bool parse_bool(const char* s, bool* out) {
    if (strcmp(s, "true") == 0) { *out = true; return true; }
    if (strcmp(s, "false") == 0) { *out = false; return true; }
    return false;
}

bool parse_vec(const Line& l, int at, Vec3* out) {
    return l.n >= at + 3 && parse_float(l.tok[at], &out->x) && parse_float(l.tok[at + 1], &out->y) && parse_float(l.tok[at + 2], &out->z);
}

void copy_str(char* dst, size_t cap, const char* s) {
    snprintf(dst, cap, "%s", s);
}

bool read_field(const ModuleInfo& mi, void* data, const Line& l) {
    const FieldInfo* fi = find_field(mi, l.tok[0]);
    if (!fi || l.n < 2) return false;
    uint8_t* p = (uint8_t*)data + fi->offset;
    switch (fi->type) {
    case FieldType::Float: { float v; if (!parse_float(l.tok[1], &v)) return false; memcpy(p, &v, 4); return true; }
    case FieldType::Int: { long v; if (!parse_int(l.tok[1], &v)) return false; int32_t i = (int32_t)v; memcpy(p, &i, 4); return true; }
    case FieldType::Object:
    case FieldType::Bits: {
        char* end = nullptr;
        unsigned long v = strtoul(l.tok[1], &end, 10);
        if (!end || *end) return false;
        uint32_t u = (uint32_t)v;
        memcpy(p, &u, 4);
        return true;
    }
    case FieldType::Bool: return parse_bool(l.tok[1], (bool*)p);
    case FieldType::Enum: {
        int n = 0;
        while (fi->enum_names && fi->enum_names[n]) n++;
        int v = index_of(fi->enum_names, n, l.tok[1]);
        if (v < 0) { long i; if (!parse_int(l.tok[1], &i) || i < 0 || i >= n) return false; v = (int)i; }
        *p = (uint8_t)v;
        return true;
    }
    case FieldType::Color:
    case FieldType::Vec3: { Vec3 v; if (!parse_vec(l, 1, &v)) return false; memcpy(p, &v, sizeof v); return true; }
    case FieldType::Text:
    case FieldType::Path: copy_str((char*)p, kPathMax, l.tok[1]); return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------- save

char* save_scene_text(Scene& scene, const char* project_dir, size_t* size, unsigned flags) {
    const bool history = (flags & kSceneTextHistory) != 0;
    Out o;
    o.f("farcade-scene %d\n", kSceneFormatVersion);
    if (!history) o.f("next_id %u\n", (unsigned)scene.next_id());
    for (int i = 0; i < scene.count(); i++) {
        const Object& ob = scene.at(i);
        o.f("\nobject %u\n", (unsigned)ob.id);
        o.raw("  name ", 7); o.str(ob.name); o.raw("\n", 1);
        o.f("  kind %s\n", kObjectKinds[(int)ob.kind]);
        if (ob.parent) o.f("  parent %u\n", (unsigned)ob.parent);
        if (!ob.expanded && !history) o.f("  expanded false\n");
        o.f("  position"); o.vec(ob.position); o.raw("\n", 1);
        o.f("  rotation"); o.num(ob.rotation.x); o.num(ob.rotation.y); o.num(ob.rotation.z); o.num(ob.rotation.w); o.raw("\n", 1);
        o.f("  scale"); o.vec(ob.scale); o.raw("\n", 1);
        if (!ob.visible) o.f("  visible false\n");
        if (ob.kind == ObjectKind::Mesh) {
            o.f("  mesh %s\n", kMeshKinds[(int)ob.mesh]);
            if (ob.model_path[0]) { o.raw("  model ", 8); o.str(relative(ob.model_path, project_dir)); o.raw("\n", 1); }
            if (ob.texture_path[0]) { o.raw("  texture ", 10); o.str(relative(ob.texture_path, project_dir)); o.raw("\n", 1); }
            if (ob.shader_path[0]) { o.raw("  shader ", 9); o.str(relative(ob.shader_path, project_dir)); o.raw("\n", 1); }
            o.f("  color"); o.vec(ob.color); o.raw("\n", 1);
            o.f("  roughness"); o.num(ob.roughness); o.raw("\n", 1);
            o.f("  metallic"); o.num(ob.metallic); o.raw("\n", 1);
            o.f("  unlit %s\n", ob.unlit ? "true" : "false");
            o.f("  cast_shadows %s\n", ob.cast_shadows ? "true" : "false");
        }
        for (int k = 0; k < kModuleKindCount; k++) {
            if (!ob.modules.has((ModuleKind)k)) continue;
            const ModuleInfo& mi = module_info((ModuleKind)k);
            o.f("  module %s\n", mi.name);
            const void* data = ob.modules.data((ModuleKind)k);
            if (mi.fields == nullptr) { o.f("  end\n"); continue; }
            // Paths inside the project are stored relative to it, like the object's.
            bool has_path = false;
            for (int f = 0; f < mi.field_count; f++) has_path |= mi.fields[f].type == FieldType::Path;
            if (has_path) {
                uint8_t tmp[sizeof(ModuleSet)];
                memcpy(tmp, data, mi.size);
                for (int f = 0; f < mi.field_count; f++) {
                    if (mi.fields[f].type != FieldType::Path) continue;
                    char* path = (char*)tmp + mi.fields[f].offset;
                    const char* rel = relative(path, project_dir);
                    if (rel != path) memmove(path, rel, strlen(rel) + 1);
                }
                write_fields(o, mi, tmp, "    ");
            } else {
                write_fields(o, mi, data, "    ");
            }
            o.f("  end\n");
        }
        for (int s = 0; s < ob.script_count; s++) {
            const ScriptSlot& sl = ob.scripts[s];
            o.raw("  script ", 9); o.str(relative(sl.path, project_dir)); o.raw("\n", 1);
            for (int p = 0; p < sl.prop_count; p++) {
                const ScriptProp& pr = sl.props[p];
                o.f("    prop %s %s", pr.name, kPropTypes[(int)pr.type]);
                switch (pr.type) {
                case PropType::Number: o.num(pr.number); o.num(pr.min); o.num(pr.max); break;
                case PropType::Text: o.raw(" ", 1); o.str(pr.text); break;
                case PropType::Toggle: o.f(" %s", pr.toggle ? "true" : "false"); break;
                case PropType::List:
                    o.raw(" ", 1);
                    o.str(pr.choice >= 0 && pr.choice < pr.choice_count ? pr.choices[pr.choice] : "");
                    for (int c = 0; c < pr.choice_count; c++) { o.raw(" ", 1); o.str(pr.choices[c]); }
                    break;
                case PropType::Object: o.f(" %u", (unsigned)pr.object); break;
                case PropType::File: o.raw(" ", 1); o.str(relative(pr.file, project_dir)); break;
                }
                o.raw("\n", 1);
            }
            o.f("  end\n");
        }
        o.f("end\n");
    }
    if (o.failed) { free(o.buf); return nullptr; }
    if (size) *size = o.len;
    return o.buf;
}

// ---------------------------------------------------------------- load

bool load_scene_text(Scene& scene, const char* text, size_t size, SceneLoadReport* rep) {
    SceneLoadReport local;
    if (!rep) rep = &local;
    *rep = SceneLoadReport();
    scene.clear();
    char* buf = (char*)malloc(size + 1);
    if (!buf) { snprintf(rep->error, sizeof rep->error, "out of memory"); return false; }
    memcpy(buf, text, size);
    buf[size] = 0;

    auto fail = [&](int line, const char* why) {
        snprintf(rep->error, sizeof rep->error, "line %d: %s", line, why);
        scene.clear();
        free(buf);
        return false;
    };
    auto bad = [&](int line) {
        rep->skipped++;
        if (!rep->first_bad_line) rep->first_bad_line = line;
    };

    enum { Top, InObject, InModule, InScript } state = Top;
    Object* ob = nullptr;
    ModuleKind mod = ModuleKind::Count;
    ScriptSlot* slot = nullptr;
    bool header = false;
    unsigned next_id = 0;
    int lineno = 0;
    char* s = buf;
    while (s && *s) {
        char* nl = strchr(s, '\n');
        if (nl) *nl = 0;
        lineno++;
        Line l;
        split(s, &l);
        s = nl ? nl + 1 : nullptr;
        if (l.n == 0) continue;
        const char* key = l.tok[0];
        if (!header) {
            long v;
            if (strcmp(key, "farcade-scene") != 0 || l.n < 2 || !parse_int(l.tok[1], &v)) return fail(lineno, "not a scene file");
            if (v > kSceneFormatVersion) return fail(lineno, "made by a newer editor");
            header = true;
            continue;
        }
        switch (state) {
        case Top:
            if (strcmp(key, "next_id") == 0 && l.n >= 2) {
                long v;
                if (parse_int(l.tok[1], &v) && v > 0) next_id = (unsigned)v; else bad(lineno);
            } else if (strcmp(key, "object") == 0 && l.n >= 2) {
                long v;
                if (!parse_int(l.tok[1], &v) || v <= 0) return fail(lineno, "bad object id");
                ob = scene.create_with_id((ObjectId)v);
                if (!ob) return fail(lineno, "object id used twice");
                rep->objects++;
                state = InObject;
            } else {
                bad(lineno);
            }
            break;
        case InObject: {
            bool ok = true;
            if (strcmp(key, "end") == 0) { state = Top; ob = nullptr; break; }
            if (strcmp(key, "name") == 0 && l.n >= 2) copy_str(ob->name, sizeof ob->name, l.tok[1]);
            else if (strcmp(key, "kind") == 0 && l.n >= 2) {
                int k = index_of(kObjectKinds, (int)ObjectKind::Count, l.tok[1]);
                if (k >= 0) ob->kind = (ObjectKind)k; else ok = false;
            } else if (strcmp(key, "parent") == 0 && l.n >= 2) {
                long v; ok = parse_int(l.tok[1], &v) && v >= 0;
                if (ok) ob->parent = (ObjectId)v;
            } else if (strcmp(key, "expanded") == 0 && l.n >= 2) ok = parse_bool(l.tok[1], &ob->expanded);
            else if (strcmp(key, "visible") == 0 && l.n >= 2) ok = parse_bool(l.tok[1], &ob->visible);
            else if (strcmp(key, "position") == 0) ok = parse_vec(l, 1, &ob->position);
            else if (strcmp(key, "scale") == 0) ok = parse_vec(l, 1, &ob->scale);
            else if (strcmp(key, "rotation") == 0) {
                Quat q;
                ok = l.n >= 5 && parse_float(l.tok[1], &q.x) && parse_float(l.tok[2], &q.y) && parse_float(l.tok[3], &q.z) && parse_float(l.tok[4], &q.w);
                if (ok) ob->rotation = q;
            } else if (strcmp(key, "mesh") == 0 && l.n >= 2) {
                int k = index_of(kMeshKinds, 5, l.tok[1]);
                if (k >= 0) ob->mesh = (MeshKind)k; else ok = false;
            } else if (strcmp(key, "model") == 0 && l.n >= 2) copy_str(ob->model_path, sizeof ob->model_path, l.tok[1]);
            else if (strcmp(key, "texture") == 0 && l.n >= 2) copy_str(ob->texture_path, sizeof ob->texture_path, l.tok[1]);
            else if (strcmp(key, "shader") == 0 && l.n >= 2) copy_str(ob->shader_path, sizeof ob->shader_path, l.tok[1]);
            else if (strcmp(key, "color") == 0) ok = parse_vec(l, 1, &ob->color);
            else if (strcmp(key, "roughness") == 0 && l.n >= 2) ok = parse_float(l.tok[1], &ob->roughness);
            else if (strcmp(key, "metallic") == 0 && l.n >= 2) ok = parse_float(l.tok[1], &ob->metallic);
            else if (strcmp(key, "unlit") == 0 && l.n >= 2) ok = parse_bool(l.tok[1], &ob->unlit);
            else if (strcmp(key, "cast_shadows") == 0 && l.n >= 2) ok = parse_bool(l.tok[1], &ob->cast_shadows);
            else if (strcmp(key, "module") == 0 && l.n >= 2) {
                mod = module_by_name(l.tok[1]);
                if (mod != ModuleKind::Count) { ob->modules.reset(mod); ob->modules.add(mod); }
                else bad(lineno);
                state = InModule;       // an unknown module's lines are skipped up to its end
                break;
            } else if (strcmp(key, "script") == 0 && l.n >= 2) {
                if (ob->script_count < kMaxScripts) {
                    slot = &ob->scripts[ob->script_count++];
                    *slot = ScriptSlot();
                    copy_str(slot->path, sizeof slot->path, l.tok[1]);
                } else {
                    slot = nullptr;
                    bad(lineno);
                }
                state = InScript;
                break;
            } else ok = false;
            if (!ok) bad(lineno);
            break;
        }
        case InModule:
            if (strcmp(key, "end") == 0) { state = InObject; mod = ModuleKind::Count; break; }
            if (mod != ModuleKind::Count && !read_field(module_info(mod), ob->modules.data(mod), l)) bad(lineno);
            break;
        case InScript: {
            if (strcmp(key, "end") == 0) { state = InObject; slot = nullptr; break; }
            if (!slot) break;
            if (strcmp(key, "prop") != 0 || l.n < 4 || slot->prop_count >= kMaxProps) { bad(lineno); break; }
            int t = index_of(kPropTypes, 6, l.tok[2]);
            if (t < 0) { bad(lineno); break; }
            ScriptProp pr;
            copy_str(pr.name, sizeof pr.name, l.tok[1]);
            pr.type = (PropType)t;
            bool ok = true;
            switch (pr.type) {
            case PropType::Number:
                ok = parse_float(l.tok[3], &pr.number);
                if (ok && l.n >= 6) ok = parse_float(l.tok[4], &pr.min) && parse_float(l.tok[5], &pr.max);
                break;
            case PropType::Text: copy_str(pr.text, sizeof pr.text, l.tok[3]); break;
            case PropType::Toggle: ok = parse_bool(l.tok[3], &pr.toggle); break;
            case PropType::List:
                for (int c = 4; c < l.n && pr.choice_count < kMaxChoices; c++) {
                    copy_str(pr.choices[pr.choice_count], sizeof pr.choices[0], l.tok[c]);
                    if (strcmp(l.tok[c], l.tok[3]) == 0) pr.choice = pr.choice_count;
                    pr.choice_count++;
                }
                if (pr.choice_count == 0) { copy_str(pr.choices[0], sizeof pr.choices[0], l.tok[3]); pr.choice_count = 1; }
                break;
            case PropType::Object: { long v; ok = parse_int(l.tok[3], &v) && v >= 0; if (ok) pr.object = (ObjectId)v; break; }
            case PropType::File: copy_str(pr.file, sizeof pr.file, l.tok[3]); break;
            }
            if (ok) slot->props[slot->prop_count++] = pr;
            else bad(lineno);
            break;
        }
        }
    }
    if (!header) return fail(lineno ? lineno : 1, "empty file");
    if (state != Top) return fail(lineno, "the file ends inside an object");
    free(buf);
    // Links to objects that are not in the file are dropped.
    for (int i = 0; i < scene.count(); i++) {
        Object& o = scene.at(i);
        if (o.parent && (o.parent == o.id || !scene.get(o.parent))) o.parent = kNoObject;
        for (int si = 0; si < o.script_count; si++)
            for (int p = 0; p < o.scripts[si].prop_count; p++) {
                ScriptProp& pr = o.scripts[si].props[p];
                if (pr.type == PropType::Object && pr.object && !scene.get(pr.object)) pr.object = kNoObject;
            }
    }
    // A parent loop (a hand-edited file) would hang the tree walks: cut it.
    for (int i = 0; i < scene.count(); i++) {
        Object& o = scene.at(i);
        ObjectId p = o.parent;
        for (int steps = 0; p && steps <= scene.count(); steps++) {
            Object* po = scene.get(p);
            p = po ? po->parent : kNoObject;
            if (p == o.id) { o.parent = kNoObject; break; }
        }
    }
    scene.keep_ids_above((ObjectId)next_id);
    scene.fix_current();
    return true;
}

}  // namespace fe
