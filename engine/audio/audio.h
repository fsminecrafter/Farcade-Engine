// The audio system of the Farcade Engine. See docs/audio.md.
//
//   AudioDevice  where the sound goes: a Minimal-OS PCM stream (stereo
//                int16, 48000 or 44100 Hz) or a Null device (no sound card:
//                host builds, tests) that keeps what it was given.
//   Mixer        32 voices (decoded clips or files decoded on the fly),
//                volume, pan, pitch, loop; linear resampling to the device
//                rate; saturating mix into stereo int16.
//   AudioSystem  runs on an ECS World (Phase::Store): the AudioListener,
//                AudioStream and AudioSampler modules, spatialization, and
//                pushing the mix to the device every frame.
//
// This header includes no decoder and no OS header.
#pragma once

#include "ecs/ecs.h"
#include <stddef.h>
#include <stdint.h>

struct FeDecoder;

namespace fe {

// ---------------------------------------------------------------- device

class AudioDevice {
public:
    enum class Backend { Auto, MinimalOS, Null };

    AudioDevice();
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // Auto: Minimal-OS stream when this is a Minimal-OS build and a sound
    // card answers, else the Null device that follows the clock (real time).
    // MinimalOS: only that; false (see os_error()) when it cannot be opened.
    // Null: never fails; it plays nothing and consumes only what the caller
    // says with null_advance() (tests), which keeps runs deterministic.
    // `rate` is the wished device rate (48000 or 44100; the OS device tries
    // it, then the other one).
    bool init(Backend b = Backend::Auto, int rate = 48000);
    void shutdown();
    bool ready() const { return backend_ != Backend::Auto; }

    Backend backend() const { return backend_; }
    bool has_sound_card() const { return backend_ == Backend::MinimalOS; }
    int rate() const { return rate_; }
    // The (negative SYS_ERR_*) result of the last failed OS open, 0 if none.
    int os_error() const { return os_error_; }

    // Frames (stereo int16 pairs) waiting to be played, and room for more.
    int queued();
    int capacity() const { return kQueueFrames; }
    int writable() { int q = queued(); return q >= kQueueFrames ? 0 : kQueueFrames - q; }
    // Never blocks. Returns the frames taken (fewer when the queue is full).
    int write(const int16_t* stereo, int frames);

    // Times the device ran dry (OS: the kernel's count; Null: the clock or
    // null_advance() wanted more than was queued), and frames given so far.
    uint32_t underruns();
    uint64_t frames_written() const { return written_; }

    // ---- Null device
    // Plays `frames` frames of the queue (a manual device only).
    void null_advance(int frames);
    // Follow the clock instead (the default of Auto's fallback).
    void null_set_realtime(bool on);
    // Copies up to `max_frames` frames written since the last capture() call
    // (the oldest are lost when more than kCaptureFrames pile up). Returns
    // the count. Works on the Null device only.
    int capture(int16_t* out, int max_frames);

    static constexpr int kQueueFrames = 16384;      // the OS stream's capacity
    static constexpr int kCaptureFrames = 1 << 18;

private:
    Backend backend_ = Backend::Auto;       // Auto = not initialised
    int rate_ = 0;
    int os_error_ = 0;
    int os_handle_ = -1;
    uint64_t written_ = 0;
    // Null device
    bool realtime_ = false;
    double null_queued_ = 0;                // frames (fractional while following the clock)
    int64_t last_ns_ = 0;
    uint32_t null_underruns_ = 0;
    int16_t* ring_ = nullptr;
    uint64_t cap_read_ = 0;                 // frames read by capture()

    void null_sync();
};

// ---------------------------------------------------------------- clips

// A fully decoded sound: int16 interleaved, 1 or 2 channels, any rate.
struct Clip {
    int16_t* data = nullptr;
    uint32_t frames = 0;
    int channels = 1;
    int rate = 44100;

    // Decodes a WAV, MP3 or Ogg Vorbis file held in memory (all of it).
    // nullptr when it cannot be decoded. Free with destroy().
    static Clip* decode(const void* file, size_t size);
    // Copies PCM (frames * channels samples).
    static Clip* from_pcm(const int16_t* pcm, uint32_t frames, int channels, int rate);
    void destroy();
    float seconds() const { return rate > 0 ? (float)frames / (float)rate : 0.0f; }
};

// ---------------------------------------------------------------- mixer

struct VoiceParams {
    float volume = 1.0f;
    float pan = 0.0f;           // -1 left .. +1 right (a stereo source: balance)
    float pitch = 1.0f;         // playback speed, 1 = as recorded
    bool loop = false;
};

// A voice handle: 0 and negative are "none". It stays valid as a name after
// the voice ended (is_active() says false), it is never reused by another
// sound.
typedef int32_t VoiceId;

class Mixer {
public:
    static constexpr int kMaxVoices = 32;

    Mixer();
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    // `voices` up to kMaxVoices; `rate` is the output rate.
    bool init(int rate, int voices = kMaxVoices);
    void shutdown();                // stops everything

    // Plays a clip (not owned: it must outlive the voice, or stop_all()
    // first). When every voice is busy the oldest one-shot is stolen; -1 if
    // there is none to steal (all streams).
    VoiceId play_clip(const Clip* clip, const VoiceParams& p);
    // Plays a file decoded on the fly. The mixer takes ownership of `file`
    // (a malloc'd buffer of the whole file) and frees it when the voice ends
    // - also when this fails (-1: not decodable, or no voice free).
    VoiceId play_stream(void* file, size_t size, const VoiceParams& p);

    bool is_active(VoiceId v) const;
    void stop(VoiceId v);
    void stop_all();
    void pause(VoiceId v, bool paused);
    bool is_paused(VoiceId v) const;
    void set(VoiceId v, const VoiceParams& p);      // volume, pan, pitch, loop
    // Seconds played, in the sound's own time (not scaled by pitch).
    float position(VoiceId v) const;

    // Overall volume (the listener's volume).
    float master = 1.0f;

    // A reverb and a low-pass on the whole mix (the Audio Reverb Zone module):
    // four damped comb filters and two all-passes per side (a Freeverb-like
    // tail) mixed in by `wet`, then a one-pole low-pass (`lowpass` 0 = open,
    // 1 = very muffled). The values move towards the new ones over a few
    // blocks, so a zone's edge does not click. wet 0 and lowpass 0 cost
    // nothing; the buffers (about 60 KB) are made on first use. The tail
    // rings out after the last voice ends.
    struct ReverbParams { float wet = 0, room = 0.5f, damping = 0.5f, lowpass = 0; };
    void set_reverb(const ReverbParams& p);
    bool reverb_active() const;

    int rate() const { return rate_; }
    int voice_count() const { return nvoices_; }
    int active_count() const;
    // Voices that ended by themselves since init (non-looping sounds).
    uint32_t ended_count() const { return ended_; }

    // Mixes `frames` frames: stereo interleaved int16, saturating. Silence
    // when nothing plays.
    void mix(int16_t* out, int frames);

    struct Voice;       // mixer.cpp
private:
    Voice* voices_ = nullptr;
    int nvoices_ = 0;
    int rate_ = 48000;
    uint32_t serial_ = 0;
    uint32_t ended_ = 0;
    float* acc_ = nullptr;
    struct Reverb;
    Reverb* rv_ = nullptr;
    ReverbParams rv_target_;
    int tail_ = 0;

    Voice* find(VoiceId v) const;
    Voice* alloc();
    void mix_block(int frames);
};

// ---------------------------------------------------------------- system

// What the engine's host provides.
struct AudioHost {
    // The bytes of a project file, malloc'd (the system frees them), or
    // nullptr. `path` is as stored in the modules; the editor resolves it.
    void* (*read_file)(const char* path, size_t* size, void* user) = nullptr;
    // One line of text (no newline) for problems: missing or undecodable
    // files. May be null.
    void (*log)(const char* line, void* user) = nullptr;
    void* user = nullptr;
};

struct AudioStats {
    int voices_active = 0;
    uint32_t underruns = 0;         // of the device
    float mix_ms = 0;               // the last update: mixing + pushing
    float mix_ms_max = 0;
    uint64_t frames_pushed = 0;
    int clips_cached = 0;
};

class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // Registers the update on `world` (Phase::Store). `device` must be ready
    // and outlive this. Both pointers are kept.
    bool init(World* world, AudioDevice* device, const AudioHost& host);
    void shutdown();

    // One update now (the registered system calls it every frame): starts
    // new streams, syncs voices to the modules, spatializes, ends finished
    // voices, mixes and pushes to the device. Public for tests.
    void update(float dt);

    // Forget what started and stop everything: call when the world's
    // content is replaced (Play pressed again), so that play_on_start streams
    // start anew. Entities found later start their play_on_start stream on
    // the first update that sees them.
    void restart();

    // ---- for scripts and the editor
    // Plays the sampler's clip on a free voice, with pitch_variation. At most
    // `voices` of one entity overlap (the oldest is cut). false when the
    // entity has no AudioSampler or its file is missing/undecodable.
    bool play_sampler(Entity e);
    bool play_stream(Entity e);     // (re)starts from the beginning
    void stop_stream(Entity e);
    void pause_stream(Entity e, bool paused = true);    // false resumes
    bool is_playing(Entity e) const;    // a stream playing (not paused) or any sampler voice
    void stop_all();

    // Off: Audio Reverb Zones are ignored (saves the effect's mixing time).
    bool reverb_enabled = true;

    AudioStats stats() const;
    Mixer& mixer() { return mixer_; }

    // Drops decoded sampler clips (stops their voices first).
    void clear_cache();

private:
    struct Impl;
    Impl* impl_ = nullptr;
    Mixer mixer_;
};

}  // namespace fe
