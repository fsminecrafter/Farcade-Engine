// mofconv: convert glTF / OBJ models to .mof and inspect .mof files.
//
//   mofconv input.(gltf|glb|obj) output.mof [--info]
//   mofconv --info file.mof
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mof/mof.h"
#include "mof/mof_import.h"
#include "mof_info.h"

static bool ends_with(const char* s, const char* suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    if (a < b) return false;
    for (size_t i = 0; i < b; i++) {
        char c = s[a - b + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != suffix[i]) return false;
    }
    return true;
}

static int show_info_file(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "mofconv: cannot open %s\n", path); return 1; }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 0) { fclose(fp); return 1; }
    size_t cap = ((size_t)n + 16) & ~(size_t)15;
    void* buf = aligned_alloc(16, cap ? cap : 16);  // reader needs a 16-byte aligned buffer
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) { fprintf(stderr, "mofconv: read error\n"); fclose(fp); free(buf); return 1; }
    fclose(fp);
    mof_file f;
    char err[256];
    if (!mof_open(buf, (size_t)n, &f, err, sizeof err)) {
        fprintf(stderr, "mofconv: %s: invalid .mof: %s\n", path, err);
        free(buf);
        return 1;
    }
    printf("%s\n", path);
    mof_print_info(&f, stdout);
    free(buf);
    return 0;
}

static void usage() {
    fprintf(stderr,
            "usage: mofconv input.(gltf|glb|obj) output.mof [--info]\n"
            "       mofconv --info file.mof\n");
}

int main(int argc, char** argv) {
    const char* pos[2] = {nullptr, nullptr};
    int npos = 0;
    bool info = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--info")) info = true;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else if (npos < 2) pos[npos++] = argv[i];
        else { usage(); return 2; }
    }
    if (npos == 1 && info && ends_with(pos[0], ".mof")) return show_info_file(pos[0]);
    if (npos != 2) { usage(); return 2; }

    mof_model model;
    char err[512];
    if (!mof_import_file(pos[0], &model, err, sizeof err)) {
        fprintf(stderr, "mofconv: %s\n", err);
        return 1;
    }
    if (model.warnings[0]) fprintf(stderr, "mofconv: warnings:\n%s", model.warnings);
    mof_buffer buf;
    bool ok = mof_write(&model.desc, &buf, err, sizeof err);
    mof_model_free(&model);
    if (!ok) { fprintf(stderr, "mofconv: write failed: %s\n", err); return 1; }
    FILE* fp = fopen(pos[1], "wb");
    if (!fp || fwrite(buf.data, 1, buf.size, fp) != buf.size) {
        fprintf(stderr, "mofconv: cannot write %s\n", pos[1]);
        if (fp) fclose(fp);
        mof_buffer_free(&buf);
        return 1;
    }
    fclose(fp);
    printf("wrote %s (%zu bytes)\n", pos[1], buf.size);
    mof_buffer_free(&buf);
    return info ? show_info_file(pos[1]) : 0;
}
