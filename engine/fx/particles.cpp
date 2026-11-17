#include "fx/particles.h"
#include "fx/internal.h"

#include <stdlib.h>
#include <string.h>
#include <new>

namespace fe {

namespace {

float lerpf(float a, float b, float t) { return a + (b - a) * t; }
constexpr float kGravity = 9.81f;

}  // namespace

// ================================================================= ParticleSim

ParticleSim::~ParticleSim() { free(p_); }

float ParticleSim::rnd() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return (float)(rng_ >> 8) * (1.0f / 16777216.0f);
}

Vec3 ParticleSim::unit_vector() {
    for (int i = 0; i < 8; i++) {
        Vec3 v(rnd() * 2 - 1, rnd() * 2 - 1, rnd() * 2 - 1);
        float l2 = dot(v, v);
        if (l2 > 0.0001f && l2 <= 1.0f) return v * (1.0f / fsqrt(l2));
    }
    return Vec3(0, 1, 0);
}

void ParticleSim::reserve(int n) {
    if (n > kMaxPool) n = kMaxPool;
    if (n <= cap_) return;
    SimParticle* np = (SimParticle*)realloc(p_, sizeof(SimParticle) * (size_t)n);
    if (!np) return;
    p_ = np;
    cap_ = n;
}

void ParticleSim::spawn(const ParticleEmitter& cfg, const Mat4& world, float age) {
    int limit = cfg.max_particles < 1 ? 1 : (cfg.max_particles > kMaxPool ? kMaxPool : cfg.max_particles);
    if (count_ >= limit) return;
    reserve(limit);
    if (count_ >= cap_) return;
    Vec3 lp, ld;
    switch (cfg.shape) {
    case EmitShape::Sphere: {
        Vec3 u = unit_vector();
        float r = cfg.shape_radius * rnd();
        lp = u * r;
        ld = u;
        break;
    }
    case EmitShape::Cone: {
        float th = radians(cfg.cone_angle) * rnd(), ph = 6.2831853f * rnd();
        ld = Vec3(fsin(th) * fcos(ph), fsin(th) * fsin(ph), -fcos(th));
        float r = cfg.shape_radius * fsqrt(rnd()), a = 6.2831853f * rnd();
        lp = Vec3(r * fcos(a), r * fsin(a), 0);
        break;
    }
    case EmitShape::Box:
        lp = Vec3((rnd() - 0.5f) * cfg.box_size.x, (rnd() - 0.5f) * cfg.box_size.y, (rnd() - 0.5f) * cfg.box_size.z);
        ld = Vec3(0, 0, -1);
        break;
    default:
        ld = unit_vector();
        break;
    }
    float speed = rnd(cfg.speed_min, cfg.speed_max);
    float lmin = cfg.lifetime_min, lmax = cfg.lifetime_max < lmin ? lmin : cfg.lifetime_max;
    SimParticle& s = p_[count_++];
    s.life = rnd(lmin, lmax);
    if (s.life < 0.01f) s.life = 0.01f;
    s.age = age < s.life ? age : 0;
    if (cfg.space == ParticleSpace::Local) {
        s.pos = lp;
        s.vel = ld * speed;
    } else {
        s.pos = transform_point(world, lp);
        s.vel = normalize(transform_dir(world, ld)) * speed;
    }
    s.pos += s.vel * s.age;
}

void ParticleSim::start(const ParticleEmitter& cfg, const Mat4& world) {
    uint32_t seed = (uint32_t)cfg.seed * 2654435761u + 0x9E3779B9u;
    rng_ = seed ? seed : 1u;
    playing_ = true;
    elapsed_ = 0;
    accum_ = 0;
    burst_timer_ = 0;
    burst_pending_ = cfg.burst_count > 0;
    if (cfg.looping && cfg.prewarm) {
        // Run forward in coarse steps so the emitter starts in full flow.
        float span = cfg.lifetime_max < 5.0f ? cfg.lifetime_max : 5.0f;
        Mat4 inv;
        bool have = invert(world, &inv);
        const float dt = 0.05f;
        for (float t = 0; t < span; t += dt) step(cfg, world, inv, have, dt, nullptr, 0, 0);
        elapsed_ = 0;
    }
}

void ParticleSim::emit(const ParticleEmitter& cfg, const Mat4& world, int n) {
    if (n > kMaxPool) n = kMaxPool;
    for (int i = 0; i < n; i++) spawn(cfg, world, 0);
}

void ParticleSim::step(const ParticleEmitter& cfg, const Mat4& world, const Mat4& inv, bool have_inv, float dt,
                       const FieldVolume* fields, int nf, float time) {
    // Emission.
    if (playing_) {
        if (burst_pending_) {
            emit(cfg, world, cfg.burst_count);
            burst_pending_ = false;
        }
        if (cfg.burst_count > 0 && cfg.burst_interval > 0) {
            burst_timer_ += dt;
            if (burst_timer_ >= cfg.burst_interval) {
                burst_timer_ -= cfg.burst_interval;
                emit(cfg, world, cfg.burst_count);
            }
        }
        accum_ += cfg.emission_rate * dt;
        int n = (int)accum_;
        accum_ -= (float)n;
        if (n > kMaxPool) n = kMaxPool;
        for (int i = 0; i < n; i++) spawn(cfg, world, rnd() * dt);
        elapsed_ += dt;
        if (!cfg.looping && elapsed_ >= cfg.duration) playing_ = false;
    }
    // Motion.
    const bool local = cfg.space == ParticleSpace::Local;
    Vec3 g(0, -kGravity * cfg.gravity, 0);
    if (local && have_inv) g = transform_dir(inv, g);
    const float damp = 1.0f / (1.0f + cfg.drag * dt);
    for (int i = 0; i < count_;) {
        SimParticle& s = p_[i];
        s.age += dt;
        if (s.age >= s.life) { s = p_[--count_]; continue; }
        Vec3 a = g;
        for (int f = 0; f < nf; f++) {
            if (!fields[f].f.affect_particles) continue;
            Vec3 wp = local ? transform_point(world, s.pos) : s.pos;
            Vec3 F = field_force(fields[f], wp, time);
            a += (local && have_inv) ? transform_dir(inv, F) : F;
        }
        s.vel += a * dt;
        s.vel = s.vel * damp;
        s.pos += s.vel * dt;
        i++;
    }
}

void ParticleSim::update(const ParticleEmitter& cfg, const Mat4& world, float dt, const FieldVolume* fields, int field_count, float time) {
    if (dt <= 0) return;
    if (dt > 0.1f) dt = 0.1f;
    Mat4 inv;
    bool have = invert(world, &inv);
    step(cfg, world, inv, have, dt, fields, field_count, time);
}

int ParticleSim::collect(const ParticleEmitter& cfg, const Mat4& world, gfx::Particle* out, int max) const {
    const bool local = cfg.space == ParticleSpace::Local;
    int n = count_ < max ? count_ : max;
    for (int i = 0; i < n; i++) {
        const SimParticle& s = p_[i];
        float u = s.life > 0 ? s.age / s.life : 1.0f;
        gfx::Particle& q = out[i];
        q.pos = local ? transform_point(world, s.pos) : s.pos;
        q.size = lerpf(cfg.size_start, cfg.size_end, u);
        q.r = lerpf(cfg.color_start.x, cfg.color_end.x, u);
        q.g = lerpf(cfg.color_start.y, cfg.color_end.y, u);
        q.b = lerpf(cfg.color_start.z, cfg.color_end.z, u);
        q.a = lerpf(cfg.alpha_start, cfg.alpha_end, u);
        q.rot = 0;
    }
    return n;
}

// ================================================================= ParticleBank

ParticleBank::~ParticleBank() {
    for (int i = 0; i < n_; i++) delete e_[i].sim;
    free(e_);
    free(quads_);
    free(batches_);
}

void ParticleBank::begin() {
    for (int i = 0; i < n_; i++) e_[i].used = false;
}

ParticleSim* ParticleBank::get(uint64_t key, bool create) {
    for (int i = 0; i < n_; i++)
        if (e_[i].key == key) return e_[i].sim;
    if (!create) return nullptr;
    if (n_ == cap_) {
        int nc = cap_ ? cap_ * 2 : 16;
        Entry* ne = (Entry*)malloc(sizeof(Entry) * (size_t)nc);
        if (!ne) return nullptr;
        for (int i = 0; i < n_; i++) new (&ne[i]) Entry(e_[i]);
        free(e_);
        e_ = ne;
        cap_ = nc;
    }
    Entry* en = new (&e_[n_++]) Entry();
    en->key = key;
    en->sim = new ParticleSim();
    en->init_done = true;
    return en->sim;
}

ParticleSim* ParticleBank::update(uint64_t key, const ParticleEmitter& cfg, const Mat4& world, float dt, const FieldVolume* fields, int field_count, float time) {
    Entry* en = nullptr;
    for (int i = 0; i < n_; i++)
        if (e_[i].key == key) { en = &e_[i]; break; }
    if (!en) {
        if (!get(key, true)) return nullptr;
        en = &e_[n_ - 1];
        en->init_done = false;
    }
    if (!en->init_done) {
        en->init_done = true;
        if (cfg.play_on_start) en->sim->start(cfg, world);
    }
    en->used = true;
    en->cfg = cfg;
    en->world = world;
    en->sim->update(cfg, world, dt, fields, field_count, time);
    return en->sim;
}

void ParticleBank::end() {
    for (int i = 0; i < n_;) {
        if (!e_[i].used) {
            delete e_[i].sim;
            e_[i].sim = nullptr;
            e_[i] = e_[--n_];
        } else {
            i++;
        }
    }
}

int ParticleBank::collect(const Vec3& eye, const gfx::ParticleBatch** out) {
    int total = 0, nb = 0;
    for (int i = 0; i < n_; i++) if (e_[i].used && e_[i].sim->count() > 0) { total += e_[i].sim->count(); nb++; }
    *out = nullptr;
    if (total == 0) return 0;
    if (total > gfx::kMaxQuads) total = gfx::kMaxQuads;
    if (quad_cap_ < total) {
        gfx::Particle* nq = (gfx::Particle*)realloc(quads_, sizeof(gfx::Particle) * (size_t)(total < 512 ? 512 : total * 2));
        if (!nq) return 0;
        quads_ = nq;
        quad_cap_ = total < 512 ? 512 : total * 2;
    }
    if (batch_cap_ < nb) {
        gfx::ParticleBatch* nbp = (gfx::ParticleBatch*)realloc(batches_, sizeof(gfx::ParticleBatch) * (size_t)(nb < 16 ? 16 : nb * 2));
        if (!nbp) return 0;
        batches_ = nbp;
        batch_cap_ = nb < 16 ? 16 : nb * 2;
    }
    // Far emitters first.
    int used[256];
    float dist[256];
    int m = 0;
    for (int i = 0; i < n_ && m < 256; i++) {
        if (!e_[i].used || e_[i].sim->count() == 0) continue;
        Vec3 d = Vec3(e_[i].world.m[12], e_[i].world.m[13], e_[i].world.m[14]) - eye;
        used[m] = i;
        dist[m] = dot(d, d);
        m++;
    }
    for (int i = 1; i < m; i++) {
        int u = used[i];
        float d = dist[i];
        int j = i;
        while (j > 0 && dist[j - 1] < d) { used[j] = used[j - 1]; dist[j] = dist[j - 1]; j--; }
        used[j] = u;
        dist[j] = d;
    }
    int q = 0, b = 0;
    for (int k = 0; k < m && q < total; k++) {
        Entry& en = e_[used[k]];
        int n = en.sim->collect(en.cfg, en.world, quads_ + q, total - q);
        if (n <= 0) continue;
        gfx::ParticleBatch& pb = batches_[b++];
        pb.items = quads_ + q;
        pb.count = n;
        pb.texture = en.cfg.texture[0] ? en.cfg.texture : nullptr;     // the copy in e_[] stays put until the next frame
        pb.additive = en.cfg.blend == ParticleBlend::Additive;
        q += n;
    }
    *out = batches_;
    return b;
}

// ================================================================= ParticleSystem

bool ParticleSystem::init(World* world) {
    shutdown();
    if (!world || !world->valid()) return false;
    world_ = world;
    system_ = world->add_system(Phase::PostUpdate, system, this);
    return system_ != 0;
}

void ParticleSystem::shutdown() {
    if (world_ && system_ && world_->valid()) world_->remove_system(system_);
    world_ = nullptr;
    system_ = 0;
    free(scan_);
    free(fscan_);
    scan_ = fscan_ = nullptr;
    scan_cap_ = fscan_cap_ = 0;
}

Mat4 ParticleSystem::pose_of(Entity e) const {
    const WorldTransform* wt = world_->world_transform(e);
    return wt ? wt->matrix : Mat4::identity();
}

void ParticleSystem::system(World& w, float dt, void* user) {
    ParticleSystem* S = (ParticleSystem*)user;
    S->time_ += dt;
    // The force fields that push particles.
    FieldVolume fields[kMaxFieldsPerStep];
    int nf = 0;
    int fn = fx_scan(w, ModuleKind::ForceField, &S->fscan_, &S->fscan_cap_);
    for (int i = 0; i < fn && nf < kMaxFieldsPerStep; i++) {
        const ForceField* f = (const ForceField*)w.get(S->fscan_[i], ModuleKind::ForceField);
        const WorldTransform* wt = w.world_transform(S->fscan_[i]);
        if (f && wt && f->affect_particles && field_volume(*f, wt->matrix, &fields[nf])) nf++;
    }
    S->bank_.begin();
    int n = fx_scan(w, ModuleKind::ParticleEmitter, &S->scan_, &S->scan_cap_);
    for (int i = 0; i < n; i++) {
        Entity e = S->scan_[i];
        ParticleEmitter* m = (ParticleEmitter*)w.get_mut(e, ModuleKind::ParticleEmitter);
        const WorldTransform* wt = w.world_transform(e);
        if (!m || !wt) continue;
        bool known = S->bank_.get(e) != nullptr;
        // The `playing` field is also a switch: scripts may set it.
        if (known) {
            ParticleSim* sim = S->bank_.get(e);
            if (m->playing && !sim->playing()) sim->start(*m, wt->matrix);
            else if (!m->playing && sim->playing()) sim->stop();
        }
        ParticleSim* sim = S->bank_.update(e, *m, wt->matrix, dt, fields, nf, (float)S->time_);
        if (!sim) continue;
        m->playing = sim->playing();
        m->particle_count = sim->count();
    }
    S->bank_.end();
}

bool ParticleSystem::play(Entity e) {
    if (!world_ || !world_->has(e, ModuleKind::ParticleEmitter)) return false;
    ParticleEmitter* m = (ParticleEmitter*)world_->get_mut(e, ModuleKind::ParticleEmitter);
    ParticleSim* sim = bank_.get(e, true);
    if (!sim) return false;
    if (!sim->playing()) sim->start(*m, pose_of(e));
    m->playing = true;
    return true;
}

bool ParticleSystem::stop(Entity e) {
    if (!world_ || !world_->has(e, ModuleKind::ParticleEmitter)) return false;
    ParticleEmitter* m = (ParticleEmitter*)world_->get_mut(e, ModuleKind::ParticleEmitter);
    if (ParticleSim* sim = bank_.get(e, true)) sim->stop();
    m->playing = false;
    return true;
}

bool ParticleSystem::clear(Entity e) {
    if (!world_ || !world_->has(e, ModuleKind::ParticleEmitter)) return false;
    if (ParticleSim* sim = bank_.get(e)) sim->clear();
    ParticleEmitter* m = (ParticleEmitter*)world_->get_mut(e, ModuleKind::ParticleEmitter);
    m->particle_count = 0;
    return true;
}

bool ParticleSystem::emit(Entity e, int n) {
    if (!world_ || !world_->has(e, ModuleKind::ParticleEmitter)) return false;
    ParticleEmitter* m = (ParticleEmitter*)world_->get_mut(e, ModuleKind::ParticleEmitter);
    ParticleSim* sim = bank_.get(e, true);
    if (!sim) return false;
    sim->emit(*m, pose_of(e), n);
    m->particle_count = sim->count();
    return true;
}

}  // namespace fe
