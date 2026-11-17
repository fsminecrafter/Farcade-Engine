#include "fx/animator.h"
#include "fx/fx.h"
#include "fx/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>

namespace fe {

// ================================================================= .fanim

namespace {

char* next_token(char** p) {
    char* s = *p;
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    if (!*s || *s == '#') { *p = s; return nullptr; }
    char* t = s;
    while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '#') s++;
    if (*s == '#') { *s = 0; *p = s; }
    else if (*s) { *s = 0; *p = s + 1; }
    else *p = s;
    return t;
}

bool to_float(const char* s, float* out) {
    if (!s) return false;
    char* end = nullptr;
    double v = strtod(s, &end);
    if (end == s || *end) return false;
    *out = (float)v;
    return true;
}

void fail(char* error, int cap, int line, const char* why) {
    if (error && cap > 0) snprintf(error, (size_t)cap, "line %d: %s", line, why);
}

}  // namespace

void fanim_free(FanimFile* f) {
    for (int c = 0; c < f->clip_count; c++) {
        for (int t = 0; t < f->clips[c].track_count; t++) free(f->clips[c].tracks[t].keys);
        free(f->clips[c].tracks);
    }
    free(f->clips);
    f->clips = nullptr;
    f->clip_count = 0;
}

bool fanim_parse(const char* text, size_t size, FanimFile* out, char* error, int error_cap) {
    fanim_free(out);
    char* buf = (char*)malloc(size + 1);
    if (!buf) return false;
    memcpy(buf, text, size);
    buf[size] = 0;
    bool ok = true, header = false;
    int line_no = 0;
    FanimClip* clip = nullptr;
    FanimTrack* track = nullptr;
    int clip_cap = 0, track_cap = 0, key_cap = 0;
    char* p = buf;
    while (ok && *p) {
        char* line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        line_no++;
        char* cur = line;
        char* a = next_token(&cur);
        if (!a) continue;
        if (!header) {
            if (strcmp(a, "fanim") != 0) { fail(error, error_cap, line_no, "not a .fanim file (it starts with 'fanim 1')"); ok = false; break; }
            header = true;
            continue;
        }
        if (strcmp(a, "clip") == 0) {
            char* name = next_token(&cur);
            float dur = 0;
            char* d = next_token(&cur);
            if (!name || (d && !to_float(d, &dur))) { fail(error, error_cap, line_no, "clip <name> [length]"); ok = false; break; }
            if (out->clip_count == clip_cap) {
                clip_cap = clip_cap ? clip_cap * 2 : 4;
                FanimClip* nc = (FanimClip*)realloc(out->clips, sizeof(FanimClip) * (size_t)clip_cap);
                if (!nc) { ok = false; break; }
                out->clips = nc;
            }
            clip = &out->clips[out->clip_count++];
            memset(clip, 0, sizeof *clip);
            snprintf(clip->name, sizeof clip->name, "%s", name);
            clip->duration = dur;
            track = nullptr;
            track_cap = 0;
        } else if (strcmp(a, "track") == 0) {
            char* target = next_token(&cur);
            char* ch = next_token(&cur);
            int channel = !ch ? -1 : (!strcmp(ch, "position") ? 0 : (!strcmp(ch, "rotation") ? 1 : (!strcmp(ch, "scale") ? 2 : -1)));
            if (!clip || !target || channel < 0) { fail(error, error_cap, line_no, "track <object path or .> position|rotation|scale, inside a clip"); ok = false; break; }
            if (clip->track_count == track_cap) {
                track_cap = track_cap ? track_cap * 2 : 4;
                FanimTrack* nt = (FanimTrack*)realloc(clip->tracks, sizeof(FanimTrack) * (size_t)track_cap);
                if (!nt) { ok = false; break; }
                clip->tracks = nt;
            }
            track = &clip->tracks[clip->track_count++];
            memset(track, 0, sizeof *track);
            snprintf(track->target, sizeof track->target, "%s", target);
            track->channel = (uint8_t)channel;
            key_cap = 0;
        } else if (strcmp(a, "key") == 0) {
            float v[4];
            bool good = track != nullptr;
            for (int i = 0; i < 4 && good; i++) good = to_float(next_token(&cur), &v[i]);
            if (!good) { fail(error, error_cap, line_no, "key <time> <x> <y> <z>, inside a track"); ok = false; break; }
            if (track->key_count == key_cap) {
                key_cap = key_cap ? key_cap * 2 : 8;
                FanimKey* nk = (FanimKey*)realloc(track->keys, sizeof(FanimKey) * (size_t)key_cap);
                if (!nk) { ok = false; break; }
                track->keys = nk;
            }
            if (track->key_count > 0 && v[0] < track->keys[track->key_count - 1].t) { fail(error, error_cap, line_no, "key times must not go backwards"); ok = false; break; }
            track->keys[track->key_count++] = { v[0], Vec3(v[1], v[2], v[3]) };
            if (v[0] > clip->duration && clip->duration <= 0) { /* set below */ }
        } else {
            fail(error, error_cap, line_no, "expected clip, track or key");
            ok = false;
        }
    }
    free(buf);
    if (ok && !header) { fail(error, error_cap, 1, "empty file"); ok = false; }
    if (!ok) { fanim_free(out); return false; }
    // A clip without a length lasts until its last key.
    for (int c = 0; c < out->clip_count; c++) {
        FanimClip& cl = out->clips[c];
        if (cl.duration > 0) continue;
        for (int t = 0; t < cl.track_count; t++)
            if (cl.tracks[t].key_count && cl.tracks[t].keys[cl.tracks[t].key_count - 1].t > cl.duration) cl.duration = cl.tracks[t].keys[cl.tracks[t].key_count - 1].t;
    }
    return true;
}

Vec3 fanim_sample(const FanimTrack& tr, float t) {
    if (tr.key_count == 0) return Vec3(0, 0, 0);
    if (t <= tr.keys[0].t || tr.key_count == 1) return tr.keys[0].v;
    if (t >= tr.keys[tr.key_count - 1].t) return tr.keys[tr.key_count - 1].v;
    int i = 0;
    while (i + 1 < tr.key_count && tr.keys[i + 1].t <= t) i++;
    const FanimKey &a = tr.keys[i], &b = tr.keys[i + 1];
    float span = b.t - a.t;
    float u = span > 1e-6f ? (t - a.t) / span : 1.0f;
    return a.v + (b.v - a.v) * u;
}

Quat fanim_sample_rot(const FanimTrack& tr, float t) {
    if (tr.key_count == 0) return Quat::euler_deg(Vec3(0, 0, 0));
    if (t <= tr.keys[0].t || tr.key_count == 1) return Quat::euler_deg(tr.keys[0].v);
    if (t >= tr.keys[tr.key_count - 1].t) return Quat::euler_deg(tr.keys[tr.key_count - 1].v);
    int i = 0;
    while (i + 1 < tr.key_count && tr.keys[i + 1].t <= t) i++;
    const FanimKey &a = tr.keys[i], &b = tr.keys[i + 1];
    float span = b.t - a.t;
    float u = span > 1e-6f ? (t - a.t) / span : 1.0f;
    Quat qa = Quat::euler_deg(a.v), qb = Quat::euler_deg(b.v), q;
    if (qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w < 0) { qb.x = -qb.x; qb.y = -qb.y; qb.z = -qb.z; qb.w = -qb.w; }
    q.x = qa.x + (qb.x - qa.x) * u; q.y = qa.y + (qb.y - qa.y) * u; q.z = qa.z + (qb.z - qa.z) * u; q.w = qa.w + (qb.w - qa.w) * u;
    return normalize(q);
}

// ================================================================= system

struct AnimatorSystem::State {
    Entity e = 0;
    bool seen = false, started = false;
    const FanimFile* file = nullptr;
    int clip = -1;
    char clip_name[48] = {};
    float time = 0, last_written = 0;
    bool playing = false;
    // The clip being faded out.
    int from_clip = -1;
    float from_time = 0, fade = 0, fade_total = 0;
    Entity* targets = nullptr;          // per track of the clip: the entity it moves
    int target_cap = 0;
    Entity* from_targets = nullptr;
    int from_cap = 0;
};

bool AnimatorSystem::init(World* world, const FxHost* host) {
    shutdown();
    if (!world || !world->valid()) return false;
    world_ = world;
    host_ = host;
    system_ = world->add_system(Phase::Update, system, this);
    return system_ != 0;
}

void AnimatorSystem::shutdown() {
    if (world_ && system_ && world_->valid()) world_->remove_system(system_);
    system_ = 0;
    world_ = nullptr;
    for (int i = 0; i < count_; i++) { free(states_[i].targets); free(states_[i].from_targets); }
    free(states_);
    states_ = nullptr;
    count_ = cap_ = 0;
    for (int i = 0; i < file_count_; i++) fanim_free(&files_[i]);
    free(files_);
    files_ = nullptr;
    file_count_ = 0;
    free(scan_);
    scan_ = nullptr;
    scan_cap_ = 0;
}

AnimatorSystem::State* AnimatorSystem::state(Entity e, bool create) {
    for (int i = 0; i < count_; i++) if (states_[i].e == e) return &states_[i];
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
    State* s = new (&states_[count_++]) State();
    s->e = e;
    return s;
}

const FanimFile* AnimatorSystem::load(const char* path) {
    if (!path || !path[0]) return nullptr;
    for (int i = 0; i < file_count_; i++)
        if (strcmp(files_[i].path, path) == 0) return files_[i].clip_count ? &files_[i] : nullptr;
    if (!host_ || !host_->read_file) return nullptr;
    FanimFile* nf = (FanimFile*)realloc(files_, sizeof(FanimFile) * (size_t)(file_count_ + 1));
    if (!nf) return nullptr;
    files_ = nf;
    FanimFile* f = new (&files_[file_count_++]) FanimFile();
    snprintf(f->path, sizeof f->path, "%s", path);
    size_t size = 0;
    char* text = (char*)host_->read_file(path, &size, host_->user);
    if (!text) { if (host_->log) host_->log(host_->user, "Animator: cannot read the animation file", path); return nullptr; }
    char err[96];
    bool ok = fanim_parse(text, size, f, err, sizeof err);
    free(text);
    if (!ok) { if (host_->log) host_->log(host_->user, err, path); return nullptr; }
    return f;
}

static void source_path(World* w, Entity e, const Animator& m, char* out, int cap) {
    out[0] = 0;
    if (m.source[0]) { snprintf(out, (size_t)cap, "%s", m.source); return; }
    const Renderable* r = w->renderable(e);
    if (!r || !r->model_path[0]) return;
    snprintf(out, (size_t)cap, "%s", r->model_path);
    char* dot = strrchr(out, '.');
    char* slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash)) *dot = 0;
    size_t n = strlen(out);
    if (n + 7 < (size_t)cap) memcpy(out + n, ".fanim", 7);
}

static Entity resolve(World* w, Entity root, const char* path) {
    if (!path[0] || !strcmp(path, ".")) return root;
    char part[96];
    Entity cur = root;
    const char* p = path;
    while (*p && cur) {
        int n = 0;
        while (p[n] && p[n] != '/' && n < 95) { part[n] = p[n]; n++; }
        part[n] = 0;
        p += n;
        if (*p == '/') p++;
        if (n) cur = w->lookup_child(cur, part);
    }
    return cur;
}

bool AnimatorSystem::start_clip(State& st, Entity e, Animator& m, const char* clip, float fade) {
    char path[256];
    source_path(world_, e, m, path, sizeof path);
    const FanimFile* f = load(path);
    if (!f) return false;
    const char* want = clip && clip[0] ? clip : m.clip;
    int idx = -1;
    for (int i = 0; i < f->clip_count; i++) if (want[0] ? !strcmp(f->clips[i].name, want) : i == 0) { idx = i; break; }
    if (idx < 0) return false;
    float fd = fade < 0 ? m.crossfade : fade;
    if (st.playing && st.file == f && st.clip >= 0 && fd > 0) {
        // Keep the old clip going while it fades out.
        st.from_clip = st.clip;
        st.from_time = st.time;
        st.fade = st.fade_total = fd;
        const FanimClip& oc = f->clips[st.from_clip];
        if (st.from_cap < oc.track_count) { st.from_targets = (Entity*)realloc(st.from_targets, sizeof(Entity) * (size_t)oc.track_count); st.from_cap = oc.track_count; }
        if (st.from_targets && st.targets) memcpy(st.from_targets, st.targets, sizeof(Entity) * (size_t)oc.track_count);
    } else {
        st.from_clip = -1;
        st.fade = 0;
    }
    st.file = f;
    st.clip = idx;
    snprintf(st.clip_name, sizeof st.clip_name, "%s", f->clips[idx].name);
    const FanimClip& c = f->clips[idx];
    if (st.target_cap < c.track_count) { st.targets = (Entity*)realloc(st.targets, sizeof(Entity) * (size_t)c.track_count); st.target_cap = c.track_count; }
    for (int t = 0; t < c.track_count && st.targets; t++) st.targets[t] = resolve(world_, e, c.tracks[t].target);
    st.time = m.speed < 0 ? c.duration : 0;
    st.last_written = st.time;
    st.playing = true;
    snprintf(m.clip, sizeof m.clip, "%s", c.name);
    m.playing = true;
    m.time = st.time;
    m.duration = c.duration;
    return true;
}

bool AnimatorSystem::play(Entity e, const char* clip, float fade) {
    if (!world_ || !world_->has(e, ModuleKind::Animator)) return false;
    State* st = state(e, true);
    Animator* m = (Animator*)world_->get_mut(e, ModuleKind::Animator);
    if (!st || !m) return false;
    st->started = true;
    return start_clip(*st, e, *m, clip, fade);
}

bool AnimatorSystem::stop(Entity e) {
    if (!world_ || !world_->has(e, ModuleKind::Animator)) return false;
    if (State* st = state(e, false)) st->playing = false;
    ((Animator*)world_->get_mut(e, ModuleKind::Animator))->playing = false;
    return true;
}

static void apply_track(World* w, Entity target, const FanimTrack& tr, float t, const FanimTrack* from, float from_t, float mix) {
    Transform* tf = w->transform(target);
    if (!tf) return;
    Vec3 v = fanim_sample(tr, t);
    if (tr.channel == 1) {
        Quat q = fanim_sample_rot(tr, t);
        if (from) {
            Quat p = fanim_sample_rot(*from, from_t);
            float d = p.x * q.x + p.y * q.y + p.z * q.z + p.w * q.w;
            if (d < 0) { q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w; }
            q.x = p.x + (q.x - p.x) * mix; q.y = p.y + (q.y - p.y) * mix; q.z = p.z + (q.z - p.z) * mix; q.w = p.w + (q.w - p.w) * mix;
            q = normalize(q);
        }
        tf->rotation = q;
        return;
    }
    if (from) { Vec3 p = fanim_sample(*from, from_t); v = p + (v - p) * mix; }
    if (tr.channel == 0) tf->position = v; else tf->scale = v;
}

void AnimatorSystem::system(World& w, float dt, void* user) {
    AnimatorSystem* S = (AnimatorSystem*)user;
    int n = fx_scan(w, ModuleKind::Animator, &S->scan_, &S->scan_cap_);
    for (int i = 0; i < S->count_; i++) S->states_[i].seen = false;
    for (int i = 0; i < n; i++) {
        Entity e = S->scan_[i];
        Animator* m = (Animator*)w.get_mut(e, ModuleKind::Animator);
        State* st = S->state(e, true);
        if (!m || !st) continue;
        st->seen = true;
        if (!st->started) {
            st->started = true;
            if (m->play_on_start) S->start_clip(*st, e, *m, nullptr, 0);
        } else {
            // The fields are controls too: playing, clip and time may be set by scripts.
            if (m->playing && !st->playing && st->clip >= 0) st->playing = true;
            else if (m->playing && !st->playing) S->start_clip(*st, e, *m, nullptr, 0);
            else if (!m->playing && st->playing) st->playing = false;
            if (st->playing && strcmp(m->clip, st->clip_name) != 0) S->start_clip(*st, e, *m, m->clip, -1);
            if (st->clip >= 0 && fabsf_(m->time - st->last_written) > 1e-4f) st->time = m->time;
        }
        if (st->clip < 0 || !st->file) continue;
        const FanimClip& c = st->file->clips[st->clip];
        if (st->playing && dt > 0) {
            st->time += dt * m->speed;
            if (st->fade > 0) { st->fade -= dt; st->from_time += dt * m->speed; }
            if (c.duration > 0 && (st->time >= c.duration || st->time < 0)) {
                if (m->loop) {
                    while (st->time >= c.duration) st->time -= c.duration;
                    while (st->time < 0) st->time += c.duration;
                } else {
                    st->time = st->time < 0 ? 0 : c.duration;
                    st->playing = false;
                }
            }
        }
        float mix = 1.0f;
        const FanimClip* fc = nullptr;
        if (st->fade > 0 && st->from_clip >= 0) {
            fc = &st->file->clips[st->from_clip];
            mix = 1.0f - st->fade / st->fade_total;
            if (mix < 0) mix = 0;
            // The faded-out clip keeps its own loop.
            if (fc->duration > 0) { while (st->from_time >= fc->duration) st->from_time -= fc->duration; while (st->from_time < 0) st->from_time += fc->duration; }
        }
        for (int t = 0; t < c.track_count; t++) {
            Entity target = st->targets ? st->targets[t] : 0;
            if (!target || !w.alive(target)) continue;
            const FanimTrack* from = nullptr;
            if (fc && st->from_targets)
                for (int k = 0; k < fc->track_count; k++)
                    if (fc->tracks[k].channel == c.tracks[t].channel && st->from_targets[k] == target) { from = &fc->tracks[k]; break; }
            apply_track(&w, target, c.tracks[t], st->time, from, st->from_time, mix);
        }
        if (S->host_ && S->host_->animator_pose)       // the model animation hook (fx.h)
            S->host_->animator_pose(S->host_->user, e, c.name, st->time, fc ? fc->name : nullptr, st->from_time, fc ? mix : 0.0f);
        m->playing = st->playing;
        m->time = st->time;
        m->duration = c.duration;
        st->last_written = st->time;
    }
    for (int i = 0; i < S->count_;) {
        if (!S->states_[i].seen) { free(S->states_[i].targets); free(S->states_[i].from_targets); S->states_[i] = S->states_[--S->count_]; }
        else i++;
    }
}

}  // namespace fe
