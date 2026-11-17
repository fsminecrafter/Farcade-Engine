// Feature levels (plan C4.1, E1.3): the one place that turns what the GPU
// device says (fglGetDeviceInfo caps) into the pipeline the renderer runs.
//
//   FL0       today's FGPU v2 on the CPU backend: the forward base tier (lights,
//             hemisphere / probe ambient, sky, post folded into the shaders),
//             CPU GI, a cheap post stack. Needs nothing else.
//   FL1Ready  FL0 plus the capabilities the kernel is adding (scissor, mipmaps,
//             16 texture units). The passes that use them switch on by cap bit
//             (DeviceCaps::scissor ...), never by backend name.
//   GpuTier   the multi pass pipeline (cascaded shadows, SSAO, bloom, HDR post,
//             FXAA, PBR): on a graphics card (FGPU_CAP_HW_3D), or forced.
//
// Everything is plain data and pure functions, so tests can check the choice
// for any set of caps without a device. Cap bits that today's ABI does not
// define are picked up when fgpu_abi.h defines FGPU_CAP_SCISSOR,
// FGPU_CAP_MIPMAPS or FGPU_CAP_MRT; until then they are off (tests and the
// settings UI can still set them with `DeviceCaps::forced_bits`).
#pragma once

#include <stdint.h>

namespace fe::gfx {

enum class FeatureLevel : uint8_t { FL0, FL1Ready, GpuTier };

// What Settings > Graphics asks for. Auto picks from the device caps.
enum class FeatureLevelMode : uint8_t { Auto, FL0, FL1Ready, GpuTier };

struct DeviceCaps {
    bool hw_3d = false;                 // FarcadeGL runs on a graphics card
    bool shaders = false;
    bool scissor = false;               // scissor rectangle (kernel work in progress)
    bool mipmaps = false;               // textures with mip chains
    bool mrt = false;                   // more than one color target per pass
    int texture_units = 4;              // FGPU_MAX_TEXTURE_UNITS of this SDK
    char backend[40] = "";

    // Builds the caps from the kernel's bits (syscall_gpu_info_t::caps).
    static DeviceCaps from_abi(uint32_t abi_caps, const char* backend_name);
    // A cap set for tests: pass FeatureCapBits.
    static DeviceCaps from_bits(uint32_t bits, int texture_units);
    bool tex16() const { return texture_units >= 16; }
    // Everything FL1Ready needs.
    bool fl1_complete() const { return scissor && mipmaps && tex16(); }
};

enum FeatureCapBits : uint32_t {
    kCapHw3d = 1u << 0, kCapShaders = 1u << 1, kCapScissor = 1u << 2, kCapMipmaps = 1u << 3, kCapMrt = 1u << 4,
};

struct FeatureLevelChoice {
    FeatureLevel level = FeatureLevel::FL0;
    FeatureLevel best_possible = FeatureLevel::FL0;     // what the caps allow without forcing
    bool forced = false;                                // GpuTier on a backend that has no graphics card
    bool clamped = false;                               // the request needed caps the device lacks
};

// `allow_gpu_on_cpu` is GraphicsSettings::allow_gpu_effects_on_cpu (forces GpuTier).
FeatureLevelChoice pick_feature_level(const DeviceCaps& caps, FeatureLevelMode mode, bool allow_gpu_on_cpu);

const char* feature_level_name(FeatureLevel l);          // "FL0", "FL1-ready", "GPU tier"
const char* feature_level_mode_name(FeatureLevelMode m);

// The capability gated switches of a frame (what a pass may use). GPU-only
// passes still need level == GpuTier; these say which optional hardware
// features exist so a pass can take the cheaper path (a scissored pass, a
// mip based blur, a single pass with several targets).
struct PassCaps {
    bool scissor = false, mipmaps = false, mrt = false, tex16 = false;
};
PassCaps pass_caps(const DeviceCaps& caps);

}  // namespace fe::gfx
