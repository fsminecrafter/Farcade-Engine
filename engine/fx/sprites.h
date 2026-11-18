// Billboard and Text 3D modules: gathers the pictures of a world into the
// renderer's batches and labels (gfx/particles.h), once per frame, for a
// camera. Billboards are quads that face the camera (a texture or a flat
// color, alpha or additive); Text 3D is a label in a built-in bitmap font.
// A host that draws with its own tools (the editor draws them with ImGui)
// does not need this.
#pragma once

#include "ecs/ecs.h"
#include "gfx/particles.h"

namespace fe {

constexpr int kMaxSprites = 256;        // billboards and labels each, per frame

class SpriteBank {
public:
    ~SpriteBank();
    // Gathers the visible Billboards and Text 3Ds of `w`, far ones first.
    void collect(World& w, const Vec3& eye);
    const gfx::ParticleBatch* batches() const { return batches_; }
    int batch_count() const { return nb_; }
    const gfx::TextLabel* labels() const { return labels_; }
    int label_count() const { return nl_; }

private:
    gfx::Particle* quads_ = nullptr;
    gfx::ParticleBatch* batches_ = nullptr;
    gfx::TextLabel* labels_ = nullptr;
    int nb_ = 0, nl_ = 0;
    Entity* scan_ = nullptr;
    int scan_cap_ = 0;
    float* dist_ = nullptr;
};

}  // namespace fe
