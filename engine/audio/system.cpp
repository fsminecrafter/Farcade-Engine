// AudioSystem: the ECS side - modules, spatialization, pushing to the device.
#include "audio/audio.h"
#include "audio/internal.h"
#include "core/math.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fe {

namespace {

constexpr float kTargetMinMs = 80.0f;   // audio kept queued in the device ...
constexpr float kTargetMaxMs = 250.0f;  // ... more when frames are slow (2 x the frame time)
constexpr int kPushChunk = 1024;

struct Entry {
    Entity e = 0;
    bool started = false;               // its play_on_start was handled
    bool paused = false;
    uint32_t seen = 0;
    VoiceId stream = -1;
    int nsv = 0;
    VoiceId sv[Mixer::kMaxVoices];      // sampler voices, oldest first
    float sv_mul[Mixer::kMaxVoices];    // each one's random pitch factor
};

struct CacheEntry {
    char path[kPathMax];
    Clip* clip;                         // nullptr: failed (reported once)
};

double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
}

}  // namespace

struct AudioSystem::Impl {
    World* world = nullptr;
    AudioDevice* dev = nullptr;
    AudioHost host;
    Entity system = 0;
    uint32_t frame = 0;
    uint32_t rng = 0x9e3779b9u;
    Entry** entries = nullptr;
    int nentries = 0, centries = 0;
    CacheEntry* cache = nullptr;
    int ncache = 0, ccache = 0;
    Entity* scan = nullptr;
    int scan_cap = 0;
    int16_t push_buf[kPushChunk * 2];
    AudioStats stats;
    // listener
    bool have_listener = false;
    Vec3 lpos, lright;

    float rnd() {   // 0..1
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return (float)(rng >> 8) * (1.0f / 16777216.0f);
    }

    void log(const char* fmt, const char* a) {
        if (!host.log) return;
        char line[kPathMax + 96];
        snprintf(line, sizeof line, fmt, a);
        host.log(line, host.user);
    }

    Entry* find(Entity e) const {
        for (int i = 0; i < nentries; i++) if (entries[i]->e == e) return entries[i];
        return nullptr;
    }
    Entry* get(Entity e) {
        if (Entry* x = find(e)) return x;
        if (nentries == centries) {
            int nc = centries ? centries * 2 : 32;
            Entry** ne = (Entry**)realloc(entries, sizeof(Entry*) * (size_t)nc);
            if (!ne) return nullptr;
            entries = ne;
            centries = nc;
        }
        Entry* x = new Entry();
        x->e = e;
        entries[nentries++] = x;
        return x;
    }
    void drop(int i, Mixer& mx) {
        Entry* x = entries[i];
        mx.stop(x->stream);
        for (int k = 0; k < x->nsv; k++) mx.stop(x->sv[k]);
        delete x;
        entries[i] = entries[--nentries];
    }

    // The clip of a sampler path: decoded once, cached; one log line when it fails.
    Clip* clip_for(const char* path) {
        for (int i = 0; i < ncache; i++) if (strcmp(cache[i].path, path) == 0) return cache[i].clip;
        Clip* c = nullptr;
        size_t size = 0;
        void* data = host.read_file ? host.read_file(path, &size, host.user) : nullptr;
        if (!data) {
            log("audio: cannot read '%s'", path);
        } else {
            c = Clip::decode(data, size);
            if (!c) log("audio: cannot decode '%s'", path);
            free(data);
        }
        if (ncache == ccache) {
            int nc = ccache ? ccache * 2 : 16;
            CacheEntry* nb = (CacheEntry*)realloc(cache, sizeof(CacheEntry) * (size_t)nc);
            if (!nb) return c;      // not cached (leaks nothing: the clip is returned)
            cache = nb;
            ccache = nc;
        }
        snprintf(cache[ncache].path, kPathMax, "%s", path);
        cache[ncache].clip = c;
        ncache++;
        return c;
    }

    void read_listener() {
        have_listener = false;
        Entity le = world->current(ModuleKind::AudioListener);
        if (!le) return;
        const WorldTransform* wt = world->world_transform(le);
        if (!wt) return;
        have_listener = true;
        lpos = Vec3(wt->matrix.m[12], wt->matrix.m[13], wt->matrix.m[14]);
        lright = normalize(Vec3(wt->matrix.m[0], wt->matrix.m[1], wt->matrix.m[2]));
    }

    // Distance gain and pan of the entity as heard by the listener.
    void spatialize(Entity e, bool spatial, float mind, float maxd, float* gain, float* pan) {
        *gain = 1.0f;
        *pan = 0.0f;
        if (!spatial || !have_listener) return;
        const WorldTransform* wt = world->world_transform(e);
        if (!wt) return;
        Vec3 rel(wt->matrix.m[12] - lpos.x, wt->matrix.m[13] - lpos.y, wt->matrix.m[14] - lpos.z);
        float d = length(rel);
        if (mind < 0.01f) mind = 0.01f;
        if (maxd <= mind) maxd = mind + 0.01f;
        if (d >= maxd) { *gain = 0.0f; }
        else if (d > mind) {
            // Inverse distance, shifted so that it reaches 0 at max_distance.
            float lo = mind / maxd;
            *gain = (mind / d - lo) / (1.0f - lo);
        }
        if (d > 1e-4f) {
            float side = dot(rel, lright) / d;
            float near = d / mind;      // right on top of the listener: no pan
            if (near > 1.0f) near = 1.0f;
            *pan = fclamp(side * near, -1.0f, 1.0f);
        }
    }

    VoiceParams stream_params(Entity e, const AudioStream& s) {
        float g, p;
        spatialize(e, s.spatial, s.min_distance, s.max_distance, &g, &p);
        VoiceParams vp;
        vp.volume = s.volume * g;
        vp.pan = p;
        vp.pitch = s.pitch;
        vp.loop = s.loop;
        return vp;
    }

    bool start_stream(Entry* x, Mixer& mx) {
        const AudioStream* s = (const AudioStream*)world->get(x->e, ModuleKind::AudioStream);
        if (!s || !s->path[0]) return false;
        mx.stop(x->stream);
        x->stream = -1;
        x->paused = false;
        size_t size = 0;
        void* data = host.read_file ? host.read_file(s->path, &size, host.user) : nullptr;
        if (!data) { log("audio: cannot read '%s'", s->path); return false; }
        VoiceId v = mx.play_stream(data, size, stream_params(x->e, *s));
        if (v < 0) { log("audio: cannot decode '%s'", s->path); return false; }
        x->stream = v;
        return true;
    }

    void sync_stream(Entry* x, Mixer& mx) {
        if (x->stream < 0) return;
        if (!mx.is_active(x->stream)) { x->stream = -1; x->paused = false; return; }
        const AudioStream* s = (const AudioStream*)world->get(x->e, ModuleKind::AudioStream);
        if (!s) return;
        mx.set(x->stream, stream_params(x->e, *s));
    }

    void sync_samplers(Entry* x, Mixer& mx) {
        const AudioSampler* s = (const AudioSampler*)world->get(x->e, ModuleKind::AudioSampler);
        int n = 0;
        for (int i = 0; i < x->nsv; i++) {
            if (!mx.is_active(x->sv[i])) continue;
            x->sv[n] = x->sv[i];
            x->sv_mul[n] = x->sv_mul[i];
            if (s) {
                float g, p;
                spatialize(x->e, s->spatial, s->min_distance, s->max_distance, &g, &p);
                VoiceParams vp;
                vp.volume = s->volume * g;
                vp.pan = p;
                vp.pitch = s->pitch * x->sv_mul[n];
                vp.loop = false;
                mx.set(x->sv[n], vp);
            }
            n++;
        }
        x->nsv = n;
    }
};

AudioSystem::AudioSystem() {}
AudioSystem::~AudioSystem() { shutdown(); }

static void frame_fn(World&, float dt, void* user) { ((AudioSystem*)user)->update(dt); }

bool AudioSystem::init(World* world, AudioDevice* device, const AudioHost& host) {
    shutdown();
    if (!world || !world->valid() || !device || !device->ready()) return false;
    if (!mixer_.init(device->rate())) return false;
    impl_ = new Impl();
    impl_->world = world;
    impl_->dev = device;
    impl_->host = host;
    impl_->system = world->add_system(Phase::Store, frame_fn, this);
    return true;
}

void AudioSystem::shutdown() {
    if (!impl_) return;
    Impl* I = impl_;
    if (I->world && I->world->valid() && I->system) I->world->remove_system(I->system);
    mixer_.shutdown();      // frees the voices before the clips
    for (int i = 0; i < I->nentries; i++) delete I->entries[i];
    free(I->entries);
    for (int i = 0; i < I->ncache; i++) if (I->cache[i].clip) I->cache[i].clip->destroy();
    free(I->cache);
    free(I->scan);
    delete I;
    impl_ = nullptr;
}

void AudioSystem::update(float dt) {
    Impl* I = impl_;
    if (!I) return;
    double t0 = now_ms();
    World& w = *I->world;
    I->frame++;
    I->read_listener();
    float master = 1.0f;
    if (I->have_listener) {
        const AudioListener* al = (const AudioListener*)w.get(w.current(ModuleKind::AudioListener), ModuleKind::AudioListener);
        if (al) master = al->volume < 0 ? 0 : al->volume;
    }
    mixer_.master = master;

    // Audio Reverb Zones: the one the listener is most inside of sets the reverb and muffling.
    {
        Mixer::ReverbParams rp;
        float best = 0;
        if (I->have_listener && reverb_enabled) {
            int nz = audio_scan(w, ModuleKind::ReverbZone, &I->scan, &I->scan_cap);
            for (int i = 0; i < nz; i++) {
                const ReverbZone* z = (const ReverbZone*)w.get(I->scan[i], ModuleKind::ReverbZone);
                const WorldTransform* wt = w.world_transform(I->scan[i]);
                Mat4 inv;
                if (!z || !z->enabled || !wt || !invert(wt->matrix, &inv)) continue;
                Vec3 p = transform_point(inv, I->lpos);
                float outside;
                if (z->shape == FieldShape::Sphere) outside = length(p) - z->radius;
                else {
                    Vec3 q(fabsf_(p.x) - z->size.x * 0.5f, fabsf_(p.y) - z->size.y * 0.5f, fabsf_(p.z) - z->size.z * 0.5f);
                    outside = length(Vec3(fmaxf_(q.x, 0), fmaxf_(q.y, 0), fmaxf_(q.z, 0)));
                }
                float wgt = outside <= 0 ? 1.0f : (z->fade_distance > 0.001f ? 1.0f - outside / z->fade_distance : 0.0f);
                if (wgt <= best) continue;
                best = wgt;
                rp.wet = z->wet * wgt;
                rp.room = z->room_size;
                rp.damping = z->damping;
                rp.lowpass = z->lowpass * wgt;
            }
        }
        mixer_.set_reverb(rp);
    }

    int n = audio_scan(w, ModuleKind::AudioStream, &I->scan, &I->scan_cap);
    for (int i = 0; i < n; i++) {
        Entry* x = I->get(I->scan[i]);
        if (!x) continue;
        x->seen = I->frame;
        const AudioStream* s = (const AudioStream*)w.get(x->e, ModuleKind::AudioStream);
        if (!x->started) {
            x->started = true;
            if (s && s->play_on_start) I->start_stream(x, mixer_);
        }
        I->sync_stream(x, mixer_);
    }
    n = audio_scan(w, ModuleKind::AudioSampler, &I->scan, &I->scan_cap);
    for (int i = 0; i < n; i++) {
        Entry* x = I->get(I->scan[i]);
        if (!x) continue;
        x->seen = I->frame;
        I->sync_samplers(x, mixer_);
    }
    for (int i = I->nentries - 1; i >= 0; i--) {
        Entry* x = I->entries[i];
        if (x->seen != I->frame) { I->drop(i, mixer_); continue; }
        if (x->stream >= 0 && !w.has(x->e, ModuleKind::AudioStream)) { mixer_.stop(x->stream); x->stream = -1; x->paused = false; }
    }

    // Keep ~80 ms queued in the device (2 x dt when frames are slower than 25 fps).
    AudioDevice& dev = *I->dev;
    float target_ms = dt * 2000.0f;
    if (target_ms < kTargetMinMs) target_ms = kTargetMinMs;
    if (target_ms > kTargetMaxMs) target_ms = kTargetMaxMs;
    int target = (int)((float)dev.rate() * target_ms * 0.001f);
    if (target > dev.capacity() - kPushChunk) target = dev.capacity() - kPushChunk;
    int need = target - dev.queued();
    int room = dev.writable();
    if (need > room) need = room;
    while (need > 0) {
        int c = need < kPushChunk ? need : kPushChunk;
        mixer_.mix(I->push_buf, c);
        int taken = dev.write(I->push_buf, c);
        I->stats.frames_pushed += (uint64_t)taken;
        if (taken < c) break;       // the device is full after all: drop the rest
        need -= c;
    }

    double ms = now_ms() - t0;
    I->stats.mix_ms = (float)ms;
    if (ms > I->stats.mix_ms_max) I->stats.mix_ms_max = (float)ms;
}

void AudioSystem::restart() {
    Impl* I = impl_;
    if (!I) return;
    mixer_.stop_all();
    for (int i = 0; i < I->nentries; i++) delete I->entries[i];
    I->nentries = 0;
}

bool AudioSystem::play_sampler(Entity e) {
    Impl* I = impl_;
    if (!I) return false;
    const AudioSampler* s = (const AudioSampler*)I->world->get(e, ModuleKind::AudioSampler);
    if (!s || !s->path[0]) return false;
    Entry* x = I->get(e);
    if (!x) return false;
    Clip* clip = I->clip_for(s->path);
    if (!clip) return false;
    // Drop ended voices, then make room: the oldest is cut.
    I->sync_samplers(x, mixer_);
    int cap = s->voices < 1 ? 1 : (s->voices > Mixer::kMaxVoices ? Mixer::kMaxVoices : s->voices);
    while (x->nsv >= cap) {
        mixer_.stop(x->sv[0]);
        for (int i = 1; i < x->nsv; i++) { x->sv[i - 1] = x->sv[i]; x->sv_mul[i - 1] = x->sv_mul[i]; }
        x->nsv--;
    }
    float mul = 1.0f + s->pitch_variation * (I->rnd() * 2.0f - 1.0f);
    if (mul < 0.05f) mul = 0.05f;
    float g, p;
    I->read_listener();
    I->spatialize(e, s->spatial, s->min_distance, s->max_distance, &g, &p);
    VoiceParams vp;
    vp.volume = s->volume * g;
    vp.pan = p;
    vp.pitch = s->pitch * mul;
    VoiceId v = mixer_.play_clip(clip, vp);
    if (v < 0) return false;
    // A voice stolen by the mixer (all 32 busy) may belong to any entity:
    // its owner notices at its next sync.
    x->sv[x->nsv] = v;
    x->sv_mul[x->nsv] = mul;
    x->nsv++;
    return true;
}

bool AudioSystem::play_stream(Entity e) {
    Impl* I = impl_;
    if (!I) return false;
    Entry* x = I->get(e);
    if (!x) return false;
    x->started = true;
    I->read_listener();
    return I->start_stream(x, mixer_);
}

void AudioSystem::stop_stream(Entity e) {
    Impl* I = impl_;
    Entry* x = I ? I->find(e) : nullptr;
    if (!x) return;
    mixer_.stop(x->stream);
    x->stream = -1;
    x->paused = false;
}

void AudioSystem::pause_stream(Entity e, bool paused) {
    Impl* I = impl_;
    Entry* x = I ? I->find(e) : nullptr;
    if (!x || !mixer_.is_active(x->stream)) return;
    mixer_.pause(x->stream, paused);
    x->paused = paused;
}

bool AudioSystem::is_playing(Entity e) const {
    const Impl* I = impl_;
    const Entry* x = I ? I->find(e) : nullptr;
    if (!x) return false;
    if (mixer_.is_active(x->stream) && !mixer_.is_paused(x->stream)) return true;
    for (int i = 0; i < x->nsv; i++) if (mixer_.is_active(x->sv[i])) return true;
    return false;
}

void AudioSystem::stop_all() {
    mixer_.stop_all();
}

AudioStats AudioSystem::stats() const {
    AudioStats s;
    if (!impl_) return s;
    s = impl_->stats;
    s.voices_active = mixer_.active_count();
    s.underruns = impl_->dev->underruns();
    int clips = 0;
    for (int i = 0; i < impl_->ncache; i++) clips += impl_->cache[i].clip ? 1 : 0;
    s.clips_cached = clips;
    return s;
}

void AudioSystem::clear_cache() {
    Impl* I = impl_;
    if (!I) return;
    mixer_.stop_all();
    for (int i = 0; i < I->ncache; i++) if (I->cache[i].clip) I->cache[i].clip->destroy();
    I->ncache = 0;
}

}  // namespace fe
