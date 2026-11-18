// Model file conversion (.gltf/.glb/.obj -> .mof), no editor needed.
#include "model_import.h"
#include "platform/platform.h"
#include "mof/mof_import.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe {

bool path_is_importable_model(const char* path) {
    const char* e = strrchr(path, '.');
    if (!e || strrchr(path, '/') > e) return false;
    char l[8] = {};
    for (int i = 0; e[i] && i < 7; i++) l[i] = (char)((e[i] >= 'A' && e[i] <= 'Z') ? e[i] + 32 : e[i]);
    return !strcmp(l, ".gltf") || !strcmp(l, ".glb") || !strcmp(l, ".obj");
}

bool model_convert_file(const char* src, char* out_path, size_t out_cap, char* message, size_t message_cap) {
    message[0] = 0;
    mof_model model;
    char err[256] = {};
    if (!mof_import_file(src, &model, err, sizeof err)) {
        snprintf(message, message_cap, "%s", err[0] ? err : "cannot import the file");
        return false;
    }
    mof_buffer buf;
    mof_buffer_init(&buf, mof_libc_alloc());
    bool ok = mof_write(&model.desc, &buf, err, sizeof err);
    if (ok) {
        snprintf(out_path, out_cap, "%s", src);
        char* dot = strrchr(out_path, '.');
        char* slash = strrchr(out_path, '/');
        if (dot && (!slash || dot > slash)) *dot = 0;
        size_t n = strlen(out_path);
        snprintf(out_path + n, out_cap - n, ".mof");
        ok = platform::write_file(out_path, buf.data, buf.size);
        if (!ok) snprintf(err, sizeof err, "cannot write %s", out_path);
    }
    if (ok) snprintf(message, message_cap, "%s", model.warnings);
    else snprintf(message, message_cap, "%s", err);
    mof_buffer_free(&buf);
    mof_model_free(&model);
    return ok;
}

}  // namespace fe
