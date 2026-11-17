#include "gfx/feature_level.h"
#include "fgpu_abi.h"

#include <string.h>

namespace fe::gfx {

DeviceCaps DeviceCaps::from_abi(uint32_t abi, const char* backend_name) {
    DeviceCaps c;
    c.hw_3d = (abi & FGPU_CAP_HW_3D) != 0;
    c.shaders = (abi & FGPU_CAP_SHADERS) != 0;
#if defined(FGPU_CAP_SCISSOR)
    c.scissor = (abi & FGPU_CAP_SCISSOR) != 0;
#endif
#if defined(FGPU_CAP_MIPMAPS)
    c.mipmaps = (abi & FGPU_CAP_MIPMAPS) != 0;
#endif
#if defined(FGPU_CAP_MRT)
    c.mrt = (abi & FGPU_CAP_MRT) != 0;
#endif
    c.texture_units = (int)FGPU_MAX_TEXTURE_UNITS;
    if (backend_name) {
        strncpy(c.backend, backend_name, sizeof c.backend - 1);
        c.backend[sizeof c.backend - 1] = 0;
    }
    return c;
}

DeviceCaps DeviceCaps::from_bits(uint32_t bits, int texture_units) {
    DeviceCaps c;
    c.hw_3d = (bits & kCapHw3d) != 0;
    c.shaders = (bits & kCapShaders) != 0;
    c.scissor = (bits & kCapScissor) != 0;
    c.mipmaps = (bits & kCapMipmaps) != 0;
    c.mrt = (bits & kCapMrt) != 0;
    c.texture_units = texture_units;
    return c;
}

FeatureLevelChoice pick_feature_level(const DeviceCaps& caps, FeatureLevelMode mode, bool allow_gpu_on_cpu) {
    FeatureLevelChoice r;
    r.best_possible = caps.hw_3d ? FeatureLevel::GpuTier : (caps.fl1_complete() ? FeatureLevel::FL1Ready : FeatureLevel::FL0);
    FeatureLevel want = r.best_possible;
    switch (mode) {
        case FeatureLevelMode::Auto: break;
        case FeatureLevelMode::FL0: want = FeatureLevel::FL0; break;
        case FeatureLevelMode::FL1Ready: want = FeatureLevel::FL1Ready; break;
        case FeatureLevelMode::GpuTier: want = FeatureLevel::GpuTier; break;
    }
    if (allow_gpu_on_cpu && mode == FeatureLevelMode::Auto) want = FeatureLevel::GpuTier;   // the old switch
    r.level = want;
    if (want == FeatureLevel::GpuTier && !caps.hw_3d) {
        r.forced = allow_gpu_on_cpu || mode == FeatureLevelMode::GpuTier;
        if (!r.forced) { r.level = r.best_possible; r.clamped = true; }
    } else if (want == FeatureLevel::FL1Ready && !caps.fl1_complete() && !caps.hw_3d) {
        r.level = FeatureLevel::FL0;
        r.clamped = true;
    }
    // GpuTier asked for explicitly on a CPU backend is the "force" of the settings UI.
    return r;
}

const char* feature_level_name(FeatureLevel l) {
    switch (l) {
        case FeatureLevel::FL0: return "FL0";
        case FeatureLevel::FL1Ready: return "FL1-ready";
        case FeatureLevel::GpuTier: return "GPU tier";
    }
    return "?";
}

const char* feature_level_mode_name(FeatureLevelMode m) {
    switch (m) {
        case FeatureLevelMode::Auto: return "Auto";
        case FeatureLevelMode::FL0: return "FL0";
        case FeatureLevelMode::FL1Ready: return "FL1-ready";
        case FeatureLevelMode::GpuTier: return "GPU tier";
    }
    return "?";
}

PassCaps pass_caps(const DeviceCaps& c) {
    PassCaps p;
    p.scissor = c.scissor;
    p.mipmaps = c.mipmaps;
    p.mrt = c.mrt;
    p.tex16 = c.tex16();
    return p;
}

}  // namespace fe::gfx
