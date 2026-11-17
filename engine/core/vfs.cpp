#include "core/vfs.h"
#include "core/pak.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe::vfs {

namespace {
constexpr int kMaxPaks = 8;
PakReader* g_paks[kMaxPaks];
int g_pak_count = 0;
Disk g_disk;
FileAccess g_access;
char g_root[256];
bool g_fallback = true;

struct FileSrc { FileAccess fa; void* handle; };
bool file_read_at(void* user, uint64_t off, void* dst, size_t n) { FileSrc* f = (FileSrc*)user; return f->fa.read_at(f->handle, off, dst, n); }
void file_close(void* user) { FileSrc* f = (FileSrc*)user; if (f->fa.close) f->fa.close(f->handle); free(f); }

struct OwnedBlock { uint8_t* data; size_t size; };
bool block_read(void* user, uint64_t off, void* dst, size_t n) {
    OwnedBlock* b = (OwnedBlock*)user;
    if (off > b->size || n > b->size - off) return false;
    memcpy(dst, b->data + off, n);
    return true;
}
void block_close(void* user) { OwnedBlock* b = (OwnedBlock*)user; free(b->data); free(b); }

bool add_reader(const PakSource& src, const char** error) {
    if (g_pak_count == kMaxPaks) { if (error) *error = "too many packages"; return false; }
    PakReader* r = new PakReader();
    if (!r->open(src)) {
        if (error) *error = r->error();
        delete r;
        return false;
    }
    g_paks[g_pak_count++] = r;
    return true;
}
}  // namespace

void set_disk(const Disk& disk) { g_disk = disk; }
void set_file_access(const FileAccess& fa) { g_access = fa; }
void set_root(const char* dir) { snprintf(g_root, sizeof g_root, "%s", dir ? dir : ""); size_t n = strlen(g_root); while (n && g_root[n - 1] == '/') g_root[--n] = 0; }
const char* root() { return g_root; }
void set_disk_fallback(bool on) { g_fallback = on; }
int mounted_paks() { return g_pak_count; }

bool mount_pak_file(const char* file_path, const char** error) {
    if (g_access.open && g_access.read_at) {
        uint64_t size = 0;
        void* h = g_access.open(file_path, &size);
        if (!h) { if (error) *error = "cannot open the package"; return false; }
        FileSrc* f = (FileSrc*)malloc(sizeof(FileSrc));
        f->fa = g_access;
        f->handle = h;
        PakSource s;
        s.user = f;
        s.read_at = file_read_at;
        s.size = size;
        s.close = file_close;
        return add_reader(s, error);        // closes the file when it fails
    }
    if (!g_disk.read_file) { if (error) *error = "no file reader"; return false; }
    size_t size = 0;
    void* data = g_disk.read_file(file_path, &size);
    if (!data) { if (error) *error = "cannot read the package"; return false; }
    OwnedBlock* b = (OwnedBlock*)malloc(sizeof(OwnedBlock));
    b->data = (uint8_t*)data;
    b->size = size;
    PakSource s;
    s.user = b;
    s.read_at = block_read;
    s.size = size;
    s.close = block_close;
    return add_reader(s, error);
}

bool mount_pak_memory(const void* data, size_t size, const char** error) {
    return add_reader(pak_memory_source(data, size), error);
}

void unmount_all() {
    for (int i = 0; i < g_pak_count; i++) delete g_paks[i];
    g_pak_count = 0;
}

void normalize(const char* path, char* out, size_t cap) {
    size_t n = 0;
    const char* p = path;
    // Strip the project root.
    size_t rl = strlen(g_root);
    char tmp[512];
    size_t t = 0;
    for (; *p && t + 1 < sizeof tmp; p++) tmp[t++] = *p == '\\' ? '/' : *p;
    tmp[t] = 0;
    p = tmp;
    if (rl && strncmp(p, g_root, rl) == 0 && p[rl] == '/') p += rl + 1;
    while (*p) {
        if (p[0] == '/' && p[1] == '/') { p++; continue; }
        if ((p == tmp || p[-1] == '/') && p[0] == '.' && p[1] == '/') { p += 2; continue; }
        if (n + 1 < cap) out[n++] = *p;
        p++;
    }
    out[n] = 0;
}

void* read(const char* path, size_t* size) {
    char v[512];
    normalize(path, v, sizeof v);
    for (int i = g_pak_count - 1; i >= 0; i--) {
        PakEntryInfo e;
        if (g_paks[i]->find(v, &e)) return g_paks[i]->read(e, size);
    }
    if (!g_fallback || !g_disk.read_file) return nullptr;
    // The path as given first (a full path), then under the root.
    size_t n = 0;
    void* data = g_disk.read_file(path, &n);
    if (!data && g_root[0] && v[0] != '/' && !strchr(v, ':')) {
        char full[800];
        snprintf(full, sizeof full, "%s/%s", g_root, v);
        data = g_disk.read_file(full, &n);
    }
    if (!data) return nullptr;
    // One zero byte after the data, as with packaged files.
    void* grown = realloc(data, n + 1);
    if (grown) { data = grown; ((uint8_t*)data)[n] = 0; }
    if (size) *size = n;
    return data;
}

bool exists(const char* path) {
    char v[512];
    normalize(path, v, sizeof v);
    for (int i = g_pak_count - 1; i >= 0; i--)
        if (g_paks[i]->find(v, nullptr)) return true;
    if (!g_fallback || !g_disk.file_exists) return false;
    if (g_disk.file_exists(path)) return true;
    if (g_root[0] && v[0] != '/' && !strchr(v, ':')) {
        char full[800];
        snprintf(full, sizeof full, "%s/%s", g_root, v);
        return g_disk.file_exists(full);
    }
    return false;
}

void* read_cb(const char* path, size_t* size, void*) { return read(path, size); }

}  // namespace fe::vfs
