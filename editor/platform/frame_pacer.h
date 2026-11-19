// When a finished frame is shown, for vsync (graphics_settings.h VsyncMode).
//
// Minimal-OS has no vertical blank signal for programs, so vsync is paced by
// the clock: refreshes happen every 1/hz seconds from the first present, and
// a frame is shown at one of them.
//   Double:   at the first refresh after the frame is done (a late frame
//             waits for the next one: 60, 30, 20 ... fps).
//   Adaptive: like Double while frames are on time; a late frame (past the
//             refresh it was meant for) is shown at once.
//   Triple:   the presenting thread shows the newest finished frame at each
//             refresh (next_refresh()); the game never waits.
#pragma once

namespace fe::platform {

enum class Vsync : int { Off = 0, Double = 1, Triple = 2, Adaptive = 3 };

class FramePacer {
public:
    void set(Vsync mode, int hz) {
        if (hz < 30) hz = 30;
        if (hz > 240) hz = 240;
        if (mode != mode_ || hz != hz_) { mode_ = mode; hz_ = hz; started_ = false; }
    }
    Vsync mode() const { return mode_; }
    double period() const { return 1.0 / (double)hz_; }

    // The first refresh at or after `now` (seconds).
    double next_refresh(double now) {
        if (!started_) { started_ = true; origin_ = now; last_ = now - period(); return now; }
        double k = (now - origin_) / period();
        long n = (long)k;
        if ((double)n < k - 1e-9) n++;
        return origin_ + (double)n * period();
    }

    // A frame is done at `now`: when to show it (now for Off and late
    // Adaptive frames). Remembers the refresh it took.
    double present_time(double now) {
        if (mode_ == Vsync::Off) return now;
        double at;
        if (mode_ == Vsync::Adaptive && started_) {
            double meant = last_ + period();
            if (now > meant + 0.001) {              // late: at once, and the refreshes count from here
                origin_ = now;
                last_ = now;
                return now;
            }
            at = meant > now ? meant : next_refresh(now);
        } else {
            at = next_refresh(now);
            // Never twice on one refresh.
            if (started_ && at < last_ + period() * 0.5) at += period();
        }
        last_ = at;
        return at;
    }

private:
    Vsync mode_ = Vsync::Off;
    int hz_ = 60;
    bool started_ = false;
    double origin_ = 0, last_ = 0;
};

}  // namespace fe::platform
