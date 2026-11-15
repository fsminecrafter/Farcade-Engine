// Tests for the audio system (engine/audio): decoders, mixer, device, and the
// AudioSystem on an ECS world, all through the Null device (deterministic).
#include "audio/audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

using namespace fe;

static int g_checks, g_failed;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const unsigned char kMp3[] = {
#include "../data/sine_mp3.inc"
};
static const unsigned char kOgg[] = {
#include "../data/sine_ogg.inc"
};

static double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
}

// ---------------------------------------------------------------- helpers

static void put32(unsigned char* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i)); }
static void put16(unsigned char* p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

// A 16-bit PCM WAV file of a sine (malloc'd). Stereo: the same sine in both.
static unsigned char* make_wav(int rate, int channels, float freq, float seconds, float amp, size_t* size) {
    int frames = (int)(seconds * (float)rate);
    size_t data = (size_t)frames * (size_t)channels * 2;
    unsigned char* f = (unsigned char*)malloc(44 + data);
    memcpy(f, "RIFF", 4); put32(f + 4, (uint32_t)(36 + data)); memcpy(f + 8, "WAVEfmt ", 8);
    put32(f + 16, 16); put16(f + 20, 1); put16(f + 22, (uint32_t)channels);
    put32(f + 24, (uint32_t)rate); put32(f + 28, (uint32_t)(rate * channels * 2));
    put16(f + 32, (uint32_t)(channels * 2)); put16(f + 34, 16);
    memcpy(f + 36, "data", 4); put32(f + 40, (uint32_t)data);
    for (int i = 0; i < frames; i++) {
        double s = amp * 32767.0 * __builtin_sin(6.283185307179586 * (double)freq * (double)i / (double)rate);
        for (int c = 0; c < channels; c++) put16(f + 44 + ((size_t)i * (size_t)channels + (size_t)c) * 2, (uint32_t)(uint16_t)(int16_t)s);
    }
    *size = 44 + data;
    return f;
}

// Rising zero crossings of channel `ch` in frames [a, b) of interleaved stereo.
static int crossings(const int16_t* s, int a, int b, int ch) {
    int n = 0;
    for (int i = a + 1; i < b; i++) if (s[(i - 1) * 2 + ch] < 0 && s[i * 2 + ch] >= 0) n++;
    return n;
}
static int crossings_mono(const int16_t* s, int frames) {
    int n = 0;
    for (int i = 1; i < frames; i++) if (s[i - 1] < 0 && s[i] >= 0) n++;
    return n;
}
static double rms(const int16_t* s, int a, int b, int ch) {
    double acc = 0;
    for (int i = a; i < b; i++) { double v = s[i * 2 + ch]; acc += v * v; }
    return b > a ? __builtin_sqrt(acc / (double)(b - a)) : 0.0;
}
static int peak(const int16_t* s, int a, int b, int ch) {
    int p = 0;
    for (int i = a; i < b; i++) { int v = s[i * 2 + ch]; if (v < 0) v = -v; if (v > p) p = v; }
    return p;
}

// The host: files in memory, log lines counted.
struct Files {
    struct F { char path[64]; unsigned char* data; size_t size; } f[8];
    int n = 0;
    int reads = 0;
    char last_log[256] = {};
    int logs = 0;
    void add(const char* path, const void* data, size_t size) {
        snprintf(f[n].path, sizeof f[n].path, "%s", path);
        f[n].data = (unsigned char*)malloc(size);
        memcpy(f[n].data, data, size);
        f[n].size = size;
        n++;
    }
    void add_owned(const char* path, unsigned char* data, size_t size) {
        add(path, data, size);
        free(data);
    }
    void add_wav(const char* path, int rate, int ch, float freq, float sec, float amp) {
        size_t n = 0;
        unsigned char* d = make_wav(rate, ch, freq, sec, amp, &n);
        add_owned(path, d, n);
    }
    ~Files() { for (int i = 0; i < n; i++) free(f[i].data); }
};

static void* host_read(const char* path, size_t* size, void* user) {
    Files* fs = (Files*)user;
    fs->reads++;
    for (int i = 0; i < fs->n; i++) {
        if (strcmp(fs->f[i].path, path) == 0) {
            void* p = malloc(fs->f[i].size);
            memcpy(p, fs->f[i].data, fs->f[i].size);
            *size = fs->f[i].size;
            return p;
        }
    }
    return nullptr;
}
static void host_log(const char* line, void* user) {
    Files* fs = (Files*)user;
    fs->logs++;
    snprintf(fs->last_log, sizeof fs->last_log, "%s", line);
}

static const int kRate = 48000;
static int16_t g_cap[kRate * 2 * 8];

// A world, a Null device, the audio system, and a capture of everything pushed.
struct Rig {
    World w;
    AudioDevice dev;
    AudioSystem sys;
    Files files;
    int cap_frames = 0;

    bool init() {
        AudioHost h;
        h.read_file = host_read;
        h.log = host_log;
        h.user = &files;
        return w.init() && dev.init(AudioDevice::Backend::Null, kRate) && sys.init(&w, &dev, h);
    }
    // Advances `seconds` of game time at 60 fps; the device plays as it goes.
    void run(float seconds) {
        int steps = (int)(seconds * 60.0f + 0.5f);
        for (int i = 0; i < steps; i++) {
            dev.null_advance(kRate / 60);
            w.progress(1.0f / 60.0f);
            int room = (int)(sizeof g_cap / sizeof g_cap[0] / 2) - cap_frames;
            cap_frames += dev.capture(g_cap + cap_frames * 2, room);
        }
    }
    void clear() { cap_frames = 0; }

    Entity listener(Vec3 pos, float volume = 1.0f) {
        Entity e = w.create("Listener");
        w.add(e, ModuleKind::AudioListener);
        ((AudioListener*)w.get_mut(e, ModuleKind::AudioListener))->volume = volume;
        w.set_current(e, ModuleKind::AudioListener);
        w.transform(e)->position = pos;
        return e;
    }
    Entity stream(const char* name, const char* path, Vec3 pos, bool loop, bool on_start, bool spatial) {
        Entity e = w.create(name);
        w.add(e, ModuleKind::AudioStream);
        AudioStream* s = (AudioStream*)w.get_mut(e, ModuleKind::AudioStream);
        snprintf(s->path, sizeof s->path, "%s", path);
        s->loop = loop;
        s->play_on_start = on_start;
        s->spatial = spatial;
        s->volume = 0.5f;
        w.transform(e)->position = pos;
        return e;
    }
    Entity sampler(const char* name, const char* path, Vec3 pos, int voices, bool spatial) {
        Entity e = w.create(name);
        w.add(e, ModuleKind::AudioSampler);
        AudioSampler* s = (AudioSampler*)w.get_mut(e, ModuleKind::AudioSampler);
        snprintf(s->path, sizeof s->path, "%s", path);
        s->voices = voices;
        s->spatial = spatial;
        s->volume = 0.5f;
        w.transform(e)->position = pos;
        return e;
    }
};

static Clip* sine_clip(int rate, int channels, float freq, float seconds, float amp) {
    size_t n;
    unsigned char* wav = make_wav(rate, channels, freq, seconds, amp, &n);
    Clip* c = Clip::decode(wav, n);
    free(wav);
    return c;
}

// ---------------------------------------------------------------- tests

static void test_decode() {
    size_t n;
    unsigned char* wav = make_wav(22050, 1, 440.0f, 0.5f, 0.8f, &n);
    Clip* c = Clip::decode(wav, n);
    CHECK(c != nullptr, "wav decodes");
    if (c) {
        CHECK(c->frames == 11025 && c->rate == 22050 && c->channels == 1, "wav: %u frames, %d Hz, %d ch", c->frames, c->rate, c->channels);
        int zc = crossings_mono(c->data, (int)c->frames);
        CHECK(zc >= 219 && zc <= 221, "440 Hz over 0.5 s = 220 crossings (%d)", zc);
        int mx = 0;
        for (uint32_t i = 0; i < c->frames; i++) { int v = c->data[i] < 0 ? -c->data[i] : c->data[i]; if (v > mx) mx = v; }
        CHECK(mx > 26000 && mx < 26400, "amplitude 0.8 (peak %d)", mx);
        c->destroy();
    }
    free(wav);

    wav = make_wav(44100, 2, 1000.0f, 0.25f, 0.5f, &n);
    c = Clip::decode(wav, n);
    CHECK(c && c->frames == 11025 && c->rate == 44100 && c->channels == 2, "stereo wav");
    if (c) c->destroy();
    free(wav);

    // Garbage and damaged files are rejected, never crash.
    unsigned char junk[512];
    unsigned s = 12345;
    for (int i = 0; i < 512; i++) { s = s * 1103515245u + 12345u; junk[i] = (unsigned char)(s >> 16); }
    CHECK(Clip::decode(junk, sizeof junk) == nullptr, "random bytes are not audio");
    CHECK(Clip::decode(junk, 3) == nullptr, "3 bytes");
    CHECK(Clip::decode(nullptr, 0) == nullptr, "null");
    memcpy(junk, "RIFF\x24\0\0\0WAVEfmt ", 16);
    CHECK(Clip::decode(junk, sizeof junk) == nullptr, "a wav header and junk");
    memcpy(junk, "OggS", 4);
    CHECK(Clip::decode(junk, sizeof junk) == nullptr, "an ogg capture pattern and junk");
    wav = make_wav(22050, 1, 440.0f, 0.1f, 0.5f, &n);
    Clip* t = Clip::decode(wav, 40);        // cut inside the header
    CHECK(t == nullptr, "truncated wav header");
    free(wav);
}

static void test_mp3_ogg() {
    for (int k = 0; k < 2; k++) {
        const char* name = k == 0 ? "mp3" : "ogg";
        const unsigned char* d = k == 0 ? kMp3 : kOgg;
        size_t n = k == 0 ? sizeof kMp3 : sizeof kOgg;
        Clip* c = Clip::decode(d, n);
        CHECK(c != nullptr, "%s decodes", name);
        if (!c) continue;
        CHECK(c->rate == 22050 && c->channels == 1, "%s: %d Hz %d ch", name, c->rate, c->channels);
        CHECK(c->frames >= 10000 && c->frames <= 13000, "%s: %u frames (0.5 s = 11025, plus codec padding)", name, c->frames);
        // Count crossings in the middle (clear of the codec's start and end).
        int a = 2000, b = (int)c->frames - 2000;
        int zc = crossings_mono(c->data + a, b - a);
        double expect = 440.0 * (double)(b - a) / 22050.0;
        CHECK(zc >= expect - 3 && zc <= expect + 3, "%s: 440 Hz (%d crossings, expect %.1f)", name, zc, expect);
        c->destroy();
    }
    CHECK(Clip::decode(kMp3, 8) == nullptr, "mp3 cut to 8 bytes is rejected");
    // Damaged data in the middle: no crash, whatever it returns.
    unsigned char* dmg = (unsigned char*)malloc(sizeof kMp3);
    memcpy(dmg, kMp3, sizeof kMp3);
    for (size_t i = 600; i < 1400; i++) dmg[i] ^= 0x5a;
    Clip* c = Clip::decode(dmg, sizeof kMp3);
    if (c) c->destroy();
    free(dmg);
    dmg = (unsigned char*)malloc(sizeof kOgg);
    memcpy(dmg, kOgg, sizeof kOgg);
    for (size_t i = 1500; i < 2500; i++) dmg[i] ^= 0x5a;
    c = Clip::decode(dmg, sizeof kOgg);
    if (c) c->destroy();
    free(dmg);
    CHECK(true, "damaged mp3/ogg survived");
}

static void test_mixer() {
    // Resampling 22050 -> 48000 keeps the frequency.
    {
        Mixer m;
        CHECK(m.init(kRate), "mixer init");
        Clip* c = sine_clip(22050, 1, 440.0f, 1.0f, 0.5f);
        VoiceParams p;
        VoiceId v = m.play_clip(c, p);
        CHECK(v > 0 && m.is_active(v), "voice plays");
        static int16_t out[kRate * 2 + 2000];
        m.mix(out, kRate + 1000);
        int zc = crossings(out, 0, kRate, 0);
        CHECK(zc >= 438 && zc <= 442, "22050 -> 48000: 440 Hz stays (%d crossings in 1 s)", zc);
        CHECK(!m.is_active(v) && m.ended_count() == 1, "the one-shot ended");
        int tail = peak(out, kRate + 50, kRate + 1000, 0);
        CHECK(tail == 0, "silence after the end (%d)", tail);
        // Mono source: centred, same level in both channels.
        CHECK(rms(out, 1000, 20000, 0) > 1000 && __builtin_fabs(rms(out, 1000, 20000, 0) - rms(out, 1000, 20000, 1)) < 1.0, "mono is centred");
        // pitch 2: an octave up, half as long
        VoiceParams p2;
        p2.pitch = 2.0f;
        m.play_clip(c, p2);
        m.mix(out, kRate / 2 + 100);
        zc = crossings(out, 0, kRate / 2, 0);
        CHECK(zc >= 438 && zc <= 442, "pitch 2: 880 Hz over 0.5 s (%d)", zc);
        CHECK(m.active_count() == 0, "pitch 2 ended after 0.5 s");
        c->destroy();
        // Same rate, stereo: untouched frequency
        Clip* st = sine_clip(kRate, 2, 1000.0f, 0.5f, 0.5f);
        m.play_clip(st, VoiceParams());
        m.mix(out, kRate / 2);
        CHECK(crossings(out, 0, kRate / 2, 1) == 500 || crossings(out, 0, kRate / 2, 1) == 499, "stereo 1000 Hz (%d)", crossings(out, 0, kRate / 2, 1));
        st->destroy();
    }
    // Saturating mix.
    {
        Mixer m;
        m.init(kRate);
        Clip* c = sine_clip(kRate, 1, 440.0f, 0.5f, 0.95f);
        for (int i = 0; i < 8; i++) { VoiceParams p; p.loop = true; m.play_clip(c, p); }
        static int16_t out[kRate * 2];
        m.mix(out, kRate / 2);
        int pk = peak(out, 0, kRate / 2, 0);
        CHECK(pk == 32767 || pk == 32768, "8 loud voices clip at full scale (peak %d)", pk);
        int zc = crossings(out, 0, kRate / 2, 0);
        CHECK(zc >= 219 && zc <= 221, "no wrap-around distortion (%d crossings)", zc);
        // Saturated, not wrapped: a positive half-wave has no negative samples.
        bool wrapped = false;
        for (int i = 100; i < 140; i++) if (out[i * 2] < 0 && out[(i - 1) * 2] > 30000) wrapped = true;
        CHECK(!wrapped, "positive peaks do not wrap to negative");
        c->destroy();
    }
    // Voice limit: 32 voices, then the oldest one-shot is stolen.
    {
        Mixer m;
        m.init(kRate);
        Clip* c = sine_clip(kRate, 1, 440.0f, 1.0f, 0.1f);
        VoiceId first = m.play_clip(c, VoiceParams());
        for (int i = 1; i < Mixer::kMaxVoices; i++) m.play_clip(c, VoiceParams());
        CHECK(m.active_count() == 32, "32 voices (%d)", m.active_count());
        VoiceId extra = m.play_clip(c, VoiceParams());
        CHECK(extra > 0 && m.active_count() == 32 && !m.is_active(first) && m.is_active(extra), "the oldest voice is stolen");
        m.stop_all();
        CHECK(m.active_count() == 0, "stop_all");
        // stale handles stay dead
        CHECK(!m.is_active(extra), "a stopped handle is not active");
        c->destroy();
    }
    // Pause, loop, pan.
    {
        Mixer m;
        m.init(kRate);
        Clip* c = sine_clip(kRate, 1, 440.0f, 0.1f, 0.5f);
        VoiceParams p;
        p.loop = true;
        p.pan = 1.0f;
        VoiceId v = m.play_clip(c, p);
        static int16_t out[kRate * 2];
        m.mix(out, kRate / 2);
        CHECK(m.is_active(v), "a looping clip keeps playing past its length");
        CHECK(rms(out, 1000, 20000, 0) < 1.0 && rms(out, 1000, 20000, 1) > 5000, "pan 1: right only (L %.1f R %.1f)", rms(out, 1000, 20000, 0), rms(out, 1000, 20000, 1));
        m.pause(v, true);
        m.mix(out, 1000);
        CHECK(peak(out, 0, 1000, 1) == 0 && m.is_paused(v), "paused is silent");
        m.pause(v, false);
        m.mix(out, 1000);
        CHECK(peak(out, 0, 1000, 1) > 0, "resumed");
        c->destroy();
    }
    // Streams: decoded on the fly from memory.
    {
        Mixer m;
        m.init(kRate);
        size_t n;
        unsigned char* wav = make_wav(22050, 1, 440.0f, 0.2f, 0.5f, &n);
        VoiceParams p;
        VoiceId v = m.play_stream(wav, n, p);       // takes ownership
        CHECK(v > 0, "stream starts");
        static int16_t out[kRate * 2];
        m.mix(out, kRate);
        CHECK(!m.is_active(v), "a non-looping stream ends");
        int zc = crossings(out, 0, 9000, 0);
        CHECK(zc >= 80 && zc <= 84, "stream 440 Hz over 9000 frames (%d)", zc);
        unsigned char* wav2 = make_wav(22050, 1, 440.0f, 0.2f, 0.5f, &n);
        p.loop = true;
        v = m.play_stream(wav2, n, p);
        m.mix(out, kRate);
        m.mix(out, kRate);
        CHECK(m.is_active(v), "a looping stream is still playing after 2 s");
        zc = crossings(out, 0, kRate, 0);
        CHECK(zc >= 436 && zc <= 444, "looping stream keeps its pitch across loop seams (%d)", zc);
        m.stop(v);
        CHECK(!m.is_active(v), "stream stopped");
        // not decodable: -1, buffer freed
        void* junk = malloc(100);
        memset(junk, 0x41, 100);
        CHECK(m.play_stream(junk, 100, p) < 0, "garbage is not a stream");
        // mp3 / ogg streams
        for (int k = 0; k < 2; k++) {
            size_t sz = k == 0 ? sizeof kMp3 : sizeof kOgg;
            void* copy = malloc(sz);
            memcpy(copy, k == 0 ? kMp3 : kOgg, sz);
            VoiceParams lp;
            lp.loop = true;
            VoiceId sv = m.play_stream(copy, sz, lp);
            CHECK(sv > 0, "%s stream starts", k == 0 ? "mp3" : "ogg");
            m.mix(out, kRate);      // 1 s: two loops of 0.5 s
            CHECK(m.is_active(sv), "%s stream loops", k == 0 ? "mp3" : "ogg");
            int z = crossings(out, 5000, kRate - 5000, 0);
            double expect = 440.0 * (double)(kRate - 10000) / (double)kRate;
            CHECK(z >= expect - 8 && z <= expect + 8, "%s stream pitch (%d, expect %.0f)", k == 0 ? "mp3" : "ogg", z, expect);
            m.stop(sv);
        }
    }
}

static void test_device() {
    {
        AudioDevice d;
        CHECK(d.init(AudioDevice::Backend::Null, 44100) && d.rate() == 44100 && d.backend() == AudioDevice::Backend::Null, "null device at 44100");
        CHECK(!d.has_sound_card(), "null: no card");
        int16_t buf[200 * 2];
        for (int i = 0; i < 200 * 2; i++) buf[i] = (int16_t)(i - 100);
        CHECK(d.write(buf, 200) == 200 && d.queued() == 200, "queued 200");
        int16_t back[300 * 2];
        CHECK(d.capture(back, 300) == 200 && memcmp(back, buf, sizeof buf) == 0, "capture returns what was written");
        CHECK(d.capture(back, 300) == 0, "and only once");
        d.null_advance(150);
        CHECK(d.queued() == 50 && d.underruns() == 0, "50 left");
        d.null_advance(100);
        CHECK(d.queued() == 0 && d.underruns() == 1, "ran dry: underrun");
        static int16_t big[20000 * 2];
        CHECK(d.write(big, 20000) == AudioDevice::kQueueFrames, "never takes more than its capacity (%d)", d.writable());
        CHECK(d.write(big, 10) == 0, "full: 0 taken, no blocking");
        // The real-time fallback: Auto on a host with no OS device.
        AudioDevice a;
        CHECK(a.init(), "auto init");
        CHECK(a.backend() == AudioDevice::Backend::Null && !a.has_sound_card(), "auto falls back to the null device on the host");
        AudioDevice os;
        CHECK(!os.init(AudioDevice::Backend::MinimalOS) && !os.ready() && os.os_error() != 0, "MinimalOS backend fails off the OS");
    }
    // The system keeps ~80 ms queued.
    {
        Rig r;
        CHECK(r.init(), "rig");
        r.w.progress(1.0f / 60.0f);
        int q = r.dev.queued();
        CHECK(q >= 3800 && q <= 3900, "first update fills 80 ms (%d frames)", q);
        for (int i = 0; i < 10; i++) { r.dev.null_advance(800); r.w.progress(1.0f / 60.0f); }
        q = r.dev.queued();
        CHECK(q >= 3800 && q <= 3900, "and keeps it (%d)", q);
        CHECK(r.dev.underruns() == 0, "no underruns while it is called every frame");
        // A stall of 100 ms (frame hiccup) -> an underrun is counted, and the queue recovers.
        r.dev.null_advance(4800);
        CHECK(r.dev.underruns() == 1, "a 100 ms stall under-runs once (%u)", r.dev.underruns());
        r.w.progress(1.0f / 60.0f);
        q = r.dev.queued();
        CHECK(q >= 3800 && q <= 3900, "recovered (%d)", q);
        AudioStats st = r.sys.stats();
        CHECK(st.underruns == 1 && st.frames_pushed > 0 && st.voices_active == 0, "stats");
    }
}

static void test_slow_frames() {
    // A 10 fps game: the target grows with the frame time, no underruns.
    Rig r;
    CHECK(r.init(), "rig");
    r.files.add_wav("tone.wav", 22050, 1, 440.0f, 1.0f, 0.5f);
    r.stream("S", "tone.wav", Vec3(), true, true, false);
    for (int i = 0; i < 30; i++) {
        r.dev.null_advance(kRate / 10);
        r.w.progress(0.1f);
    }
    CHECK(r.dev.underruns() == 0, "10 fps: no underruns (%u)", r.dev.underruns());
    int q = r.dev.queued();
    CHECK(q >= 9000 && q <= 9700, "10 fps: ~200 ms queued (%d)", q);
    // A 2 fps hiccup is capped at ~250 ms and under-runs.
    r.dev.null_advance(kRate / 2);
    CHECK(r.dev.underruns() == 1, "a half second stall under-runs once (%u)", r.dev.underruns());
    r.w.progress(0.5f);
    q = r.dev.queued();
    CHECK(q >= 11500 && q <= 12500, "capped at 250 ms (%d)", q);
}

static void test_streams_in_world() {
    Rig r;
    CHECK(r.init(), "rig");
    r.files.add_wav("tone.wav", 22050, 1, 440.0f, 0.3f, 0.5f);
    Entity looped = r.stream("Looped", "tone.wav", Vec3(), true, true, false);
    Entity once = r.stream("Once", "tone.wav", Vec3(), false, true, false);
    Entity manual = r.stream("Manual", "tone.wav", Vec3(), true, false, false);
    CHECK(!r.sys.is_playing(looped), "nothing plays before the first frame");
    r.run(1.0f / 60.0f);
    CHECK(r.sys.is_playing(looped) && r.sys.is_playing(once), "play_on_start streams start with the world");
    CHECK(!r.sys.is_playing(manual), "play_on_start = false does not");
    r.run(1.0f);
    CHECK(r.sys.is_playing(looped), "the looping stream keeps playing after 1 s");
    CHECK(!r.sys.is_playing(once), "the non-looping one ended");
    CHECK(r.files.logs == 0, "no log lines for good files");
    // Ended voices free the mixer.
    CHECK(r.sys.stats().voices_active == 1, "one voice active (%d)", r.sys.stats().voices_active);
    // The output is the looping tone, then silence from `once` is irrelevant: check the pitch.
    int zc = crossings(g_cap, 24000, 48000, 0);
    CHECK(zc >= 218 && zc <= 222, "looping stream pitch (%d crossings in 0.5 s)", zc);
    // Manual control.
    CHECK(r.sys.play_stream(manual) && r.sys.is_playing(manual), "play_stream");
    r.sys.pause_stream(manual);
    CHECK(!r.sys.is_playing(manual), "pause_stream");
    r.run(0.1f);
    r.sys.pause_stream(manual, false);
    CHECK(r.sys.is_playing(manual), "resumed");
    r.sys.stop_stream(manual);
    CHECK(!r.sys.is_playing(manual), "stop_stream");
    r.run(0.1f);
    CHECK(!r.sys.is_playing(manual), "a stopped stream stays stopped");
    // Live module settings: volume 0 silences, pitch changes frequency.
    r.clear();
    r.run(0.2f);
    double before = rms(g_cap, 4000, 9000, 0);
    ((AudioStream*)r.w.get_mut(looped, ModuleKind::AudioStream))->volume = 0.0f;
    r.run(0.3f);
    CHECK(before > 2000, "audible before (%.0f)", before);
    // the push is ~80 ms ahead of the game clock: look at the end of the capture
    double after = rms(g_cap, r.cap_frames - 3000, r.cap_frames, 0);
    CHECK(after < 1.0, "volume 0 silences it (%.1f)", after);
    ((AudioStream*)r.w.get_mut(looped, ModuleKind::AudioStream))->volume = 0.5f;
    ((AudioStream*)r.w.get_mut(looped, ModuleKind::AudioStream))->pitch = 2.0f;
    r.clear();
    r.run(0.6f);
    zc = crossings(g_cap, 2000, 2000 + 24000, 0);
    CHECK(zc >= 436 && zc <= 444, "pitch 2.0 doubles the frequency (%d crossings in 0.5 s)", zc);
    // Destroying the entity stops its sound.
    r.w.destroy(looped);
    r.run(0.1f);
    CHECK(r.sys.stats().voices_active == 0, "a destroyed entity's voice is gone (%d)", r.sys.stats().voices_active);
    // restart(): play_on_start streams start anew.
    r.sys.restart();
    r.run(0.1f);
    CHECK(r.sys.is_playing(once), "restart() starts play_on_start streams again");
}

static void test_samplers() {
    Rig r;
    CHECK(r.init(), "rig");
    r.files.add_wav("hit.wav", 22050, 1, 300.0f, 1.0f, 0.5f);
    Entity s = r.sampler("Hit", "hit.wav", Vec3(), 3, false);
    Entity none = r.w.create("None");
    CHECK(!r.sys.play_sampler(none), "no sampler module: false");
    r.run(1.0f / 60.0f);
    CHECK(!r.sys.is_playing(s), "idle until played");
    CHECK(r.sys.play_sampler(s) && r.sys.is_playing(s), "play_sampler");
    r.run(0.1f);
    r.sys.play_sampler(s);
    r.run(0.1f);
    r.sys.play_sampler(s);
    CHECK(r.sys.stats().voices_active == 3, "three overlap (%d)", r.sys.stats().voices_active);
    r.sys.play_sampler(s);
    r.sys.play_sampler(s);
    CHECK(r.sys.stats().voices_active == 3, "the voice limit holds, the oldest is cut (%d)", r.sys.stats().voices_active);
    r.run(1.2f);
    CHECK(r.sys.stats().voices_active == 0 && !r.sys.is_playing(s), "one-shots end (%d)", r.sys.stats().voices_active);
    CHECK(r.sys.stats().clips_cached == 1 && r.files.reads == 1, "the clip is decoded once and cached (%d reads)", r.files.reads);
    CHECK(r.files.logs == 0, "no logs");

    // Pitch variation: each play differs, within +- variation.
    AudioSampler* m = (AudioSampler*)r.w.get_mut(s, ModuleKind::AudioSampler);
    m->pitch_variation = 0.5f;
    m->voices = 8;
    for (int i = 0; i < 6; i++) { r.sys.play_sampler(s); }
    Mixer& mx = r.sys.mixer();
    r.run(0.2f);
    CHECK(mx.active_count() == 6, "six voices (%d)", mx.active_count());
    r.sys.stop_all();
    CHECK(r.sys.stats().voices_active == 0, "stop_all");

    // Variation 0: two plays at the same moment are identical (sum doubles).
    m->pitch_variation = 0.0f;
    r.clear();
    r.sys.play_sampler(s);
    r.run(0.1f);
    double one = rms(g_cap, 1000, 4000, 0);
    r.sys.stop_all();
    r.run(0.3f);
    r.clear();
    r.sys.play_sampler(s);
    r.sys.play_sampler(s);
    r.run(0.1f);
    double two = rms(g_cap, 1000, 4000, 0);
    CHECK(two > one * 1.9 && two < one * 2.1, "two identical voices are twice as loud (%.0f vs %.0f)", one, two);

    // Variation shifts the pitch: measure the spread of crossing counts over many plays.
    m->pitch_variation = 0.5f;
    m->voices = 1;
    int lo = 1000000, hi = 0;
    for (int i = 0; i < 6; i++) {
        r.sys.stop_all();
        r.run(0.2f);
        r.clear();
        r.sys.play_sampler(s);
        r.run(0.3f);
        int zc = crossings(g_cap, 0, 9600, 0);       // 0.2 s
        if (zc < lo) lo = zc;
        if (zc > hi) hi = zc;
    }
    CHECK(hi - lo >= 6, "pitch_variation spreads the pitch (crossings %d..%d)", lo, hi);
    CHECK(lo >= 30 && hi <= 90, "within 300 Hz * (1 +- 0.5) (%d..%d, 0.2 s)", lo, hi);

    // Missing and undecodable files: one log line, no crash.
    Entity bad = r.sampler("Bad", "nope.wav", Vec3(), 2, false);
    int logs0 = r.files.logs;
    CHECK(!r.sys.play_sampler(bad), "missing file: false");
    CHECK(r.files.logs == logs0 + 1 && strstr(r.files.last_log, "nope.wav"), "one log line: %s", r.files.last_log);
    r.sys.play_sampler(bad);
    r.sys.play_sampler(bad);
    CHECK(r.files.logs == logs0 + 1, "and no more for the same file (%d)", r.files.logs - logs0);
    r.files.add("junk.wav", "this is not a sound file at all, not even close", 48);
    Entity junk = r.sampler("Junk", "junk.wav", Vec3(), 2, false);
    CHECK(!r.sys.play_sampler(junk) && r.files.logs == logs0 + 2 && strstr(r.files.last_log, "decode"), "undecodable: one log line (%s)", r.files.last_log);
    Entity sb = r.stream("BadStream", "gone.ogg", Vec3(), true, true, false);
    r.run(0.5f);
    CHECK(r.files.logs == logs0 + 3 && !r.sys.is_playing(sb), "a missing stream logs once and stays silent (%d)", r.files.logs - logs0);
    Entity sj = r.stream("JunkStream", "junk.wav", Vec3(), true, true, false);
    r.run(0.2f);
    CHECK(r.files.logs == logs0 + 4 && !r.sys.is_playing(sj), "an undecodable stream logs once");
    Entity empty = r.stream("EmptyPath", "", Vec3(), true, true, false);
    r.run(0.1f);
    CHECK(!r.sys.is_playing(empty) && r.files.logs == logs0 + 4, "an empty path is silently nothing");
}

static void test_spatial() {
    // A source far beyond max_distance is silent; to the right it is louder on the right.
    {
        Rig r;
        r.init();
        r.files.add_wav("tone.wav", kRate, 1, 440.0f, 1.0f, 0.8f);
        r.listener(Vec3(0, 0, 0));
        Entity far = r.stream("Far", "tone.wav", Vec3(100, 0, 0), true, true, true);
        ((AudioStream*)r.w.get_mut(far, ModuleKind::AudioStream))->max_distance = 30.0f;
        r.run(0.5f);
        CHECK(r.sys.is_playing(far), "the far source is playing...");
        int pk = peak(g_cap, 3000, r.cap_frames, 0) + peak(g_cap, 3000, r.cap_frames, 1);
        CHECK(pk == 0, "...but silent beyond max_distance (peak %d)", pk);
        // Bring it close on the right.
        r.w.transform(far)->position = Vec3(5, 0, 0);
        r.clear();
        r.run(0.5f);
        double l = rms(g_cap, 6000, 20000, 0), rr = rms(g_cap, 6000, 20000, 1);
        CHECK(rr > l * 3 && rr > 500, "a source to the right: R %.0f > L %.0f", rr, l);
        // Left
        r.w.transform(far)->position = Vec3(-5, 0, 0);
        r.clear();
        r.run(0.5f);
        l = rms(g_cap, 6000, 20000, 0); rr = rms(g_cap, 6000, 20000, 1);
        CHECK(l > rr * 3, "to the left: L %.0f > R %.0f", l, rr);
        // In front (-Z): centred
        r.w.transform(far)->position = Vec3(0, 0, -5);
        r.clear();
        r.run(0.5f);
        l = rms(g_cap, 6000, 20000, 0); rr = rms(g_cap, 6000, 20000, 1);
        CHECK(__builtin_fabs(l - rr) < 0.02 * l, "in front: centred (%.0f / %.0f)", l, rr);
        // Distance falls off: min_distance 1, 5 m vs 20 m.
        r.w.transform(far)->position = Vec3(0, 0, -5);
        r.clear(); r.run(0.4f);
        double near5 = rms(g_cap, 6000, 18000, 0);
        r.w.transform(far)->position = Vec3(0, 0, -20);
        r.clear(); r.run(0.4f);
        double far20 = rms(g_cap, 6000, 18000, 0);
        r.w.transform(far)->position = Vec3(0, 0, -0.5f);
        r.clear(); r.run(0.4f);
        double close = rms(g_cap, 6000, 18000, 0);
        CHECK(near5 > far20 * 2 && far20 > 0, "gain falls with distance (%.0f at 5, %.0f at 20)", near5, far20);
        CHECK(close > near5, "and is full inside min_distance (%.0f)", close);
        // A listener turned around swaps the sides: source at +X, listener faces +X (yaw 180 deg).
        Entity le = r.w.lookup("Listener");
        r.w.transform(le)->rotation = Quat::axis_angle(Vec3(0, 1, 0), kPi);
        r.w.transform(far)->position = Vec3(5, 0, 0);
        r.clear(); r.run(0.4f);
        l = rms(g_cap, 6000, 18000, 0); rr = rms(g_cap, 6000, 18000, 1);
        CHECK(l > rr * 3, "a listener turned 180 deg hears the +X source on the left (L %.0f R %.0f)", l, rr);
        // A sampler is spatialized the same way; a non-spatial stream ignores position.
        r.w.destroy(far);
    }
    // Non-spatial streams ignore position; sampler spatial.
    {
        Rig r;
        r.init();
        r.files.add_wav("tone.wav", kRate, 1, 440.0f, 1.0f, 0.8f);
        r.listener(Vec3(0, 0, 0));
        r.stream("Flat", "tone.wav", Vec3(500, 0, 0), true, true, false);
        r.run(0.4f);
        double l = rms(g_cap, 6000, 18000, 0), rr = rms(g_cap, 6000, 18000, 1);
        CHECK(l > 1000 && __builtin_fabs(l - rr) < 0.02 * l, "a flat stream is not positioned (%.0f/%.0f)", l, rr);
        Rig r2;
        r2.init();
        r2.files.add_wav("tone.wav", kRate, 1, 440.0f, 1.0f, 0.8f);
        r2.listener(Vec3(0, 0, 0));
        Entity sp = r2.sampler("S", "tone.wav", Vec3(4, 0, 0), 2, true);
        r2.run(0.05f);
        r2.clear();
        r2.sys.play_sampler(sp);
        r2.run(0.4f);
        l = rms(g_cap, 6000, 18000, 0); rr = rms(g_cap, 6000, 18000, 1);
        CHECK(rr > l * 3, "a spatial sampler to the right (L %.0f R %.0f)", l, rr);
        r2.w.transform(sp)->position = Vec3(200, 0, 0);
        r2.clear();
        r2.sys.play_sampler(sp);
        r2.run(0.4f);
        CHECK(peak(g_cap, 2000, r2.cap_frames, 0) + peak(g_cap, 2000, r2.cap_frames, 1) == 0, "a sampler beyond max_distance is silent");
    }
    // Listener volume is the master.
    {
        double level[3];
        float vols[3] = { 1.0f, 0.5f, 0.0f };
        for (int k = 0; k < 3; k++) {
            Rig r;
            r.init();
            r.files.add_wav("tone.wav", kRate, 1, 440.0f, 1.0f, 0.8f);
            r.listener(Vec3(0, 0, 0), vols[k]);
            r.stream("S", "tone.wav", Vec3(), true, true, false);
            r.run(0.5f);
            level[k] = rms(g_cap, 8000, 20000, 0);
        }
        CHECK(level[0] > 1000, "baseline audible (%.0f)", level[0]);
        CHECK(level[1] > level[0] * 0.47 && level[1] < level[0] * 0.53, "listener volume 0.5 halves the output (%.0f vs %.0f)", level[1], level[0]);
        CHECK(level[2] < 1.0, "listener volume 0 is silent (%.1f)", level[2]);
    }
    // No listener: spatial sources are played flat.
    {
        Rig r;
        r.init();
        r.files.add_wav("tone.wav", kRate, 1, 440.0f, 1.0f, 0.8f);
        r.stream("S", "tone.wav", Vec3(900, 0, 0), true, true, true);
        r.run(0.4f);
        CHECK(rms(g_cap, 6000, 18000, 0) > 1000, "no listener: played flat");
    }
}

static void test_perf() {
    // 32 voices for 1 s of audio.
    Mixer m;
    m.init(kRate);
    Clip* mono = sine_clip(22050, 1, 440.0f, 2.0f, 0.1f);
    Clip* st = sine_clip(44100, 2, 330.0f, 2.0f, 0.1f);
    for (int i = 0; i < 32; i++) {
        VoiceParams p;
        p.loop = true;
        p.pan = (float)(i - 16) / 16.0f;
        p.pitch = 0.8f + 0.02f * (float)i;
        m.play_clip((i & 1) ? st : mono, p);
    }
    static int16_t out[kRate * 2];
    double best = 1e9;
    for (int rep = 0; rep < 5; rep++) {
        double t0 = now_ms();
        for (int i = 0; i < 48; i++) m.mix(out + i * 1000 * 2, 1000);
        double ms = now_ms() - t0;
        if (ms < best) best = ms;
    }
    CHECK(m.active_count() == 32, "32 voices active");
    printf("audio: mixing 32 clip voices for 1 s of audio: %.2f ms (%.2f%% of real time)\n", best, best / 10.0);
    CHECK(best < 200.0, "32 voices must mix far faster than real time (%.1f ms)", best);
    m.stop_all();
    mono->destroy();
    st->destroy();
    // 32 streamed voices (WAV decoded on the fly).
    size_t n;
    unsigned char* wav = make_wav(44100, 2, 500.0f, 1.0f, 0.1f, &n);
    for (int i = 0; i < 32; i++) {
        void* copy = malloc(n);
        memcpy(copy, wav, n);
        VoiceParams p;
        p.loop = true;
        p.pitch = 0.9f + 0.01f * (float)i;
        m.play_stream(copy, n, p);
    }
    free(wav);
    best = 1e9;
    for (int rep = 0; rep < 3; rep++) {
        double t0 = now_ms();
        for (int i = 0; i < 48; i++) m.mix(out + i * 1000 * 2, 1000);
        double ms = now_ms() - t0;
        if (ms < best) best = ms;
    }
    printf("audio: mixing 32 streamed (wav) voices for 1 s of audio: %.2f ms\n", best);
    CHECK(best < 400.0, "32 streams (%.1f ms)", best);
    // The mp3 decoder: 1 s of audio through a stream.
    {
        Mixer m2;
        m2.init(kRate);
        void* copy = malloc(sizeof kMp3);
        memcpy(copy, kMp3, sizeof kMp3);
        VoiceParams p;
        p.loop = true;
        m2.play_stream(copy, sizeof kMp3, p);
        double t0 = now_ms();
        for (int i = 0; i < 48; i++) m2.mix(out + i * 1000 * 2, 1000);
        printf("audio: one mp3 stream, 1 s of audio: %.2f ms\n", now_ms() - t0);
    }
}

int main() {
    test_decode();
    test_mp3_ogg();
    test_mixer();
    test_device();
    test_slow_frames();
    test_streams_in_world();
    test_samplers();
    test_spatial();
    test_perf();
    printf("audio: %d checks, %d failed\n", g_checks, g_failed);
    if (g_failed) { printf("AUDIO TESTS FAILED\n"); return 1; }
    printf("audio tests passed\n");
    return 0;
}
