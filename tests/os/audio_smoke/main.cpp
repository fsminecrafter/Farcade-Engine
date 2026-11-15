// audio_smoke: plays a 440 Hz tone for one second through the real sound
// card: the mixer mixes a clip, the device queues it. Prints
//   FE AUDIO PASS <frames written> <underruns>
//   FE AUDIO PASS nodevice          (no sound card: nothing to test)
//   FE AUDIO FAIL <why>
#include <audio.h>      // the SDK's (SYS_ERR_*)
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "audio/audio.h"
#include "core/math.h"

using namespace fe;

static const unsigned char kMp3[] = {
#include "../../data/sine_mp3.inc"
};
static const unsigned char kOgg[] = {
#include "../../data/sine_ogg.inc"
};

static void put32(unsigned char* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i)); }
static void put16(unsigned char* p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

// Decodes `file` (440 Hz, 22050 Hz, mono) and checks the pitch in the middle.
static bool check_decode(const void* file, size_t size, uint32_t min_frames, uint32_t max_frames) {
    Clip* c = Clip::decode(file, size);
    if (!c) return false;
    bool ok = c->rate == 22050 && c->channels == 1 && c->frames >= min_frames && c->frames <= max_frames;
    if (ok) {
        int a = 2000, b = (int)c->frames - 2000, zc = 0;
        for (int i = a + 1; i < b; i++) if (c->data[i - 1] < 0 && c->data[i] >= 0) zc++;
        int expect = (int)(440.0 * (double)(b - a) / 22050.0);
        ok = zc >= expect - 3 && zc <= expect + 3;
    }
    c->destroy();
    return ok;
}

static int64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main() {
    // The decoders on this runtime (its libm, malloc, qsort): WAV, MP3, Ogg.
    {
        const int frames = 11025;
        static unsigned char wav[44 + 11025 * 2];
        memcpy(wav, "RIFF", 4); put32(wav + 4, 36 + frames * 2); memcpy(wav + 8, "WAVEfmt ", 8);
        put32(wav + 16, 16); put16(wav + 20, 1); put16(wav + 22, 1); put32(wav + 24, 22050);
        put32(wav + 28, 44100); put16(wav + 32, 2); put16(wav + 34, 16);
        memcpy(wav + 36, "data", 4); put32(wav + 40, frames * 2);
        for (int i = 0; i < frames; i++)
            put16(wav + 44 + i * 2, (uint32_t)(uint16_t)(int16_t)(16000.0f * fsin(2.0f * kPi * 440.0f * (float)i / 22050.0f)));
        if (!check_decode(wav, sizeof wav, frames, frames)) { printf("FE AUDIO FAIL decode_wav\n"); return 1; }
        if (!check_decode(kMp3, sizeof kMp3, 10000, 13000)) { printf("FE AUDIO FAIL decode_mp3\n"); return 1; }
        if (!check_decode(kOgg, sizeof kOgg, 10000, 13000)) { printf("FE AUDIO FAIL decode_ogg\n"); return 1; }
        static const unsigned char junk[64] = { 1, 2, 3, 4, 5 };
        if (Clip::decode(junk, sizeof junk)) { printf("FE AUDIO FAIL decode_junk\n"); return 1; }
        printf("audio: wav, mp3 and ogg decode\n");
    }

    AudioDevice dev;
    if (!dev.init(AudioDevice::Backend::MinimalOS, 48000)) {
        if ((long)dev.os_error() == (long)SYS_ERR_GENERIC) {
            printf("audio: no sound card (mos_audio_stream_open: SYS_ERR_GENERIC)\n");
            printf("FE AUDIO PASS nodevice\n");
            return 0;
        }
        printf("FE AUDIO FAIL stream_open %d\n", dev.os_error());
        return 1;
    }
    const int rate = dev.rate();
    printf("audio: device stream at %d Hz\n", rate);

    // One second of 440 Hz at 22050 Hz mono: the mixer resamples it.
    const int src_rate = 22050;
    static int16_t tone[22050];
    for (int i = 0; i < src_rate; i++) tone[i] = (int16_t)(12000.0f * fsin(2.0f * kPi * 440.0f * (float)i / (float)src_rate));
    Clip* clip = Clip::from_pcm(tone, src_rate, 1, src_rate);
    Mixer mixer;
    if (!clip || !mixer.init(rate)) { printf("FE AUDIO FAIL mixer\n"); return 1; }
    VoiceParams vp;
    vp.volume = 0.5f;
    if (mixer.play_clip(clip, vp) < 0) { printf("FE AUDIO FAIL play\n"); return 1; }

    static int16_t buf[1024 * 2];
    int64_t start = now_ms();
    while (mixer.active_count() > 0 && now_ms() - start < 4000) {
        int target = rate * 80 / 1000;
        int need = target - dev.queued();
        if (need > dev.writable()) need = dev.writable();
        if (need > 1024) need = 1024;
        if (need > 0) {
            mixer.mix(buf, need);
            // The device takes what fits: offer the rest again.
            int done = 0, spins = 0;
            while (done < need) {
                int n = dev.write(buf + done * 2, need - done);
                if (n > 0) { done += n; spins = 0; continue; }
                if (dev.os_error() < 0) { printf("FE AUDIO FAIL write %d\n", dev.os_error()); return 1; }
                if (++spins > 400) { printf("FE AUDIO FAIL write_stuck\n"); return 1; }
                usleep(5000);
            }
        } else {
            usleep(5000);
        }
    }
    if (mixer.active_count() > 0) { printf("FE AUDIO FAIL stalled %d\n", (int)dev.frames_written()); return 1; }
    uint64_t written = dev.frames_written();
    uint32_t under = dev.underruns();      // before the queue runs dry at the end
    // Let the queue play out.
    int64_t drain = now_ms();
    while (dev.queued() > 0 && now_ms() - drain < 1000) usleep(5000);
    int64_t elapsed = now_ms() - start;
    printf("audio: %d frames written in %d ms, %u underruns\n", (int)written, (int)elapsed, (unsigned)under);
    clip->destroy();
    if (written < (uint64_t)rate * 95 / 100) { printf("FE AUDIO FAIL frames %d\n", (int)written); return 1; }
    printf("FE AUDIO PASS %d %u\n", (int)written, (unsigned)under);
    return 0;
}
