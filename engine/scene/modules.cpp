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
const char* const kTonemaps[] = { "None", "Reinhard", "ACES", "AgX", nullptr };
const char* const kEmitShapes[] = { "Point", "Sphere", "Cone", "Box", nullptr };
const char* const kBlends[] = { "Additive", "Alpha", nullptr };
const char* const kSpaces[] = { "World", "Local", nullptr };
const char* const kJointTypes[] = { "Fixed", "Hinge", "Spring", "Ball socket", nullptr };
const char* const kFieldShapes[] = { "Box", "Sphere", nullptr };
const char* const kFieldModes[] = { "Directional", "Radial", "Vortex", nullptr };
const char* const kFalloffs[] = { "None", "Linear", "Quadratic", nullptr };
const char* const kBillboardModes[] = { "Full", "Vertical", nullptr };
const char* const kAligns[] = { "Left", "Center", "Right", nullptr };
const char* const kCloudQualities[] = { "Low", "Medium", "Ultra", nullptr };

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

const FieldInfo kCharacterController[] = {
    FE_F(CharacterController3D, radius, "Radius", Float, 0.001f, 1000.0f, nullptr),
    FE_F(CharacterController3D, height, "Height", Float, 0.001f, 1000.0f, nullptr),
    FE_F(CharacterController3D, center, "Center", Vec3, 0, 0, nullptr),
    FE_F(CharacterController3D, slope_limit, "Slope limit", Float, 0, 89, nullptr),
    FE_F(CharacterController3D, use_gravity, "Use gravity", Bool, 0, 0, nullptr),
    FE_F(CharacterController3D, gravity_scale, "Gravity scale", Float, -10, 10, nullptr),
    FE_F(CharacterController3D, velocity, "Velocity", Vec3, 0, 0, nullptr),
    FE_F(CharacterController3D, grounded, "Grounded", Bool, 0, 0, nullptr),
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

const FieldInfo kParticles[] = {
    FE_F(ParticleEmitter, playing, "Playing", Bool, 0, 0, nullptr),
    FE_F(ParticleEmitter, play_on_start, "Play on start", Bool, 0, 0, nullptr),
    FE_F(ParticleEmitter, looping, "Loop", Bool, 0, 0, nullptr),
    FE_F(ParticleEmitter, duration, "Duration", Float, 0.05f, 600, nullptr),
    FE_F(ParticleEmitter, prewarm, "Prewarm", Bool, 0, 0, nullptr),
    FE_F(ParticleEmitter, emission_rate, "Rate (per s)", Float, 0, 2000, nullptr),
    FE_F(ParticleEmitter, burst_count, "Burst", Int, 0, 2000, nullptr),
    FE_F(ParticleEmitter, burst_interval, "Burst every", Float, 0, 60, nullptr),
    FE_F(ParticleEmitter, max_particles, "Max particles", Int, 1, 2000, nullptr),
    FE_F(ParticleEmitter, lifetime_min, "Lifetime min", Float, 0.05f, 60, nullptr),
    FE_F(ParticleEmitter, lifetime_max, "Lifetime max", Float, 0.05f, 60, nullptr),
    FE_F(ParticleEmitter, speed_min, "Speed min", Float, 0, 200, nullptr),
    FE_F(ParticleEmitter, speed_max, "Speed max", Float, 0, 200, nullptr),
    FE_F(ParticleEmitter, shape, "Shape", Enum, 0, 0, kEmitShapes),
    FE_F(ParticleEmitter, shape_radius, "Shape radius", Float, 0, 100, nullptr),
    FE_F(ParticleEmitter, box_size, "Box size", Vec3, 0, 0, nullptr),
    FE_F(ParticleEmitter, cone_angle, "Cone angle", Float, 0, 90, nullptr),
    FE_F(ParticleEmitter, gravity, "Gravity", Float, -5, 5, nullptr),
    FE_F(ParticleEmitter, drag, "Drag", Float, 0, 20, nullptr),
    FE_F(ParticleEmitter, size_start, "Size start", Float, 0, 50, nullptr),
    FE_F(ParticleEmitter, size_end, "Size end", Float, 0, 50, nullptr),
    FE_F(ParticleEmitter, color_start, "Color start", Color, 0, 0, nullptr),
    FE_F(ParticleEmitter, color_end, "Color end", Color, 0, 0, nullptr),
    FE_F(ParticleEmitter, alpha_start, "Alpha start", Float, 0, 1, nullptr),
    FE_F(ParticleEmitter, alpha_end, "Alpha end", Float, 0, 1, nullptr),
    FE_F(ParticleEmitter, texture, "Texture", Path, 0, 0, nullptr),
    FE_F(ParticleEmitter, blend, "Blend", Enum, 0, 0, kBlends),
    FE_F(ParticleEmitter, space, "Space", Enum, 0, 0, kSpaces),
    FE_F(ParticleEmitter, seed, "Seed", Int, 0, 1000000, nullptr),
    FE_F(ParticleEmitter, particle_count, "Alive", Int, 0, 0, nullptr),
};

const FieldInfo kJoint[] = {
    FE_F(Joint3D, type, "Type", Enum, 0, 0, kJointTypes),
    FE_F(Joint3D, connected, "Connected to", Object, 0, 0, nullptr),
    FE_F(Joint3D, anchor, "Anchor", Vec3, 0, 0, nullptr),
    FE_F(Joint3D, axis, "Axis", Vec3, 0, 0, nullptr),
    FE_F(Joint3D, use_limits, "Limits", Bool, 0, 0, nullptr),
    FE_F(Joint3D, limit_min, "Limit min", Float, -180, 180, nullptr),
    FE_F(Joint3D, limit_max, "Limit max", Float, -180, 180, nullptr),
    FE_F(Joint3D, use_motor, "Motor", Bool, 0, 0, nullptr),
    FE_F(Joint3D, motor_speed, "Motor speed", Float, -3600, 3600, nullptr),
    FE_F(Joint3D, motor_torque, "Motor torque", Float, 0, 100000, nullptr),
    FE_F(Joint3D, rest_length, "Rest length", Float, 0, 1000, nullptr),
    FE_F(Joint3D, stiffness, "Stiffness", Float, 0, 100000, nullptr),
    FE_F(Joint3D, damping, "Damping", Float, 0, 10000, nullptr),
    FE_F(Joint3D, break_force, "Break force", Float, 0, 1000000, nullptr),
    FE_F(Joint3D, collide_connected, "Collide connected", Bool, 0, 0, nullptr),
    FE_F(Joint3D, broken, "Broken", Bool, 0, 0, nullptr),
    FE_F(Joint3D, angle, "Angle", Float, 0, 0, nullptr),
    FE_F(Joint3D, force, "Force", Float, 0, 0, nullptr),
};

const FieldInfo kAnimator[] = {
    FE_F(Animator, source, "Animation file", Path, 0, 0, nullptr),
    FE_F(Animator, clip, "Clip", Text, 0, 0, nullptr),
    FE_F(Animator, speed, "Speed", Float, -4, 4, nullptr),
    FE_F(Animator, loop, "Loop", Bool, 0, 0, nullptr),
    FE_F(Animator, play_on_start, "Play on start", Bool, 0, 0, nullptr),
    FE_F(Animator, crossfade, "Crossfade", Float, 0, 5, nullptr),
    FE_F(Animator, playing, "Playing", Bool, 0, 0, nullptr),
    FE_F(Animator, time, "Time", Float, 0, 100000, nullptr),
    FE_F(Animator, duration, "Duration", Float, 0, 0, nullptr),
};

const FieldInfo kForceField[] = {
    FE_F(ForceField, enabled, "Enabled", Bool, 0, 0, nullptr),
    FE_F(ForceField, shape, "Shape", Enum, 0, 0, kFieldShapes),
    FE_F(ForceField, size, "Size", Vec3, 0, 0, nullptr),
    FE_F(ForceField, radius, "Radius", Float, 0.01f, 1000, nullptr),
    FE_F(ForceField, mode, "Mode", Enum, 0, 0, kFieldModes),
    FE_F(ForceField, strength, "Strength", Float, -100000, 100000, nullptr),
    FE_F(ForceField, direction, "Direction", Vec3, 0, 0, nullptr),
    FE_F(ForceField, falloff, "Falloff", Enum, 0, 0, kFalloffs),
    FE_F(ForceField, turbulence, "Turbulence", Float, 0, 1, nullptr),
    FE_F(ForceField, turbulence_scale, "Swirl size", Float, 0.05f, 100, nullptr),
    FE_F(ForceField, turbulence_speed, "Swirl speed", Float, 0, 20, nullptr),
    FE_F(ForceField, affect_bodies, "Pushes bodies", Bool, 0, 0, nullptr),
    FE_F(ForceField, affect_particles, "Pushes particles", Bool, 0, 0, nullptr),
    FE_F(ForceField, layers, "Layers", Bits, 0, 0, nullptr),
};

const FieldInfo kSpawner[] = {
    FE_F(Spawner, source, "Copy of", Object, 0, 0, nullptr),
    FE_F(Spawner, auto_spawn, "Automatic", Bool, 0, 0, nullptr),
    FE_F(Spawner, spawn_on_start, "One at start", Bool, 0, 0, nullptr),
    FE_F(Spawner, interval, "Every (s)", Float, 0.01f, 3600, nullptr),
    FE_F(Spawner, max_alive, "Max alive", Int, 1, 500, nullptr),
    FE_F(Spawner, total, "Total (0 = no end)", Int, 0, 100000, nullptr),
    FE_F(Spawner, offset, "Random offset", Vec3, 0, 0, nullptr),
    FE_F(Spawner, velocity, "Velocity", Vec3, 0, 0, nullptr),
    FE_F(Spawner, velocity_random, "Random speed", Float, 0, 100, nullptr),
    FE_F(Spawner, inherit_velocity, "Inherit motion", Float, 0, 2, nullptr),
    FE_F(Spawner, lifetime, "Lifetime (s)", Float, 0, 3600, nullptr),
    FE_F(Spawner, random_rotation, "Random rotation", Bool, 0, 0, nullptr),
    FE_F(Spawner, seed, "Seed", Int, 0, 1000000, nullptr),
    FE_F(Spawner, spawned, "Spawned", Int, 0, 0, nullptr),
    FE_F(Spawner, alive, "Alive", Int, 0, 0, nullptr),
};

const FieldInfo kReverb[] = {
    FE_F(ReverbZone, enabled, "Enabled", Bool, 0, 0, nullptr),
    FE_F(ReverbZone, shape, "Shape", Enum, 0, 0, kFieldShapes),
    FE_F(ReverbZone, size, "Size", Vec3, 0, 0, nullptr),
    FE_F(ReverbZone, radius, "Radius", Float, 0.01f, 1000, nullptr),
    FE_F(ReverbZone, wet, "Wet", Float, 0, 1, nullptr),
    FE_F(ReverbZone, room_size, "Room size", Float, 0, 1, nullptr),
    FE_F(ReverbZone, damping, "Damping", Float, 0, 1, nullptr),
    FE_F(ReverbZone, lowpass, "Muffle", Float, 0, 1, nullptr),
    FE_F(ReverbZone, fade_distance, "Fade distance", Float, 0, 100, nullptr),
};

const FieldInfo kBillboard[] = {
    FE_F(Billboard, visible, "Visible", Bool, 0, 0, nullptr),
    FE_F(Billboard, texture, "Texture", Path, 0, 0, nullptr),
    FE_F(Billboard, color, "Color", Color, 0, 0, nullptr),
    FE_F(Billboard, alpha, "Alpha", Float, 0, 1, nullptr),
    FE_F(Billboard, width, "Width", Float, 0.001f, 1000, nullptr),
    FE_F(Billboard, height, "Height", Float, 0.001f, 1000, nullptr),
    FE_F(Billboard, mode, "Faces", Enum, 0, 0, kBillboardModes),
    FE_F(Billboard, blend, "Blend", Enum, 0, 0, kBlends),
};

const FieldInfo kText3D[] = {
    FE_F(Text3D, visible, "Visible", Bool, 0, 0, nullptr),
    FE_F(Text3D, text, "Text", Text, 0, 0, nullptr),
    FE_F(Text3D, color, "Color", Color, 0, 0, nullptr),
    FE_F(Text3D, size, "Size", Float, 0.01f, 100, nullptr),
    FE_F(Text3D, align, "Align", Enum, 0, 0, kAligns),
    FE_F(Text3D, outline, "Outline", Bool, 0, 0, nullptr),
};

const FieldInfo kCamera[] = {
    FE_F(Camera, current, "Current", Bool, 0, 0, nullptr),
    FE_F(Camera, projection, "Projection", Enum, 0, 0, kProjections),
    FE_F(Camera, fov, "Field of view", Float, 5, 170, nullptr),
    FE_F(Camera, ortho_size, "Ortho size", Float, 0.01f, 1000, nullptr),
    FE_F(Camera, near_plane, "Near", Float, 0.001f, 100, nullptr),
    FE_F(Camera, far_plane, "Far", Float, 1, 100000, nullptr),
    FE_F(Camera, shader1, "Shader 1", Path, 0, 0, nullptr),
    FE_F(Camera, shader2, "Shader 2", Path, 0, 0, nullptr),
    FE_F(Camera, shader3, "Shader 3", Path, 0, 0, nullptr),
    FE_F(Camera, shader_params, "Shader params", Vec3, 0, 0, nullptr),
};

const FieldInfo kSkyLight[] = {
    FE_F(SkyLight, current, "Current", Bool, 0, 0, nullptr),
    FE_F(SkyLight, sky_color, "Sky color", Color, 0, 0, nullptr),
    FE_F(SkyLight, ground_color, "Ground color", Color, 0, 0, nullptr),
    FE_F(SkyLight, intensity, "Intensity", Float, 0, 10, nullptr),
    FE_F(SkyLight, gi_intensity, "GI intensity", Float, 0, 2, nullptr),
    FE_F(SkyLight, gi_bounce, "GI bounce", Float, 0, 1, nullptr),
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
    FE_F(PostProcess, bloom_radius, "Bloom radius", Float, 0, 1, nullptr),
    FE_F(PostProcess, lift, "Lift", Color, 0, 0, nullptr),
    FE_F(PostProcess, gamma, "Gamma", Vec3, 0.2f, 3, nullptr),
    FE_F(PostProcess, gain, "Gain", Vec3, 0, 3, nullptr),
    FE_F(PostProcess, chromatic_aberration, "Chromatic aberration", Float, 0, 1, nullptr),
    FE_F(PostProcess, film_grain, "Film grain", Float, 0, 1, nullptr),
    FE_F(PostProcess, auto_exposure, "Auto exposure", Bool, 0, 0, nullptr),
    FE_F(PostProcess, exposure_speed, "Adapt speed", Float, 0.1f, 10, nullptr),
    FE_F(PostProcess, auto_exposure_min, "Auto min EV", Float, -8, 0, nullptr),
    FE_F(PostProcess, auto_exposure_max, "Auto max EV", Float, 0, 8, nullptr),
};

#define FE_FOLIAGE(n) \
    { "foliage" #n "_name", "Foliage " #n " name", FieldType::Text, (uint16_t)offsetof(Terrain, foliage[n - 1].name), 0, 0, nullptr }, \
    { "foliage" #n "_model", "Foliage " #n " models", FieldType::PathList, (uint16_t)offsetof(Terrain, foliage[n - 1].model), 0, 0, nullptr }, \
    { "foliage" #n "_density", "Foliage " #n " density", FieldType::Float, (uint16_t)offsetof(Terrain, foliage[n - 1].density), 0, 50, nullptr }, \
    { "foliage" #n "_scale_min", "Foliage " #n " min scale", FieldType::Float, (uint16_t)offsetof(Terrain, foliage[n - 1].scale_min), 0.01f, 100, nullptr }, \
    { "foliage" #n "_scale_max", "Foliage " #n " max scale", FieldType::Float, (uint16_t)offsetof(Terrain, foliage[n - 1].scale_max), 0.01f, 100, nullptr }, \
    { "foliage" #n "_distance", "Foliage " #n " distance", FieldType::Float, (uint16_t)offsetof(Terrain, foliage[n - 1].distance), 1, 5000, nullptr }, \
    { "foliage" #n "_lod_distance", "Foliage " #n " LOD distance", FieldType::Float, (uint16_t)offsetof(Terrain, foliage[n - 1].lod_distance), 0, 5000, nullptr }, \
    { "foliage" #n "_max_slope", "Foliage " #n " max slope", FieldType::Float, (uint16_t)offsetof(Terrain, foliage[n - 1].max_slope), 0, 90, nullptr }, \
    { "foliage" #n "_align", "Foliage " #n " align to slope", FieldType::Bool, (uint16_t)offsetof(Terrain, foliage[n - 1].align_to_slope), 0, 0, nullptr }, \
    { "foliage" #n "_shadows", "Foliage " #n " shadows", FieldType::Bool, (uint16_t)offsetof(Terrain, foliage[n - 1].cast_shadows), 0, 0, nullptr }, \
    { "foliage" #n "_shader", "Foliage " #n " shader", FieldType::Path, (uint16_t)offsetof(Terrain, foliage[n - 1].shader), 0, 0, nullptr }

const FieldInfo kTerrain[] = {
    FE_F(Terrain, data, "Data", Path, 0, 0, nullptr),
    FE_F(Terrain, load_distance, "Load distance", Float, 1, 100000, nullptr),
    FE_F(Terrain, stream_in_editor, "Stream in editor", Bool, 0, 0, nullptr),
    FE_F(Terrain, lod_distance, "Detail distance", Float, 1, 10000, nullptr),
    FE_F(Terrain, texture, "Texture", Path, 0, 0, nullptr),
    FE_F(Terrain, color, "Color", Color, 0, 0, nullptr),
    FE_F(Terrain, roughness, "Roughness", Float, 0, 1, nullptr),
    FE_F(Terrain, uv_scale, "Texture size", Float, 0.01f, 100000, nullptr),
    FE_F(Terrain, detail_texture, "Detail texture", Path, 0, 0, nullptr),
    FE_F(Terrain, detail_scale, "Detail size", Float, 0.01f, 1000, nullptr),
    FE_F(Terrain, detail_strength, "Detail strength", Float, 0, 1, nullptr),
    FE_F(Terrain, cast_shadows, "Cast shadows", Bool, 0, 0, nullptr),
    FE_F(Terrain, friction, "Friction", Float, 0, 1, nullptr),
    FE_F(Terrain, layer, "Layers", Bits, 0, 0, nullptr),
    FE_F(Terrain, revision, "Revision", Int, 0, 0, nullptr),
    FE_FOLIAGE(1), FE_FOLIAGE(2), FE_FOLIAGE(3), FE_FOLIAGE(4), FE_FOLIAGE(5), FE_FOLIAGE(6), FE_FOLIAGE(7), FE_FOLIAGE(8),
};
static_assert(kFoliageLayers == 8, "kTerrain lists eight foliage layers");
static_assert(sizeof(Terrain) < 65536, "FieldInfo offsets are 16 bit");

const FieldInfo kClouds[] = {
    FE_F(Clouds, current, "Current", Bool, 0, 0, nullptr),
    FE_F(Clouds, quality, "Quality", Enum, 0, 0, kCloudQualities),
    FE_F(Clouds, height, "Height", Float, -10000, 100000, nullptr),
    FE_F(Clouds, thickness, "Thickness", Float, 10, 20000, nullptr),
    FE_F(Clouds, layers, "Layers", Int, 1, 3, nullptr),
    FE_F(Clouds, layer_spacing, "Layer spacing", Float, 0, 20000, nullptr),
    FE_F(Clouds, coverage, "Coverage", Float, 0, 1, nullptr),
    FE_F(Clouds, density, "Density", Float, 0, 3, nullptr),
    FE_F(Clouds, color, "Color", Color, 0, 0, nullptr),
    FE_F(Clouds, scale, "Scale", Float, 0.05f, 20, nullptr),
    FE_F(Clouds, wind, "Wind", Vec3, 0, 0, nullptr),
    FE_F(Clouds, sun_glow, "Sun glow", Float, 0, 1, nullptr),
    FE_F(Clouds, ambient, "Ambient", Float, 0, 2, nullptr),
    FE_F(Clouds, distance, "Fade distance", Float, 500, 200000, nullptr),
};

#undef FE_FOLIAGE
#undef FE_F

#define FE_M(kind, T, name, label, fields, cur, curoff) \
    { ModuleKind::kind, name, label, sizeof(T), alignof(T), fields, (int)(sizeof(fields) / sizeof(fields[0])), cur, curoff }

const ModuleInfo kInfos[kModuleKindCount] = {
    FE_M(RigidBody3D, RigidBody3D, "rigidbody3d", "Rigidbody 3D", kRigidBody, false, 0),
    FE_M(Collider3D, Collider3D, "collider3d", "Collider 3D", kCollider, false, 0),
    FE_M(CharacterController3D, CharacterController3D, "character_controller3d", "Character Controller 3D", kCharacterController, false, 0),
    FE_M(AudioListener, AudioListener, "listener", "Audio Listener", kListener, true, offsetof(AudioListener, current)),
    FE_M(AudioStream, AudioStream, "audio_stream", "Audio Stream", kStream, false, 0),
    FE_M(AudioSampler, AudioSampler, "audio_sampler", "Audio Sampler", kSampler, false, 0),
    FE_M(Light, Light, "light", "Light", kLight, false, 0),
    FE_M(ParticleEmitter, ParticleEmitter, "particles", "Particle Emitter", kParticles, false, 0),
    FE_M(Joint3D, Joint3D, "joint3d", "Joint 3D", kJoint, false, 0),
    FE_M(Animator, Animator, "animator", "Animator", kAnimator, false, 0),
    FE_M(ForceField, ForceField, "force_field", "Force Field", kForceField, false, 0),
    FE_M(Spawner, Spawner, "spawner", "Spawner", kSpawner, false, 0),
    FE_M(ReverbZone, ReverbZone, "reverb_zone", "Audio Reverb Zone", kReverb, false, 0),
    FE_M(Billboard, Billboard, "billboard", "Billboard", kBillboard, false, 0),
    FE_M(Text3D, Text3D, "text3d", "Text 3D", kText3D, false, 0),
    FE_M(Camera, Camera, "camera", "Camera", kCamera, true, offsetof(Camera, current)),
    FE_M(SkyLight, SkyLight, "skylight", "Sky Light", kSkyLight, true, offsetof(SkyLight, current)),
    FE_M(Sky, Sky, "sky", "Sky", kSky, true, offsetof(Sky, current)),
    FE_M(PostProcess, PostProcess, "post_process", "Post Processing", kPost, true, offsetof(PostProcess, current)),
    FE_M(Terrain, Terrain, "terrain", "Terrain", kTerrain, false, 0),
    FE_M(Clouds, Clouds, "clouds", "Clouds", kClouds, true, offsetof(Clouds, current)),
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
        case ObjectKind::Terrain: return ModuleKind::Terrain;
        case ObjectKind::Clouds: return ModuleKind::Clouds;
        default: return ModuleKind::Count;
    }
}

ObjectKind object_kind_of(ModuleKind kind) {
    switch (kind) {
        case ModuleKind::Camera: return ObjectKind::Camera;
        case ModuleKind::SkyLight: return ObjectKind::SkyLight;
        case ModuleKind::Sky: return ObjectKind::Sky;
        case ModuleKind::PostProcess: return ObjectKind::PostProcess;
        case ModuleKind::Terrain: return ObjectKind::Terrain;
        case ModuleKind::Clouds: return ObjectKind::Clouds;
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
        case ObjectKind::Terrain: return "Terrain";
        case ObjectKind::Clouds: return "Clouds";
        default: return "?";
    }
}

void* ModuleSet::data(ModuleKind k) {
    switch (k) {
        case ModuleKind::RigidBody3D: return &rigidbody3d;
        case ModuleKind::Collider3D: return &collider3d;
        case ModuleKind::CharacterController3D: return &character_controller3d;
        case ModuleKind::AudioListener: return &audio_listener;
        case ModuleKind::AudioStream: return &audio_stream;
        case ModuleKind::AudioSampler: return &audio_sampler;
        case ModuleKind::Light: return &light;
        case ModuleKind::ParticleEmitter: return &particles;
        case ModuleKind::Joint3D: return &joint3d;
        case ModuleKind::Animator: return &animator;
        case ModuleKind::ForceField: return &force_field;
        case ModuleKind::Spawner: return &spawner;
        case ModuleKind::ReverbZone: return &reverb_zone;
        case ModuleKind::Billboard: return &billboard;
        case ModuleKind::Text3D: return &text3d;
        case ModuleKind::Camera: return &camera;
        case ModuleKind::SkyLight: return &sky_light;
        case ModuleKind::Sky: return &sky;
        case ModuleKind::PostProcess: return &post_process;
        case ModuleKind::Terrain: return &terrain;
        case ModuleKind::Clouds: return &clouds;
        default: return nullptr;
    }
}

const void* ModuleSet::data(ModuleKind k) const { return const_cast<ModuleSet*>(this)->data(k); }

void ModuleSet::reset(ModuleKind k) {
    switch (k) {
        case ModuleKind::RigidBody3D: rigidbody3d = RigidBody3D(); break;
        case ModuleKind::Collider3D: collider3d = Collider3D(); break;
        case ModuleKind::CharacterController3D: character_controller3d = CharacterController3D(); break;
        case ModuleKind::AudioListener: audio_listener = AudioListener(); break;
        case ModuleKind::AudioStream: audio_stream = AudioStream(); break;
        case ModuleKind::AudioSampler: audio_sampler = AudioSampler(); break;
        case ModuleKind::Light: light = Light(); break;
        case ModuleKind::ParticleEmitter: particles = ParticleEmitter(); break;
        case ModuleKind::Joint3D: joint3d = Joint3D(); break;
        case ModuleKind::Animator: animator = Animator(); break;
        case ModuleKind::ForceField: force_field = ForceField(); break;
        case ModuleKind::Spawner: spawner = Spawner(); break;
        case ModuleKind::ReverbZone: reverb_zone = ReverbZone(); break;
        case ModuleKind::Billboard: billboard = Billboard(); break;
        case ModuleKind::Text3D: text3d = Text3D(); break;
        case ModuleKind::Camera: camera = Camera(); break;
        case ModuleKind::SkyLight: sky_light = SkyLight(); break;
        case ModuleKind::Sky: sky = Sky(); break;
        case ModuleKind::PostProcess: post_process = PostProcess(); break;
        case ModuleKind::Terrain: terrain = Terrain(); break;
        case ModuleKind::Clouds: clouds = Clouds(); break;
        default: break;
    }
}

}  // namespace fe
