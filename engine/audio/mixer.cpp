// Clip, Mixer: voices, linear resampling, saturating mix.
#include "audio/audio.h"
#include "audio/decode.h"
#include "core/math.h"

#include <stdlib.h>
#include <string.h>

namespace fe {

// ---------------------------------------------------------------- Clip

static int16_t to_s16(float x) {
    x *= 32768.0f;
    if (x >= 32767.0f) return 32767;
    if (x <= -32768.0f) return -32768;
    return (int16_t)(int)(x + (x >= 0 ? 0.5f : -0.5f));
}

Clip* Clip::decode(const void* file, size_t size) {
    int rate = 0, ch = 0;
    uint64_t total = 0;
    FeDecoder* d = fe_decoder_open(file, size, &rate, &ch, &total);
    if (!d) return nullptr;
    const uint64_t kMaxFrames = 1ull << 26;         // ~25 minutes at 44.1 kHz; longer is a stream's job
    uint64_t cap = total ? total : 65536;
    if (cap > kMaxFrames) cap = kMaxFrames;
    int16_t* pcm = (int16_t*)malloc((size_t)cap * (size_t)ch * sizeof(int16_t));
    uint64_t n = 0;
    float buf[1024 * 2];
    while (pcm) {
        uint64_t got = fe_decoder_read(d, buf, 1024);
        if (got == 0) break;
        if (n + got > cap) {
            if (cap >= kMaxFrames) break;
            uint64_t ncap = cap * 2 > kMaxFrames ? kMaxFrames : cap * 2;
            int16_t* np = (int16_t*)realloc(pcm, (size_t)ncap * (size_t)ch * sizeof(int16_t));
            if (!np) break;
            pcm = np;
            cap = ncap;
            if (n + got > cap) got = cap - n;
        }
        for (uint64_t i = 0; i < got * (uint64_t)ch; i++) pcm[n * (uint64_t)ch + i] = to_s16(buf[i]);
        n += got;
    }
    fe_decoder_close(d);
    if (!pcm || n == 0) { free(pcm); return nullptr; }
    Clip* c = new Clip();
    c->data = pcm;
    c->frames = (uint32_t)n;
    c->channels = ch;
    c->rate = rate;
    return c;
}

Clip* Clip::from_pcm(const int16_t* pcm, uint32_t frames, int channels, int rate) {
    if (!pcm || !frames || (channels != 1 && channels != 2) || rate < 1) return nullptr;
    size_t n = (size_t)frames * (size_t)channels;
    int16_t* d = (int16_t*)malloc(n * sizeof(int16_t));
    if (!d) return nullptr;
    memcpy(d, pcm, n * sizeof(int16_t));
    Clip* c = new Clip();
    c->data = d;
    c->frames = frames;
    c->channels = channels;
    c->rate = rate;
    return c;
}

void Clip::destroy() {
    free(data);
    data = nullptr;
    delete this;
}

// ---------------------------------------------------------------- Mixer

namespace {
constexpr int kBlock = 256;         // frames mixed at a time (gains ramp across a block)
constexpr int kFifo = 4096;         // stream: decoded frames kept (stereo float)
constexpr int kDecodeChunk = 512;
}

struct Mixer::Voice {
    bool active = false, paused = false, loop = false, is_stream = false, fresh = true;
    VoiceId id = 0;
    uint32_t serial = 0;
    const Clip* clip = nullptr;
    double pos = 0;             // clip: frame; stream: offset into the fifo
    double played = 0;          // source frames played
    double ratio = 1;           // source rate / device rate
    float volume = 1, pan = 0, pitch = 1;
    float gl = 0, gr = 0;       // current gains
    // stream
    FeDecoder* dec = nullptr;
    void* file = nullptr;
    int channels = 1, src_rate = 44100;
    float* fifo = nullptr;
    int fifo_n = 0;
    bool eof = false;
};

Mixer::Mixer() {}
Mixer::~Mixer() { shutdown(); }

bool Mixer::init(int rate, int voices) {
    shutdown();
    if (rate < 8000 || rate > 192000) return false;
    if (voices < 1) voices = 1;
    if (voices > kMaxVoices) voices = kMaxVoices;
    voices_ = new Voice[voices];
    acc_ = (float*)malloc(sizeof(float) * kBlock * 2);
    if (!acc_) { delete[] voices_; voices_ = nullptr; return false; }
    nvoices_ = voices;
    rate_ = rate;
    serial_ = 0;
    ended_ = 0;
    master = 1.0f;
    return true;
}

static void release(Mixer::Voice& v) {
    if (v.dec) fe_decoder_close(v.dec);
    free(v.file);
    free(v.fifo);
    v = Mixer::Voice();
}

// ---- reverb

struct Mixer::Reverb {
    static constexpr int kCombs = 4, kAllpass = 2;
    float* buf = nullptr;
    int comb_len[2][kCombs], ap_len[2][kAllpass];
    int comb_pos[2][kCombs] = {}, ap_pos[2][kAllpass] = {};
    float* comb[2][kCombs];
    float* ap[2][kAllpass];
    float comb_lp[2][kCombs] = {};
    float lp[2] = {};
    float wet = 0, room = 0.5f, damping = 0.5f, lowpass = 0;     // the current (ramped) values

    bool init(int rate) {
        static const int cl[kCombs] = { 1116, 1188, 1277, 1356 }, al[kAllpass] = { 556, 441 };
        int total = 0;
        for (int s = 0; s < 2; s++) {
            for (int i = 0; i < kCombs; i++) { comb_len[s][i] = (cl[i] + s * 23) * rate / 44100; total += comb_len[s][i]; }
            for (int i = 0; i < kAllpass; i++) { ap_len[s][i] = (al[i] + s * 23) * rate / 44100; total += ap_len[s][i]; }
        }
        buf = (float*)calloc((size_t)total, sizeof(float));
        if (!buf) return false;
        float* p = buf;
        for (int s = 0; s < 2; s++) {
            for (int i = 0; i < kCombs; i++) { comb[s][i] = p; p += comb_len[s][i]; }
            for (int i = 0; i < kAllpass; i++) { ap[s][i] = p; p += ap_len[s][i]; }
        }
        return true;
    }
    ~Reverb() { free(buf); }

    // Processes interleaved stereo in place.
    void process(float* x, int frames) {
        const float fb = 0.70f + 0.28f * room;
        const float d = 0.1f + 0.7f * damping;
        const float a = 1.0f - lowpass * 0.97f;
        const bool wet_on = wet > 0.0005f;
        for (int i = 0; i < frames; i++) {
            float in = (x[i * 2] + x[i * 2 + 1]) * 0.25f;
            float out[2] = { 0, 0 };
            if (wet_on) {
                for (int s = 0; s < 2; s++) {
                    float sum = 0;
                    for (int c = 0; c < kCombs; c++) {
                        float* b = comb[s][c];
                        int& pos = comb_pos[s][c];
                        float y = b[pos];
                        comb_lp[s][c] = y * (1.0f - d) + comb_lp[s][c] * d;
                        b[pos] = in + comb_lp[s][c] * fb;
                        if (++pos >= comb_len[s][c]) pos = 0;
                        sum += y;
                    }
                    for (int k = 0; k < kAllpass; k++) {
                        float* b = ap[s][k];
                        int& pos = ap_pos[s][k];
                        float bo = b[pos];
                        float y = -sum + bo;
                        b[pos] = sum + bo * 0.5f;
                        if (++pos >= ap_len[s][k]) pos = 0;
                        sum = y;
                    }
                    out[s] = sum;
                }
            }
            for (int s = 0; s < 2; s++) {
                float v = x[i * 2 + s] * (1.0f - wet * 0.35f) + out[s] * wet * 1.2f;
                lp[s] += a * (v - lp[s]);
                x[i * 2 + s] = lowpass > 0.001f ? lp[s] : v;
            }
        }
    }
};

void Mixer::set_reverb(const ReverbParams& p) {
    rv_target_ = p;
    if ((p.wet > 0.0005f || p.lowpass > 0.001f) && !rv_) {
        rv_ = new Reverb();
        if (!rv_->init(rate_)) { delete rv_; rv_ = nullptr; }
    }
}

bool Mixer::reverb_active() const {
    return rv_ && (rv_->wet > 0.0005f || rv_->lowpass > 0.001f || rv_target_.wet > 0.0005f || rv_target_.lowpass > 0.001f);
}

void Mixer::shutdown() {
    delete rv_;
    rv_ = nullptr;
    tail_ = 0;
    for (int i = 0; i < nvoices_; i++) release(voices_[i]);
    delete[] voices_;
    voices_ = nullptr;
    nvoices_ = 0;
    free(acc_);
    acc_ = nullptr;
}

Mixer::Voice* Mixer::find(VoiceId id) const {
    if (id <= 0) return nullptr;
    int i = id & 0xff;
    if (i >= nvoices_) return nullptr;
    Voice* v = &voices_[i];
    return (v->active && v->id == id) ? v : nullptr;
}

Mixer::Voice* Mixer::alloc() {
    Voice* best = nullptr;
    for (int i = 0; i < nvoices_; i++) {
        if (!voices_[i].active) { best = &voices_[i]; break; }
        if (!voices_[i].is_stream && (!best || voices_[i].serial < best->serial)) best = &voices_[i];
    }
    if (!best) return nullptr;
    int idx = (int)(best - voices_);
    if (best->active) release(*best);
    serial_++;
    if ((serial_ & 0x7fffff) == 0) serial_++;
    best->serial = serial_;
    best->id = (VoiceId)(((serial_ & 0x7fffff) << 8) | (uint32_t)idx);
    return best;
}

static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

static void apply(Mixer::Voice& v, const VoiceParams& p) {
    v.volume = p.volume < 0 ? 0 : p.volume;
    v.pan = clampf(p.pan, -1.0f, 1.0f);
    v.pitch = clampf(p.pitch, 0.05f, 8.0f);
    v.loop = p.loop;
}

VoiceId Mixer::play_clip(const Clip* clip, const VoiceParams& p) {
    if (!voices_ || !clip || !clip->data || !clip->frames) return -1;
    Voice* v = alloc();
    if (!v) return -1;
    VoiceId id = v->id;
    uint32_t serial = v->serial;
    *v = Voice();
    v->id = id;
    v->serial = serial;
    v->active = true;
    v->clip = clip;
    v->channels = clip->channels;
    v->src_rate = clip->rate;
    v->ratio = (double)clip->rate / (double)rate_;
    apply(*v, p);
    return id;
}

VoiceId Mixer::play_stream(void* file, size_t size, const VoiceParams& p) {
    if (!voices_ || !file) { free(file); return -1; }
    int rate = 0, ch = 0;
    uint64_t frames = 0;
    FeDecoder* d = fe_decoder_open(file, size, &rate, &ch, &frames);
    if (!d) { free(file); return -1; }
    float* fifo = (float*)malloc(sizeof(float) * kFifo * 2);
    Voice* v = fifo ? alloc() : nullptr;
    if (!v) { fe_decoder_close(d); free(file); free(fifo); return -1; }
    VoiceId id = v->id;
    uint32_t serial = v->serial;
    *v = Voice();
    v->id = id;
    v->serial = serial;
    v->active = true;
    v->is_stream = true;
    v->dec = d;
    v->file = file;
    v->fifo = fifo;
    v->channels = ch;
    v->src_rate = rate;
    v->ratio = (double)rate / (double)rate_;
    apply(*v, p);
    return id;
}

bool Mixer::is_active(VoiceId id) const { return find(id) != nullptr; }
void Mixer::stop(VoiceId id) { if (Voice* v = find(id)) release(*v); }
void Mixer::stop_all() { for (int i = 0; i < nvoices_; i++) release(voices_[i]); }
void Mixer::pause(VoiceId id, bool paused) { if (Voice* v = find(id)) v->paused = paused; }
bool Mixer::is_paused(VoiceId id) const { const Voice* v = find(id); return v && v->paused; }
void Mixer::set(VoiceId id, const VoiceParams& p) { if (Voice* v = find(id)) apply(*v, p); }
float Mixer::position(VoiceId id) const {
    const Voice* v = find(id);
    return v ? (float)(v->played / (double)v->src_rate) : 0.0f;
}
int Mixer::active_count() const {
    int n = 0;
    for (int i = 0; i < nvoices_; i++) n += voices_[i].active ? 1 : 0;
    return n;
}

// Decodes into the fifo until it holds `need` frames or the file ended.
static void fill(Mixer::Voice& v, int need) {
    if (need > kFifo) need = kFifo;
    int guard = 0;
    while (v.fifo_n < need && !v.eof && guard++ < 64) {
        int room = kFifo - v.fifo_n;
        int want = room < kDecodeChunk ? room : kDecodeChunk;
        if (want <= 0) break;
        float* dst = v.fifo + (size_t)v.fifo_n * 2;
        uint64_t got = fe_decoder_read(v.dec, dst, (uint64_t)want);
        if (got == 0) {
            if (v.loop && fe_decoder_rewind(v.dec)) {
                got = fe_decoder_read(v.dec, dst, (uint64_t)want);
                if (got == 0) v.eof = true;
            } else {
                v.eof = true;
            }
        }
        if (got > 0 && v.channels == 1) {
            // The decoder gave `got` mono samples at the start of dst: widen in place.
            for (int i = (int)got - 1; i >= 0; i--) { float s = dst[i]; dst[i * 2] = s; dst[i * 2 + 1] = s; }
        }
        v.fifo_n += (int)got;
    }
}

static void pan_gains(const Mixer::Voice& v, float master, float* gl, float* gr) {
    float g = v.volume * master;
    if (v.channels == 1) {
        float th = (v.pan + 1.0f) * (kPi * 0.25f);
        *gl = g * fcos(th);
        *gr = g * fsin(th);
    } else {
        *gl = g * (v.pan > 0 ? 1.0f - v.pan : 1.0f);
        *gr = g * (v.pan < 0 ? 1.0f + v.pan : 1.0f);
    }
}

void Mixer::mix_block(int frames) {
    float* acc = acc_;
    memset(acc, 0, sizeof(float) * (size_t)frames * 2);
    const float inv = 1.0f / 32768.0f;
    for (int vi = 0; vi < nvoices_; vi++) {
        Voice& v = voices_[vi];
        if (!v.active || v.paused) continue;
        float tl, tr;
        pan_gains(v, master, &tl, &tr);
        if (v.fresh) { v.gl = tl; v.gr = tr; v.fresh = false; }
        float dl = (tl - v.gl) / (float)frames, dr = (tr - v.gr) / (float)frames;
        float gl = v.gl, gr = v.gr;
        double step = v.ratio * (double)v.pitch;
        double pos = v.pos;
        bool done = false;
        int n = frames;     // frames produced before the voice ended
        if (v.is_stream) {
            fill(v, (int)(pos + step * frames) + 3);
            const float* f = v.fifo;
            for (int i = 0; i < frames; i++) {
                int idx = (int)pos;
                if (idx >= v.fifo_n) { done = v.eof; n = i; break; }
                float fr = (float)(pos - idx);
                int j = idx + 1 < v.fifo_n ? idx + 1 : idx;
                float l = f[idx * 2] + (f[j * 2] - f[idx * 2]) * fr;
                float r = f[idx * 2 + 1] + (f[j * 2 + 1] - f[idx * 2 + 1]) * fr;
                gl += dl; gr += dr;
                acc[i * 2] += l * gl;
                acc[i * 2 + 1] += r * gr;
                pos += step;
            }
            // Drop what was played.
            int drop = (int)pos;
            if (drop > v.fifo_n) drop = v.fifo_n;
            if (drop > 0) {
                memmove(v.fifo, v.fifo + (size_t)drop * 2, sizeof(float) * (size_t)(v.fifo_n - drop) * 2);
                v.fifo_n -= drop;
                pos -= drop;
            }
            if (!done && v.eof && v.fifo_n == 0) done = true;
        } else {
            const Clip& c = *v.clip;
            const int16_t* s = c.data;
            const double len = (double)c.frames;
            for (int i = 0; i < frames; i++) {
                if (pos >= len) {
                    if (v.loop) { pos -= len * (double)(long)(pos / len); if (pos >= len) pos = 0; }
                    else { done = true; n = i; break; }
                }
                uint32_t idx = (uint32_t)pos;
                float fr = (float)(pos - (double)idx);
                uint32_t j = idx + 1 < c.frames ? idx + 1 : (v.loop ? 0 : idx);
                float l, r;
                if (c.channels == 1) {
                    float a = (float)s[idx], b = (float)s[j];
                    l = r = (a + (b - a) * fr) * inv;
                } else {
                    float a0 = (float)s[idx * 2], b0 = (float)s[j * 2];
                    float a1 = (float)s[idx * 2 + 1], b1 = (float)s[j * 2 + 1];
                    l = (a0 + (b0 - a0) * fr) * inv;
                    r = (a1 + (b1 - a1) * fr) * inv;
                }
                gl += dl; gr += dr;
                acc[i * 2] += l * gl;
                acc[i * 2 + 1] += r * gr;
                pos += step;
            }
        }
        v.played += step * (double)n;
        v.pos = pos;
        v.gl = tl;
        v.gr = tr;
        if (done) { release(v); ended_++; }
    }
}

void Mixer::mix(int16_t* out, int frames) {
    if (!out || frames <= 0) return;
    const bool voices = voices_ && active_count() > 0;
    if (voices) tail_ = rate_ * 3;
    const bool ringing = reverb_active() && tail_ > 0;
    if (!voices && !ringing) { memset(out, 0, sizeof(int16_t) * (size_t)frames * 2); return; }
    while (frames > 0) {
        int n = frames < kBlock ? frames : kBlock;
        if (voices) mix_block(n); else memset(acc_, 0, sizeof(float) * (size_t)n * 2);
        if (rv_) {
            // Ramp the controls towards the target, then run the effect.
            const float k = 0.15f;
            rv_->wet += (rv_target_.wet - rv_->wet) * k;
            rv_->room += (rv_target_.room - rv_->room) * k;
            rv_->damping += (rv_target_.damping - rv_->damping) * k;
            rv_->lowpass += (rv_target_.lowpass - rv_->lowpass) * k;
            rv_->process(acc_, n);
            if (!voices) tail_ -= n;
        }
        for (int i = 0; i < n * 2; i++) out[i] = to_s16(acc_[i]);
        out += n * 2;
        frames -= n;
    }
}

}  // namespace fe
