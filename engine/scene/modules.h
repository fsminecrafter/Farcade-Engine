// Modules and special objects: the data the editor edits, the ECS stores as
// components, the renderer and (later) physics and audio read, and scripts
// reach.
//
// - Modules attach to any object, at most one of each kind per object:
//   RigidBody3D, Collider3D, AudioListener, AudioStream, AudioSampler, Light.
// - Special objects are object kinds with their own settings: Camera,
//   SkyLight, Sky, PostProcess (ObjectKind).
// - Some kinds have a "current" flag: only one object (or module) of that
//   kind is current in a scene at a time - the camera the game renders
//   with, the active sky, the listener that hears. Setting one current
//   clears the others (Scene::set_current in the editor, the ECS's
//   set_current at run time).
//
// Every struct here is plain data (copyable with memcpy, fixed size) and has
// a field table (reflection: name, type, offset, range, enum names) that the
// Inspector draws from, the ECS registers with flecs meta, and the Lua
// bindings will use. Add a field: add it to the struct and to its table in
// modules.cpp.
#pragma once

#include "core/math.h"
#include <stddef.h>
#include <stdint.h>

namespace fe {

// ---------------------------------------------------------------- kinds

// What an object is. Mesh objects draw a mesh (Object::mesh); Empty is a
// plain transform (a group, a spawn point, a holder for modules); the others
// carry the settings struct of the same name.
enum class ObjectKind : uint8_t { Mesh, Empty, Camera, SkyLight, Sky, PostProcess, Count };

// Modules, and the settings of special objects, share one numbering so one
// reflection table covers both.
enum class ModuleKind : uint8_t {
    RigidBody3D, Collider3D, AudioListener, AudioStream, AudioSampler, Light,   // modules
    Camera, SkyLight, Sky, PostProcess,                                          // special objects
    Count
};
constexpr int kModuleKindCount = (int)ModuleKind::Count;
constexpr int kFirstObjectSettings = (int)ModuleKind::Camera;

constexpr int kPathMax = 256;

// ---------------------------------------------------------------- modules

enum class BodyType : uint8_t { Dynamic, Kinematic, Static };

// Gives an object physics: gravity, velocity, collisions with colliders.
struct RigidBody3D {
    BodyType type = BodyType::Dynamic;
    float mass = 1.0f;                  // kg
    float linear_damping = 0.05f;
    float angular_damping = 0.05f;
    bool use_gravity = true;
    float gravity_scale = 1.0f;
    bool freeze_rotation = false;
    Vec3 velocity;                      // initial, m/s
};

enum class ColliderShape : uint8_t { Box, Sphere, Capsule, Mesh };
enum class ColliderMode : uint8_t { Collision, Trigger };

// A shape that collides (bodies bounce off it) or triggers (it reports
// objects entering and leaving, to scripts: on_trigger_enter/exit).
struct Collider3D {
    ColliderShape shape = ColliderShape::Box;
    ColliderMode mode = ColliderMode::Collision;
    Vec3 center;                        // offset from the object, local space
    Vec3 size = Vec3(1, 1, 1);          // Box: full extents
    float radius = 0.5f;                // Sphere, Capsule
    float height = 2.0f;                // Capsule: full height along local y
    float friction = 0.5f;
    float bounciness = 0.0f;
    uint32_t layer = 1;                 // collision layer bits
};

// Where the game hears from (usually on the camera). Current: one at a time.
struct AudioListener {
    float volume = 1.0f;                // master volume
    bool current = false;
};

// A long sound streamed from a file: music, ambience, dialogue.
struct AudioStream {
    char path[kPathMax] = {};
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = true;
    bool play_on_start = true;
    bool spatial = false;               // positioned in 3D (else played flat)
    float min_distance = 1.0f;
    float max_distance = 50.0f;
};

// Short sounds loaded into memory and played on demand (footsteps, hits):
// scripts call play(); several can overlap, up to `voices`.
struct AudioSampler {
    char path[kPathMax] = {};
    float volume = 1.0f;
    float pitch = 1.0f;
    float pitch_variation = 0.0f;       // random +- per play
    int voices = 4;
    bool spatial = true;
    float min_distance = 1.0f;
    float max_distance = 30.0f;
};

enum class LightType : uint8_t { Directional, Point, Spot };

// A light. Directional lights shine along the object's -Z (forward) axis;
// point and spot lights sit at the object (spots also point along -Z).
struct Light {
    LightType type = LightType::Point;
    Vec3 color = Vec3(1, 1, 1);         // linear RGB
    float intensity = 1.0f;
    float range = 10.0f;                // Point, Spot: where it fades to zero
    float spot_angle = 45.0f;           // Spot: full cone angle, degrees
    float spot_blend = 0.15f;           // Spot: soft edge, 0..1 of the cone
    bool shadows = true;
};

// ---------------------------------------------------------------- special objects

enum class Projection : uint8_t { Perspective, Orthographic };

// A camera looks along its object's -Z axis with +Y up. The current camera
// is the one the game renders with.
struct Camera {
    Projection projection = Projection::Perspective;
    float fov = 60.0f;                  // vertical, degrees
    float ortho_size = 5.0f;            // half the view height, meters
    float near_plane = 0.1f;
    float far_plane = 500.0f;
    bool current = false;
};

// Ambient light from the sky and the ground (hemisphere lighting).
struct SkyLight {
    Vec3 sky_color = Vec3(0.55f, 0.65f, 0.80f);
    Vec3 ground_color = Vec3(0.25f, 0.22f, 0.20f);
    float intensity = 0.6f;
    bool current = false;
};

enum class SkyMode : uint8_t { Gradient, Color };

// The background: a three-color gradient (zenith, horizon, ground) with a
// sun disc towards the strongest directional light, or one flat color.
struct Sky {
    SkyMode mode = SkyMode::Gradient;
    Vec3 zenith = Vec3(0.20f, 0.42f, 0.78f);
    Vec3 horizon = Vec3(0.72f, 0.80f, 0.90f);
    Vec3 ground = Vec3(0.30f, 0.28f, 0.26f);
    float sun_size = 1.5f;              // degrees; 0 = no sun disc
    float intensity = 1.0f;
    bool current = false;
};

enum class Tonemap : uint8_t { None, Reinhard, ACES };

// The look of the final image.
struct PostProcess {
    float exposure = 0.0f;              // EV stops
    Tonemap tonemap = Tonemap::ACES;
    float contrast = 1.0f;
    float saturation = 1.0f;
    Vec3 tint = Vec3(1, 1, 1);
    float vignette = 0.2f;              // 0..1
    float bloom = 0.0f;                 // 0..1 strength
    float bloom_threshold = 1.0f;
    bool current = false;
};

// ---------------------------------------------------------------- reflection

enum class FieldType : uint8_t { Float, Int, Bool, Enum, Color, Vec3, Path, Bits };

struct FieldInfo {
    const char* name;                   // "intensity" (also the Lua name)
    const char* label;                  // "Intensity" (the Inspector's)
    FieldType type;
    uint16_t offset;
    float min, max;                     // Float/Int: min == max means no range
    const char* const* enum_names;      // Enum: names, nullptr-terminated
};

struct ModuleInfo {
    ModuleKind kind;
    const char* name;                   // "rigidbody3d" (the Lua name: obj.rigidbody3d)
    const char* label;                  // "Rigidbody 3D"
    uint32_t size;                      // sizeof the struct
    uint32_t align;
    const FieldInfo* fields;
    int field_count;
    bool has_current;                   // has a `current` bool (only one per scene)
    uint16_t current_offset;            // where it is, when has_current
};

const ModuleInfo& module_info(ModuleKind kind);
// The kind called `name` ("camera", "light"), or ModuleKind::Count.
ModuleKind module_by_name(const char* name);
// The field called `name`, or nullptr.
const FieldInfo* find_field(const ModuleInfo& info, const char* name);

// The settings each special object kind carries (ModuleKind::Count for Mesh
// and Empty), and the other way round.
ModuleKind settings_of(ObjectKind kind);
ObjectKind object_kind_of(ModuleKind kind);     // ObjectKind::Count for modules
const char* object_kind_name(ObjectKind kind);  // "Camera"

inline bool is_module(ModuleKind k) { return (int)k < kFirstObjectSettings; }

// Reading and writing the `current` flag of any struct that has one.
inline bool get_current(const ModuleInfo& m, const void* data) {
    return m.has_current && *((const bool*)data + m.current_offset);
}
inline void set_current_flag(const ModuleInfo& m, void* data, bool on) {
    if (m.has_current) *((bool*)data + m.current_offset) = on;
}

// ---------------------------------------------------------------- all of an object's data

// Every module and settings struct an object can have, plus which ones it
// has (bit (1 << ModuleKind)). The editor's Object holds one; the ECS turns
// each present one into a component.
struct ModuleSet {
    uint32_t present = 0;
    RigidBody3D rigidbody3d;
    Collider3D collider3d;
    AudioListener audio_listener;
    AudioStream audio_stream;
    AudioSampler audio_sampler;
    Light light;
    Camera camera;
    SkyLight sky_light;
    Sky sky;
    PostProcess post_process;

    bool has(ModuleKind k) const { return (present >> (int)k) & 1u; }
    void add(ModuleKind k) { present |= 1u << (int)k; }
    void remove(ModuleKind k) { present &= ~(1u << (int)k); }
    // The struct of kind k (whether present or not).
    void* data(ModuleKind k);
    const void* data(ModuleKind k) const;
    // Resets kind k to its defaults.
    void reset(ModuleKind k);
};

}  // namespace fe
