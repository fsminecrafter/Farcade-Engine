// What the editor needs from the system it runs on.
//
// Two implementations: platform_minimalos.cpp (full screen on Minimal-OS:
// framebuffer, USB keyboard and mouse, MinimaFS, spawning programs) and
// platform_headless.cpp (Linux: no window; input comes from a script and
// frames can be written to PNG files, for tests and screenshots).
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace fe {
struct Framebuffer;
}

namespace fe::platform {

enum class EventType : uint8_t { None, MouseMove, MouseButton, MouseWheel, Key, Text, Resize, Quit };

// Keys the editor cares about beyond text input.
enum Key : uint16_t {
    KEY_NONE = 0, KEY_TAB, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_PAGEUP, KEY_PAGEDOWN,
    KEY_HOME, KEY_END, KEY_INSERT, KEY_DELETE, KEY_BACKSPACE, KEY_SPACE, KEY_ENTER, KEY_ESCAPE,
    KEY_LCTRL, KEY_LSHIFT, KEY_LALT, KEY_LSUPER, KEY_RCTRL, KEY_RSHIFT, KEY_RALT, KEY_RSUPER,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_A = 0x100, // KEY_A + n for letters a..z
    KEY_0 = 0x120, // KEY_0 + n for digits
};

enum Mod : uint8_t { MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4, MOD_SUPER = 8 };

struct Event {
    EventType type = EventType::None;
    float x = 0, y = 0;         // MouseMove / MouseButton: position in pixels
    int button = 0;             // MouseButton: 0 left, 1 right, 2 middle
    bool down = false;          // MouseButton / Key
    float wheel_x = 0, wheel_y = 0;
    uint16_t key = KEY_NONE;    // Key
    uint8_t mods = 0;           // Mod bits at the time of the event
    uint32_t codepoint = 0;     // Text
    int width = 0, height = 0;  // Resize
};

// Opens the screen (or a headless surface) of the given size; 0x0 = the
// whole screen. False when the screen cannot be taken.
bool init(int width, int height);
void shutdown();

int width();
int height();

// The framebuffer to draw into this frame (32-bit 0x00RRGGBB).
Framebuffer& framebuffer();
// Shows the framebuffer.
void present();
// Shows only a rectangle of it (the rest of the screen already shows the
// framebuffer's previous content there).
void present_region(int x, int y, int w, int h);

// Takes the next event; false when there is none.
bool poll(Event& e);
// Sleeps until an event arrives or timeout_ms passes (the editor only
// redraws when something changes).
void wait_event(int timeout_ms);

double time_seconds();
// True when the editor is a window on a desktop (it draws no mouse cursor
// of its own then); false when it has the whole screen.
bool windowed();

// ---- files

struct DirEntry {
    char name[128];
    bool is_dir;
    uint64_t size;
};

// Calls fn for every entry of `path` (not "." and ".."); false when the
// directory cannot be read.
bool list_dir(const char* path, void (*fn)(const DirEntry& e, void* user), void* user);
bool file_exists(const char* path);
// Reads a whole file into memory from malloc (caller frees); NULL on error.
void* read_file(const char* path, size_t* size);
bool write_file(const char* path, const void* data, size_t size);
bool copy_file(const char* from, const char* to);
bool make_dir(const char* path);

// The directory projects start in ("0:/projects" on Minimal-OS).
const char* default_project_root();

// ---- other programs

// Starts `program` with `file` as its argument. False when it could not
// be started.
bool open_with(const char* program, const char* file);
// Shows `path` in the system's file manager. False when the system has no
// way to do that yet.
bool reveal_in_file_manager(const char* path);
// A short line describing why reveal_in_file_manager() is not available, or
// NULL when it is.
const char* reveal_unavailable_reason();

}  // namespace fe::platform
