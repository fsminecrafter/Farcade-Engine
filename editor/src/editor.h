// The Farcade editor: state shared by the panels, and the frame loop.
#pragma once

#include "gfx/graphics_settings.h"

#include "scene.h"
#include "widgets.h"
#include <stdint.h>

struct ImFont;

namespace fe {

class View3D;
class World;
class PhysicsWorld;
class ScriptRuntime;
class AudioDevice;
class AudioSystem;

enum class Tool : uint8_t { Select, Move, Rotate, Scale };
enum class LogLevel : uint8_t { Info, Warning, Error };
enum class ViewMode : uint8_t { Lit, Unlit, Wireframe };


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
#if FE_MINIMALOS
    bool view_supersample = false;      // the CPU preview is slow under emulation
    bool view_shadows = false;
#else
    bool view_supersample = true;       // 2x2 supersampling in the viewport
    bool view_shadows = true;           // shadows on the ground
#endif
    bool play_through_camera = true;    // Play shows the scene through the current Camera object
    gfx::GraphicsSettings graphics;     // FarcadeGL quality (GPU tier: graphics card only)
    float snap_move = 0.5f;
    float snap_rotate = 15.0f;          // degrees
    float snap_scale = 0.1f;
    FileEditor editors[16];
    int editor_count = 0;
    const char* program_for(const char* path) const;
};

struct ViewCamera {     // the editor's orbit camera (not a scene Camera object)
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
    ~Editor();
    bool init(int width, int height);
    // One UI frame: builds the UI and renders it into the platform's
    // framebuffer. Returns false when the editor should quit.
    bool frame();
    void shutdown();

    // ---- used by the panels
    Scene scene;
    Settings settings;
    ViewCamera camera;
    Tool tool = Tool::Move;
    bool gizmo_local = false;           // gizmo along the object's axes (X toggles); Scale always is
    ViewMode view_mode = ViewMode::Lit;
    // What draws the viewport: the CPU preview (grid, outline, shadows) or
    // FarcadeGL (lights, sky, sky light, post processing; engine/gfx).
    enum class ViewRenderer : uint8_t { CpuPreview, FarcadeGL };
    ViewRenderer view_renderer = ViewRenderer::CpuPreview;
    bool looking_through_camera = false;        // last frame: the view was the current camera's
    // What the FarcadeGL renderer reported last time it ran.
    char fgl_backend[64] = {};
    gfx::GraphicsFeatures fgl_features;
    // Collider 3D: the active object's collider is drawn in the viewport;
    // with edit_collider on, handles on it change its size/radius/height.
    bool edit_collider = false;
    // Draws the collider (and its handles) over the viewport rectangle;
    // true while a handle is hovered or dragged (the gizmo and picking wait).
    bool collider_overlay(const Mat4& view, const Mat4& proj, float x, float y, float w, float h);
    Palette pal = {};
    char project_dir[256] = {};
    char files_dir[256] = {};

    void log(LogLevel level, const char* fmt, ...);
    void open_file(const char* path);           // double-click in Files
    void apply_file_to_object(const char* path, ObjectId id);   // a file dropped on an object
    ObjectId create_object(const char* name, MeshKind mesh, const Vec3& at);
    void delete_selection();                    // with their children
    ObjectId create_child(const char* name, MeshKind mesh, ObjectId parent);
    // Special objects (Camera, Sky, Sky Light, Post Processing, Empty) and
    // lights (an Empty with a Light module), under `parent` when given.
    ObjectId create_kind(const char* name, ObjectKind kind, const Vec3& at, ObjectId parent = kNoObject);
    ObjectId create_light(LightType type, const Vec3& at, ObjectId parent = kNoObject);
    // The items of every "add object" menu (Create, the Hierarchy's + and
    // right-click menus): meshes, empty, lights, camera, sky ...
    void create_menu_items(ObjectId parent, const Vec3& at);
    ObjectId duplicate_tree(ObjectId src, ObjectId parent, bool offset);    // an object and its children
    // Scripts: adds one to an object and reads the inputs it declares
    // (-- @prop lines, see script.h). Returns its index or -1.
    int add_script(ObjectId id, const char* path);
    // Reads a script's file again and brings its inputs up to date, keeping
    // the values set in the editor. False when the file cannot be read.
    bool reload_script(ObjectId id, int index);
    int reload_all_scripts();                   // every script in the scene; returns how many failed
    void duplicate_selection();
    void frame_selection();
    void apply_theme();
    void set_view(float yaw, float pitch);      // orbit the camera to a fixed direction

    // ---- play mode: Play saves the scene, Stop puts it back. Game code
    // (Lua, C++) does not run yet; that comes with the runtime.
    bool playing() const { return play_backup_ != nullptr; }
    void play();
    void stop();
    double play_time = 0.0;
    // While playing, the scene runs as an ECS world (engine/ecs): one entity
    // per object, with its transform, modules and mesh. Systems (physics,
    // scripts, audio) plug into it; each frame the transforms come back to
    // the scene so the viewport shows them.
    World* play_world() { return world_; }
    PhysicsWorld* play_physics() { return physics_; }
    ScriptRuntime* play_scripts() { return scripts_; }
    AudioSystem* play_audio() { return audio_; }
    AudioDevice* audio_device() { return audio_device_; }
    // A script file's text (full path, or relative to the project); malloc'd, or null.
    char* read_script_text(const char* path, size_t* size);
    uint64_t entity_of(ObjectId id) const;      // 0 when not playing / unknown
    ObjectId object_of(uint64_t entity) const;
    // Physics events while playing ("entered" / "left" a trigger, "hit"):
    // logged to the console; the script runtime takes them over when set.
    void (*on_physics_event)(Editor* ed, const char* what, uint64_t a, uint64_t b) = nullptr;
    int physics_event_count = 0;
    void physics_event(const char* what, uint64_t a, uint64_t b);

    ImFont* font_ui = nullptr;
    ImFont* font_medium = nullptr;      // headings, tabs, section titles
    ImFont* font_mono = nullptr;
    bool show_settings = false;
    bool show_demo = false;
    bool want_reset_layout = true;
    bool quit = false;

    // Where things were drawn in the last frame (tests and tools use these).
    float view_rect[4] = {};            // viewport x, y, width, height
    // Named rectangles of the last frame: "hier:<id>", "eye:<id>",
    // "menu:<label>", "tool:<name>", "run:play", "run:stop", "axis:+x", ...
    struct UiRect { char key[48]; float x, y, w, h; };
    static constexpr int kMaxUiRects = 256;
    UiRect ui_rects[kMaxUiRects];
    int ui_rect_count = 0;
    void track(const char* key);                // records the last item's rectangle
    void track_rect(const char* key, float x, float y, float w, float h);
    const UiRect* find_rect(const char* key) const;
    // ImGui::MenuItem that also records its rectangle as "menu:<label>".
    bool menu_item(const char* label, const char* shortcut = nullptr, bool selected = false, bool enabled = true);
    // Viewport statistics of the last frame.
    int view_triangles = 0;
    float view_ms = 0.0f;
    double (*perf_clock)() = nullptr;           // a real clock for view_ms (default: platform::time_seconds)
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
    int collider_drag_ = -1;                    // the handle being dragged
    ObjectId collider_drag_object_ = kNoObject;
    World* world_ = nullptr;
    PhysicsWorld* physics_ = nullptr;
    ScriptRuntime* scripts_ = nullptr;
    AudioDevice* audio_device_ = nullptr;       // opened at the first Play, kept
    AudioSystem* audio_ = nullptr;
    ObjectId* world_objects_ = nullptr;         // [i] = object of world_entities_[i]
    uint64_t* world_entities_ = nullptr;
    int world_count_ = 0;
    int script_count_ = 0;
    void build_world();
    void step_world(float dt);
    void free_world();
    ObjectId duplicate_one(ObjectId src, ObjectId parent, bool offset, ObjectId* from, ObjectId* to, int* pairs);
    void build_ui();
    void script_section(Object* o, int index);
    void add_script_button(Object* o);
    bool module_section(Object* o, ModuleKind kind);   // false when the module was removed
    void add_module_button(Object* o);
    void settings_graphics();
    void draw_object_shape(View3D& view3d, const Object& o, const Mat4& model);
    void main_menu();
    void run_controls();
    void status_bar(float height);
    void dockspace(float top, float bottom);
    void viewport_overlays(float x, float y, float w, float h);
    uint64_t view_hash(int w, int h) const;
    bool view_interacting_ = false;     // a drag in the viewport (last frame)
    bool view_supersample_now() const { return settings.view_supersample && !view_interacting_; }
    bool view_shadows_now() const { return settings.view_shadows && !view_interacting_; }
    void render_view(View3D& view3d, const void* view);
    void orientation_gizmo(float x, float y, float size);
    void panel_hierarchy();
    void panel_inspector();
    void panel_files();
    void panel_console();
    void panel_viewport();
    void window_settings();
    void shortcuts();
    Scene* play_backup_ = nullptr;
};

// File type helpers shared by the panels.
const char* path_extension(const char* path);
const char* path_filename(const char* path);
Icon object_icon(const Object& o);
Icon module_icon(ModuleKind k);
bool path_is_image(const char* path);
bool path_is_model(const char* path);
bool path_is_script(const char* path);

}  // namespace fe
