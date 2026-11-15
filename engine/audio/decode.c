// The decoders behind decode.h.
#ifndef DR_WAV_NO_STDIO
#define DR_WAV_NO_STDIO
#endif
#ifndef DR_MP3_NO_STDIO
#define DR_MP3_NO_STDIO
#endif
#ifndef STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_STDIO
#endif
#define STB_VORBIS_HEADER_ONLY
#include "dr_libs/dr_wav.h"
#include "dr_libs/dr_mp3.h"
#include "stb/stb_vorbis.c"
#include "audio/decode.h"

#include <stdlib.h>
#include <string.h>

enum { KIND_WAV, KIND_MP3, KIND_OGG };

struct FeDecoder {
    int kind;
    int channels;       // of the file
    drwav wav;
    drmp3 mp3;
    stb_vorbis* ogg;
    float* tmp;         // for files with more than 2 channels
    uint64_t tmp_frames;
};

static int is_wav(const unsigned char* p, size_t n) {
    if (n >= 12 && (!memcmp(p, "RIFF", 4) || !memcmp(p, "RF64", 4)) && !memcmp(p + 8, "WAVE", 4)) return 1;
    return n >= 16 && !memcmp(p, "riff", 4);      // Wave64
}

FeDecoder* fe_decoder_open(const void* data, size_t size, int* rate, int* channels, uint64_t* frames) {
    const unsigned char* p = (const unsigned char*)data;
    if (!p || size < 16) return NULL;
    FeDecoder* d = (FeDecoder*)calloc(1, sizeof *d);
    if (!d) return NULL;
    uint64_t total = 0;
    int r = 0, ch = 0;
    if (size >= 4 && !memcmp(p, "OggS", 4)) {
        int err = 0;
        d->kind = KIND_OGG;
        d->ogg = stb_vorbis_open_memory(p, (int)(size > 0x7fffffff ? 0x7fffffff : size), &err, NULL);
        if (!d->ogg) { free(d); return NULL; }
        stb_vorbis_info info = stb_vorbis_get_info(d->ogg);
        r = (int)info.sample_rate; ch = info.channels;
        total = stb_vorbis_stream_length_in_samples(d->ogg);
    } else if (is_wav(p, size)) {
        d->kind = KIND_WAV;
        if (!drwav_init_memory(&d->wav, p, size, NULL)) { free(d); return NULL; }
        r = (int)d->wav.sampleRate; ch = d->wav.channels;
        total = d->wav.totalPCMFrameCount;
    } else {
        d->kind = KIND_MP3;
        if (!drmp3_init_memory(&d->mp3, p, size, NULL)) { free(d); return NULL; }
        r = (int)d->mp3.sampleRate; ch = d->mp3.channels;
        total = 0;
    }
    if (r < 1000 || r > 400000 || ch < 1 || ch > 8) { d->channels = ch; fe_decoder_close(d); return NULL; }
    d->channels = ch;
    *rate = r;
    *channels = ch > 2 ? 2 : ch;
    *frames = total;
    return d;
}

static uint64_t read_raw(FeDecoder* d, float* out, uint64_t frames) {
    switch (d->kind) {
    case KIND_WAV: return drwav_read_pcm_frames_f32(&d->wav, frames, out);
    case KIND_MP3: return drmp3_read_pcm_frames_f32(&d->mp3, frames, out);
    default: {
        // stb_vorbis counts channels in the call; it returns frames.
        int n = stb_vorbis_get_samples_float_interleaved(d->ogg, d->channels, out,
                                                         (int)(frames * (uint64_t)d->channels));
        return (uint64_t)(n < 0 ? 0 : n);
    }
    }
}

uint64_t fe_decoder_read(FeDecoder* d, float* out, uint64_t frames) {
    if (!d || !frames) return 0;
    if (d->channels <= 2) return read_raw(d, out, frames);
    // More than two channels: keep the first two.
    uint64_t done = 0;
    while (done < frames) {
        uint64_t want = frames - done;
        if (want > 256) want = 256;
        if (d->tmp_frames < want) {
            free(d->tmp);
            d->tmp = (float*)malloc(sizeof(float) * (size_t)d->channels * 256);
            if (!d->tmp) { d->tmp_frames = 0; break; }
            d->tmp_frames = 256;
        }
        uint64_t got = read_raw(d, d->tmp, want);
        for (uint64_t i = 0; i < got; i++) {
            out[(done + i) * 2] = d->tmp[i * (uint64_t)d->channels];
            out[(done + i) * 2 + 1] = d->tmp[i * (uint64_t)d->channels + 1];
        }
        done += got;
        if (got < want) break;
    }
    return done;
}

int fe_decoder_rewind(FeDecoder* d) {
    if (!d) return 0;
    switch (d->kind) {
    case KIND_WAV: return drwav_seek_to_pcm_frame(&d->wav, 0) ? 1 : 0;
    case KIND_MP3: return drmp3_seek_to_pcm_frame(&d->mp3, 0) ? 1 : 0;
    default: return stb_vorbis_seek_start(d->ogg) ? 1 : 0;
    }
}

void fe_decoder_close(FeDecoder* d) {
    if (!d) return;
    switch (d->kind) {
    case KIND_WAV: drwav_uninit(&d->wav); break;
    case KIND_MP3: drmp3_uninit(&d->mp3); break;
    default: if (d->ogg) stb_vorbis_close(d->ogg); break;
    }
    free(d->tmp);
    free(d);
}
