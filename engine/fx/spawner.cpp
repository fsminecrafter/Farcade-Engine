#include "fx/spawner.h"
#include "fx/fx.h"
#include "fx/internal.h"

#include <stdlib.h>
#include <string.h>
#include <new>

namespace fe {

bool SpawnerSystem::init(World* world, const FxHost* host) {
    shutdown();
    if (!world || !world->valid()) return false;
    world_ = world;
    host_ = host;
    return true;
}

void SpawnerSystem::shutdown() {
    for (int i = 0; i < count_; i++) free(states_[i].copies);
    free(states_);
    free(scan_);
    states_ = nullptr;
    scan_ = nullptr;
    count_ = cap_ = scan_cap_ = 0;
    world_ = nullptr;
    time_ = 0;
}

SpawnerSystem::State* SpawnerSystem::state(Entity e, bool create) {
    for (int i = 0; i < count_; i++)
        if (states_[i].spawner == e) return &states_[i];
    if (!create) return nullptr;
    if (count_ == cap_) {
        int nc = cap_ ? cap_ * 2 : 8;
        State* ns = (State*)malloc(sizeof(State) * (size_t)nc);
        if (!ns) return nullptr;
        for (int i = 0; i < count_; i++) new (&ns[i]) State(states_[i]);
        free(states_);
        states_ = ns;
        cap_ = nc;
    }
    State* st = new (&states_[count_++]) State();
    st->spawner = e;
    return st;
}

float SpawnerSystem::rnd(State& st) {
    st.rng ^= st.rng << 13;
    st.rng ^= st.rng >> 17;
    st.rng ^= st.rng << 5;
    return (float)(st.rng >> 8) * (1.0f / 16777216.0f);
}

void SpawnerSystem::drop_copy(State& st, int i) {
    if (world_->alive(st.copies[i].e)) world_->destroy(st.copies[i].e);
    st.copies[i] = st.copies[--st.n];
}

bool SpawnerSystem::spawn_one(State& st, Spawner& m) {
    if (!host_ || !host_->spawn) return false;
    Entity src = world_->link_entity(m.source);
    const WorldTransform* wt = world_->world_transform(st.spawner);
    if (!src || !wt) return false;
    int limit = m.max_alive < 1 ? 1 : m.max_alive;
    // The oldest goes when the spawner is full (copies are kept in birth order).
    while (st.n >= limit) {
        int oldest = 0;
        for (int i = 1; i < st.n; i++) if (st.copies[i].born < st.copies[oldest].born) oldest = i;
        drop_copy(st, oldest);
    }
    Vec3 t, s;
    Quat r;
    decompose(wt->matrix, &t, &r, &s);
    Vec3 off((rnd(st) * 2 - 1) * m.offset.x, (rnd(st) * 2 - 1) * m.offset.y, (rnd(st) * 2 - 1) * m.offset.z);
    Vec3 pos = transform_point(wt->matrix, off);
    Quat rot = r;
    if (m.random_rotation) {
        Vec3 ax(rnd(st) * 2 - 1, rnd(st) * 2 - 1, rnd(st) * 2 - 1);
        if (length(ax) < 0.01f) ax = Vec3(0, 1, 0);
        rot = Quat::axis_angle(normalize(ax), rnd(st) * 6.2831853f);
    }
    Vec3 scale(1, 1, 1);
    if (const WorldTransform* sw = world_->world_transform(src)) {
        Vec3 st_, ss;
        Quat sr;
        decompose(sw->matrix, &st_, &sr, &ss);
        scale = ss;
    }
    Entity e = host_->spawn(host_->user, src, Mat4::trs(pos, rot, scale));
    if (!e) return false;
    if (RigidBody3D* rb = (RigidBody3D*)world_->get_mut(e, ModuleKind::RigidBody3D)) {
        Vec3 v = rotate(r, m.velocity);
        if (m.velocity_random > 0) {
            Vec3 d(rnd(st) * 2 - 1, rnd(st) * 2 - 1, rnd(st) * 2 - 1);
            v += normalize(d) * (m.velocity_random * rnd(st));
        }
        v += st.vel * m.inherit_velocity;
        rb->velocity += v;
    }
    if (st.n == st.cap) {
        int nc = st.cap ? st.cap * 2 : 16;
        Copy* nb = (Copy*)realloc(st.copies, sizeof(Copy) * (size_t)nc);
        if (!nb) return true;
        st.copies = nb;
        st.cap = nc;
    }
    st.copies[st.n++] = { e, time_ };
    m.spawned++;
    m.alive = st.n;
    return true;
}

bool SpawnerSystem::spawn(Entity e) {
    if (!world_ || !world_->has(e, ModuleKind::Spawner)) return false;
    State* st = state(e, true);
    Spawner* m = (Spawner*)world_->get_mut(e, ModuleKind::Spawner);
    if (!st || !m) return false;
    if (!st->started) { st->started = true; st->rng = (uint32_t)m->seed * 2654435761u + 0x9E3779B9u; if (!st->rng) st->rng = 1; }
    return spawn_one(*st, *m);
}

bool SpawnerSystem::clear(Entity e) {
    if (!world_ || !world_->has(e, ModuleKind::Spawner)) return false;
    if (State* st = state(e, false)) {
        while (st->n > 0) drop_copy(*st, st->n - 1);
    }
    ((Spawner*)world_->get_mut(e, ModuleKind::Spawner))->alive = 0;
    return true;
}

void SpawnerSystem::update(float dt) {
    if (!world_ || dt <= 0) return;
    time_ += dt;
    int n = fx_scan(*world_, ModuleKind::Spawner, &scan_, &scan_cap_);
    for (int i = 0; i < count_; i++) states_[i].seen = false;
    for (int i = 0; i < n; i++) {
        Entity e = scan_[i];
        Spawner* m = (Spawner*)world_->get_mut(e, ModuleKind::Spawner);
        const WorldTransform* wt = world_->world_transform(e);
        State* st = state(e, true);
        if (!m || !wt || !st) continue;
        st->seen = true;
        Vec3 pos(wt->matrix.m[12], wt->matrix.m[13], wt->matrix.m[14]);
        if (st->have_last) st->vel = (pos - st->last_pos) * (1.0f / dt);
        st->last_pos = pos;
        st->have_last = true;
        if (!st->started) {
            st->started = true;
            st->rng = (uint32_t)m->seed * 2654435761u + 0x9E3779B9u;
            if (!st->rng) st->rng = 1;
            if (m->spawn_on_start) spawn_one(*st, *m);
        }
        // Copies that are gone or too old.
        for (int c = 0; c < st->n;) {
            if (!world_->alive(st->copies[c].e)) st->copies[c] = st->copies[--st->n];
            else if (m->lifetime > 0 && time_ - st->copies[c].born >= m->lifetime) drop_copy(*st, c);
            else c++;
        }
        if (m->auto_spawn) {
            st->timer += dt;
            int guard = 0;
            while (st->timer >= m->interval && guard++ < 4) {
                st->timer -= m->interval;
                if (m->total > 0 && m->spawned >= m->total) { st->timer = 0; break; }
                spawn_one(*st, *m);
            }
            if (guard >= 4) st->timer = 0;
        }
        m->alive = st->n;
    }
    // Spawners that are gone leave their copies alone.
    for (int i = 0; i < count_;) {
        if (!states_[i].seen) { free(states_[i].copies); states_[i] = states_[--count_]; }
        else i++;
    }
}

}  // namespace fe
