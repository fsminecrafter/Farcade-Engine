// How sharp the viewport image is while the scene changes, and when it is
// refined: a render at 1/d of the viewport size (d = 1..8) shown stretched.
//
// The cost of a render is proportional to its pixels (the CPU preview and
// FarcadeGL alike), so the controller measures milliseconds per pixel and picks
// the smallest d that keeps a render under a budget: a short one while the
// image keeps changing (camera drag, Play), a longer one for the single
// refining render after it stopped. On a fast machine d is 1 all the time.
#pragma once

namespace fe {

class ViewQuality {
public:
    static constexpr double kInteractiveMs = 200.0;     // render budget while the view keeps changing
    static constexpr double kSettledMs = 500.0;         // the one refining render after it stopped
    static constexpr int kMaxDiv = 8;

    // has_draft: the renderer has a cheaper draft shading for images that are
    // replaced at once (FarcadeGL), used while the view changes.
    explicit ViewQuality(bool has_draft = false) : has_draft_(has_draft) {}

    bool has_draft() const { return has_draft_; }
    int interactive_div(int w, int h) const { return div_for(w, h, kInteractiveMs, has_draft_); }
    int settled_div(int w, int h) const { return div_for(w, h, kSettledMs, false); }

    // A render of `pixels` pixels took `ms` (with draft shading or not).
    void report(long pixels, double ms, bool draft) {
        if (pixels <= 0 || ms <= 0) return;
        double c = ms / (double)pixels;
        double& k = cost_[draft ? 1 : 0];
        k = k > 0 ? k * 0.5 + c * 0.5 : c;
    }
    bool known() const { return cost_[0] > 0 || cost_[1] > 0; }

private:
    // ms per pixel, measured for the kind of shading, or guessed from the other
    // (draft shading costs about 0.4 of the full one).
    double cost(bool draft) const {
        if (cost_[draft ? 1 : 0] > 0) return cost_[draft ? 1 : 0];
        return draft ? cost_[0] * 0.4 : cost_[1] * 2.5;
    }
    int div_for(int w, int h, double budget, bool draft) const {
        double c = cost(draft);
        if (c <= 0) return 4;                           // not measured yet: a quick first image
        double px = (double)w * (double)h;
        for (int d = 1; d < kMaxDiv; d++)
            if (px / (double)(d * d) * c <= budget) return d;
        return kMaxDiv;
    }
    double cost_[2] = { 0, 0 };
    bool has_draft_;
};

}  // namespace fe
