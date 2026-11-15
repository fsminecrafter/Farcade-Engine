// AudioDevice: the Minimal-OS PCM stream and the Null device.
#include "audio/audio.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef FE_MINIMALOS
#include <audio.h>      // the SDK's (not engine/audio/audio.h)
#endif

namespace fe {

static int64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

AudioDevice::AudioDevice() {}
AudioDevice::~AudioDevice() { shutdown(); }

bool AudioDevice::init(Backend b, int rate) {
    shutdown();
    if (rate != 44100) rate = 48000;
    os_error_ = 0;
    if (b == Backend::Auto || b == Backend::MinimalOS) {
#ifdef FE_MINIMALOS
        int other = rate == 48000 ? 44100 : 48000;
        int h = mos_audio_stream_open((uint32_t)rate, 2);
        int used = rate;
        if (h < 0 && (long)h == (long)SYS_ERR_INVAL) { h = mos_audio_stream_open((uint32_t)other, 2); used = other; }
        if (h >= 0) {
            os_handle_ = h;
            rate_ = used;
            backend_ = Backend::MinimalOS;
            written_ = 0;
            return true;
        }
        os_error_ = h;
#else
        os_error_ = -1;     // not a Minimal-OS build
#endif
        if (b == Backend::MinimalOS) return false;
        // Auto: no sound card, play into the void in real time.
        b = Backend::Null;
        realtime_ = true;
    } else {
        realtime_ = false;
    }
    ring_ = (int16_t*)calloc((size_t)kCaptureFrames * 2, sizeof(int16_t));
    if (!ring_) return false;
    rate_ = rate;
    backend_ = Backend::Null;
    written_ = 0;
    cap_read_ = 0;
    null_queued_ = 0;
    null_underruns_ = 0;
    last_ns_ = now_ns();
    return true;
}

void AudioDevice::shutdown() {
#ifdef FE_MINIMALOS
    if (backend_ == Backend::MinimalOS && os_handle_ >= 0) mos_audio_stream_close(os_handle_);
#endif
    os_handle_ = -1;
    free(ring_);
    ring_ = nullptr;
    backend_ = Backend::Auto;
}

void AudioDevice::null_sync() {
    if (!realtime_) return;
    int64_t t = now_ns();
    double frames = (double)(t - last_ns_) * (double)rate_ * 1e-9;
    last_ns_ = t;
    if (frames <= 0) return;
    if (frames > null_queued_) {
        if (written_ > 0) null_underruns_++;
        null_queued_ = 0;
    } else {
        null_queued_ -= frames;
    }
}

void AudioDevice::null_advance(int frames) {
    if (backend_ != Backend::Null || realtime_ || frames <= 0) return;
    if ((double)frames > null_queued_) {
        if (written_ > 0) null_underruns_++;
        null_queued_ = 0;
    } else {
        null_queued_ -= frames;
    }
}

void AudioDevice::null_set_realtime(bool on) {
    if (backend_ != Backend::Null) return;
    realtime_ = on;
    last_ns_ = now_ns();
}

int AudioDevice::queued() {
    switch (backend_) {
#ifdef FE_MINIMALOS
    case Backend::MinimalOS: {
        long q = mos_audio_stream_queued(os_handle_);
        return q < 0 ? 0 : (int)q;
    }
#endif
    case Backend::Null:
        null_sync();
        return (int)null_queued_;
    default:
        return 0;
    }
}

int AudioDevice::write(const int16_t* stereo, int frames) {
    if (frames <= 0 || !stereo) return 0;
    int room = writable();
    if (frames > room) frames = room;
    if (frames <= 0) return 0;
    int taken = 0;
    if (backend_ == Backend::Null) {
        for (int i = 0; i < frames; i++) {
            size_t slot = (size_t)((written_ + (uint64_t)i) % (uint64_t)kCaptureFrames) * 2;
            ring_[slot] = stereo[i * 2];
            ring_[slot + 1] = stereo[i * 2 + 1];
        }
        null_queued_ += frames;
        taken = frames;
    }
#ifdef FE_MINIMALOS
    else if (backend_ == Backend::MinimalOS) {
        long n = mos_audio_stream_write(os_handle_, stereo, (uint32_t)frames);
        taken = n < 0 ? 0 : (int)n;
        if (n < 0) os_error_ = (int)n;
    }
#endif
    written_ += (uint64_t)taken;
    return taken;
}

uint32_t AudioDevice::underruns() {
    if (backend_ == Backend::Null) { null_sync(); return null_underruns_; }
#ifdef FE_MINIMALOS
    if (backend_ == Backend::MinimalOS) {
        syscall_audio_stream_t info;
        memset(&info, 0, sizeof info);
        if (mos_audio_stream_query(os_handle_, &info) >= 0) return info.underruns;
    }
#endif
    return 0;
}

int AudioDevice::capture(int16_t* out, int max_frames) {
    if (backend_ != Backend::Null || !out || max_frames <= 0) return 0;
    if (written_ - cap_read_ > (uint64_t)kCaptureFrames) cap_read_ = written_ - kCaptureFrames;
    int n = 0;
    while (n < max_frames && cap_read_ < written_) {
        size_t slot = (size_t)(cap_read_ % (uint64_t)kCaptureFrames) * 2;
        out[n * 2] = ring_[slot];
        out[n * 2 + 1] = ring_[slot + 1];
        cap_read_++;
        n++;
    }
    return n;
}

}  // namespace fe
