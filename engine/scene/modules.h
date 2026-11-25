// Modules and special objects: the data the editor edits, the ECS stores as
// components, the renderer and (later) physics and audio read, and scripts
// reach.
//
// - Modules attach to any object, at most one of each kind per object:
//   RigidBody3D, Collider3D, CharacterController3D, AudioListener,
//   AudioStream, AudioSampler, Light, ParticleEmitter, Joint3D, Animator,
//   ForceField, Spawner, ReverbZone, Billboard, Text3D.
// - Special objects are object kinds with their own settings: Camera,
//   SkyLight, Sky, PostProcess, Terrain, Clouds (ObjectKind).
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
enum class ObjectKind : uint8_t { Mesh, Empty, Camera, SkyLight, Sky, PostProcess, Terrain, Clouds, Count };

// Modules, and the settings of special objects, share one numbering so one
// reflection table covers both.
enum class ModuleKind : uint8_t {
    RigidBody3D, Collider3D, CharacterController3D, AudioListener, AudioStream, AudioSampler, Light, // modules
    ParticleEmitter, Joint3D, Animator, ForceField, Spawner, ReverbZone, Billboard, Text3D,         // more modules
    Camera, SkyLight, Sky, PostProcess, Terrain, Clouds,                         // special objects
    Count
};
constexpr int kModuleKindCount = (int)ModuleKind::Count;
constexpr int kFirstObjectSettings = (int)ModuleKind::Camera;

constexpr int kPathMax = 256;
constexpr int kPathListMax = 1024;      // a FieldType::PathList: paths separated by ';'

// How an object is loaded at run time (the Object's Streaming property; scene/terrain.h).
// Dynamic: loaded while the terrain chunk under it is (always, when it is over no terrain).
// AlwaysLoaded: never unloaded. Unloaded: not loaded until a script sets obj.loaded = true.
enum class StreamMode : uint8_t { Dynamic, AlwaysLoaded, Unloaded };

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
    Vec3 velocity;                      // m/s: the start velocity, and the live one while playing
    Vec3 angular_velocity;              // rad/s around world x, y, z: likewise
};

// Capsule and Cylinder stand along the object's local Y axis (height is
// the full height; a capsule's includes its round ends). Mesh is the convex
// hull of the object's mesh.
enum class ColliderShape : uint8_t { Box, Sphere, Capsule, Cylinder, Mesh };
enum class ColliderMode : uint8_t { Collision, Trigger };

// A shape that collides (bodies bounce off it) or triggers (it reports
// objects entering and leaving, to scripts: on_trigger_enter/exit).
struct Collider3D {
    ColliderShape shape = ColliderShape::Box;
    ColliderMode mode = ColliderMode::Collision;
    Vec3 center;                        // offset from the object, local space
    Vec3 size = Vec3(1, 1, 1);          // Box: full extents
    float radius = 0.5f;                // Sphere, Capsule, Cylinder
    float height = 2.0f;                // Capsule, Cylinder: full height along local y
    float friction = 0.5f;
    float bounciness = 0.0f;
    uint32_t layer = 1;                 // collision layer bits
};

// An upright capsule body controlled by setting velocity in m/s. Physics
// applies gravity, resolves collisions and reports whether it is grounded.
struct CharacterController3D {
    float radius = 0.5f;
    float height = 2.0f;
    Vec3 center = Vec3(0, 1, 0);        // feet at the object's origin by default
    float slope_limit = 45.0f;          // degrees; steeper contacts are not grounded
    bool use_gravity = true;
    float gravity_scale = 1.0f;
    Vec3 velocity;                      // live velocity in m/s; set from scripts to move
    bool grounded = false;              // updated by the physics step
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

// ---------------------------------------------------------------- effects, joints, animation, ...

// Links between objects are stored as the editor's object id (ObjectId in
// scene.h, 0 = none). At run time World::link_entity (ecs.h) turns one into
// the entity.
typedef uint32_t ObjectLink;

enum class EmitShape : uint8_t { Point, Sphere, Cone, Box };
enum class ParticleBlend : uint8_t { Additive, Alpha };
enum class ParticleSpace : uint8_t { World, Local };

// CPU particles (engine/fx/particles.h): emitted at the object, drawn as
// camera-facing quads. Cone and Box particles fly along the object's -Z axis
// (Cone spread by cone_angle); Sphere particles fly away from the center,
// Point particles in any direction.
struct ParticleEmitter {
    float emission_rate = 20.0f;        // particles per second
    int burst_count = 0;                // particles in one burst (0 = none)
    float burst_interval = 0.0f;        // seconds between bursts; 0 = one burst when playing starts
    int max_particles = 200;            // alive at once; emission waits when the pool is full
    float lifetime_min = 1.0f;          // seconds
    float lifetime_max = 2.0f;
    float speed_min = 1.0f;             // m/s at birth
    float speed_max = 3.0f;
    EmitShape shape = EmitShape::Cone;
    float shape_radius = 0.1f;          // Sphere, Cone: where particles start
    Vec3 box_size = Vec3(1, 1, 1);      // Box: full extents
    float cone_angle = 25.0f;           // Cone: half angle, degrees
    float gravity = 0.0f;               // times the world gravity (1 = falls like a rock, -0.2 = rises)
    float drag = 0.0f;                  // 1/s: speed lost to the air
    float size_start = 0.15f;           // meters
    float size_end = 0.02f;
    Vec3 color_start = Vec3(1.0f, 0.75f, 0.30f);
    Vec3 color_end = Vec3(1.0f, 0.15f, 0.05f);
    float alpha_start = 1.0f;
    float alpha_end = 0.0f;
    char texture[kPathMax] = {};        // empty = a soft round dot
    ParticleBlend blend = ParticleBlend::Additive;
    ParticleSpace space = ParticleSpace::World;     // Local: particles move with the object
    bool play_on_start = true;
    bool looping = true;
    float duration = 5.0f;              // seconds of emission when not looping
    bool prewarm = false;               // looping emitters start as if they ran for a while
    int seed = 1;                       // the random sequence (the same seed gives the same particles)
    bool playing = false;               // live: emitting now (scripts: play(), stop())
    int particle_count = 0;             // live: particles alive
};

enum class JointType : uint8_t { Fixed, Hinge, Spring, BallSocket };

// A constraint between this object's rigid body and another (or the world).
// The anchor is a point on this object; the joint keeps it on the same point
// of the connected body, as the two sit at the start of Play. A Spring is
// different: it hangs from the anchor (a point of the connected body, or of
// the world) to this object's origin, and rest_length is the distance between
// those two. Hinge angle, motor speed and limits count this body turning
// about its axis, relative to the connected one.
struct Joint3D {
    JointType type = JointType::Fixed;
    ObjectLink connected = 0;           // the other body; none = fixed to the world
    Vec3 anchor;                        // on this object, local space
    Vec3 axis = Vec3(0, 1, 0);          // Hinge: the turning axis, local space
    bool use_limits = false;            // Hinge
    float limit_min = -45.0f;           // degrees, from the starting angle
    float limit_max = 45.0f;
    bool use_motor = false;             // Hinge: drives the turn
    float motor_speed = 90.0f;          // degrees per second
    float motor_torque = 50.0f;         // N m at most
    float rest_length = 1.0f;           // Spring: distance between the anchors with no force
    float stiffness = 50.0f;            // Spring: N/m
    float damping = 2.0f;               // Spring: N s/m
    float break_force = 0.0f;           // N; the joint breaks above it (0 = never)
    bool collide_connected = false;     // the two bodies may touch each other
    bool broken = false;                // live: set when it broke (scripts: break())
    float angle = 0.0f;                 // live: Hinge angle in degrees
    float force = 0.0f;                 // live: force the joint applied last step, N
};

// Plays a clip from an animation file (.fanim, docs/animation.md) on the
// object and its children: tracks move, turn and scale named objects. (.mof
// has no animation data yet, and the renderer has no skinning: this is
// transform animation.) engine/fx/animator.h.
struct Animator {
    char source[kPathMax] = {};         // the .fanim file; empty = the object's model path with .fanim
    char clip[kPathMax] = {};           // clip name; empty = the first one
    float speed = 1.0f;
    bool loop = true;
    bool play_on_start = true;
    float crossfade = 0.2f;             // seconds to blend from the clip it replaces
    bool playing = false;               // live
    float time = 0.0f;                  // live: seconds into the clip (writable: seeks)
    float duration = 0.0f;              // live: length of the clip, seconds
};

enum class FieldShape : uint8_t { Box, Sphere };
enum class FieldMode : uint8_t { Directional, Radial, Vortex };
enum class FieldFalloff : uint8_t { None, Linear, Quadratic };

// A volume that pushes rigid bodies and particles inside it (wind, a fan, a
// vortex, an explosion's pull). The force on a body is `strength` newtons at
// full effect.
struct ForceField {
    FieldShape shape = FieldShape::Box;
    Vec3 size = Vec3(4, 4, 4);          // Box: full extents
    float radius = 3.0f;                // Sphere
    FieldMode mode = FieldMode::Directional;
    float strength = 10.0f;             // N (negative: Radial pulls in, Vortex turns the other way)
    Vec3 direction = Vec3(0, 0, -1);    // Directional: local space
    FieldFalloff falloff = FieldFalloff::None;  // fades towards the edge of the volume
    float turbulence = 0.0f;            // 0..1: how much the force varies in space and time
    float turbulence_scale = 1.0f;      // meters per swirl
    float turbulence_speed = 1.0f;
    bool affect_bodies = true;
    bool affect_particles = true;
    bool enabled = true;
    uint32_t layers = 0xFFFFFFFFu;      // bodies on these collision layers
};

// Makes copies of another object (engine/fx/spawner.h). The template stays in
// the scene; hide it or park it away if it should not show.
struct Spawner {
    ObjectLink source = 0;              // the object to copy (with its children)
    bool auto_spawn = true;
    bool spawn_on_start = false;
    float interval = 1.0f;              // seconds between copies
    int max_alive = 20;                 // at most this many of its copies at once (the oldest goes first)
    int total = 0;                      // stop after this many (0 = no end)
    Vec3 offset = Vec3(0, 0, 0);        // random shift: each axis up to +- this
    Vec3 velocity = Vec3(0, 0, 0);      // added to the copy's body velocity (spawner's local space)
    float velocity_random = 0.0f;       // random extra speed, m/s
    float inherit_velocity = 0.0f;      // fraction of the spawner's own motion given to the copy
    float lifetime = 0.0f;              // seconds until a copy is destroyed (0 = never)
    bool random_rotation = false;
    int seed = 1;
    int spawned = 0;                    // live: copies made so far
    int alive = 0;                      // live: copies alive
};

// Changes how sounds sound inside its volume: a short echo tail and a muffled
// top end. Only the listener's position matters (engine/audio).
struct ReverbZone {
    FieldShape shape = FieldShape::Box;
    Vec3 size = Vec3(10, 5, 10);        // Box: full extents
    float radius = 6.0f;                // Sphere
    float wet = 0.35f;                  // 0..1: how much echo is mixed in
    float room_size = 0.5f;             // 0..1: longer tail
    float damping = 0.5f;               // 0..1: the tail loses its highs
    float lowpass = 0.0f;               // 0..1: muffles everything (a sound behind a wall)
    float fade_distance = 1.0f;         // meters outside the volume over which the effect fades out
    bool enabled = true;
};

enum class BillboardMode : uint8_t { Full, Vertical };

// A textured quad that faces the camera: icons, foliage cards, glow sprites
// (engine/fx/sprites.h).
struct Billboard {
    char texture[kPathMax] = {};        // empty = a white square
    Vec3 color = Vec3(1, 1, 1);
    float alpha = 1.0f;
    float width = 1.0f;                 // meters
    float height = 1.0f;
    BillboardMode mode = BillboardMode::Full;    // Vertical: turns around Y only (trees)
    ParticleBlend blend = ParticleBlend::Alpha;
    bool visible = true;
};

enum class TextAlign : uint8_t { Left, Center, Right };

// A text label in the world, facing the camera (engine/fx/sprites.h).
struct Text3D {
    char text[kPathMax] = "Text";
    Vec3 color = Vec3(1, 1, 1);
    float size = 0.5f;                  // height of a line, meters
    TextAlign align = TextAlign::Center;
    bool outline = true;
    bool visible = true;
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
    // Screen shaders: full screen passes over what the camera sees, in order (renderer.h
    // RenderScene::screen_shaders; docs/graphics.md). A .fshader with the fragment part only.
    char shader1[kPathMax] = {};
    char shader2[kPathMax] = {};
    char shader3[kPathMax] = {};
    Vec3 shader_params = Vec3(1.0f, 0.0f, 0.0f);   // u_params.xyz of the screen shaders
};

// Ambient light from the sky and the ground (hemisphere lighting).
struct SkyLight {
    Vec3 sky_color = Vec3(0.55f, 0.65f, 0.80f);
    Vec3 ground_color = Vec3(0.25f, 0.22f, 0.20f);
    float intensity = 0.6f;
    bool current = false;
    // Probe GI (Settings > Graphics > GI = probes): how strongly the probe light replaces this hemisphere
    // ambient, and how much of the light a surface got is re-emitted as bounce.
    float gi_intensity = 1.0f;
    float gi_bounce = 0.8f;
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
    float fog = 0.0f;                   // 0..1: distance fog towards the horizon color
    bool current = false;
};

enum class Tonemap : uint8_t { None, Reinhard, ACES, AgX };

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
    // Color grading in display light, after tonemap, contrast and saturation:
    // out = pow(max(c * gain + lift * (1 - c), 0), 1 / gamma).
    Vec3 lift = Vec3(0, 0, 0);          // tints the shadows
    Vec3 gamma = Vec3(1, 1, 1);         // mid tones: > 1 brightens
    Vec3 gain = Vec3(1, 1, 1);          // tints / scales the highlights
    float bloom_radius = 0.5f;          // 0..1: how far the glow spreads (weight of the wide levels)
    float chromatic_aberration = 0.0f;  // 0..1: color fringes towards the edges (GPU tier)
    float film_grain = 0.0f;            // 0..1
    // Auto exposure: the image's average brightness is measured every frame
    // and the exposure eases towards a mid grey (Exposure becomes a
    // compensation on top).
    bool auto_exposure = false;
    float exposure_speed = 2.0f;        // adaptation rate, 1 / seconds
    float auto_exposure_min = -3.0f;    // EV range of the automatic part
    float auto_exposure_max = 3.0f;
};

// ---------------------------------------------------------------- terrain

constexpr int kFoliageLayers = 4;

// One kind of foliage on a terrain: a .mof model scattered where the layer is
// painted, turned randomly around its up axis (never tipped into the ground),
// thinned out with distance and drawn in batches per chunk (scene/terrain.h).
struct FoliageLayer {
    // A name scripts find the layer by (fe.find_foliage("grass")); names match without regard to case.
    char name[kPathMax] = {};
    // The .mof models, separated by ';' ("grass_a.mof;grass_b.mof;flower.mof"): each instance takes one of
    // them at random. Empty = the layer is off.
    char model[kPathListMax] = {};
    float density = 2.0f;               // instances per square meter where painted at full strength
    float scale_min = 0.8f;
    float scale_max = 1.2f;
    float distance = 60.0f;             // meters: drawn up to here, thinning out over the last 60%
    float max_slope = 40.0f;            // degrees: steeper ground gets none
    bool align_to_slope = false;        // stand along the ground's normal (else straight up)
    bool cast_shadows = false;
    // A .fshader for every instance (a wind sway): its @displace / @surface hooks run in every pass, with u_time
    // (gfx/shader_file.h). Empty = the models' textures and colours as they are.
    char shader[kPathMax] = {};
};

// A heightfield split into square chunks. Heights, holes and the painted
// foliage live in the `data` file (.fterrain, scene/terrain.h); the terrain
// sits at its object's position (rotation and scale are not used). Chunks
// farther than load_distance from the camera are unloaded at run time, with
// the Dynamic objects standing on them.
struct Terrain {
    char data[kPathMax] = {};           // the .fterrain file (made by Generate)
    float load_distance = 250.0f;       // meters from the camera to a chunk's edge
    bool stream_in_editor = false;      // the editor view unloads chunks (and objects) too
    float lod_distance = 64.0f;         // full detail up to here; half the detail per further step
    char texture[kPathMax] = {};
    Vec3 color = Vec3(0.34f, 0.46f, 0.24f);
    float roughness = 0.9f;
    float uv_scale = 4.0f;              // meters per texture repeat
    bool cast_shadows = true;
    float friction = 0.8f;
    uint32_t layer = 1;                 // collision layer bits
    int revision = 0;                   // the data's edit revision (undo / redo; set by the editor)
    FoliageLayer foliage[kFoliageLayers];
};

// ---------------------------------------------------------------- clouds

enum class CloudQuality : uint8_t { Low, Medium, Ultra };

// Volumetric clouds, ray marched in the sky (gfx/clouds.h): lit by the sun (its colour and direction, with
// forward scattering around it), the Sky Light's colours as ambient, and the sky behind them. They sit in
// up to three layers above the world, drift with the wind and fade towards the horizon. Quality is the
// performance preset (steps along the ray and towards the sun); Settings > Graphics > Clouds caps it.
// Current: one at a time.
struct Clouds {
    bool current = false;
    CloudQuality quality = CloudQuality::Medium;
    float height = 1500.0f;             // meters (world y): the bottom of the lowest layer
    float thickness = 900.0f;           // meters, each layer
    int layers = 1;                     // 1..3, stacked
    float layer_spacing = 700.0f;       // meters between one layer's top and the next one's bottom
    float coverage = 0.5f;              // 0..1: share of the sky covered
    float density = 0.7f;               // how thick (dark at the bottom, opaque) they are, 0..3
    Vec3 color = Vec3(1.0f, 1.0f, 1.0f);    // what they reflect (the sun's colour is applied on top)
    float scale = 1.0f;                 // size of the cloud shapes: 1 = about 4 km across a big one
    Vec3 wind = Vec3(12.0f, 0.0f, 5.0f);    // m/s
    float sun_glow = 0.6f;              // 0..1: the bright rim towards the sun (forward scattering)
    float ambient = 1.0f;               // 0..2: light from the sky on the shaded sides
    float distance = 25000.0f;          // meters: faded into the horizon by here
};

// ---------------------------------------------------------------- reflection

// Object: a link to another object (an ObjectLink, 32 bits, 0 = none).
// Text: free text in a char[kPathMax] (like Path, but not a file).
// PathList: files in a char[kPathListMax], separated by ';' (path_list_next() walks them).
enum class FieldType : uint8_t { Float, Int, Bool, Enum, Color, Vec3, Path, Bits, Object, Text, PathList };

// The bytes a field of this type takes.
inline uint32_t field_size(FieldType t) {
    switch (t) {
        case FieldType::Path: case FieldType::Text: return (uint32_t)kPathMax;
        case FieldType::PathList: return (uint32_t)kPathListMax;
        case FieldType::Color: case FieldType::Vec3: return (uint32_t)sizeof(Vec3);
        case FieldType::Bool: case FieldType::Enum: return 1;
        default: return 4;
    }
}

// Walks a PathList: copies the next path (spaces around it trimmed) into out and returns where the one after
// starts, or nullptr when there are no more. Empty entries are skipped.
inline const char* path_list_next(const char* list, char* out, int cap) {
    while (list && *list) {
        while (*list == ';' || *list == ' ') list++;
        if (!*list) break;
        const char* end = list;
        while (*end && *end != ';') end++;
        const char* e = end;
        while (e > list && e[-1] == ' ') e--;
        int n = (int)(e - list);
        if (n >= cap) n = cap - 1;
        for (int i = 0; i < n; i++) out[i] = list[i];
        out[n] = 0;
        return end;
    }
    if (cap > 0) out[0] = 0;
    return nullptr;
}

// Name lists: names separated by ';' (an object's groups). Names compare without regard to case; spaces around
// a name are ignored. name_list_has: whether `name` is in the list.
inline bool name_eq_nocase(const char* a, const char* b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
        if (!x) return true;
    }
}
inline bool name_list_has(const char* list, const char* name) {
    char item[kPathMax];
    for (const char* p = path_list_next(list, item, (int)sizeof item); p; p = path_list_next(p, item, (int)sizeof item))
        if (name_eq_nocase(item, name)) return true;
    return false;
}
// Whether a name may go into a list: not empty, at most 63 characters, no ';' and no spaces at its ends.
inline bool name_list_valid(const char* name) {
    int n = 0;
    while (name[n]) { if (name[n] == ';') return false; n++; }
    return n > 0 && n < 64 && name[0] != ' ' && name[n - 1] != ' ';
}
// Adds a name (when valid and not there yet). False when it is invalid or the list (cap bytes) is full.
inline bool name_list_add(char* list, int cap, const char* name) {
    if (!name_list_valid(name)) return false;
    if (name_list_has(list, name)) return true;
    int len = 0, n = 0;
    while (list[len]) len++;
    while (name[n]) n++;
    int need = len + (len ? 1 : 0) + n;
    if (need >= cap) return false;
    if (len) list[len++] = ';';
    for (int i = 0; i <= n; i++) list[len + i] = name[i];
    return true;
}
// Removes a name; returns whether it was there. The list is rewritten without empty entries.
inline bool name_list_remove(char* list, int cap, const char* name) {
    char out[kPathListMax];
    char item[kPathMax];
    int len = 0;
    bool found = false;
    out[0] = 0;
    for (const char* p = path_list_next(list, item, (int)sizeof item); p; p = path_list_next(p, item, (int)sizeof item)) {
        if (name_eq_nocase(item, name)) { found = true; continue; }
        int n = 0;
        while (item[n]) n++;
        if (len + (len ? 1 : 0) + n >= (int)sizeof out) break;
        if (len) out[len++] = ';';
        for (int i = 0; i <= n; i++) out[len + i] = item[i];
        len += n;
    }
    if (len >= cap) len = cap - 1;
    for (int i = 0; i < len; i++) list[i] = out[i];
    list[len] = 0;
    return found;
}

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
    CharacterController3D character_controller3d;
    AudioListener audio_listener;
    AudioStream audio_stream;
    AudioSampler audio_sampler;
    Light light;
    ParticleEmitter particles;
    Joint3D joint3d;
    Animator animator;
    ForceField force_field;
    Spawner spawner;
    ReverbZone reverb_zone;
    Billboard billboard;
    Text3D text3d;
    Camera camera;
    SkyLight sky_light;
    Sky sky;
    PostProcess post_process;
    Terrain terrain;
    Clouds clouds;

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
