// feplayer: runs an exported game.
//
//   feplayer [game.fepak | project-folder] [--scene path] [--frames N]
//            [--png out.png] [--scale N] [--no-audio]
//
// With no package argument it looks next to itself for <program name>.fepak
// (Demo.run -> Demo.fepak), then game.fepak, then 0:/games/<program name>/.
// Esc or closing the window quits. Prints "FE PLAYER READY ..." after the first
// frame and "FE PLAYER DONE ..." at the end; lines the game's scripts print
// are passed through.
#include "player.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace fe;

namespace {

bool ends_with(const char* s, const char* suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    if (a < b) return false;
    for (size_t i = 0; i < b; i++) {
        char c = s[a - b + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != suffix[i]) return false;
    }
    return true;
}

// Finds the package that belongs to this program.
bool find_pak(const char* argv0, char* out, size_t cap) {
    char dir[256] = {}, base[128] = {};
    if (argv0 && *argv0) {
        const char* slash = nullptr;
        for (const char* p = argv0; *p; p++)
            if (*p == '/' || *p == '\\') slash = p;
        const char* name = slash ? slash + 1 : argv0;
        if (slash) snprintf(dir, sizeof dir, "%.*s", (int)(slash - argv0), argv0);
        snprintf(base, sizeof base, "%s", name);
        char* dot = strrchr(base, '.');
        if (dot) *dot = 0;
    }
    char cand[512];
    if (dir[0] && base[0]) {
        snprintf(cand, sizeof cand, "%s/%s.fepak", dir, base);
        if (platform::file_exists(cand)) { snprintf(out, cap, "%s", cand); return true; }
        snprintf(cand, sizeof cand, "%s/game.fepak", dir);
        if (platform::file_exists(cand)) { snprintf(out, cap, "%s", cand); return true; }
    }
    if (base[0]) {
        snprintf(cand, sizeof cand, "0:/games/%s/%s.fepak", base, base);
        if (platform::file_exists(cand)) { snprintf(out, cap, "%s", cand); return true; }
    }
    if (base[0]) {
        snprintf(cand, sizeof cand, "%s.fepak", base);
        if (platform::file_exists(cand)) { snprintf(out, cap, "%s", cand); return true; }
    }
    if (platform::file_exists("game.fepak")) { snprintf(out, cap, "game.fepak"); return true; }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    player::Options opt;
    char pak[512] = {};
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "--frames") && i + 1 < argc) opt.frames = atol(argv[++i]);
        else if (!strcmp(a, "--png") && i + 1 < argc) opt.png_path = argv[++i];
        else if (!strcmp(a, "--scene") && i + 1 < argc) opt.scene = argv[++i];
        else if (!strcmp(a, "--scale") && i + 1 < argc) opt.render_div = atoi(argv[++i]);
        else if (!strcmp(a, "--no-audio")) opt.audio = false;
        else if (!strcmp(a, "--quiet")) opt.quiet = true;
        else if (a[0] == '-') { printf("feplayer: unknown option %s\n", a); return 2; }
        else if (ends_with(a, ".fepak")) opt.pak_path = a;
        else opt.project_dir = a;
    }
    if (!opt.pak_path && !opt.project_dir) {
        if (find_pak(argc > 0 ? argv[0] : nullptr, pak, sizeof pak)) opt.pak_path = pak;
        else {
            printf("FE PLAYER FAIL: no game package (usage: feplayer game.fepak | project-folder)\n");
            return 1;
        }
    }
    player::Result r;
    int rc = player::run(opt, &r);
    if (rc != 0) printf("FE PLAYER FAIL: %s\n", r.error);
    return rc;
}
