// Tests of the renderer's upgrade work: feature levels, the post stack
// (tonemaps, auto exposure, grading, dynamic resolution), cascaded shadows and
// the CPU global illumination. The older tests are in gfx_tests.cpp.
#include "gfx/renderer.h"
#include "gfx/feature_level.h"
#include "gfx/meshgen.h"
#include "gfx/host/farcadegl_host.h"
#include "mof_check.h"

#include <math.h>
#include <string.h>

using namespace fe;
using namespace fe::gfx;

namespace {

// ---------------------------------------------------------------- feature levels

void test_feature_levels() {
    // Today's FGPU v2 on the CPU backend: FL0.
    DeviceCaps cpu = DeviceCaps::from_bits(kCapShaders, 4);
    FeatureLevelChoice c = pick_feature_level(cpu, FeatureLevelMode::Auto, false);
    CHECK(c.level == FeatureLevel::FL0 && !c.forced && !c.clamped);
    // The old switch forces the GPU tier.
    c = pick_feature_level(cpu, FeatureLevelMode::Auto, true);
    CHECK(c.level == FeatureLevel::GpuTier && c.forced);
    // A graphics card: GPU tier, and the user can ask for less.
    DeviceCaps gpu = DeviceCaps::from_bits(kCapShaders | kCapHw3d, 4);
    c = pick_feature_level(gpu, FeatureLevelMode::Auto, false);
    CHECK(c.level == FeatureLevel::GpuTier && !c.forced);
    c = pick_feature_level(gpu, FeatureLevelMode::FL0, false);
    CHECK(c.level == FeatureLevel::FL0 && c.best_possible == FeatureLevel::GpuTier);
    // Scissor + mipmaps alone are not FL1-ready (16 units are needed too); with them it is.
    DeviceCaps part = DeviceCaps::from_bits(kCapShaders | kCapScissor | kCapMipmaps, 4);
    CHECK(pick_feature_level(part, FeatureLevelMode::Auto, false).level == FeatureLevel::FL0);
    DeviceCaps fl1 = DeviceCaps::from_bits(kCapShaders | kCapScissor | kCapMipmaps, 16);
    CHECK(fl1.fl1_complete());
    c = pick_feature_level(fl1, FeatureLevelMode::Auto, false);
    CHECK(c.level == FeatureLevel::FL1Ready && c.best_possible == FeatureLevel::FL1Ready);
    // Asking for FL1 without the caps is clamped to FL0; asking for the GPU tier without a card is forced.
    c = pick_feature_level(cpu, FeatureLevelMode::FL1Ready, false);
    CHECK(c.level == FeatureLevel::FL0 && c.clamped);
    c = pick_feature_level(cpu, FeatureLevelMode::GpuTier, false);
    CHECK(c.level == FeatureLevel::GpuTier && c.forced);
    PassCaps pc = pass_caps(fl1);
    CHECK(pc.scissor && pc.mipmaps && pc.tex16 && !pc.mrt);
    CHECK_STR(feature_level_name(FeatureLevel::FL1Ready), "FL1-ready");

    // The renderer reports the same on the host device (a CPU backend).
    Renderer r;
    GraphicsSettings g;
    r.set_settings(g);
    CHECK(r.init(64, 48));
    GraphicsFeatures f = r.features();
    CHECK(f.level == FeatureLevel::FL0 && !f.gpu_tier && !f.gpu);
    CHECK(!r.caps().hw_3d && r.caps().shaders);
    g.allow_gpu_effects_on_cpu = true;
    r.set_settings(g);
    f = r.features();
    CHECK(f.level == FeatureLevel::GpuTier && f.level_forced && f.gpu_tier);
    g.feature_level = FeatureLevelMode::FL0;
    g.allow_gpu_effects_on_cpu = false;
    r.set_settings(g);
    CHECK(r.feature_level() == FeatureLevel::FL0);
    r.shutdown();
}

}  // namespace

int main() {
    test_feature_levels();
    return finish_tests("gfx_stack_tests");
}
