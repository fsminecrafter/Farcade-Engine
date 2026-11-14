// The Farcade editor: state shared by the panels, and the frame loop.
#pragma once

#include "scene.h"
#include <stdint.h>

struct ImFont;

namespace fe {

enum class Tool : uint8_t { Select, Move, Rotate, Scale };
enum class LogLevel : uint8_t { Info, Warning, Error };

// What opens a file on double-click: a program per extension (Settings).
struct FileEditor {
    char extension[16];         // ".lua", or "*" for everything else
    char program[192];          // e.g. "0:/programs/edit.run"
};

struct Settings {
    bool dark_theme = true;
    float ui_scale = 1.0f;
    float grid_size = 1.0f;             // also the move snap
    int grid_lines = 20;                // to each side of the origin
    bool show_grid = true;
    bool snap = false;
    float snap_move = 0.5f;
    float snap_rotate = 15.0f;          // degrees
    float snap_scale = 0.1f;
    FileEditor editors[16];
    int editor_count = 0;
    const char* program_for(const char* path) const;
};

struct Camera {
    Vec3 target = Vec3(0, 0.5f, 0);
    float yaw = 35.0f, pitch = 25.0f;   // degrees
    float distance = 9.0f;
    float fov = 50.0f;
    Vec3 eye() const;
    Mat4 view() const;
    Mat4 projection(float aspect) const;
};

class Editor {
public:
    bool init(int width, int height);
    // One UI frame: builds the UI and renders it into the platform's
    // framebuffer. Returns false when the editor should quit.
    bool frame();
    void shutdown();

    // ---- used by the panels
    Scene scene;
    Settings settings;
    Camera camera;
    Tool tool = Tool::Move;
    bool gizmo_local = false;
    char project_dir[256] = {};
    char files_dir[256] = {};

    void log(LogLevel level, const char* fmt, ...);
    void open_file(const char* path);           // double-click in Files
    void apply_file_to_object(const char* path, ObjectId id);   // a file dropped on an object
    ObjectId create_object(const char* name, MeshKind mesh, const Vec3& at);
    void delete_selection();
    void duplicate_selection();
    void frame_selection();
    void apply_theme();

    ImFont* font_ui = nullptr;
    ImFont* font_mono = nullptr;
    bool show_settings = false;
    bool show_demo = false;
    bool want_reset_layout = true;
    bool quit = false;

    // Where things were drawn in the last frame (tests and tools use these).
    float view_rect[4] = {};            // viewport x, y, width, height
    struct TileRect { char path[256]; float x, y, w, h; };
    static constexpr int kMaxTiles = 64;
    TileRect file_tiles[kMaxTiles];
    int file_tile_count = 0;

    // Log storage (a ring of the newest lines).
    struct LogLine { LogLevel level; char text[192]; double time; };
    static constexpr int kLogLines = 512;
    LogLine log_lines[kLogLines];
    int log_count = 0, log_head = 0;
    bool log_scroll_to_end = false;

private:
    void build_ui();
    void main_menu();
    void toolbar(float y, float height);
    void dockspace(float top);
    void panel_hierarchy();
    void panel_inspector();
    void panel_files();
    void panel_console();
    void panel_viewport();
    void window_settings();
    void shortcuts();
};

// File type helpers shared by the panels.
const char* path_extension(const char* path);
const char* path_filename(const char* path);
bool path_is_image(const char* path);
bool path_is_model(const char* path);
bool path_is_script(const char* path);

}  // namespace fe
