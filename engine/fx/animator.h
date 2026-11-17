// The Animator module (scene/modules.h): plays clips of an animation file on
// the object and its children.
//
// .mof has no animation data yet and the renderer has no skinning, so this
// is transform animation: a clip's tracks move, turn and scale named objects
// (docs/animation.md). The file format, .fanim, is plain text:
//
//   fanim 1
//   clip spin 2.0                   # name and length in seconds
//     track . rotation              # "." is the animated object; "arm/hand" a child path
//       key 0   0 0 0               # time, then x y z (rotation in degrees)
//       key 2   0 360 0
//     track lid position
//       key 0 0 0 0
//       key 1 0 0.5 0
//
// Keys are interpolated linearly (rotations as quaternions). Clips loop or
// stop at their end by the module's `loop`. Switching clip (play(name, fade)
// or setting `clip`) crossfades the tracks both clips animate.
#pragma once

#include "ecs/ecs.h"

namespace fe {

struct FxHost;

struct FanimKey { float t; Vec3 v; };
struct FanimTrack {
    char target[96];
    uint8_t channel;                    // 0 position, 1 rotation (degrees), 2 scale
    FanimKey* keys;
    int key_count;
};
struct FanimClip {
    char name[48];
    float duration;
    FanimTrack* tracks;
    int track_count;
};
struct FanimFile {
    char path[256];
    FanimClip* clips = nullptr;
    int clip_count = 0;
};

// Parses a .fanim text; false (with `error`) when it is not one. Free with fanim_free.
bool fanim_parse(const char* text, size_t size, FanimFile* out, char* error, int error_cap);
void fanim_free(FanimFile* f);
// The value of a track at time t (clamped to its first and last key).
Vec3 fanim_sample(const FanimTrack& tr, float t);
// A rotation track's value as a quaternion (keys blend the short way round).
Quat fanim_sample_rot(const FanimTrack& tr, float t);

class AnimatorSystem {
public:
    ~AnimatorSystem() { shutdown(); }
    // Registers the system (Phase::Update) on `world`.
    bool init(World* world, const FxHost* host);
    void shutdown();
    // Starts a clip (empty name = the module's `clip`), blending from the
    // current one over `fade` seconds (< 0: the module's crossfade).
    bool play(Entity e, const char* clip, float fade);
    bool stop(Entity e);

private:
    struct State;
    static void system(World& w, float dt, void* user);
    State* state(Entity e, bool create);
    const FanimFile* load(const char* path);
    bool start_clip(State& st, Entity e, Animator& m, const char* clip, float fade);
    World* world_ = nullptr;
    const FxHost* host_ = nullptr;
    Entity system_ = 0;
    State* states_ = nullptr;
    int count_ = 0, cap_ = 0;
    FanimFile* files_ = nullptr;
    int file_count_ = 0;
    Entity* scan_ = nullptr;
    int scan_cap_ = 0;
};

}  // namespace fe
