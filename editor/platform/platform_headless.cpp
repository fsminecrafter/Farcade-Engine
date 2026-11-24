// Linux headless platform: an in-memory framebuffer, events fed by a test
// or script (headless_push), PNG output (headless_save_png), POSIX files.
#include "platform/platform.h"
#include "platform/headless.h"
#include "swraster.h"

#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define STB_IMAGE_WRITE_STATIC           // the importers (mof_ext_impl.c) have their own copy
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace fe::platform {

namespace {
Framebuffer g_fb;
uint32_t* g_pixels;
Event g_queue[4096];
int g_head, g_tail;
double g_fake_time = -1.0;      // < 0: real clock
int g_presents;
char g_last_open[512];
char g_last_reveal[512];
}  // namespace

namespace {
int g_opt_w, g_opt_h;
}
void set_window_options(const char*, int width, int height) { g_opt_w = width; g_opt_h = height; }

bool init(int width, int height) {
    if (width <= 0) width = g_opt_w > 0 ? g_opt_w : 1280;
    if (height <= 0) height = g_opt_h > 0 ? g_opt_h : 800;
    g_pixels = (uint32_t*)calloc((size_t)width * height, 4);
    if (!g_pixels) return false;
    g_fb.pixels = g_pixels;
    g_fb.width = width;
    g_fb.height = height;
    g_fb.pitch = width;
    return true;
}

void shutdown() {
    free(g_pixels);
    g_pixels = nullptr;
    g_fb = Framebuffer();
}

int width() { return g_fb.width; }
int height() { return g_fb.height; }
Framebuffer& framebuffer() { return g_fb; }
void present() { g_presents++; }
void present_region(int, int, int, int) { g_presents++; }

bool poll(Event& e) {
    if (g_head == g_tail) return false;
    e = g_queue[g_head];
    g_head = (g_head + 1) % 4096;
    return true;
}

void wait_event(int) {}

bool windowed() { return false; }
bool toggle_fullscreen() { return true; }
bool is_fullscreen() { return true; }
void set_vsync(int, int) {}

bool gpu_stats(GpuStats* out) { (void)out; return false; }

double time_seconds() {
    if (g_fake_time >= 0) return g_fake_time;
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

bool list_dir(const char* path, void (*fn)(const DirEntry& e, void* user), void* user) {
    DIR* d = opendir(path);
    if (!d) return false;
    while (dirent* de = readdir(d)) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        DirEntry e;
        snprintf(e.name, sizeof(e.name), "%s", de->d_name);
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        struct stat st;
        e.is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        e.size = e.is_dir ? 0 : (uint64_t)st.st_size;
        fn(e, user);
    }
    closedir(d);
    return true;
}

bool file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

void* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* data = malloc(n > 0 ? (size_t)n : 1);
    if (data && n > 0 && fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = nullptr; }
    fclose(f);
    if (data && size) *size = (size_t)n;
    return data;
}

bool write_file(const char* path, const void* data, size_t size) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}

bool copy_file(const char* from, const char* to) {
    size_t n = 0;
    void* data = read_file(from, &n);
    if (!data) return false;
    bool ok = write_file(to, data, n);
    free(data);
    return ok;
}

bool make_dir(const char* path) { return mkdir(path, 0755) == 0 || file_exists(path); }

namespace {
void remove_child(const DirEntry& e, void* user) {
    char full[1024];
    snprintf(full, sizeof full, "%s/%s", (const char*)user, e.name);
    remove_path(full);
}
}  // namespace

bool remove_path(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;
    list_dir(path, remove_child, (void*)path);
    return rmdir(path) == 0;
}

bool rename_path(const char* from, const char* to) {
    struct stat st;
    if (stat(to, &st) == 0) return false;
    return rename(from, to) == 0;
}

void* open_read(const char* path, uint64_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) { fclose(f); return nullptr; }
    if (size) *size = (uint64_t)n;
    return f;
}

bool read_at(void* handle, uint64_t offset, void* dst, size_t n) {
    FILE* f = (FILE*)handle;
    if (fseek(f, (long)offset, SEEK_SET) != 0) return false;
    return n == 0 || fread(dst, 1, n, f) == n;
}

void close_file(void* handle) { fclose((FILE*)handle); }

const char* default_project_root() { return "."; }

bool open_with(const char* program, const char* file) {
    snprintf(g_last_open, sizeof(g_last_open), "%s %s", program, file);
    return true;
}

int run_program(const char* program, const char* const* argv, bool wait) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execv(program, (char* const*)argv);
        _exit(127);
    }
    if (!wait) return (int)pid;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool reveal_in_file_manager(const char* path) {
    snprintf(g_last_reveal, sizeof(g_last_reveal), "%s", path);
    return true;
}

const char* reveal_unavailable_reason() { return nullptr; }

// ---- headless-only controls

void headless_push(const Event& e) {
    int next = (g_tail + 1) % 4096;
    if (next == g_head) return;
    g_queue[g_tail] = e;
    g_tail = next;
}

void headless_set_time(double t) { g_fake_time = t; }

bool headless_save_png(const char* path) {
    int w = g_fb.width, h = g_fb.height;
    uint8_t* rgb = (uint8_t*)malloc((size_t)w * h * 3);
    if (!rgb) return false;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t p = g_fb.pixels[(size_t)y * g_fb.pitch + x];
            uint8_t* o = rgb + ((size_t)y * w + x) * 3;
            o[0] = (uint8_t)(p >> 16);
            o[1] = (uint8_t)(p >> 8);
            o[2] = (uint8_t)p;
        }
    int ok = stbi_write_png(path, w, h, 3, rgb, w * 3);
    free(rgb);
    return ok != 0;
}

const char* headless_last_open() { return g_last_open; }
const char* headless_last_reveal() { return g_last_reveal; }

}  // namespace fe::platform
