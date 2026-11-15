// Minimal-OS platform: the whole screen (SYS_GRAPHICS acquire, blit and
// present), raw USB keyboard and mouse (SYS_INPUT_READ, which the screen
// owner may read), MinimaFS files and SYS_SPAWN.
//
// The editor runs full screen for now: a window under mosWM comes once the
// desktop has been tested with OS-5's shared window frames.
#include "platform/platform.h"
#include "swraster.h"

#include "minimalos.h"
#include "graphics.h"
#include "process.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe::platform {

namespace {
Framebuffer g_fb;
uint32_t* g_pixels;
bool g_have_screen;
float g_mouse_x, g_mouse_y;
uint8_t g_buttons;
uint8_t g_mods;
Event g_pending[512];
int g_head, g_tail;
uint64_t g_t0;

void push(const Event& e) {
    int next = (g_tail + 1) % 512;
    if (next == g_head) return;
    g_pending[g_tail] = e;
    g_tail = next;
}

// USB HID usage codes (what the kernel queues) to editor keys.
uint16_t map_key(uint8_t hid) {
    if (hid >= 0x04 && hid <= 0x1D) return (uint16_t)(KEY_A + (hid - 0x04));
    if (hid >= 0x1E && hid <= 0x26) return (uint16_t)(KEY_0 + 1 + (hid - 0x1E));
    switch (hid) {
        case 0x27: return KEY_0;
        case 0x28: return KEY_ENTER;
        case 0x29: return KEY_ESCAPE;
        case 0x2A: return KEY_BACKSPACE;
        case 0x2B: return KEY_TAB;
        case 0x2C: return KEY_SPACE;
        case 0x3A: return KEY_F1;
        case 0x3B: return KEY_F2;
        case 0x3C: return KEY_F3;
        case 0x3D: return KEY_F4;
        case 0x3E: return KEY_F5;
        case 0x49: return KEY_INSERT;
        case 0x4A: return KEY_HOME;
        case 0x4B: return KEY_PAGEUP;
        case 0x4C: return KEY_DELETE;
        case 0x4D: return KEY_END;
        case 0x4E: return KEY_PAGEDOWN;
        case 0x4F: return KEY_RIGHT;
        case 0x50: return KEY_LEFT;
        case 0x51: return KEY_DOWN;
        case 0x52: return KEY_UP;
        case 0xE0: return KEY_LCTRL;
        case 0xE1: return KEY_LSHIFT;
        case 0xE2: return KEY_LALT;
        case 0xE3: return KEY_LSUPER;
        case 0xE4: return KEY_RCTRL;
        case 0xE5: return KEY_RSHIFT;
        case 0xE6: return KEY_RALT;
        case 0xE7: return KEY_RSUPER;
        default: return KEY_NONE;
    }
}

// The character a key types (US layout), or 0.
uint32_t key_char(uint8_t hid, bool shift) {
    if (hid >= 0x04 && hid <= 0x1D) return (uint32_t)((shift ? 'A' : 'a') + (hid - 0x04));
    static const char digits[] = "1234567890";
    static const char shifted_digits[] = "!@#$%^&*()";
    if (hid >= 0x1E && hid <= 0x27) return (uint32_t)(shift ? shifted_digits : digits)[hid - 0x1E];
    switch (hid) {
        case 0x2C: return ' ';
        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        default: return 0;
    }
}

uint8_t to_mods(uint8_t m) {
    return (uint8_t)(((m & SYSCALL_INPUT_MOD_SHIFT) ? MOD_SHIFT : 0) | ((m & SYSCALL_INPUT_MOD_CTRL) ? MOD_CTRL : 0) |
                     ((m & SYSCALL_INPUT_MOD_ALT) ? MOD_ALT : 0));
}

// Reads raw input (waiting up to timeout_ms for the first event) and turns
// it into editor events.
void pump(uint32_t timeout_ms) {
    syscall_input_event_t evs[64];
    syscall_input_read_request_t req;
    memset(&req, 0, sizeof(req));
    req.size = sizeof(req);
    req.version = SYS_INPUT_ABI_VERSION;
    req.events = evs;
    req.capacity = 64;
    req.timeout_ms = timeout_ms;
    long n = mos_syscall(SYS_INPUT_READ, (long)(uintptr_t)&req, 0, 0);
    if (n <= 0) return;
    for (long i = 0; i < n; i++) {
        const syscall_input_event_t& r = evs[i];
        Event e;
        switch (r.type) {
            case SYSCALL_INPUT_MOUSE_MOVE:
                g_mouse_x += (float)r.dx;
                g_mouse_y += (float)r.dy;
                if (g_mouse_x < 0) g_mouse_x = 0;
                if (g_mouse_y < 0) g_mouse_y = 0;
                if (g_mouse_x > g_fb.width - 1) g_mouse_x = (float)(g_fb.width - 1);
                if (g_mouse_y > g_fb.height - 1) g_mouse_y = (float)(g_fb.height - 1);
                e.type = EventType::MouseMove;
                e.x = g_mouse_x;
                e.y = g_mouse_y;
                push(e);
                break;
            case SYSCALL_INPUT_MOUSE_BUTTON: {
                static const uint8_t bits[3] = { SYSCALL_INPUT_BTN_LEFT, SYSCALL_INPUT_BTN_RIGHT, SYSCALL_INPUT_BTN_MIDDLE };
                for (int b = 0; b < 3; b++) {
                    bool was = (g_buttons & bits[b]) != 0, now = (r.buttons & bits[b]) != 0;
                    if (was == now) continue;
                    e = Event();
                    e.type = EventType::MouseButton;
                    e.x = g_mouse_x;
                    e.y = g_mouse_y;
                    e.button = b;
                    e.down = now;
                    e.mods = g_mods;
                    push(e);
                }
                g_buttons = r.buttons;
                break;
            }
            case SYSCALL_INPUT_MOUSE_WHEEL:
                e.type = EventType::MouseWheel;
                e.wheel_y = (float)-r.wheel;
                push(e);
                break;
            case SYSCALL_INPUT_KEY_DOWN:
            case SYSCALL_INPUT_KEY_UP: {
                bool down = r.type == SYSCALL_INPUT_KEY_DOWN;
                g_mods = to_mods(r.mods);
                e.type = EventType::Key;
                e.key = map_key(r.key);
                e.down = down;
                e.mods = g_mods;
                if (e.key != KEY_NONE) push(e);
                if (down && !(g_mods & (MOD_CTRL | MOD_ALT))) {
                    uint32_t ch = key_char(r.key, (g_mods & MOD_SHIFT) != 0);
                    if (ch) {
                        Event t;
                        t.type = EventType::Text;
                        t.codepoint = ch;
                        push(t);
                    }
                }
                break;
            }
            default: break;
        }
    }
}
}  // namespace

bool init(int width, int height) {
    if (mos_graphics_acquire() != 0) {
        printf("farcade: the screen is in use\n");
        return false;
    }
    g_have_screen = true;
    syscall_graphics_info_t info;
    memset(&info, 0, sizeof(info));
    if (mos_graphics_info(&info) != 0 || !info.width || !info.height) {
        mos_graphics_release();
        g_have_screen = false;
        return false;
    }
    int w = (int)info.width, h = (int)info.height;
    if (width > 0 && width < w) w = width;
    if (height > 0 && height < h) h = height;
    g_pixels = (uint32_t*)malloc((size_t)w * h * 4);
    if (!g_pixels) return false;
    g_fb.pixels = g_pixels;
    g_fb.width = w;
    g_fb.height = h;
    g_fb.pitch = w;
    g_mouse_x = w * 0.5f;
    g_mouse_y = h * 0.5f;
    g_t0 = mos_clock_monotonic_ns();
    return true;
}

void shutdown() {
    if (g_have_screen) mos_graphics_release();
    g_have_screen = false;
    free(g_pixels);
    g_pixels = nullptr;
}

int width() { return g_fb.width; }
int height() { return g_fb.height; }
Framebuffer& framebuffer() { return g_fb; }

void present() {
    mos_graphics_blit_surface(g_fb.pixels, (uint32_t)g_fb.width, (uint32_t)g_fb.height, (uint32_t)g_fb.pitch * 4, 0, 0,
                              g_fb.width, g_fb.height, 0, 0, SYS_GRAPHICS_BLIT_OPAQUE);
    mos_graphics_present(0, 0, g_fb.width, g_fb.height);
}

void present_region(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_fb.width) w = g_fb.width - x;
    if (y + h > g_fb.height) h = g_fb.height - y;
    if (w <= 0 || h <= 0) return;
    mos_graphics_blit_surface(g_fb.pixels, (uint32_t)g_fb.width, (uint32_t)g_fb.height, (uint32_t)g_fb.pitch * 4, x, y, w, h, x, y,
                              SYS_GRAPHICS_BLIT_OPAQUE);
    mos_graphics_present(x, y, w, h);
}

bool poll(Event& e) {
    if (g_head == g_tail) pump(0);
    if (g_head == g_tail) return false;
    e = g_pending[g_head];
    g_head = (g_head + 1) % 512;
    return true;
}

void wait_event(int timeout_ms) {
    if (g_head != g_tail) return;
    pump(timeout_ms < 0 ? 0 : (uint32_t)timeout_ms);
}

double time_seconds() { return (double)(mos_clock_monotonic_ns() - g_t0) * 1e-9; }

// ---- files

bool list_dir(const char* path, void (*fn)(const DirEntry& e, void* user), void* user) {
    static syscall_dirent_t ents[512];
    long n = mos_listdir(path, ents, 512);
    if (n < 0) return false;
    for (long i = 0; i < n; i++) {
        if (ents[i].hidden) continue;
        DirEntry e;
        snprintf(e.name, sizeof(e.name), "%s", ents[i].name);
        e.is_dir = ents[i].type == SYSCALL_DIRENT_TYPE_DIR;
        e.size = 0;
        if (!e.is_dir) {
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", path, ents[i].name);
            syscall_stat_t st;
            if (mos_stat(full, &st) == 0) e.size = st.size;
        }
        fn(e, user);
    }
    return true;
}

bool file_exists(const char* path) {
    syscall_stat_t st;
    return mos_stat(path, &st) == 0;
}

void* read_file(const char* path, size_t* size) {
    syscall_stat_t st;
    if (mos_stat(path, &st) != 0 || st.type != SYS_STAT_FILE) return nullptr;
    long fd = mos_open(path, SYS_O_RDONLY);
    if (fd < 0) return nullptr;
    uint8_t* data = (uint8_t*)malloc(st.size ? (size_t)st.size : 1);
    size_t got = 0;
    while (data && got < st.size) {
        long r = mos_read(fd, data + got, (size_t)(st.size - got));
        if (r <= 0) break;
        got += (size_t)r;
    }
    mos_close(fd);
    if (data && got != st.size) { free(data); data = nullptr; }
    if (data && size) *size = got;
    return data;
}

// MinimaFS has no "create or truncate": the data goes into a new file next
// to `path` (mos_create, then SYS_FWRITE), which then replaces the old one,
// so a failed write leaves the old file as it was.
bool write_file(const char* path, const void* data, size_t size) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    if (mos_exists(tmp)) mos_delete(tmp);
    if (mos_create(tmp, nullptr, nullptr) < 0) return false;
    long fd = mos_open(tmp, SYS_O_RDWR);
    if (fd < 0) { mos_delete(tmp); return false; }
    size_t done = 0;
    while (done < size) {
        long w = mos_fwrite(fd, (const uint8_t*)data + done, size - done);
        if (w <= 0) break;
        done += (size_t)w;
    }
    mos_close(fd);
    if (done != size) { mos_delete(tmp); return false; }
    if (mos_exists(path) && mos_delete(path) < 0) { mos_delete(tmp); return false; }
    return mos_rename(tmp, path) == 0;
}

bool copy_file(const char* from, const char* to) {
    size_t n = 0;
    void* d = read_file(from, &n);
    if (!d) return false;
    bool ok = write_file(to, d, n);
    free(d);
    return ok;
}

bool make_dir(const char* path) { return mos_mkdir(path) == 0 || file_exists(path); }

const char* default_project_root() {
    make_dir("0:/projects");
    return "0:/projects";
}

// ---- other programs

bool open_with(const char* program, const char* file) {
    const char* argv[3] = { program, file, nullptr };
    return mos_spawn(program, argv) > 0;
}

bool reveal_in_file_manager(const char*) { return false; }

const char* reveal_unavailable_reason() {
    return "MOSdesk's file browser is part of mosWM and cannot be asked to open a folder yet";
}

}  // namespace fe::platform
