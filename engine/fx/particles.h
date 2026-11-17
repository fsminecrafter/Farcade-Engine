// CPU particles for the Particle Emitter module (scene/modules.h).
//
//   ParticleSim     one emitter's pool of particles, with a seeded random
//                   sequence: the same seed and the same calls give the same
//                   particles.
//   ParticleBank    many sims by key (an entity, or the editor's object id for
//                   its preview), updated, and gathered into the renderer's
//                   batches (gfx/particles.h).
//   ParticleSystem  a ParticleBank on a World: each frame (Phase::PostUpdate)
//                   it simulates every entity that has the module, reading its
//                   world transform and the Force Fields that push particles.
//
// Pools are allocated once (max_particles, at most kMaxPool) and reused: a
// frame allocates nothing. See docs/modules.md.
#pragma once

#include "ecs/ecs.h"
#include "fx/forcefield.h"
#include "gfx/particles.h"
#include "scene/modules.h"

namespace fe {

// How much the effects may cost (the editor's Settings > Graphics > Effects;
// a game sets its own). Everything defaults to full quality; `particles` off
// draws and simulates nothing.
struct FxQuality {
    bool particles = true;              // false: particles are off
    float density = 1.0f;               // 0.1..1: share of each emitter's rate and max_particles
    bool depth_test = true;             // renderer: meshes hide particles (off is cheaper: no depth map)
    bool sprites = true;                // Billboard and Text 3D modules are drawn
    bool reverb = true;                 // audio: Audio Reverb Zones apply (off: the effect is skipped)
};

constexpr int kMaxPool = 2000;          // particles per emitter
constexpr int kMaxFieldsPerStep = 16;   // force fields applied to particles

struct SimParticle {
    Vec3 pos, vel;                      // world space, or the emitter's local space (ParticleSpace::Local)
    float age, life;
};

class ParticleSim {
public:
    ParticleSim() {}
    ~ParticleSim();
    ParticleSim(const ParticleSim&) = delete;
    ParticleSim& operator=(const ParticleSim&) = delete;

    // Starts emitting: restarts the clock, reseeds the random sequence (cfg.seed)
    // and, with Prewarm, runs a looping emitter forward so it begins in full flow.
    void start(const ParticleEmitter& cfg, const Mat4& world);
    void stop() { playing_ = false; }       // no new particles; the rest live out
    void clear() { count_ = 0; }
    // `n` particles now, whether playing or not.
    void emit(const ParticleEmitter& cfg, const Mat4& world, int n);
    // Advances by dt seconds (clamped to 0.1). `time` is the clock the fields' turbulence uses.
    void update(const ParticleEmitter& cfg, const Mat4& world, float dt, const FieldVolume* fields, int field_count, float time);

    bool playing() const { return playing_; }
    int count() const { return count_; }
    // Appends the particles as renderer quads (world space), at most `max`; returns how many.
    int collect(const ParticleEmitter& cfg, const Mat4& world, gfx::Particle* out, int max) const;

    const SimParticle* particles() const { return p_; }

private:
    float rnd();                        // 0..1
    float rnd(float lo, float hi) { return lo + (hi - lo) * rnd(); }
    Vec3 unit_vector();
    void spawn(const ParticleEmitter& cfg, const Mat4& world, float age);
    void reserve(int n);
    void step(const ParticleEmitter& cfg, const Mat4& world, const Mat4& inv, bool have_inv, float dt,
              const FieldVolume* fields, int nf, float time);
    SimParticle* p_ = nullptr;
    int count_ = 0, cap_ = 0;
    uint32_t rng_ = 1;
    bool playing_ = false;
    float elapsed_ = 0, accum_ = 0, burst_timer_ = 0;
    bool burst_pending_ = false;
};

// Many emitters, by key.
class ParticleBank {
public:
    ParticleBank() {}
    ~ParticleBank();
    ParticleBank(const ParticleBank&) = delete;
    ParticleBank& operator=(const ParticleBank&) = delete;

    // Frame protocol: begin(), update() for each emitter that exists, end()
    // (drops the sims of emitters that were not updated).
    void begin();
    FxQuality quality;                  // applied by update() and collect()
    // Simulates emitter `key`. A new one starts playing when cfg.play_on_start. Returns its sim.
    ParticleSim* update(uint64_t key, const ParticleEmitter& cfg, const Mat4& world, float dt, const FieldVolume* fields, int field_count, float time);
    void end();

    // The sim of `key`, or null; with create, a new one that update() will not start by itself.
    ParticleSim* get(uint64_t key, bool create = false);
    int sim_count() const { return n_; }
    // Appends every updated emitter's particles as batches, far emitters
    // first. The arrays belong to the bank and stay valid until the next
    // collect() or begin(). Returns the number of batches; the quads total at most gfx::kMaxQuads.
    int collect(const Vec3& eye, const gfx::ParticleBatch** batches);

    struct Entry {
        uint64_t key = 0;
        ParticleSim* sim = nullptr;
        bool used = false;
        bool init_done = false;         // play_on_start was applied (or a control chose)
        ParticleEmitter cfg;            // the last config and pose, for collect()
        Mat4 world;
    };

private:
    Entry* e_ = nullptr;
    int n_ = 0, cap_ = 0;
    gfx::Particle* quads_ = nullptr;
    int quad_cap_ = 0;
    gfx::ParticleBatch* batches_ = nullptr;
    int batch_cap_ = 0;
};

class ParticleSystem {
public:
    ParticleSystem() {}
    ~ParticleSystem() { shutdown(); }
    // Registers the system (Phase::PostUpdate) on `world`.
    bool init(World* world);
    void shutdown();

    // Script and editor controls (false when the entity has no emitter).
    bool play(Entity e);
    bool stop(Entity e);
    bool clear(Entity e);
    bool emit(Entity e, int n);

    // The batches of the last frame for a camera at `eye` (see ParticleBank::collect).
    int collect(const Vec3& eye, const gfx::ParticleBatch** batches) { return bank_.collect(eye, batches); }
    ParticleBank& bank() { return bank_; }

private:
    static void system(World& w, float dt, void* user);
    World* world_ = nullptr;
    Entity system_ = 0;
    ParticleBank bank_;
    Entity* scan_ = nullptr;
    int scan_cap_ = 0;
    Entity* fscan_ = nullptr;
    int fscan_cap_ = 0;
    double time_ = 0;
    // A control asked for before the sim exists (it is created by the next frame).
    Mat4 pose_of(Entity e) const;
};

}  // namespace fe
