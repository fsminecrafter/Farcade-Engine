#include "export.h"

#include "core/pak.h"
#include "mpkg.h"
#include "platform/platform.h"
#include "scene.h"
#include "scene_io.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if FE_HOST
#include <sys/stat.h>
#endif

namespace fe {

namespace {

struct Ctx {
    const ExportOptions* opt;
    ExportReport* rep;
    char project_dir[512];
    PakWriter pak;
};

void say(Ctx& c, int level, const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (c.opt->log) c.opt->log(level, buf, c.opt->user);
}

bool fail(Ctx& c, const char* fmt, const char* a = "", const char* b = "") {
    snprintf(c.rep->error, sizeof c.rep->error, fmt, a, b);
    say(c, 2, "%s", c.rep->error);
    return false;
}

bool is_full_path(const char* p) {
    const char* colon = strchr(p, ':');
    return p[0] == '/' || (colon && colon[1] == '/' && !memchr(p, '/', (size_t)(colon - p)));
}

// A path from a scene as a project relative virtual path ("./a\b" -> "a/b").
// False when it points outside the project.
bool virtual_path(const Ctx& c, const char* in, char* out, size_t cap) {
    char tmp[512];
    size_t n = 0;
    for (const char* p = in; *p && n + 1 < sizeof tmp; p++) tmp[n++] = *p == '\\' ? '/' : *p;
    tmp[n] = 0;
    const char* p = tmp;
    size_t pl = strlen(c.project_dir);
    if (pl && strncmp(p, c.project_dir, pl) == 0 && p[pl] == '/') p += pl + 1;
    else if (is_full_path(p)) return false;
    n = 0;
    while (*p) {
        if (p[0] == '/' && p[1] == '/') { p++; continue; }
        if ((n == 0 || out[n - 1] == '/') && p[0] == '.' && p[1] == '/') { p += 2; continue; }
        if (n + 1 < cap) out[n++] = *p;
        p++;
    }
    out[n] = 0;
    return n > 0 && strstr(out, "../") == nullptr;
}

void full_path(const Ctx& c, const char* rel, char* out, size_t cap) {
    if (c.project_dir[0]) snprintf(out, cap, "%s/%s", c.project_dir, rel);
    else snprintf(out, cap, "%s", rel);
}

// Adds a project file (by virtual path) once. Missing files are warned about.
void add_file(Ctx& c, const char* path_in, const char* what, const char* from) {
    if (!path_in || !*path_in) return;
    char v[512];
    if (!virtual_path(c, path_in, v, sizeof v)) {
        say(c, 1, "%s of %s is outside the project (%s): not packed", what, from, path_in);
        c.rep->missing++;
        return;
    }
    if (c.pak.has(v)) return;
    char full[1100];
    full_path(c, v, full, sizeof full);
    size_t n = 0;
    void* data = platform::read_file(full, &n);
    if (!data) {
        say(c, 1, "%s of %s not found: %s", what, from, v);
        c.rep->missing++;
        return;
    }
    if (!c.pak.add(v, data, n)) say(c, 2, "cannot pack %s", v);
    free(data);
}

// Every file an object refers to.
void collect_object(Ctx& c, const Object& o, const char* scene_name) {
    char who[160];
    snprintf(who, sizeof who, "%s (%s)", o.name, scene_name);
    if (o.kind == ObjectKind::Mesh) {
        if (o.mesh == MeshKind::Model || o.model_path[0]) add_file(c, o.model_path, "model", who);
        add_file(c, o.texture_path, "texture", who);
        add_file(c, o.shader_path, "shader", who);
    }
    for (int k = 0; k < kModuleKindCount; k++) {
        if (!o.modules.has((ModuleKind)k)) continue;
        const ModuleInfo& mi = module_info((ModuleKind)k);
        const uint8_t* data = (const uint8_t*)o.modules.data((ModuleKind)k);
        for (int f = 0; f < mi.field_count; f++)
            if (mi.fields[f].type == FieldType::Path) add_file(c, (const char*)data + mi.fields[f].offset, mi.fields[f].name, who);
    }
    // An Animator without a file of its own plays <model>.fanim next to the model, if there is one.
    if (o.modules.has(ModuleKind::Animator) && !o.modules.animator.source[0] && o.model_path[0]) {
        char anim[kPathMax + 8];
        snprintf(anim, sizeof anim, "%s", o.model_path);
        char* dot = strrchr(anim, '.');
        char* slash = strrchr(anim, '/');
        if (dot && (!slash || dot > slash)) *dot = 0;
        strncat(anim, ".fanim", sizeof anim - strlen(anim) - 1);
        char v[512], full[1100];
        if (virtual_path(c, anim, v, sizeof v)) {
            full_path(c, v, full, sizeof full);
            if (platform::file_exists(full)) add_file(c, anim, "animation", who);
        }
    }
    for (int s = 0; s < o.script_count; s++) {
        const ScriptSlot& sl = o.scripts[s];
        add_file(c, sl.path, "script", who);
        for (int p = 0; p < sl.prop_count; p++)
            if (sl.props[p].type == PropType::File) add_file(c, sl.props[p].file, sl.props[p].name, who);
    }
}

struct ListCtx { char* names; int count, cap; char dir[512]; };

void scan_cb(const platform::DirEntry& e, void* user) {
    ListCtx* l = (ListCtx*)user;
    if (l->count >= l->cap) return;
    snprintf(l->names + (size_t)l->count * 160, 160, "%s%s", e.name, e.is_dir ? "/" : "");
    l->count++;
}

// Every *.fscene under dir (relative to the project), a few levels deep.
void find_scenes(Ctx& c, const char* rel, int depth, char (*out)[256], int* n, int max) {
    char dir[1100];
    if (rel[0]) full_path(c, rel, dir, sizeof dir);
    else snprintf(dir, sizeof dir, "%s", c.project_dir[0] ? c.project_dir : ".");
    ListCtx l;
    l.cap = 256;
    l.count = 0;
    l.names = (char*)malloc((size_t)l.cap * 160);
    if (!l.names) return;
    if (platform::list_dir(dir, scan_cb, &l)) {
        for (int i = 0; i < l.count && *n < max; i++) {
            const char* name = l.names + (size_t)i * 160;
            size_t len = strlen(name);
            char child[256];
            if (len && name[len - 1] == '/') {
                if (depth >= 6 || name[0] == '.' || strcmp(name, "build/") == 0 || strcmp(name, "export/") == 0) continue;
                snprintf(child, sizeof child, "%s%s%.*s", rel, rel[0] ? "/" : "", (int)len - 1, name);
                find_scenes(c, child, depth + 1, out, n, max);
            } else if (len > 7 && strcmp(name + len - 7, ".fscene") == 0) {
                snprintf(out[(*n)++], 256, "%s%s%s", rel, rel[0] ? "/" : "", name);
            }
        }
    }
    free(l.names);
}

bool make_dirs(const char* path) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t n = strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;
    // each prefix, after the drive ("0:/") or the root
    size_t start = 0;
    const char* colon = strchr(tmp, ':');
    if (colon && colon[1] == '/') start = (size_t)(colon - tmp) + 2;
    else if (tmp[0] == '/') start = 1;
    for (size_t i = start; i <= n; i++) {
        if (tmp[i] == '/' || tmp[i] == 0) {
            char save = tmp[i];
            tmp[i] = 0;
            if (i > start && !platform::make_dir(tmp)) return false;
            tmp[i] = save;
        }
    }
    return true;
}

const char* path_extension_of(const char* path) {
    const char* dot = nullptr;
    for (const char* p = path; *p; p++) {
        if (*p == '.') dot = p;
        if (*p == '/' || *p == '\\') dot = nullptr;
    }
    return dot ? dot : "";
}

bool ends_with_ci(const char* s, const char* suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    if (a < b) return false;
    for (size_t i = 0; i < b; i++) {
        char ch = s[a - b + i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        if (ch != suffix[i]) return false;
    }
    return true;
}

}  // namespace

const char* export_default_player() {
#if FE_MINIMALOS
    return "0:/programs/feplayer.run";
#else
    return "build/host/bin/feplayer";
#endif
}

void export_default_out_dir(const char* name, char* out, size_t cap) {
    char f[96];
    project_file_name(name, f, sizeof f);
#if FE_MINIMALOS
    snprintf(out, cap, "0:/games/%s", f);
#else
    snprintf(out, cap, "export/%s", f);
#endif
}

// ---- MINIRUN1

uint8_t* run_bundle_add_resource(const uint8_t* run, size_t run_size, const char* name, const void* data, size_t size, size_t* out_size) {
    constexpr size_t kHeader = 16, kEntry = 72;     // "MINIRUN1", u32 version, u32 count; then name[64], u32 offset, u32 size
    if (run_size < kHeader || memcmp(run, "MINIRUN1", 8) != 0) return nullptr;
    auto rd32 = [](const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; };
    auto wr32 = [](uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); };
    uint32_t count = rd32(run + 12);
    if (strlen(name) >= 64 || kHeader + (size_t)count * kEntry > run_size) return nullptr;
    // Old payload (everything after the old table) moves by one table entry.
    size_t old_table = kHeader + (size_t)count * kEntry;
    size_t total = run_size + kEntry + size;
    uint8_t* out = (uint8_t*)malloc(total);
    if (!out) return nullptr;
    memcpy(out, run, kHeader);
    wr32(out + 12, count + 1);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = run + kHeader + (size_t)i * kEntry;
        uint8_t* o = out + kHeader + (size_t)i * kEntry;
        memcpy(o, e, 64);
        wr32(o + 64, rd32(e + 64) + (uint32_t)kEntry);
        wr32(o + 68, rd32(e + 68));
    }
    uint8_t* ne = out + old_table;
    memset(ne, 0, kEntry);
    memcpy(ne, name, strlen(name));
    wr32(ne + 64, (uint32_t)(run_size + kEntry));
    wr32(ne + 68, (uint32_t)size);
    memcpy(out + old_table + kEntry, run + old_table, run_size - old_table);
    memcpy(out + run_size + kEntry, data, size);
    if (out_size) *out_size = total;
    return out;
}

// ---- the export

bool export_game(const ExportOptions& opt, ExportReport* report) {
    ExportReport local;
    ExportReport* rep = report ? report : &local;
    *rep = ExportReport();
    Ctx* cp = new Ctx();
    Ctx& c = *cp;
    c.opt = &opt;
    c.rep = rep;
    struct Free { Ctx* p; ~Free() { delete p; } } guard{ cp };

    if (!opt.project_dir || !*opt.project_dir) return fail(c, "no project folder%s%s");
    if (!opt.out_dir || !*opt.out_dir) return fail(c, "no output folder%s%s");
    snprintf(c.project_dir, sizeof c.project_dir, "%s", opt.project_dir);
    for (size_t n = strlen(c.project_dir); n > 1 && c.project_dir[n - 1] == '/'; n--) c.project_dir[n - 1] = 0;
    if (strcmp(c.project_dir, ".") == 0) c.project_dir[0] = 0;

    // Project settings: project.fe when there is one.
    ProjectSettings ps;
    {
        char pf[1100];
        full_path(c, "project.fe", pf, sizeof pf);
        size_t n = 0;
        char* text = (char*)platform::read_file(pf, &n);
        if (text) {
            if (!project_parse(text, n, &ps)) say(c, 1, "project.fe is not a project file; using defaults");
            free(text);
        } else {
            say(c, 1, "no project.fe in %s; using defaults", c.project_dir[0] ? c.project_dir : ".");
        }
    }
    if (opt.name && *opt.name) snprintf(ps.name, sizeof ps.name, "%s", opt.name);
    if (opt.entry_scene && *opt.entry_scene) {
        char v[512];
        if (!virtual_path(c, opt.entry_scene, v, sizeof v)) return fail(c, "the entry scene %s is outside the project%s", opt.entry_scene);
        snprintf(ps.entry_scene, sizeof ps.entry_scene, "%s", v);
    }

    // Scenes: the entry scene first, then every other one in the project.
    static char scenes[128][256];
    int ns = 0;
    snprintf(scenes[ns++], 256, "%s", ps.entry_scene);
    if (opt.all_scenes) {
        int found = 0;
        static char found_scenes[128][256];
        find_scenes(c, "", 0, found_scenes, &found, 127);
        for (int i = 0; i < found && ns < 128; i++)
            if (strcmp(found_scenes[i], ps.entry_scene) != 0) snprintf(scenes[ns++], 256, "%s", found_scenes[i]);
    }
    bool entry_ok = false;
    for (int i = 0; i < ns; i++) {
        char full[1100];
        full_path(c, scenes[i], full, sizeof full);
        size_t n = 0;
        char* text = (char*)platform::read_file(full, &n);
        if (!text) {
            if (i == 0) return fail(c, "cannot read the entry scene %s%s", full);
            continue;
        }
        Scene* scene = new Scene();
        SceneLoadReport lr;
        if (!load_scene_text(*scene, text, n, &lr)) {
            delete scene;
            free(text);
            if (i == 0) return fail(c, "cannot load the entry scene %s: %s", scenes[i], lr.error);
            say(c, 1, "%s: %s; not packed", scenes[i], lr.error);
            continue;
        }
        if (i == 0) entry_ok = true;
        for (int k = 0; k < scene->count(); k++) collect_object(c, scene->at(k), scenes[i]);
        c.pak.add(scenes[i], text, n);
        rep->scenes++;
        say(c, 0, "scene %s: %d objects", scenes[i], scene->count());
        delete scene;
        free(text);
    }
    if (!entry_ok) return fail(c, "no entry scene%s%s");

    // The icon.
    void* icon = nullptr;
    size_t icon_size = 0;
    if (ps.icon[0]) {
        char v[512];
        if (virtual_path(c, ps.icon, v, sizeof v)) {
            char full[1100];
            full_path(c, v, full, sizeof full);
            icon = platform::read_file(full, &icon_size);
            if (icon) c.pak.add(v, icon, icon_size);
        }
        if (!icon) say(c, 1, "icon %s not found", ps.icon);
    }

    // graphics.cfg: the graphics settings the player applies at start.
    if (ps.graphics) {
        char full[1100];
        full_path(c, "graphics.cfg", full, sizeof full);
        size_t gn = 0;
        void* g = platform::read_file(full, &gn);
        if (g) { c.pak.add("graphics.cfg", g, gn); free(g); }
        else say(c, 0, "no graphics.cfg: the game uses the default graphics settings");
    }

    // project.fe as exported.
    {
        size_t n = 0;
        char* text = project_text(ps, &n);
        if (text) { c.pak.add("project.fe", text, n); free(text); }
    }

    // The package.
    char file[96];
    project_file_name(ps.name, file, sizeof file);
    if (!make_dirs(opt.out_dir)) { free(icon); return fail(c, "cannot create %s%s", opt.out_dir); }
    snprintf(rep->pak_path, sizeof rep->pak_path, "%s/%s.fepak", opt.out_dir, file);
    size_t pak_size = 0;
    uint8_t* pak = c.pak.build(&pak_size);
    if (!pak) { free(icon); return fail(c, "out of memory building the package%s%s"); }
    bool ok = platform::write_file(rep->pak_path, pak, pak_size);
    free(pak);
    if (!ok) { free(icon); return fail(c, "cannot write %s%s", rep->pak_path); }
    rep->files = c.pak.count();
    rep->pak_bytes = pak_size;
    say(c, 0, "wrote %s: %d files, %u bytes", rep->pak_path, rep->files, (unsigned)pak_size);

    // The player, next to it.
    const char* player = opt.player_path && *opt.player_path ? opt.player_path : export_default_player();
    size_t psize = 0;
    uint8_t* pdata = (uint8_t*)platform::read_file(player, &psize);
    if (!pdata) { free(icon); return fail(c, "the player program %s was not found (build it with make os / make host, or pass --player)%s", player); }
#if FE_MINIMALOS
    snprintf(rep->run_path, sizeof rep->run_path, "%s/%s.run", opt.out_dir, file);
#else
    snprintf(rep->run_path, sizeof rep->run_path, "%s/%s", opt.out_dir, file);
#endif
    if (icon && ends_with_ci(ps.icon, ".bmp")) {
        size_t bsize = 0;
        uint8_t* bundled = run_bundle_add_resource(pdata, psize, "Resources/icon.bmp", icon, icon_size, &bsize);
        if (bundled) { free(pdata); pdata = bundled; psize = bsize; }
    }
    ok = platform::write_file(rep->run_path, pdata, psize);
    if (ok && opt.mpkg) {
        // Installable package: the program in programs/, the package and icon in games/<Name>/.
        MpkgWriter mp;
        char nm[300];
        mp.add_dir("programs");
        mp.add_dir("games");
        snprintf(nm, sizeof nm, "games/%s", file);
        mp.add_dir(nm);
        snprintf(nm, sizeof nm, "programs/%s.run", file);
        bool good = mp.add_file(nm, pdata, psize);
        size_t pk_n = 0;
        void* pk = platform::read_file(rep->pak_path, &pk_n);
        snprintf(nm, sizeof nm, "games/%s/%s.fepak", file, file);
        good = good && pk && mp.add_file(nm, pk, pk_n);
        free(pk);
        if (icon) {
            snprintf(nm, sizeof nm, "games/%s/icon%s", file, path_extension_of(ps.icon));
            mp.add_file(nm, icon, icon_size);
        }
        size_t mn = 0;
        uint8_t* mimg = good ? mp.build(&mn) : nullptr;
        snprintf(rep->mpkg_path, sizeof rep->mpkg_path, "%s/%s.mpkg", opt.out_dir, file);
        if (!mimg || !platform::write_file(rep->mpkg_path, mimg, mn)) { rep->mpkg_path[0] = 0; say(c, 1, "could not write the .mpkg"); }
        else say(c, 0, "wrote %s (%u bytes)", rep->mpkg_path, (unsigned)mn);
        free(mimg);
    }
    free(pdata);
    if (!ok) { free(icon); return fail(c, "cannot write %s%s", rep->run_path); }
#if FE_HOST
    chmod(rep->run_path, 0755);
#endif
    say(c, 0, "wrote %s (%u bytes)", rep->run_path, (unsigned)psize);
    if (icon) {
        char ip[600];
        snprintf(ip, sizeof ip, "%s/icon%s", opt.out_dir, path_extension_of(ps.icon));
        platform::write_file(ip, icon, icon_size);
        free(icon);
    }
    return true;
}

// ---- command line

namespace {
void cli_log(int level, const char* line, void*) {
    printf("%s%s\n", level == 2 ? "error: " : (level == 1 ? "warning: " : ""), line);
}
}  // namespace

int export_cli(int argc, char** argv, int i) {
    if (i + 2 >= argc) {
        printf("FE EXPORT FAIL: usage: --export <project-or-scene> <outdir> [--name N] [--scene S] [--player P] [--mpkg] [--run [FRAMES]]\n");
        return 2;
    }
    const char* in = argv[i + 1];
    const char* out = argv[i + 2];
    ExportOptions opt;
    opt.out_dir = out;
    opt.log = cli_log;
    bool run = false;
    long run_frames = 0;
    for (int k = i + 3; k < argc; k++) {
        if (!strcmp(argv[k], "--name") && k + 1 < argc) opt.name = argv[++k];
        else if (!strcmp(argv[k], "--scene") && k + 1 < argc) opt.entry_scene = argv[++k];
        else if (!strcmp(argv[k], "--player") && k + 1 < argc) opt.player_path = argv[++k];
        else if (!strcmp(argv[k], "--entry-only")) opt.all_scenes = false;
        else if (!strcmp(argv[k], "--mpkg")) opt.mpkg = true;
        else if (!strcmp(argv[k], "--run")) {
            run = true;
            if (k + 1 < argc && argv[k + 1][0] >= '0' && argv[k + 1][0] <= '9') run_frames = atol(argv[++k]);
        }
    }
    // A scene file: its project is the folder above scenes/ (or the file's own folder).
    char project[512], scene_rel[512];
    project[0] = scene_rel[0] = 0;
    size_t len = strlen(in);
    if (len > 7 && strcmp(in + len - 7, ".fscene") == 0) {
        char dir[512];
        snprintf(dir, sizeof dir, "%s", in);
        char* slash = strrchr(dir, '/');
        const char* file = slash ? slash + 1 : dir;
        if (slash) *slash = 0; else strcpy(dir, ".");
        char* up = strrchr(dir, '/');
        bool in_scenes = (up ? strcmp(up + 1, "scenes") : strcmp(dir, "scenes")) == 0;
        if (in_scenes) {
            snprintf(scene_rel, sizeof scene_rel, "scenes/%s", file);
            if (up) { *up = 0; snprintf(project, sizeof project, "%s", dir); } else strcpy(project, ".");
        } else {
            snprintf(scene_rel, sizeof scene_rel, "%s", file);
            snprintf(project, sizeof project, "%s", dir);
        }
        if (!opt.entry_scene) opt.entry_scene = scene_rel;
    } else {
        snprintf(project, sizeof project, "%s", in);
    }
    opt.project_dir = project;
    ExportReport rep;
    if (!export_game(opt, &rep)) {
        printf("FE EXPORT FAIL: %s\n", rep.error);
        return 1;
    }
    printf("FE EXPORT OK %s files=%d scenes=%d bytes=%u missing=%d\n", rep.pak_path, rep.files, rep.scenes, (unsigned)rep.pak_bytes, rep.missing);
    if (run) {
        char frames[24];
        snprintf(frames, sizeof frames, "%ld", run_frames);
        const char* args[6] = { rep.run_path, rep.pak_path, nullptr, nullptr, nullptr, nullptr };
        if (run_frames > 0) { args[2] = "--frames"; args[3] = frames; }
        printf("FE EXPORT running %s\n", rep.run_path);
        fflush(stdout);
        int rc = platform::run_program(rep.run_path, args, true);
        printf("FE EXPORT game exited %d\n", rc);
        return rc == 0 ? 0 : 1;
    }
    return 0;
}

}  // namespace fe
