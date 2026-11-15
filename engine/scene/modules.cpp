#include "scene/modules.h"

#include <string.h>

namespace fe {

namespace {

const char* const kBodyTypes[] = { "Dynamic", "Kinematic", "Static", nullptr };
const char* const kShapes[] = { "Box", "Sphere", "Capsule", "Cylinder", "Mesh", nullptr };
const char* const kColliderModes[] = { "Collision", "Trigger", nullptr };
const char* const kLightTypes[] = { "Directional", "Point", "Spot", nullptr };
const char* const kProjections[] = { "Perspective", "Orthographic", nullptr };
const char* const kSkyModes[] = { "Gradient", "Color", nullptr };
const char* const kTonemaps[] = { "None", "Reinhard", "ACES", nullptr };

#define FE_F(T, f, label, type, lo, hi, enums) { #f, label, FieldType::type, (uint16_t)offsetof(T, f), lo, hi, enums }

const FieldInfo kRigidBody[] = {
    FE_F(RigidBody3D, type, "Type", Enum, 0, 0, kBodyTypes),
    FE_F(RigidBody3D, mass, "Mass", Float, 0.001f, 10000.0f, nullptr),
    FE_F(RigidBody3D, linear_damping, "Damping", Float, 0, 10, nullptr),
    FE_F(RigidBody3D, angular_damping, "Spin damping", Float, 0, 10, nullptr),
    FE_F(RigidBody3D, use_gravity, "Use gravity", Bool, 0, 0, nullptr),
    FE_F(RigidBody3D, gravity_scale, "Gravity scale", Float, -10, 10, nullptr),
    FE_F(RigidBody3D, freeze_rotation, "Lock rotation", Bool, 0, 0, nullptr),
    FE_F(RigidBody3D, velocity, "Start velocity", Vec3, 0, 0, nullptr),
    FE_F(RigidBody3D, angular_velocity, "Start spin", Vec3, 0, 0, nullptr),
};

const FieldInfo kCollider[] = {
    FE_F(Collider3D, shape, "Shape", Enum, 0, 0, kShapes),
    FE_F(Collider3D, mode, "Mode", Enum, 0, 0, kColliderModes),
    FE_F(Collider3D, center, "Center", Vec3, 0, 0, nullptr),
    FE_F(Collider3D, size, "Size", Vec3, 0, 0, nullptr),
    FE_F(Collider3D, radius, "Radius", Float, 0.001f, 1000.0f, nullptr),
    FE_F(Collider3D, height, "Height", Float, 0.001f, 1000.0f, nullptr),
    FE_F(Collider3D, friction, "Friction", Float, 0, 1, nullptr),
    FE_F(Collider3D, bounciness, "Bounciness", Float, 0, 1, nullptr),
    FE_F(Collider3D, layer, "Layers", Bits, 0, 0, nullptr),
};

const FieldInfo kListener[] = {
    FE_F(AudioListener, volume, "Volume", Float, 0, 2, nullptr),
    FE_F(AudioListener, current, "Current", Bool, 0, 0, nullptr),
};

const FieldInfo kStream[] = {
    FE_F(AudioStream, path, "File", Path, 0, 0, nullptr),
    FE_F(AudioStream, volume, "Volume", Float, 0, 2, nullptr),
    FE_F(AudioStream, pitch, "Pitch", Float, 0.25f, 4, nullptr),
    FE_F(AudioStream, loop, "Loop", Bool, 0, 0, nullptr),
    FE_F(AudioStream, play_on_start, "Play on start", Bool, 0, 0, nullptr),
    FE_F(AudioStream, spatial, "Spatial", Bool, 0, 0, nullptr),
    FE_F(AudioStream, min_distance, "Min distance", Float, 0, 1000, nullptr),
    FE_F(AudioStream, max_distance, "Max distance", Float, 0, 10000, nullptr),
};

const FieldInfo kSampler[] = {
    FE_F(AudioSampler, path, "File", Path, 0, 0, nullptr),
    FE_F(AudioSampler, volume, "Volume", Float, 0, 2, nullptr),
    FE_F(AudioSampler, pitch, "Pitch", Float, 0.25f, 4, nullptr),
    FE_F(AudioSampler, pitch_variation, "Pitch random", Float, 0, 1, nullptr),
    FE_F(AudioSampler, voices, "Voices", Int, 1, 32, nullptr),
    FE_F(AudioSampler, spatial, "Spatial", Bool, 0, 0, nullptr),
    FE_F(AudioSampler, min_distance, "Min distance", Float, 0, 1000, nullptr),
    FE_F(AudioSampler, max_distance, "Max distance", Float, 0, 10000, nullptr),
};

const FieldInfo kLight[] = {
    FE_F(Light, type, "Type", Enum, 0, 0, kLightTypes),
    FE_F(Light, color, "Color", Color, 0, 0, nullptr),
    FE_F(Light, intensity, "Intensity", Float, 0, 100, nullptr),
    FE_F(Light, range, "Range", Float, 0.01f, 1000, nullptr),
    FE_F(Light, spot_angle, "Spot angle", Float, 1, 179, nullptr),
    FE_F(Light, spot_blend, "Spot blend", Float, 0, 1, nullptr),
    FE_F(Light, shadows, "Shadows", Bool, 0, 0, nullptr),
};

const FieldInfo kCamera[] = {
    FE_F(Camera, current, "Current", Bool, 0, 0, nullptr),
    FE_F(Camera, projection, "Projection", Enum, 0, 0, kProjections),
    FE_F(Camera, fov, "Field of view", Float, 5, 170, nullptr),
    FE_F(Camera, ortho_size, "Ortho size", Float, 0.01f, 1000, nullptr),
    FE_F(Camera, near_plane, "Near", Float, 0.001f, 100, nullptr),
    FE_F(Camera, far_plane, "Far", Float, 1, 100000, nullptr),
};

const FieldInfo kSkyLight[] = {
    FE_F(SkyLight, current, "Current", Bool, 0, 0, nullptr),
    FE_F(SkyLight, sky_color, "Sky color", Color, 0, 0, nullptr),
    FE_F(SkyLight, ground_color, "Ground color", Color, 0, 0, nullptr),
    FE_F(SkyLight, intensity, "Intensity", Float, 0, 10, nullptr),
};

const FieldInfo kSky[] = {
    FE_F(Sky, current, "Current", Bool, 0, 0, nullptr),
    FE_F(Sky, mode, "Mode", Enum, 0, 0, kSkyModes),
    FE_F(Sky, zenith, "Zenith", Color, 0, 0, nullptr),
    FE_F(Sky, horizon, "Horizon", Color, 0, 0, nullptr),
    FE_F(Sky, ground, "Ground", Color, 0, 0, nullptr),
    FE_F(Sky, sun_size, "Sun size", Float, 0, 20, nullptr),
    FE_F(Sky, intensity, "Intensity", Float, 0, 10, nullptr),
    FE_F(Sky, fog, "Fog", Float, 0, 1, nullptr),
};

const FieldInfo kPost[] = {
    FE_F(PostProcess, current, "Current", Bool, 0, 0, nullptr),
    FE_F(PostProcess, exposure, "Exposure", Float, -8, 8, nullptr),
    FE_F(PostProcess, tonemap, "Tonemap", Enum, 0, 0, kTonemaps),
    FE_F(PostProcess, contrast, "Contrast", Float, 0, 2, nullptr),
    FE_F(PostProcess, saturation, "Saturation", Float, 0, 2, nullptr),
    FE_F(PostProcess, tint, "Tint", Color, 0, 0, nullptr),
    FE_F(PostProcess, vignette, "Vignette", Float, 0, 1, nullptr),
    FE_F(PostProcess, bloom, "Bloom", Float, 0, 1, nullptr),
    FE_F(PostProcess, bloom_threshold, "Bloom from", Float, 0, 10, nullptr),
};

#undef FE_F

#define FE_M(kind, T, name, label, fields, cur, curoff) \
    { ModuleKind::kind, name, label, sizeof(T), alignof(T), fields, (int)(sizeof(fields) / sizeof(fields[0])), cur, curoff }

const ModuleInfo kInfos[kModuleKindCount] = {
    FE_M(RigidBody3D, RigidBody3D, "rigidbody3d", "Rigidbody 3D", kRigidBody, false, 0),
    FE_M(Collider3D, Collider3D, "collider3d", "Collider 3D", kCollider, false, 0),
    FE_M(AudioListener, AudioListener, "listener", "Audio Listener", kListener, true, offsetof(AudioListener, current)),
    FE_M(AudioStream, AudioStream, "audio_stream", "Audio Stream", kStream, false, 0),
    FE_M(AudioSampler, AudioSampler, "audio_sampler", "Audio Sampler", kSampler, false, 0),
    FE_M(Light, Light, "light", "Light", kLight, false, 0),
    FE_M(Camera, Camera, "camera", "Camera", kCamera, true, offsetof(Camera, current)),
    FE_M(SkyLight, SkyLight, "skylight", "Sky Light", kSkyLight, true, offsetof(SkyLight, current)),
    FE_M(Sky, Sky, "sky", "Sky", kSky, true, offsetof(Sky, current)),
    FE_M(PostProcess, PostProcess, "post_process", "Post Processing", kPost, true, offsetof(PostProcess, current)),
};

#undef FE_M

}  // namespace

const ModuleInfo& module_info(ModuleKind kind) {
    int i = (int)kind;
    if (i < 0 || i >= kModuleKindCount) i = 0;
    return kInfos[i];
}

ModuleKind module_by_name(const char* name) {
    if (!name) return ModuleKind::Count;
    for (int i = 0; i < kModuleKindCount; i++)
        if (strcmp(kInfos[i].name, name) == 0) return (ModuleKind)i;
    return ModuleKind::Count;
}

const FieldInfo* find_field(const ModuleInfo& info, const char* name) {
    for (int i = 0; i < info.field_count; i++)
        if (strcmp(info.fields[i].name, name) == 0) return &info.fields[i];
    return nullptr;
}

ModuleKind settings_of(ObjectKind kind) {
    switch (kind) {
        case ObjectKind::Camera: return ModuleKind::Camera;
        case ObjectKind::SkyLight: return ModuleKind::SkyLight;
        case ObjectKind::Sky: return ModuleKind::Sky;
        case ObjectKind::PostProcess: return ModuleKind::PostProcess;
        default: return ModuleKind::Count;
    }
}

ObjectKind object_kind_of(ModuleKind kind) {
    switch (kind) {
        case ModuleKind::Camera: return ObjectKind::Camera;
        case ModuleKind::SkyLight: return ObjectKind::SkyLight;
        case ModuleKind::Sky: return ObjectKind::Sky;
        case ModuleKind::PostProcess: return ObjectKind::PostProcess;
        default: return ObjectKind::Count;
    }
}

const char* object_kind_name(ObjectKind kind) {
    switch (kind) {
        case ObjectKind::Mesh: return "Mesh";
        case ObjectKind::Empty: return "Empty";
        case ObjectKind::Camera: return "Camera";
        case ObjectKind::SkyLight: return "Sky Light";
        case ObjectKind::Sky: return "Sky";
        case ObjectKind::PostProcess: return "Post Processing";
        default: return "?";
    }
}

void* ModuleSet::data(ModuleKind k) {
    switch (k) {
        case ModuleKind::RigidBody3D: return &rigidbody3d;
        case ModuleKind::Collider3D: return &collider3d;
        case ModuleKind::AudioListener: return &audio_listener;
        case ModuleKind::AudioStream: return &audio_stream;
        case ModuleKind::AudioSampler: return &audio_sampler;
        case ModuleKind::Light: return &light;
        case ModuleKind::Camera: return &camera;
        case ModuleKind::SkyLight: return &sky_light;
        case ModuleKind::Sky: return &sky;
        case ModuleKind::PostProcess: return &post_process;
        default: return nullptr;
    }
}

const void* ModuleSet::data(ModuleKind k) const { return const_cast<ModuleSet*>(this)->data(k); }

void ModuleSet::reset(ModuleKind k) {
    switch (k) {
        case ModuleKind::RigidBody3D: rigidbody3d = RigidBody3D(); break;
        case ModuleKind::Collider3D: collider3d = Collider3D(); break;
        case ModuleKind::AudioListener: audio_listener = AudioListener(); break;
        case ModuleKind::AudioStream: audio_stream = AudioStream(); break;
        case ModuleKind::AudioSampler: audio_sampler = AudioSampler(); break;
        case ModuleKind::Light: light = Light(); break;
        case ModuleKind::Camera: camera = Camera(); break;
        case ModuleKind::SkyLight: sky_light = SkyLight(); break;
        case ModuleKind::Sky: sky = Sky(); break;
        case ModuleKind::PostProcess: post_process = PostProcess(); break;
        default: break;
    }
}

}  // namespace fe
